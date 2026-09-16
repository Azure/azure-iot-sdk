// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTT v5 twin client.
 *
 *   Device -> service : ih/{device_id}/srv/twin
 *   Service -> device : ih/{device_id}/dev/twin
 *
 * One topic each way. What a message is rides a `type` user property --
 * "get:1", "reported-patch:1" outbound; "get-response", "reported-patch-
 * response", "twin-push", "desired-patch" inbound -- and the body is protobuf
 * (common/Protos/twin.proto), published at QoS 0.
 *
 * Correlation splits by who started the exchange. A GET or a reported patch
 * carries a 16-byte UUID the device mints per attempt and the service echoes.
 * A twin-push or desired-patch is backend-initiated and instead carries the
 * connection's birth nonce, so traffic left over from a connection that has
 * since been replaced is recognizable and dropped.
 *
 * No subscription of its own: the presence handshake already holds
 * "ih/{device_id}/dev/#", which covers the inbound topic.
 */
#include <stdbool.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/gen2/az_iot_twin_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/proto3.h"
#include "internal/reconnect.h"
#include "internal/span_writer.h"
#include "internal/twin_client_internal.h"

#define AZ_IOT_TWIN_TOPIC_MAX 192

/* Topic space. */
#define TWIN_TOPIC_ROOT "ih/"
#define TWIN_INBOUND_SUFFIX "/dev/twin"
#define TWIN_OUTBOUND_SUFFIX "/srv/twin"

/* Message kinds, carried in the `type` user property. Outbound values include
 * the schema version the service expects; inbound ones are matched on the bare
 * name so a later schema revision still routes. */
#define TWIN_TYPE_KEY "type"
#define TWIN_CONTENT_TYPE "application/protobuf"
#define TWIN_TYPE_GET "get:1"
#define TWIN_TYPE_REPORTED_PATCH "reported-patch:1"
#define TWIN_TYPE_GET_RESPONSE "get-response"
#define TWIN_TYPE_PATCH_RESPONSE "reported-patch-response"
#define TWIN_TYPE_TWIN_PUSH "twin-push"
#define TWIN_TYPE_DESIRED_PATCH "desired-patch"

/* twin.proto field numbers. */
#define TWIN_F_GET_SECTIONS 1u
#define TWIN_F_GET_IF_NOT_MATCH_DESIRED 2u
#define TWIN_F_GET_IF_NOT_MATCH_REPORTED 3u
#define TWIN_F_SECTION_VERSION 1u
#define TWIN_F_SECTION_PAYLOAD 2u
#define TWIN_F_PUSH_DESIRED 1u
#define TWIN_F_PUSH_REPORTED 2u
#define TWIN_F_PATCH_VERSION 1u
#define TWIN_F_PATCH_PAYLOAD 2u
#define TWIN_F_GET_RESP_DESIRED_VERSION 1u
#define TWIN_F_GET_RESP_REPORTED_VERSION 2u
#define TWIN_F_GET_RESP_DESIRED_PAYLOAD 3u
#define TWIN_F_GET_RESP_REPORTED_PAYLOAD 4u
#define TWIN_F_REPORTED_IF_MATCH 1u
#define TWIN_F_REPORTED_PAYLOAD 2u
#define TWIN_F_PATCH_RESP_RESULT 1u
#define TWIN_F_PATCH_RESP_VERSION 2u

/* TwinGet.Sections: request both sections. */
#define TWIN_SECTIONS_BOTH 3u

/* Largest TwinGet body this client emits: the sections selector plus two
 * optional if-not-match versions. */
#define TWIN_GET_BODY_MAX 32

/* Upper bound of the jitter added to every defensive timeout. */
#define TWIN_TIMEOUT_JITTER_MS 300000u

/* The pending slot kind values stored in _internal.pending[].kind */
#define TWIN_PENDING_NONE 0
#define TWIN_PENDING_GET 1
#define TWIN_PENDING_PATCH 2

/* Internal shorthand to access _internal fields */
#define TI(t) ((t)->_internal)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Allocate an unused pending slot. Returns slot index or -1 if full. */
static int alloc_pending(az_iot_gen2_twin_client* t)
{
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (!TI(t).pending[i].in_use)
    {
      return i;
    }
  }
  return -1;
}

/* True when the message's `type` user property is @p want, matching either the
 * bare name or the "<name>:<schemaVersion>" form the service stamps. */
static bool type_is(const az_iot_mqtt_message* msg, const char* want)
{
  for (size_t i = 0; i < msg->user_properties_count; ++i)
  {
    const az_iot_mqtt_user_property* up = &msg->user_properties[i];
    if (!up->key || strcmp(up->key, TWIN_TYPE_KEY) != 0)
    {
      continue;
    }
    if (!up->value)
    {
      return false;
    }
    size_t n = strlen(want);
    return strncmp(up->value, want, n) == 0 && (up->value[n] == '\0' || up->value[n] == ':');
  }
  return false;
}

/* True when @p topic is exactly "ih/{device_id}/dev/twin".
 *
 * The inbound handler is registered by prefix and dispatch takes the longest
 * match, so a message published to a longer topic under the same root still
 * arrives here. Nothing below this topic is part of the protocol, so matching
 * the length is enough to reject it. 0 until bind_topics() runs, which rejects
 * everything. */
static bool topic_is_inbound(az_iot_gen2_twin_client* t, const char* topic)
{
  return TI(t).inbound_topic_len > 0 && strlen(topic) == TI(t).inbound_topic_len;
}

/* Find the pending slot whose correlation id matches the message's Correlation
 * Data. Returns the slot index or -1. */
static int find_pending_by_corr(az_iot_gen2_twin_client* t, const az_iot_mqtt_message* msg)
{
  if (!msg->correlation_data || msg->correlation_data_len != AZ_IOT_CORRELATION_UUID_LEN)
  {
    return -1;
  }

  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(t).pending[i].in_use
        && memcmp(TI(t).pending[i].corr, msg->correlation_data, AZ_IOT_CORRELATION_UUID_LEN) == 0)
    {
      return i;
    }
  }
  return -1;
}

/* True when a backend-initiated message belongs to the connection the client is
 * currently on. twin-push and desired-patch carry the birth nonce as
 * Correlation Data precisely so traffic from a defunct connection can be
 * dropped rather than applied to state it no longer describes. */
static bool matches_connection(az_iot_gen2_twin_client* t, const az_iot_mqtt_message* msg)
{
  uint8_t nonce[AZ_IOT_CORRELATION_UUID_LEN];
  if (az_iot_connection_client__presence_nonce(TI(t).conn, nonce) != AZ_IOT_OK)
  {
    return false;
  }

  return msg->correlation_data != NULL && msg->correlation_data_len == AZ_IOT_CORRELATION_UUID_LEN
      && memcmp(msg->correlation_data, nonce, AZ_IOT_CORRELATION_UUID_LEN) == 0;
}

/* Defensive timeout schedule, matching the presence handshake: the base grows
 * 5, 6, 8 then 10 minutes, each with up to 5 minutes of jitter.
 *
 * Deliberately long. MQTT keep-alive is what proves the connection is healthy,
 * so an exchange going unanswered on a healthy connection means degeneration
 * somewhere the device cannot influence -- a slow backend, egress lag, a
 * dispatch stall. Retrying quickly would only add load to something already
 * struggling, and without jitter every affected device would retry in
 * lockstep. */
static uint64_t defensive_deadline(az_iot_gen2_twin_client* t, uint32_t attempt)
{
  static const uint64_t k_base_ms[] = { 300000u, 360000u, 480000u, 600000u };
  uint64_t base = k_base_ms[attempt < 4u ? attempt : 3u];

  /* Draw the jitter from the connection's PRNG. */
  uint8_t r[AZ_IOT_CORRELATION_UUID_LEN];
  az_iot_connection_client__gen_uuid(TI(t).conn, r);
  uint64_t jitter = ((uint64_t)(((uint32_t)r[0] << 8) | r[1]) * TWIN_TIMEOUT_JITTER_MS) / 65535u;

  return az_iot_time_mono_ms() + base + jitter;
}

/* Forget everything scoped to the resync in progress, if any. */
static void resync_reset(az_iot_gen2_twin_client* t)
{
  TI(t).resyncing = false;
  TI(t).resync_used = 0;
  TI(t).resync_count = 0;
}

/* Adopt the authoritative versions the birth-ack carried, once per connection.
 *
 * The birth nonce identifies the connection those versions belong to, so a
 * reconnect is noticed here and the versions re-seeded: they are the service's
 * state as of the new birth admission, not the stale ones this client was
 * tracking against a session that has ended.
 *
 * `reported_version` is adopted directly -- it is only ever the if_match for
 * the next write. `desired_local` is reset to 0 instead, because the device
 * advertised version 0 in its birth (this SDK does not persist twin state) and
 * holds no desired payload for this connection until a push or a GET delivers
 * one. Treating the authoritative version as applied would make the next
 * incremental patch look in-order and get merged onto state the device does
 * not have.
 *
 * Also arms the twin-push expectation: one timer covers both sections, since a
 * push is a single message. */
static void sync_versions(az_iot_gen2_twin_client* t)
{
  uint8_t nonce[AZ_IOT_CORRELATION_UUID_LEN];
  if (az_iot_connection_client__presence_nonce(TI(t).conn, nonce) != AZ_IOT_OK)
  {
    return;
  }

  if (TI(t).nonce_valid && memcmp(TI(t).nonce, nonce, AZ_IOT_CORRELATION_UUID_LEN) == 0)
  {
    return;
  }

  uint64_t desired = 0;
  uint64_t reported = 0;
  (void)az_iot_connection_client__presence_twin_versions(TI(t).conn, &desired, &reported);

  TI(t).desired_auth = desired;
  TI(t).desired_local = 0;
  TI(t).reported_version = reported;
  memcpy(TI(t).nonce, nonce, AZ_IOT_CORRELATION_UUID_LEN);
  TI(t).nonce_valid = true;

  resync_reset(t);

  bool push_desired = false;
  bool push_reported = false;
  az_iot_connection_client__twin_push_flags(TI(t).conn, &push_desired, &push_reported);

  TI(t).push_expected = (push_desired && TI(t).desired_local != TI(t).desired_auth)
      || (push_reported && reported != 0);
  TI(t).push_attempt = 0;
  if (TI(t).push_expected)
  {
    TI(t).push_deadline_ms = defensive_deadline(t, 0);
  }
}

/* ------------------------------------------------------------------------- */
/* completing a pending request                                              */
/* ------------------------------------------------------------------------- */

/* Release slot @p idx and hand the outcome to whichever callback it holds.
 *
 * The slot is released BEFORE the callback runs so a callback that re-issues
 * its request immediately can claim it. @p twin and @p result are the decoded
 * results and are NULL on failure. */
static void release_pending(
    az_iot_gen2_twin_client* t,
    int idx,
    az_iot_result status,
    const az_iot_gen2_twin_state* twin,
    const az_iot_gen2_twin_patch_result* result)
{
  int kind = TI(t).pending[idx].kind;
  bool internal = TI(t).pending[idx].internal;
  void* ctx = TI(t).pending[idx].user_ctx;
  az_iot_gen2_twin_get_callback get_cb = TI(t).pending[idx].cb.get_cb;
  az_iot_gen2_twin_patch_ack_callback patch_cb = TI(t).pending[idx].cb.patch_cb;

  memset(&TI(t).pending[idx], 0, sizeof(TI(t).pending[idx]));

  /* The SDK issues its own resync GET; there is no caller to report to. */
  if (internal)
  {
    return;
  }

  if (kind == TWIN_PENDING_GET)
  {
    if (get_cb)
    {
      get_cb(status, twin, ctx);
    }
  }
  else if (kind == TWIN_PENDING_PATCH)
  {
    if (patch_cb)
    {
      patch_cb(status, result, ctx);
    }
  }
}

/* Complete every pending request with an error and release its slot.
 *
 * Called when the MQTT session ends. The responses these requests were waiting
 * for would have arrived on that session, so they can never come now; holding
 * the slots would leak one per outage until AZ_IOT_TWIN_MAX_PENDING is gone and
 * the client refuses every further request. */
static void twin_fail_pending(void* user_ctx)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t)
  {
    return;
  }

  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(t).pending[i].in_use)
    {
      release_pending(t, i, AZ_IOT_ERR_NOT_CONNECTED, NULL, NULL);
    }
  }

  /* The resync buffer holds patches from the ended connection's patch
   * sequence; they mean nothing against a fresh one. */
  resync_reset(t);
  TI(t).push_expected = false;
}

/* ------------------------------------------------------------------------- */
/* desired resync                                                            */
/* ------------------------------------------------------------------------- */

/* Forward declaration: the desired state machine issues its own GET. */
static az_iot_result issue_get(
    az_iot_gen2_twin_client* t,
    const az_iot_gen2_twin_get_options* opts,
    az_iot_gen2_twin_get_callback cb,
    void* user_ctx,
    bool internal);

/* Hand a desired patch to the application, if it is listening. */
static void dispatch_desired(
    az_iot_gen2_twin_client* t,
    const uint8_t* payload,
    size_t len,
    uint64_t version)
{
  if (TI(t).desired_handler)
  {
    TI(t).desired_handler(payload, len, version, TI(t).desired_handler_ctx);
  }
}

/* Buffer a desired patch received while resyncing. Returns false when the
 * arena or the index is full, which sends the caller down the overflow path. */
static bool resync_buffer_patch(
    az_iot_gen2_twin_client* t,
    uint64_t version,
    const uint8_t* payload,
    size_t len)
{
  if (!TI(t).resync_buffer || TI(t).resync_count >= AZ_IOT_GEN2_TWIN_MAX_RESYNC_PATCHES
      || len > TI(t).resync_buffer_len - TI(t).resync_used)
  {
    return false;
  }

  size_t idx = TI(t).resync_count;
  TI(t).resync_patches[idx].version = version;
  TI(t).resync_patches[idx].offset = TI(t).resync_used;
  TI(t).resync_patches[idx].len = len;
  if (len)
  {
    memcpy(TI(t).resync_buffer + TI(t).resync_used, payload, len);
  }
  TI(t).resync_used += len;
  TI(t).resync_count++;
  return true;
}

/* Cancel the SDK's own outstanding resync GET. A response that arrives for it
 * afterwards matches no pending slot and is dropped. */
static void resync_cancel_get(az_iot_gen2_twin_client* t)
{
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(t).pending[i].in_use && TI(t).pending[i].internal)
    {
      memset(&TI(t).pending[i], 0, sizeof(TI(t).pending[i]));
    }
  }
}

/* Enter the resyncing state: the desired-patch sequence has a gap, so the
 * device cannot apply what it just received on top of what it holds. Ask for a
 * snapshot of the desired section and buffer whatever keeps arriving until it
 * lands. */
static void resync_begin(az_iot_gen2_twin_client* t)
{
  TI(t).resyncing = true;
  TI(t).resync_used = 0;
  TI(t).resync_count = 0;

  az_iot_gen2_twin_get_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.sections = AZ_IOT_GEN2_TWIN_SECTIONS_DESIRED;
  (void)issue_get(t, &opts, NULL, NULL, true);
}

/* Leave the resyncing state by applying a snapshot at @p version, then
 * replaying the buffered patches newer than it, in order. Patches at or below
 * the snapshot version are already folded into it and are discarded. */
static void resync_complete(
    az_iot_gen2_twin_client* t,
    uint64_t version,
    const uint8_t* payload,
    size_t payload_len)
{
  TI(t).desired_local = version;
  dispatch_desired(t, payload, payload_len, version);

  for (size_t i = 0; i < TI(t).resync_count; ++i)
  {
    if (TI(t).resync_patches[i].version <= version)
    {
      continue;
    }
    TI(t).desired_local = TI(t).resync_patches[i].version;
    dispatch_desired(
        t,
        TI(t).resync_buffer + TI(t).resync_patches[i].offset,
        TI(t).resync_patches[i].len,
        TI(t).resync_patches[i].version);
  }

  resync_reset(t);
}

/* ------------------------------------------------------------------------- */
/* decoding                                                                  */
/* ------------------------------------------------------------------------- */

/* Decode a Section: { 1 uint64 version, 2 bytes payload }. */
static void decode_section(const uint8_t* buf, size_t len, az_iot_gen2_twin_section* out)
{
  size_t pos = 0;
  while (buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      return;
    }

    if (field == TWIN_F_SECTION_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      if (!az_iot_proto3_read_varint(buf, len, &pos, &out->version))
      {
        return;
      }
    }
    else if (field == TWIN_F_SECTION_PAYLOAD && wire == AZ_IOT_PROTO3_WIRE_LEN)
    {
      if (!az_iot_proto3_read_bytes(buf, len, &pos, &out->payload, &out->payload_len))
      {
        return;
      }
    }
    else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
    {
      return;
    }
  }
}

/* twin-push -- TwinPush { 1 Section desired, 2 Section reported }. Either
 * section may be absent: the service pushes only what the device is behind on
 * and asked for on the birth. */
static void on_twin_push(az_iot_gen2_twin_client* t, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_state twin;
  memset(&twin, 0, sizeof(twin));

  const uint8_t* buf = msg->payload;
  size_t len = msg->payload_len;
  size_t pos = 0;
  while (buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      break;
    }

    if (wire == AZ_IOT_PROTO3_WIRE_LEN
        && (field == TWIN_F_PUSH_DESIRED || field == TWIN_F_PUSH_REPORTED))
    {
      const uint8_t* sec = NULL;
      size_t sec_len = 0;
      if (!az_iot_proto3_read_bytes(buf, len, &pos, &sec, &sec_len))
      {
        break;
      }
      decode_section(sec, sec_len, (field == TWIN_F_PUSH_DESIRED) ? &twin.desired : &twin.reported);
    }
    else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
    {
      break;
    }
  }

  /* A push carries the authoritative state, so it also advances the versions
   * the client sends back to the service. */
  if (twin.desired.version)
  {
    TI(t).desired_auth = twin.desired.version;
  }
  if (twin.reported.version)
  {
    TI(t).reported_version = twin.reported.version;
  }

  /* The push is what the birth-triggered expectation was waiting for. */
  TI(t).push_expected = false;

  /* A desired snapshot is exactly what a resync is chasing, so a push settles
   * it and makes the outstanding GET redundant. */
  if (twin.desired.version)
  {
    if (TI(t).resyncing)
    {
      resync_cancel_get(t);
      resync_complete(t, twin.desired.version, twin.desired.payload, twin.desired.payload_len);
    }
    else
    {
      TI(t).desired_local = twin.desired.version;
    }
  }

  if (TI(t).push_cb)
  {
    TI(t).push_cb(&twin, TI(t).push_user_ctx);
  }
}

/* desired-patch -- DesiredPatch { 1 uint64 version, 2 optional bytes payload }.
 *
 * Desired patches are incremental, so they may only be applied in order. A
 * version beyond the next one means the device missed something, and merging
 * this patch onto state it does not have would silently corrupt it: the client
 * asks for a snapshot instead and buffers what keeps arriving.
 *
 * A patch with no payload is a version probe -- the service asking "are you
 * current?" -- which never reaches the application. */
static void on_desired_patch(az_iot_gen2_twin_client* t, const az_iot_mqtt_message* msg)
{
  uint64_t version = 0;
  const uint8_t* patch = NULL;
  size_t patch_len = 0;
  bool has_payload = false;

  const uint8_t* buf = msg->payload;
  size_t len = msg->payload_len;
  size_t pos = 0;
  while (buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      break;
    }

    if (field == TWIN_F_PATCH_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      if (!az_iot_proto3_read_varint(buf, len, &pos, &version))
      {
        break;
      }
    }
    else if (field == TWIN_F_PATCH_PAYLOAD && wire == AZ_IOT_PROTO3_WIRE_LEN)
    {
      if (!az_iot_proto3_read_bytes(buf, len, &pos, &patch, &patch_len))
      {
        break;
      }
      has_payload = true;
    }
    else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
    {
      break;
    }
  }

  if (version > TI(t).desired_auth)
  {
    TI(t).desired_auth = version;
  }

  /* Anything at or behind the applied version is stale in every state,
   * resyncing included. desired_local only advances through resync_complete()
   * (which ends the resync) or the non-resyncing branch below, so it stays
   * pinned at the last applied version for the whole resync -- and
   * resync_complete() would skip such a patch on replay anyway, since the
   * snapshot it applies is never older. Buffering one would only burn resync
   * buffer space and a replay slot, and an overflow there costs a cancelled GET
   * and a second round trip. */
  if (version <= TI(t).desired_local)
  {
    return;
  }

  if (TI(t).resyncing)
  {
    /* A probe carries nothing to replay, and the in-flight GET already
     * resolves at a version no older than the probe's. */
    if (!has_payload)
    {
      return;
    }

    /* With no buffer configured there is nowhere to replay from, so the patch
     * is simply dropped: the snapshot already on its way subsumes it. Only a
     * buffer that exists and filled up is an overflow worth re-GETting for --
     * treating "no buffer" as overflow would cancel and re-issue the GET for
     * every patch that arrived, and a steady stream of them would keep the
     * resync from ever completing. */
    if (!TI(t).resync_buffer)
    {
      return;
    }

    if (!resync_buffer_patch(t, version, patch, patch_len))
    {
      /* Overflow. Discard the buffer and re-GET: the fresh snapshot is at least
       * as new as the highest patch that was buffered, so it subsumes
       * everything dropped. */
      resync_cancel_get(t);
      resync_begin(t);
    }
    return;
  }

  if (!has_payload)
  {
    /* A probe ahead of the applied version reveals a patch that never arrived. */
    resync_begin(t);
    return;
  }

  if (version == TI(t).desired_local + 1u)
  {
    TI(t).desired_local = version;
    dispatch_desired(t, patch, patch_len, version);
    return;
  }

  /* A gap: seed the buffer with this patch, then resynchronize. */
  resync_begin(t);
  (void)resync_buffer_patch(t, version, patch, patch_len);
}

/* get-response -- TwinGetResponse { 1 desired_version, 2 reported_version,
 * 3 optional desired_payload, 4 optional reported_payload }. Both versions are
 * always present; a payload is omitted when the section was not requested or
 * the device's if-not-match filter already matched. */
static void on_get_response(az_iot_gen2_twin_client* t, int idx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_state twin;
  memset(&twin, 0, sizeof(twin));

  const uint8_t* buf = msg->payload;
  size_t len = msg->payload_len;
  size_t pos = 0;
  while (buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      break;
    }

    bool ok = true;
    if (field == TWIN_F_GET_RESP_DESIRED_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      ok = az_iot_proto3_read_varint(buf, len, &pos, &twin.desired.version);
    }
    else if (field == TWIN_F_GET_RESP_REPORTED_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      ok = az_iot_proto3_read_varint(buf, len, &pos, &twin.reported.version);
    }
    else if (field == TWIN_F_GET_RESP_DESIRED_PAYLOAD && wire == AZ_IOT_PROTO3_WIRE_LEN)
    {
      ok = az_iot_proto3_read_bytes(
          buf, len, &pos, &twin.desired.payload, &twin.desired.payload_len);
    }
    else if (field == TWIN_F_GET_RESP_REPORTED_PAYLOAD && wire == AZ_IOT_PROTO3_WIRE_LEN)
    {
      ok = az_iot_proto3_read_bytes(
          buf, len, &pos, &twin.reported.payload, &twin.reported.payload_len);
    }
    else
    {
      ok = az_iot_proto3_skip_field(buf, len, &pos, wire);
    }

    if (!ok)
    {
      break;
    }
  }

  if (twin.desired.version)
  {
    TI(t).desired_auth = twin.desired.version;
  }
  if (twin.reported.version)
  {
    TI(t).reported_version = twin.reported.version;
  }

  /* A desired snapshot ends a resync: apply it, then replay the patches that
   * are newer than it. Done before the slot is released so the replay is not
   * interleaved with whatever the application does in its callback. */
  if (TI(t).resyncing && twin.desired.version)
  {
    resync_complete(t, twin.desired.version, twin.desired.payload, twin.desired.payload_len);
  }

  release_pending(t, idx, AZ_IOT_OK, &twin, NULL);
}

/* reported-patch-response -- ReportedPatchResponse { 1 Result result,
 * 2 uint64 version }. On OK `version` is the new authoritative version; on
 * VERSION_MISMATCH it is the current one, which the client adopts so a retry
 * issued from the callback carries a correct if_match. */
static void on_patch_response(az_iot_gen2_twin_client* t, int idx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_patch_result result;
  memset(&result, 0, sizeof(result));

  const uint8_t* buf = msg->payload;
  size_t len = msg->payload_len;
  size_t pos = 0;
  while (buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      break;
    }

    if (field == TWIN_F_PATCH_RESP_RESULT && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      uint64_t v = 0;
      if (!az_iot_proto3_read_varint(buf, len, &pos, &v))
      {
        break;
      }
      result.status = (az_iot_gen2_twin_patch_status)v;
    }
    else if (field == TWIN_F_PATCH_RESP_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      if (!az_iot_proto3_read_varint(buf, len, &pos, &result.version))
      {
        break;
      }
    }
    else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
    {
      break;
    }
  }

  if (result.version)
  {
    TI(t).reported_version = result.version;
  }

  release_pending(t, idx, AZ_IOT_OK, NULL, &result);
}

/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------- */

/* Single inbound handler for "ih/{device_id}/dev/twin". Everything the service
 * sends for twin arrives here and is routed on the `type` user property. */
static void on_twin_inbound(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic || !topic_is_inbound(t, msg->topic))
  {
    return;
  }

  /* Seed from the birth-ack before anything here can advance the versions.
   * Seeding lazily at publish time instead would let a twin-push that arrived
   * first be undone: the push moves the version forward, and the first patch
   * would then find the seed had never run and overwrite it with the older
   * birth-ack value. */
  sync_versions(t);

  if (type_is(msg, TWIN_TYPE_TWIN_PUSH))
  {
    if (matches_connection(t, msg))
    {
      on_twin_push(t, msg);
    }
    return;
  }
  if (type_is(msg, TWIN_TYPE_DESIRED_PATCH))
  {
    if (matches_connection(t, msg))
    {
      on_desired_patch(t, msg);
    }
    return;
  }

  /* What is left answers a request the device made, so correlate it to the
   * slot that issued it. */
  int idx = find_pending_by_corr(t, msg);
  if (idx < 0)
  {
    return; /* stale, unknown or uncorrelated */
  }

  if (type_is(msg, TWIN_TYPE_GET_RESPONSE) && TI(t).pending[idx].kind == TWIN_PENDING_GET)
  {
    on_get_response(t, idx, msg);
  }
  else if (type_is(msg, TWIN_TYPE_PATCH_RESPONSE) && TI(t).pending[idx].kind == TWIN_PENDING_PATCH)
  {
    on_patch_response(t, idx, msg);
  }
  else
  {
    /* The correlation id is one this client minted, so the message is the
     * answer to that request -- but it is missing its `type`, carries one the
     * SDK does not know, or answers a different request kind than the slot is
     * waiting for. The service will not send a second answer, so dropping it
     * would strand the slot until the session ends and, after
     * AZ_IOT_TWIN_MAX_PENDING such messages, wedge the client. Release it and
     * let the caller decide whether to retry. */
    AZ_IOT_LOG_WARN("gen2_twin: releasing a pending request whose response was not usable");
    release_pending(t, idx, AZ_IOT_ERR_PROTOCOL, NULL, NULL);
  }
}

/* ------------------------------------------------------------------------- */
/* connect-time topic binding                                                */
/* ------------------------------------------------------------------------- */

/* Build "ih/{device_id}<suffix>" into @p topic. */
static az_iot_result build_topic(
    az_iot_connection_client* conn,
    const char* suffix,
    char (*topic)[AZ_IOT_TWIN_TOPIC_MAX],
    size_t* out_len)
{
  const char* device_id = az_iot_connection_client__device_id(conn);
  if (!device_id)
  {
    AZ_IOT_LOG_WARN("gen2_twin: the device id is not assigned yet");
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  const char* parts[] = { TWIN_TOPIC_ROOT, device_id, suffix };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(*topic), out_len, parts, 3) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF("gen2_twin: the '%s' topic did not fit AZ_IOT_TWIN_TOPIC_MAX bytes", suffix);
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_OK;
}

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_gen2_twin_client* client = (az_iot_gen2_twin_client*)owner;

  char topic[AZ_IOT_TWIN_TOPIC_MAX];
  size_t topic_len = 0;
  az_iot_result r = build_topic(conn, TWIN_INBOUND_SUFFIX, &topic, &topic_len);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("gen2_twin: cannot bind topics -- the inbound topic could not be built");
    return (r == AZ_IOT_ERR_NOT_CONNECTED) ? AZ_IOT_ERR_NOT_INITIALIZED : AZ_IOT_ERR_INTERNAL;
  }
  TI(client).inbound_topic_len = topic_len;

  r = az_iot_connection_client__register_inbound_handler(conn, topic, on_twin_inbound, client);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        "gen2_twin: could not register delivery for the twin topic (%s)",
        az_iot_result_to_string(r));
    return r;
  }

  /* No SUBSCRIBEs on this path: the presence handshake already holds
   * ih/{device_id}/dev/#, which covers the inbound topic. */
  return AZ_IOT_OK;
}

/* Withdraw everything _init() and bind_topics() registered. Shared by the
 * partial-init unwind and _deinit() so the two can never drift apart. */
static void withdraw_registrations(az_iot_connection_client* conn, az_iot_gen2_twin_client* client)
{
  (void)az_iot_connection_client__unregister_session_end_handler(conn, client);
  az_iot_connection_client__unregister_feature_client_bind(conn, client);
  (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_gen2_twin_client_init(
    az_iot_gen2_twin_client* client,
    az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_result result
      = az_iot_connection_client__require_profile(conn, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  if (result != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return result;
  }

  memset(client, 0, sizeof(*client));
  TI(client).conn = conn;

  result = az_iot_connection_client__register_feature_client_bind(conn, client, bind_topics);
  /* Be told when the session ends so pending GET/PATCH requests are completed
   * and their slots released, rather than waiting for a response that died
   * with the session. Registered last: everything above can still fail and
   * unwind, and this must not outlive a failed init. */
  if (result == AZ_IOT_OK)
  {
    result
        = az_iot_connection_client__register_session_end_handler(conn, twin_fail_pending, client);
  }

  if (result != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        "gen2_twin: init failed (%s); withdrawing partial registrations",
        az_iot_result_to_string(result));
    withdraw_registrations(conn, client);
    az_iot_connection_client__release_profile(conn);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_gen2_twin_client_deinit(az_iot_gen2_twin_client* client)
{
  if (!client || !TI(client).conn)
  {
    return;
  }
  withdraw_registrations(TI(client).conn, client);
  az_iot_connection_client__release_profile(TI(client).conn);
  memset(client, 0, sizeof(*client));
}

/* Publish a twin request and claim @p idx for it.
 *
 * The slot is reserved before the publish so a response that arrives inside it
 * still correlates, and released again if the publish is refused. */
static az_iot_result publish_request(
    az_iot_gen2_twin_client* twin,
    int idx,
    int kind,
    const char* type_value,
    const uint8_t* body,
    size_t body_len)
{
  char topic[AZ_IOT_TWIN_TOPIC_MAX];
  az_iot_result r = build_topic(TI(twin).conn, TWIN_OUTBOUND_SUFFIX, &topic, NULL);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  az_iot_connection_client__gen_uuid(TI(twin).conn, TI(twin).pending[idx].corr);
  TI(twin).pending[idx].in_use = true;
  TI(twin).pending[idx].kind = kind;
  /* Arm the defensive timeout: at QoS 0 nothing in the transport will ever
   * report this request going unanswered. */
  TI(twin).pending[idx].deadline_ms = defensive_deadline(twin, TI(twin).pending[idx].attempt);

  az_iot_mqtt_user_property type_prop = { TWIN_TYPE_KEY, type_value };

  az_iot_mqtt_message out = { 0 };
  out.topic = topic;
  out.payload = body;
  out.payload_len = body_len;
  /* QoS 0: the service answers with a full response message rather than a
   * PUBACK, so the transport ack would add a round trip and prove nothing. */
  out.qos = AZ_IOT_MQTT_QOS_0;
  out.content_type = TWIN_CONTENT_TYPE;
  out.user_properties = &type_prop;
  out.user_properties_count = 1;
  out.correlation_data = TI(twin).pending[idx].corr;
  out.correlation_data_len = AZ_IOT_CORRELATION_UUID_LEN;

  r = az_iot_connection_client__publish(TI(twin).conn, &out, NULL, NULL);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen2_twin: the '%s' publish was refused (%s); releasing its pending slot",
        type_value,
        az_iot_result_to_string(r));
    memset(&TI(twin).pending[idx], 0, sizeof(TI(twin).pending[idx]));
  }
  return r;
}

/* Encode a TwinGet body from @p opts into @p body. */
static bool encode_get_body(
    const az_iot_gen2_twin_get_options* opts,
    uint8_t* body,
    size_t cap,
    size_t* out_len)
{
  *out_len = 0;
  uint64_t sections = opts->sections ? (uint64_t)opts->sections : TWIN_SECTIONS_BOTH;
  return az_iot_proto3_write_varint_field(body, cap, out_len, TWIN_F_GET_SECTIONS, sections)
      && (!opts->if_not_match_desired
          || az_iot_proto3_write_varint_field(
              body, cap, out_len, TWIN_F_GET_IF_NOT_MATCH_DESIRED, opts->if_not_match_desired))
      && (!opts->if_not_match_reported
          || az_iot_proto3_write_varint_field(
              body, cap, out_len, TWIN_F_GET_IF_NOT_MATCH_REPORTED, opts->if_not_match_reported));
}

/* Issue a GET. @p internal marks the SDK's own resync request, which has no
 * caller to report back to. */
static az_iot_result issue_get(
    az_iot_gen2_twin_client* t,
    const az_iot_gen2_twin_get_options* opts,
    az_iot_gen2_twin_get_callback cb,
    void* user_ctx,
    bool internal)
{
  int idx = alloc_pending(t);
  if (idx < 0)
  {
    AZ_IOT_LOG_WARN("gen2_twin: refusing a GET -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  uint8_t body[TWIN_GET_BODY_MAX];
  size_t body_len = 0;
  if (!encode_get_body(opts, body, sizeof(body), &body_len))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  TI(t).pending[idx].cb.get_cb = cb;
  TI(t).pending[idx].user_ctx = user_ctx;
  TI(t).pending[idx].internal = internal;
  TI(t).pending[idx].get_opts = *opts;
  return publish_request(t, idx, TWIN_PENDING_GET, TWIN_TYPE_GET, body, body_len);
}

az_iot_result az_iot_gen2_twin_client_get(
    az_iot_gen2_twin_client* twin,
    az_iot_gen2_twin_get_callback cb,
    void* user_ctx)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_gen2_twin_get_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.sections = AZ_IOT_GEN2_TWIN_SECTIONS_BOTH;
  return issue_get(twin, &opts, cb, user_ctx, false);
}

az_iot_result az_iot_gen2_twin_client_get_with_options(
    az_iot_gen2_twin_client* twin,
    const az_iot_gen2_twin_get_options* opts,
    az_iot_gen2_twin_get_callback cb,
    void* user_ctx)
{
  if (!twin || !opts)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return issue_get(twin, opts, cb, user_ctx, false);
}

/* Frame the saved reported patch into ReportedPatch { 1 if_match, 2 payload }
 * with the current authoritative version.
 *
 * The encode buffer holds the caller's payload first and the framed message
 * after it, so a retry can re-frame with a newer if_match once the caller's own
 * buffer is gone. */
static bool frame_patch(az_iot_gen2_twin_client* t, size_t* out_len)
{
  uint8_t* saved = TI(t).encode_buffer;
  size_t saved_len = TI(t).saved_patch_len;
  uint8_t* frame = saved + saved_len;
  size_t frame_cap = TI(t).encode_buffer_len - saved_len;

  size_t pos = 0;
  if ((TI(t).reported_version
       && !az_iot_proto3_write_varint_field(
           frame, frame_cap, &pos, TWIN_F_REPORTED_IF_MATCH, TI(t).reported_version))
      || !az_iot_proto3_write_bytes_field(
          frame, frame_cap, &pos, TWIN_F_REPORTED_PAYLOAD, saved, saved_len))
  {
    return false;
  }
  *out_len = pos;
  return true;
}

/* True when a reported patch is already awaiting its response. The saved
 * payload frame_patch() re-frames from is single-slot, so only one patch may be
 * outstanding at a time. */
static bool patch_in_flight(const az_iot_gen2_twin_client* t)
{
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(t).pending[i].in_use && TI(t).pending[i].kind == TWIN_PENDING_PATCH)
    {
      return true;
    }
  }
  return false;
}

az_iot_result az_iot_gen2_twin_client_patch_reported(
    az_iot_gen2_twin_client* twin,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_gen2_twin_patch_ack_callback cb,
    void* user_ctx)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (patch_len > 0 && patch == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!TI(twin).encode_buffer || patch_len > TI(twin).encode_buffer_len)
  {
    AZ_IOT_LOG_ERROR("gen2_twin: refusing a patch -- no encode buffer large enough; call "
                     "az_iot_gen2_twin_client_set_encode_buffer()");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  /* One at a time: a retry re-frames from the single saved payload, so a second
   * patch would overwrite the bytes the first one still needs and its retry
   * would publish the wrong body under the first's correlation id. */
  if (patch_in_flight(twin))
  {
    AZ_IOT_LOG_WARN("gen2_twin: refusing a patch -- another reported patch is still in flight");
    return AZ_IOT_ERR_BUSY;
  }

  int idx = alloc_pending(twin);
  if (idx < 0)
  {
    AZ_IOT_LOG_WARN(
        "gen2_twin: refusing a patch -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  /* if_match is the device's view of the authoritative reported version; the
   * service rejects the write with VERSION_MISMATCH if it has moved on.
   * Version 0 means the device has no view yet, and proto3 omits it. */
  sync_versions(twin);

  /* Keep the caller's payload: it is theirs to free the moment this returns,
   * and a retry has to re-frame from it minutes later. */
  if (patch_len)
  {
    memcpy(TI(twin).encode_buffer, patch, patch_len);
  }
  TI(twin).saved_patch_len = patch_len;

  size_t body_len = 0;
  if (!frame_patch(twin, &body_len))
  {
    AZ_IOT_LOG_ERROR("gen2_twin: the framed patch did not fit the encode buffer");
    TI(twin).saved_patch_len = 0;
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  TI(twin).pending[idx].cb.patch_cb = cb;
  TI(twin).pending[idx].user_ctx = user_ctx;
  return publish_request(
      twin,
      idx,
      TWIN_PENDING_PATCH,
      TWIN_TYPE_REPORTED_PATCH,
      TI(twin).encode_buffer + TI(twin).saved_patch_len,
      body_len);
}

az_iot_result az_iot_gen2_twin_client_set_desired_handler(
    az_iot_gen2_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  TI(twin).desired_handler = cb;
  TI(twin).desired_handler_ctx = user_ctx;
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen2_twin_client_set_push_callback(
    az_iot_gen2_twin_client* twin,
    az_iot_gen2_twin_push_callback cb,
    void* user_ctx)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  TI(twin).push_cb = cb;
  TI(twin).push_user_ctx = user_ctx;
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen2_twin_client_set_encode_buffer(
    az_iot_gen2_twin_client* twin,
    uint8_t* buffer,
    size_t buffer_len)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* The documented contract is "largest patch plus the overhead", so a buffer
   * of exactly the overhead is the valid minimum: it holds the framing for a
   * zero-length patch. Rejecting it would make the smallest size the header
   * describes impossible to supply. This is only an early check for an
   * obviously unusable buffer -- every field write is bounds-checked against
   * encode_buffer_len at publish time, which is what actually guarantees the
   * patch fits. */
  if (buffer && buffer_len < AZ_IOT_GEN2_TWIN_ENCODE_OVERHEAD)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  TI(twin).encode_buffer = buffer;
  TI(twin).encode_buffer_len = buffer ? buffer_len : 0;
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen2_twin_client_set_resync_buffer(
    az_iot_gen2_twin_client* twin,
    uint8_t* buffer,
    size_t buffer_len)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  TI(twin).resync_buffer = buffer;
  TI(twin).resync_buffer_len = buffer ? buffer_len : 0;
  /* Anything already buffered lives in the old arena. */
  TI(twin).resync_used = 0;
  TI(twin).resync_count = 0;
  return AZ_IOT_OK;
}

/* Re-publish a timed-out exchange with a fresh correlation id and the next step
 * of the escalating schedule. The identifier must change so a late response to
 * the previous attempt is not mistaken for this one. */
static void retry_pending(az_iot_gen2_twin_client* t, int idx)
{
  uint8_t body[TWIN_GET_BODY_MAX];
  size_t body_len = 0;
  const uint8_t* payload = body;
  const char* type_value = TWIN_TYPE_GET;

  if (TI(t).pending[idx].kind == TWIN_PENDING_GET)
  {
    if (!encode_get_body(&TI(t).pending[idx].get_opts, body, sizeof(body), &body_len))
    {
      return;
    }
  }
  else
  {
    /* Re-frame with the current authoritative version: it may have advanced
     * since the first attempt. */
    type_value = TWIN_TYPE_REPORTED_PATCH;
    if (!frame_patch(t, &body_len))
    {
      return;
    }
    payload = TI(t).encode_buffer + TI(t).saved_patch_len;
  }

  TI(t).pending[idx].attempt++;
  AZ_IOT_LOG_WARNF(
      "gen2_twin: a '%s' went unanswered; re-issuing it (attempt %u)",
      type_value,
      (unsigned)TI(t).pending[idx].attempt);
  (void)publish_request(t, idx, TI(t).pending[idx].kind, type_value, payload, body_len);
}

az_iot_result az_iot_gen2_twin_client_do_work(az_iot_gen2_twin_client* twin)
{
  if (!twin || !TI(twin).conn)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Everything below is scoped to a live connection. Once it drops, the
   * exchanges riding on it cannot be resumed (QoS 0) -- the session-end handler
   * has already abandoned them and told their callers. */
  uint8_t nonce[AZ_IOT_CORRELATION_UUID_LEN];
  if (az_iot_connection_client__presence_nonce(TI(twin).conn, nonce) != AZ_IOT_OK)
  {
    return AZ_IOT_OK;
  }

  sync_versions(twin);

  uint64_t now = az_iot_time_mono_ms();

  /* A birth-triggered push that never arrived means the service believes it
   * dispatched state the device never saw. Only a fresh connection re-runs that
   * decision, so reconnect rather than paper over it with a GET. */
  if (TI(twin).push_expected && now >= TI(twin).push_deadline_ms)
  {
    TI(twin).push_expected = false;
    TI(twin).push_attempt++;
    AZ_IOT_LOG_WARN(
        "gen2_twin: the birth-triggered twin-push did not arrive; reconnecting to re-run the "
        "service's push decision");
    (void)az_iot_connection_client_close(TI(twin).conn);
    return AZ_IOT_OK;
  }

  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(twin).pending[i].in_use && now >= TI(twin).pending[i].deadline_ms)
    {
      retry_pending(twin, i);
    }
  }

  return AZ_IOT_OK;
}

void az_iot_gen2_twin_client__force_timeouts(az_iot_gen2_twin_client* twin)
{
  if (!twin)
  {
    return;
  }
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    TI(twin).pending[i].deadline_ms = 0;
  }
  if (TI(twin).push_expected)
  {
    TI(twin).push_deadline_ms = 0;
  }
}
