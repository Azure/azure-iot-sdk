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
#include "internal/span_writer.h"

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

/* Largest TwinGet body this client emits: one small varint field. */
#define TWIN_GET_BODY_MAX 16

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

/* Adopt the authoritative versions the birth-ack carried, once per connection.
 *
 * The birth nonce identifies the connection those versions belong to, so a
 * reconnect is noticed here and the versions re-seeded: they are the service's
 * state as of the new birth admission, not the stale ones this client was
 * tracking against a session that has ended. */
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
  if (az_iot_connection_client__presence_twin_versions(TI(t).conn, &desired, &reported)
      == AZ_IOT_OK)
  {
    TI(t).desired_version = desired;
    TI(t).reported_version = reported;
  }
  memcpy(TI(t).nonce, nonce, AZ_IOT_CORRELATION_UUID_LEN);
  TI(t).nonce_valid = true;
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
  void* ctx = TI(t).pending[idx].user_ctx;
  az_iot_gen2_twin_get_callback get_cb = TI(t).pending[idx].cb.get_cb;
  az_iot_gen2_twin_patch_ack_callback patch_cb = TI(t).pending[idx].cb.patch_cb;

  TI(t).pending[idx].in_use = false;
  TI(t).pending[idx].kind = TWIN_PENDING_NONE;

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
    TI(t).desired_version = twin.desired.version;
  }
  if (twin.reported.version)
  {
    TI(t).reported_version = twin.reported.version;
  }

  if (TI(t).push_cb)
  {
    TI(t).push_cb(&twin, TI(t).push_user_ctx);
  }
}

/* desired-patch -- DesiredPatch { 1 uint64 version, 2 optional bytes payload }.
 *
 * An absent payload is a version probe: the service confirming which version
 * the device should be at. It advances nothing for the application, so it
 * updates the tracked version and is not dispatched to the handler. */
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

  if (version)
  {
    TI(t).desired_version = version;
  }
  if (has_payload && TI(t).desired_handler)
  {
    TI(t).desired_handler(patch, patch_len, version, TI(t).desired_handler_ctx);
  }
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
    TI(t).desired_version = twin.desired.version;
  }
  if (twin.reported.version)
  {
    TI(t).reported_version = twin.reported.version;
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
    TI(twin).pending[idx].in_use = false;
    TI(twin).pending[idx].kind = TWIN_PENDING_NONE;
  }
  return r;
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

  int idx = alloc_pending(twin);
  if (idx < 0)
  {
    AZ_IOT_LOG_WARN("gen2_twin: refusing a GET -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  /* TwinGet { 1 Sections sections }: both sections, no if-not-match filter. */
  uint8_t body[TWIN_GET_BODY_MAX];
  size_t body_len = 0;
  if (!az_iot_proto3_write_varint_field(
          body, sizeof(body), &body_len, TWIN_F_GET_SECTIONS, TWIN_SECTIONS_BOTH))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  TI(twin).pending[idx].cb.get_cb = cb;
  TI(twin).pending[idx].user_ctx = user_ctx;
  return publish_request(twin, idx, TWIN_PENDING_GET, TWIN_TYPE_GET, body, body_len);
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
  if (!TI(twin).encode_buffer)
  {
    AZ_IOT_LOG_ERROR("gen2_twin: refusing a patch -- no encode buffer; call "
                     "az_iot_gen2_twin_client_set_encode_buffer()");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  int idx = alloc_pending(twin);
  if (idx < 0)
  {
    AZ_IOT_LOG_WARN(
        "gen2_twin: refusing a patch -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  /* ReportedPatch { 1 uint64 if_match, 2 bytes payload }. if_match is the
   * device's view of the authoritative reported version; the service rejects
   * the write with VERSION_MISMATCH if it has moved on. Version 0 means the
   * device has no view yet, and proto3 omits it. */
  sync_versions(twin);

  size_t pos = 0;
  if ((TI(twin).reported_version
       && !az_iot_proto3_write_varint_field(
           TI(twin).encode_buffer,
           TI(twin).encode_buffer_len,
           &pos,
           TWIN_F_REPORTED_IF_MATCH,
           TI(twin).reported_version))
      || !az_iot_proto3_write_bytes_field(
          TI(twin).encode_buffer,
          TI(twin).encode_buffer_len,
          &pos,
          TWIN_F_REPORTED_PAYLOAD,
          patch,
          patch_len))
  {
    AZ_IOT_LOG_ERROR("gen2_twin: the framed patch did not fit the encode buffer");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  TI(twin).pending[idx].cb.patch_cb = cb;
  TI(twin).pending[idx].user_ctx = user_ctx;
  return publish_request(
      twin, idx, TWIN_PENDING_PATCH, TWIN_TYPE_REPORTED_PATCH, TI(twin).encode_buffer, pos);
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
