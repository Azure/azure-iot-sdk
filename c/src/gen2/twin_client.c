// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file
 * @brief MQTT v5 twin client.
 *
 * Device -> service on `ih/{id}/srv/twin`, service -> device on `ih/{id}/dev/twin`,
 * protobuf bodies (common/Protos/twin.proto) at QoS 0, message kind in the `type`
 * user property. GET and reported patch correlate on a per-request UUID;
 * twin-push and desired-patch carry the connection's birth nonce.
 *
 * Desired state is level-triggered: the client tracks the service's desired
 * version and the version last delivered to the handler. An in-order patch is
 * delivered; whenever the device is otherwise behind, one desired snapshot GET
 * is issued. Nothing is buffered or replayed.
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

#define AZ_IOT_TWIN_TOPIC_MAX 192

#define TWIN_TOPIC_ROOT "ih/"
#define TWIN_INBOUND_SUFFIX "/dev/twin"
#define TWIN_OUTBOUND_SUFFIX "/srv/twin"

/* Outbound `type` values carry the schema version; inbound ones are matched as
 * "<name>:" TWIN_SCHEMA_VERSION. */
#define TWIN_TYPE_KEY "type"
#define TWIN_SCHEMA_VERSION "1"
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

/* Largest TwinGet body: the selector and two 10-byte-varint filters. */
#define TWIN_GET_BODY_MAX 32

#define TWIN_PENDING_NONE 0
#define TWIN_PENDING_GET 1
#define TWIN_PENDING_PATCH 2

#define TI(t) ((t)->_internal)

/* The public arrays are sized by the public constant but filled and compared
 * with the core's; fail the build if they diverge. */
typedef char az_iot_twin_correlation_id_len_agrees
    [(AZ_IOT_GEN2_TWIN_CORRELATION_ID_LEN == AZ_IOT_CORRELATION_UUID_LEN) ? 1 : -1];

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/** @brief Index of a free pending slot, or -1. */
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

/** @brief True when the `type` user property is exactly "<want>:1". */
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
    return strncmp(up->value, want, n) == 0 && up->value[n] == ':'
        && strcmp(up->value + n + 1, TWIN_SCHEMA_VERSION) == 0;
  }
  return false;
}

/** @brief True when @p topic is exactly `ih/{id}/dev/twin` (dispatch matches by prefix). */
static bool topic_is_inbound(const az_iot_gen2_twin_client* t, const char* topic)
{
  return TI(t).inbound_topic_len > 0 && strlen(topic) == TI(t).inbound_topic_len;
}

/** @brief True when @p msg carries @p id as its Correlation Data. */
static bool correlation_is(const az_iot_mqtt_message* msg, const uint8_t* id)
{
  return msg->correlation_data && msg->correlation_data_len == AZ_IOT_CORRELATION_UUID_LEN
      && memcmp(msg->correlation_data, id, AZ_IOT_CORRELATION_UUID_LEN) == 0;
}

/** @brief Pending slot answered by @p msg, or -1. */
static int find_pending_by_correlation_id(
    const az_iot_gen2_twin_client* t,
    const az_iot_mqtt_message* msg)
{
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(t).pending[i].in_use && correlation_is(msg, TI(t).pending[i].correlation_id))
    {
      return i;
    }
  }
  return -1;
}

/** @brief True when a backend-initiated message belongs to the current connection. */
static bool matches_connection(const az_iot_gen2_twin_client* t, const az_iot_mqtt_message* msg)
{
  uint8_t birth_nonce[AZ_IOT_CORRELATION_UUID_LEN];
  return az_iot_connection_client__presence_nonce(TI(t).conn, birth_nonce) == AZ_IOT_OK
      && correlation_is(msg, birth_nonce);
}

/** @brief Raise the tracked service desired version; versions only move forward. */
static void note_desired_service_version(az_iot_gen2_twin_client* t, uint64_t version)
{
  if (version > TI(t).desired_properties_service_version)
  {
    TI(t).desired_properties_service_version = version;
  }
}

static uint64_t deadline_from_now(const az_iot_gen2_twin_client* t)
{
  return az_iot_time_mono_ms() + TI(t).request_timeout_ms;
}

/* ------------------------------------------------------------------------- */
/* publishing                                                                */
/* ------------------------------------------------------------------------- */

/** @brief Build `ih/{device_id}<suffix>`. */
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

/** @brief Mint a fresh id into @p correlation_id and publish a twin request under it. */
static az_iot_result publish_twin(
    az_iot_gen2_twin_client* t,
    const char* type_value,
    const uint8_t* body,
    size_t body_len,
    uint8_t* correlation_id)
{
  char topic[AZ_IOT_TWIN_TOPIC_MAX];
  az_iot_result r = build_topic(TI(t).conn, TWIN_OUTBOUND_SUFFIX, &topic, NULL);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  az_iot_connection_client__gen_uuid(TI(t).conn, correlation_id);

  az_iot_mqtt_user_property type_prop = { TWIN_TYPE_KEY, type_value };
  az_iot_mqtt_message out = { 0 };
  out.topic = topic;
  out.payload = body;
  out.payload_len = body_len;
  out.qos = AZ_IOT_MQTT_QOS_0; /* answered by a response message, not a PUBACK */
  out.content_type = TWIN_CONTENT_TYPE;
  out.user_properties = &type_prop;
  out.user_properties_count = 1;
  out.correlation_data = correlation_id;
  out.correlation_data_len = AZ_IOT_CORRELATION_UUID_LEN;

  r = az_iot_connection_client__publish(TI(t).conn, &out, NULL, NULL);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen2_twin: the '%s' publish was refused (%s)", type_value, az_iot_result_to_string(r));
  }
  return r;
}

/** @brief Encode TwinGet from @p opts. */
static bool encode_get_body(
    const az_iot_gen2_twin_get_options* opts,
    uint8_t* body,
    size_t cap,
    size_t* out_len)
{
  *out_len = 0;
  return az_iot_proto3_write_varint_field(
             body, cap, out_len, TWIN_F_GET_SECTIONS, (uint64_t)opts->sections)
      && (!opts->if_not_match_desired
          || az_iot_proto3_write_varint_field(
              body, cap, out_len, TWIN_F_GET_IF_NOT_MATCH_DESIRED, opts->if_not_match_desired))
      && (!opts->if_not_match_reported
          || az_iot_proto3_write_varint_field(
              body, cap, out_len, TWIN_F_GET_IF_NOT_MATCH_REPORTED, opts->if_not_match_reported));
}

/**
 * @brief Issue the SDK's desired-snapshot GET when the handler is behind.
 *
 * At most one is in flight. `if_not_match` is the delivered version, so a
 * handler already current gets no payload back.
 */
static void request_snapshot_if_behind(az_iot_gen2_twin_client* t)
{
  if (!TI(t).desired_handler || TI(t).snapshot.in_flight
      || TI(t).desired_properties_service_version <= TI(t).desired_properties_device_version
      || !az_iot_connection_client__is_connected(TI(t).conn))
  {
    return;
  }

  az_iot_gen2_twin_get_options opts = az_iot_gen2_twin_get_options_default();
  opts.sections = AZ_IOT_GEN2_TWIN_SECTIONS_DESIRED;
  opts.if_not_match_desired = TI(t).desired_properties_device_version;

  uint8_t body[TWIN_GET_BODY_MAX];
  size_t body_len = 0;
  if (!encode_get_body(&opts, body, sizeof(body), &body_len)
      || publish_twin(t, TWIN_TYPE_GET, body, body_len, TI(t).snapshot.correlation_id) != AZ_IOT_OK)
  {
    return; /* re-evaluated on the next desired event */
  }
  TI(t).snapshot.in_flight = true;
  TI(t).snapshot.deadline_ms = deadline_from_now(t);
}

/* ------------------------------------------------------------------------- */
/* completing requests                                                       */
/* ------------------------------------------------------------------------- */

/** @brief Free slot @p idx, then report to its callback (so the callback may reuse it). */
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

  memset(&TI(t).pending[idx], 0, sizeof(TI(t).pending[idx]));

  if (kind == TWIN_PENDING_GET && get_cb)
  {
    get_cb(status, twin, ctx);
  }
  else if (kind == TWIN_PENDING_PATCH && patch_cb)
  {
    patch_cb(status, result, ctx);
  }
}

/** @brief Session-end handler: nothing outstanding can be answered on a new session. */
static void twin_fail_pending(void* user_ctx)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t)
  {
    return;
  }
  TI(t).snapshot.in_flight = false;
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(t).pending[i].in_use)
    {
      release_pending(t, i, AZ_IOT_ERR_NOT_CONNECTED, NULL, NULL);
    }
  }
}

/* ------------------------------------------------------------------------- */
/* desired delivery                                                          */
/* ------------------------------------------------------------------------- */

/** @brief Deliver @p section as a snapshot if it is newer than what the handler has. */
static void deliver_snapshot(az_iot_gen2_twin_client* t, const az_iot_gen2_twin_section* section)
{
  note_desired_service_version(t, section->version);
  if (!TI(t).desired_handler || !section->payload
      || section->version <= TI(t).desired_properties_device_version)
  {
    return;
  }
  TI(t).desired_properties_device_version = section->version;
  TI(t).desired_handler(
      AZ_IOT_GEN2_TWIN_DESIRED_SNAPSHOT,
      section->version,
      section->payload,
      section->payload_len,
      TI(t).desired_handler_ctx);
}

/* ------------------------------------------------------------------------- */
/* decoding                                                                  */
/* ------------------------------------------------------------------------- */

/** @brief Decode Section { 1 version, 2 payload }. */
static bool decode_section(const uint8_t* buf, size_t len, az_iot_gen2_twin_section* out)
{
  size_t pos = 0;
  while (buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      return false;
    }
    bool ok;
    if (field == TWIN_F_SECTION_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      ok = az_iot_proto3_read_varint(buf, len, &pos, &out->version);
    }
    else if (field == TWIN_F_SECTION_PAYLOAD && wire == AZ_IOT_PROTO3_WIRE_LEN)
    {
      ok = az_iot_proto3_read_bytes(buf, len, &pos, &out->payload, &out->payload_len);
    }
    else
    {
      ok = az_iot_proto3_skip_field(buf, len, &pos, wire);
    }
    if (!ok)
    {
      return false;
    }
  }
  return true;
}

/** @brief Decode TwinGetResponse { 1 desired_version, 2 reported_version, 3/4 payloads }. */
static bool decode_get_response(const az_iot_mqtt_message* msg, az_iot_gen2_twin_state* twin)
{
  memset(twin, 0, sizeof(*twin));
  const uint8_t* buf = msg->payload;
  size_t len = msg->payload_len;
  size_t pos = 0;
  while (buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      return false;
    }
    bool ok;
    if (field == TWIN_F_GET_RESP_DESIRED_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      ok = az_iot_proto3_read_varint(buf, len, &pos, &twin->desired.version);
    }
    else if (field == TWIN_F_GET_RESP_REPORTED_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      ok = az_iot_proto3_read_varint(buf, len, &pos, &twin->reported.version);
    }
    else if (field == TWIN_F_GET_RESP_DESIRED_PAYLOAD && wire == AZ_IOT_PROTO3_WIRE_LEN)
    {
      ok = az_iot_proto3_read_bytes(
          buf, len, &pos, &twin->desired.payload, &twin->desired.payload_len);
    }
    else if (field == TWIN_F_GET_RESP_REPORTED_PAYLOAD && wire == AZ_IOT_PROTO3_WIRE_LEN)
    {
      ok = az_iot_proto3_read_bytes(
          buf, len, &pos, &twin->reported.payload, &twin->reported.payload_len);
    }
    else
    {
      ok = az_iot_proto3_skip_field(buf, len, &pos, wire);
    }
    if (!ok)
    {
      return false;
    }
  }
  return true;
}

/* ------------------------------------------------------------------------- */
/* inbound handlers                                                          */
/* ------------------------------------------------------------------------- */

/** @brief twin-push: TwinPush { 1 desired Section, 2 reported Section }. */
static void on_twin_push(az_iot_gen2_twin_client* t, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_state twin;
  memset(&twin, 0, sizeof(twin));

  const uint8_t* buf = msg->payload;
  size_t len = msg->payload_len;
  size_t pos = 0;
  bool ok = true;
  while (ok && buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      ok = false;
    }
    else if (
        wire == AZ_IOT_PROTO3_WIRE_LEN
        && (field == TWIN_F_PUSH_DESIRED || field == TWIN_F_PUSH_REPORTED))
    {
      const uint8_t* sec = NULL;
      size_t sec_len = 0;
      ok = az_iot_proto3_read_bytes(buf, len, &pos, &sec, &sec_len)
          && decode_section(
               sec, sec_len, (field == TWIN_F_PUSH_DESIRED) ? &twin.desired : &twin.reported);
    }
    else
    {
      ok = az_iot_proto3_skip_field(buf, len, &pos, wire);
    }
  }
  if (!ok)
  {
    AZ_IOT_LOG_WARN("gen2_twin: dropping a malformed twin-push");
    return;
  }

  if (twin.reported.version)
  {
    TI(t).reported_properties_service_version = twin.reported.version;
  }
  if (twin.reported.payload && TI(t).reported_handler)
  {
    TI(t).reported_handler(&twin.reported, TI(t).reported_handler_ctx);
  }
  deliver_snapshot(t, &twin.desired);
  request_snapshot_if_behind(t);
}

/**
 * @brief desired-patch: DesiredPatch { 1 version, 2 optional payload }.
 *
 * Delivered only when it is the next version. A payload-less patch is a probe
 * that only reports the service version.
 */
static void on_desired_patch(az_iot_gen2_twin_client* t, const az_iot_mqtt_message* msg)
{
  uint64_t version = 0;
  const uint8_t* patch = NULL;
  size_t patch_len = 0;
  bool has_payload = false;

  const uint8_t* buf = msg->payload;
  size_t len = msg->payload_len;
  size_t pos = 0;
  bool ok = true;
  while (ok && buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      ok = false;
    }
    else if (field == TWIN_F_PATCH_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      ok = az_iot_proto3_read_varint(buf, len, &pos, &version);
    }
    else if (field == TWIN_F_PATCH_PAYLOAD && wire == AZ_IOT_PROTO3_WIRE_LEN)
    {
      ok = has_payload = az_iot_proto3_read_bytes(buf, len, &pos, &patch, &patch_len);
    }
    else
    {
      ok = az_iot_proto3_skip_field(buf, len, &pos, wire);
    }
  }
  if (!ok)
  {
    AZ_IOT_LOG_WARN("gen2_twin: dropping a malformed desired-patch");
    return;
  }

  note_desired_service_version(t, version);
  if (TI(t).desired_handler && has_payload && version > TI(t).desired_properties_device_version
      && version - TI(t).desired_properties_device_version == 1u)
  {
    TI(t).desired_properties_device_version = version;
    TI(t).desired_handler(
        AZ_IOT_GEN2_TWIN_DESIRED_PATCH, version, patch, patch_len, TI(t).desired_handler_ctx);
    return;
  }
  request_snapshot_if_behind(t);
}

/** @brief Response to the SDK's own snapshot GET. */
static void on_snapshot_response(az_iot_gen2_twin_client* t, const az_iot_mqtt_message* msg)
{
  TI(t).snapshot.in_flight = false;

  az_iot_gen2_twin_state twin;
  if (!type_is(msg, TWIN_TYPE_GET_RESPONSE) || !decode_get_response(msg, &twin))
  {
    AZ_IOT_LOG_WARN("gen2_twin: dropping an unusable desired-snapshot response");
    return;
  }
  if (twin.reported.version)
  {
    TI(t).reported_properties_service_version = twin.reported.version;
  }
  deliver_snapshot(t, &twin.desired);
  /* Still behind if the service moved on while the GET was in flight. */
  request_snapshot_if_behind(t);
}

/** @brief Response to an application GET. Desired state is not delivered from it. */
static void on_get_response(az_iot_gen2_twin_client* t, int idx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_state twin;
  if (!decode_get_response(msg, &twin))
  {
    AZ_IOT_LOG_WARN("gen2_twin: dropping a malformed get-response");
    release_pending(t, idx, AZ_IOT_ERR_PROTOCOL, NULL, NULL);
    return;
  }
  if (twin.reported.version)
  {
    TI(t).reported_properties_service_version = twin.reported.version;
  }
  note_desired_service_version(t, twin.desired.version);
  release_pending(t, idx, AZ_IOT_OK, &twin, NULL);
  request_snapshot_if_behind(t);
}

/** @brief reported-patch-response: ReportedPatchResponse { 1 result, 2 version }. */
static void on_patch_response(az_iot_gen2_twin_client* t, int idx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_patch_result result;
  memset(&result, 0, sizeof(result));

  const uint8_t* buf = msg->payload;
  size_t len = msg->payload_len;
  size_t pos = 0;
  bool ok = true;
  while (ok && buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      ok = false;
    }
    else if (field == TWIN_F_PATCH_RESP_RESULT && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      uint64_t v = 0;
      ok = az_iot_proto3_read_varint(buf, len, &pos, &v);
      result.status = (az_iot_gen2_twin_patch_status)v;
    }
    else if (field == TWIN_F_PATCH_RESP_VERSION && wire == AZ_IOT_PROTO3_WIRE_VARINT)
    {
      ok = az_iot_proto3_read_varint(buf, len, &pos, &result.version);
    }
    else
    {
      ok = az_iot_proto3_skip_field(buf, len, &pos, wire);
    }
  }
  if (!ok)
  {
    AZ_IOT_LOG_WARN("gen2_twin: dropping a malformed reported-patch-response");
    release_pending(t, idx, AZ_IOT_ERR_PROTOCOL, NULL, NULL);
    return;
  }

  if (result.version)
  {
    TI(t).reported_properties_service_version = result.version;
  }
  release_pending(t, idx, AZ_IOT_OK, NULL, &result);
}

/** @brief Single handler for `ih/{id}/dev/twin`, routed on `type`. */
static void on_twin_inbound(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic || !topic_is_inbound(t, msg->topic))
  {
    return;
  }

  if (type_is(msg, TWIN_TYPE_TWIN_PUSH) || type_is(msg, TWIN_TYPE_DESIRED_PATCH))
  {
    if (!matches_connection(t, msg))
    {
      return; /* from a connection since replaced */
    }
    if (type_is(msg, TWIN_TYPE_TWIN_PUSH))
    {
      on_twin_push(t, msg);
    }
    else
    {
      on_desired_patch(t, msg);
    }
    return;
  }

  if (TI(t).snapshot.in_flight && correlation_is(msg, TI(t).snapshot.correlation_id))
  {
    on_snapshot_response(t, msg);
    return;
  }

  int idx = find_pending_by_correlation_id(t, msg);
  if (idx < 0)
  {
    return; /* stale or unknown */
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
    /* Ours, but unreadable. The service sends one answer; do not wait for another. */
    AZ_IOT_LOG_WARN("gen2_twin: releasing a pending request whose response was not usable");
    release_pending(t, idx, AZ_IOT_ERR_PROTOCOL, NULL, NULL);
  }
}

/**
 * @brief Hub CONNECTED (after the birth-ack): adopt its versions and catch up.
 *
 * When `twin_push.push_desired` is set the service pushes the desired snapshot
 * itself, so no GET is issued here.
 */
static void on_hub_connected(az_iot_gen2_twin_client* t)
{
  uint64_t desired = 0;
  uint64_t reported = 0;
  if (az_iot_connection_client__presence_twin_versions(TI(t).conn, &desired, &reported)
      != AZ_IOT_OK)
  {
    return;
  }
  TI(t).reported_properties_service_version = reported;
  note_desired_service_version(t, desired);
  if (!az_iot_connection_client__twin_push_desired(TI(t).conn))
  {
    request_snapshot_if_behind(t);
  }
}

static void on_connection_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (t && event && event->scope == AZ_IOT_CONN_SCOPE_HUB
      && event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    on_hub_connected(t);
  }
}

/* ------------------------------------------------------------------------- */
/* binding                                                                   */
/* ------------------------------------------------------------------------- */

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
  }
  return r;
}

/** @brief Withdraw everything init registered; shared by the init unwind and deinit. */
static void withdraw_registrations(az_iot_connection_client* conn, az_iot_gen2_twin_client* client)
{
  (void)az_iot_connection_client__remove_state_observer(conn, on_connection_state, client);
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

  memset(client, 0, sizeof(*client));
  az_iot_result result
      = az_iot_connection_client__require_profile(conn, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  if (result != AZ_IOT_OK)
  {
    return result;
  }

  TI(client).conn = conn;
  TI(client).request_timeout_ms = AZ_IOT_GEN2_TWIN_REQUEST_TIMEOUT_MS_DEFAULT;

  result = az_iot_connection_client__register_feature_client_bind(conn, client, bind_topics);
  if (result == AZ_IOT_OK)
  {
    result
        = az_iot_connection_client__register_session_end_handler(conn, twin_fail_pending, client);
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__add_state_observer(conn, on_connection_state, client);
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

  /* Observers are not told about a session already up; catch up on it. */
  if (az_iot_connection_client__is_connected(conn))
  {
    on_hub_connected(client);
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

az_iot_gen2_twin_get_options az_iot_gen2_twin_get_options_default(void)
{
  az_iot_gen2_twin_get_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.sections = AZ_IOT_GEN2_TWIN_SECTIONS_BOTH;
  return opts;
}

/** @brief Claim a slot, publish @p body under it, and release the slot if refused. */
static az_iot_result submit_request(
    az_iot_gen2_twin_client* twin,
    int kind,
    const char* type_value,
    const uint8_t* body,
    size_t body_len,
    int idx)
{
  TI(twin).pending[idx].in_use = true;
  TI(twin).pending[idx].kind = kind;
  TI(twin).pending[idx].deadline_ms = deadline_from_now(twin);
  az_iot_result r
      = publish_twin(twin, type_value, body, body_len, TI(twin).pending[idx].correlation_id);
  if (r != AZ_IOT_OK)
  {
    memset(&TI(twin).pending[idx], 0, sizeof(TI(twin).pending[idx]));
  }
  return r;
}

az_iot_result az_iot_gen2_twin_client_get(
    az_iot_gen2_twin_client* twin,
    az_iot_gen2_twin_get_callback cb,
    void* user_ctx)
{
  az_iot_gen2_twin_get_options opts = az_iot_gen2_twin_get_options_default();
  return az_iot_gen2_twin_client_get_with_options(twin, &opts, cb, user_ctx);
}

az_iot_result az_iot_gen2_twin_client_get_with_options(
    az_iot_gen2_twin_client* twin,
    const az_iot_gen2_twin_get_options* opts,
    az_iot_gen2_twin_get_callback cb,
    void* user_ctx)
{
  if (!twin || !opts || (unsigned)opts->sections < (unsigned)AZ_IOT_GEN2_TWIN_SECTIONS_DESIRED
      || (unsigned)opts->sections > (unsigned)AZ_IOT_GEN2_TWIN_SECTIONS_BOTH)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  uint8_t body[TWIN_GET_BODY_MAX];
  size_t body_len = 0;
  if (!encode_get_body(opts, body, sizeof(body), &body_len))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  int idx = alloc_pending(twin);
  if (idx < 0)
  {
    AZ_IOT_LOG_WARN("gen2_twin: refusing a GET -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  TI(twin).pending[idx].cb.get_cb = cb;
  TI(twin).pending[idx].user_ctx = user_ctx;
  return submit_request(twin, TWIN_PENDING_GET, TWIN_TYPE_GET, body, body_len, idx);
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
  return az_iot_gen2_twin_client_patch_reported_if_match(
      twin, TI(twin).reported_properties_service_version, patch, patch_len, cb, user_ctx);
}

az_iot_result az_iot_gen2_twin_client_patch_reported_if_match(
    az_iot_gen2_twin_client* twin,
    uint64_t if_match,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_gen2_twin_patch_ack_callback cb,
    void* user_ctx)
{
  if (!twin || (patch_len > 0 && patch == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!TI(twin).encode_buffer)
  {
    AZ_IOT_LOG_ERROR("gen2_twin: refusing a patch -- no encode buffer; call "
                     "az_iot_gen2_twin_client_set_encode_buffer()");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  /* ReportedPatch { 1 if_match, 2 payload }; proto3 omits a zero if_match. */
  size_t pos = 0;
  if ((if_match
       && !az_iot_proto3_write_varint_field(
           TI(twin).encode_buffer,
           TI(twin).encode_buffer_len,
           &pos,
           TWIN_F_REPORTED_IF_MATCH,
           if_match))
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

  int idx = alloc_pending(twin);
  if (idx < 0)
  {
    AZ_IOT_LOG_WARN(
        "gen2_twin: refusing a patch -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  TI(twin).pending[idx].cb.patch_cb = cb;
  TI(twin).pending[idx].user_ctx = user_ctx;
  return submit_request(
      twin, TWIN_PENDING_PATCH, TWIN_TYPE_REPORTED_PATCH, TI(twin).encode_buffer, pos, idx);
}

az_iot_result az_iot_gen2_twin_client_set_desired_handler(
    az_iot_gen2_twin_client* twin,
    az_iot_gen2_twin_desired_callback cb,
    void* user_ctx)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  TI(twin).desired_handler = cb;
  TI(twin).desired_handler_ctx = user_ctx;
  /* A new or resumed handler holds nothing yet. */
  TI(twin).desired_properties_device_version = 0;
  request_snapshot_if_behind(twin);
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen2_twin_client_set_reported_handler(
    az_iot_gen2_twin_client* twin,
    az_iot_gen2_twin_reported_callback cb,
    void* user_ctx)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  TI(twin).reported_handler = cb;
  TI(twin).reported_handler_ctx = user_ctx;
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
  /* Exactly the overhead is the valid minimum: it frames an empty patch. */
  if (buffer && buffer_len < AZ_IOT_GEN2_TWIN_ENCODE_OVERHEAD)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  TI(twin).encode_buffer = buffer;
  TI(twin).encode_buffer_len = buffer ? buffer_len : 0;
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen2_twin_client_set_request_timeout(
    az_iot_gen2_twin_client* twin,
    uint32_t timeout_ms)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  TI(twin).request_timeout_ms
      = timeout_ms ? timeout_ms : AZ_IOT_GEN2_TWIN_REQUEST_TIMEOUT_MS_DEFAULT;
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen2_twin_client_do_work(az_iot_gen2_twin_client* twin)
{
  if (!twin || !TI(twin).conn)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  uint64_t now = az_iot_time_mono_ms();
  if (TI(twin).snapshot.in_flight && now >= TI(twin).snapshot.deadline_ms)
  {
    /* Re-requested on the next desired event or reconnect, not on a timer. */
    AZ_IOT_LOG_WARN("gen2_twin: the desired-snapshot GET went unanswered");
    TI(twin).snapshot.in_flight = false;
  }
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(twin).pending[i].in_use && now >= TI(twin).pending[i].deadline_ms)
    {
      release_pending(twin, i, AZ_IOT_ERR_TIMEOUT, NULL, NULL);
    }
  }
  return AZ_IOT_OK;
}
