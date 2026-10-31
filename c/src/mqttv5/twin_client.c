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
#include "azure/iot/mqttv5/az_iot_twin_client.h"

#include "internal/connection_client_internal.h"
#include "internal/deadline.h"
#include "internal/defensive_timeout.h"
#include "internal/delivery_tracker.h"
#include "internal/log_internal.h"
#include "internal/proto3.h"
#include "internal/mono_time.h"
#include "internal/span_writer.h"
#include "internal/twin_client_internal.h"

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

/* Largest TwinGet body: three varint fields, each a one-byte key (field <= 15)
 * and a value of at most AZ_IOT_PROTO3_VARINT_MAX_BYTES. */
#define TWIN_GET_BODY_MAX (3u * (1u + AZ_IOT_PROTO3_VARINT_MAX_BYTES))

#define TWIN_PENDING_NONE 0
#define TWIN_PENDING_GET 1
#define TWIN_PENDING_PATCH 2
#define TWIN_PENDING_SNAPSHOT 3 /* The client's own desired-snapshot GET. */

#define TI(t) ((t)->_internal)

/* The public arrays are sized by the public constant but filled and compared
 * with the core's; fail the build if they diverge. */
typedef char az_iot_twin_correlation_id_len_agrees
    [(AZ_IOT_MQTTV5_TWIN_CORRELATION_ID_LEN == AZ_IOT_CORRELATION_UUID_LEN) ? 1 : -1];
typedef char
    az_iot_twin_get_body_fits[(TWIN_GET_BODY_MAX <= AZ_IOT_MQTTV5_TWIN_GET_BODY_MAX) ? 1 : -1];

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/** @brief Number of application requests (GET and reported patch) in flight. */
static size_t count_application_requests(const az_iot_mqttv5_twin_client* t)
{
  size_t count = 0;
  for (size_t i = 0; i < AZ_IOT_MQTTV5_TWIN_TRACKED_MESSAGES; ++i)
  {
    int const kind = TI(t).requests[i].kind;
    count += (kind == TWIN_PENDING_GET || kind == TWIN_PENDING_PATCH) ? 1u : 0u;
  }
  return count;
}

/** @brief Whether the client's own desired-snapshot GET is in flight. */
static bool snapshot_in_flight(const az_iot_mqttv5_twin_client* t)
{
  for (size_t i = 0; i < AZ_IOT_MQTTV5_TWIN_TRACKED_MESSAGES; ++i)
  {
    if (TI(t).requests[i].kind == TWIN_PENDING_SNAPSHOT)
    {
      return true;
    }
  }
  return false;
}

/** @brief The request tracker entry @p entry_index holds, or NULL. */
static az_iot_mqttv5_twin_request* request_of_entry(
    az_iot_mqttv5_twin_client* t,
    size_t entry_index)
{
  if (entry_index >= AZ_IOT_MQTTV5_TWIN_TRACKED_MESSAGES
      || TI(t).requests[entry_index].kind == TWIN_PENDING_NONE)
  {
    return NULL;
  }
  return &TI(t).requests[entry_index];
}

/** @brief True when the `type` user property is exactly "<want>:1". */
static bool type_is(const az_iot_mqtt_message* msg, const char* want)
{
  az_iot_mqtt_user_property_view type;
  if (az_iot_mqtt_user_properties_find(&msg->user_properties, TWIN_TYPE_KEY, &type) != AZ_IOT_OK)
  {
    return false;
  }
  size_t const n = strlen(want);
  size_t const v = strlen(TWIN_SCHEMA_VERSION);
  return type.value_len == n + 1 + v && memcmp(type.value, want, n) == 0 && type.value[n] == ':'
      && memcmp(type.value + n + 1, TWIN_SCHEMA_VERSION, v) == 0;
}

/** @brief True when @p topic is exactly `ih/{id}/dev/twin` (dispatch matches by prefix). */
static bool topic_is_inbound(const az_iot_mqttv5_twin_client* t, const char* topic)
{
  return TI(t).inbound_topic_len > 0 && strlen(topic) == TI(t).inbound_topic_len;
}

/** @brief True when @p msg carries @p id as its Correlation Data. */
static bool correlation_is(const az_iot_mqtt_message* msg, const uint8_t* id)
{
  return msg->correlation_data && msg->correlation_data_len == AZ_IOT_CORRELATION_UUID_LEN
      && memcmp(msg->correlation_data, id, AZ_IOT_CORRELATION_UUID_LEN) == 0;
}

/** @brief Tracker entry of the request @p msg answers, or -1. */
static int find_request_by_correlation_id(
    const az_iot_mqttv5_twin_client* t,
    const az_iot_mqtt_message* msg)
{
  for (int i = 0; i < (int)AZ_IOT_MQTTV5_TWIN_TRACKED_MESSAGES; ++i)
  {
    if (TI(t).requests[i].kind != TWIN_PENDING_NONE
        && correlation_is(msg, TI(t).requests[i].correlation_id))
    {
      return i;
    }
  }
  return -1;
}

/**
 * @brief True when a backend-initiated message belongs to the current connection.
 *
 * Accepted from the birth on: the service sends the twin-push before the birth-ack. Both come
 * from the same session open, so the birth-ack baseline matches what the push delivered.
 */
static bool matches_connection(const az_iot_mqttv5_twin_client* t, const az_iot_mqtt_message* msg)
{
  uint8_t birth_nonce[AZ_IOT_CORRELATION_UUID_LEN];
  return az_iot_connection_client__birth_nonce(TI(t).conn, birth_nonce) == AZ_IOT_OK
      && correlation_is(msg, birth_nonce);
}

/** @brief Raise the tracked service desired version; versions only move forward. */
static void note_desired_service_version(az_iot_mqttv5_twin_client* t, uint64_t version)
{
  if (version > TI(t).desired_properties_service_version)
  {
    TI(t).desired_properties_service_version = version;
  }
}

/* ------------------------------------------------------------------------- */
/* twin-push defensive timeout                                               */
/*                                                                           */
/* A birth with push_desired or push_reported is answered by a twin-push; a  */
/* session that never gets it gets no desired patches either. The wait is    */
/* armed on hub CONNECTED and timed like the birth-ack (defensive_timeout.h);*/
/* on expiry the session is ended with a DISCONNECT. Attempt N is the wait   */
/* after N-1 consecutive expired waits; it restarts at 1 once a push arrives */
/* or the hub scope goes IDLE or FAULTED (the next open() is a fresh cycle). */
/* ------------------------------------------------------------------------- */

#define TWIN_PUSH_SECTION_DESIRED 1u
#define TWIN_PUSH_SECTION_REPORTED 2u

/**
 * @brief Sections this birth's twin-push carries: the requested ones whose birth-ack version
 * differs from the birth's, which is 0. twin.proto omits a section already in sync, and sends no
 * push when none is out of sync.
 */
static uint8_t twin_push_sections_expected(const az_iot_mqttv5_twin_client* t)
{
  uint64_t desired_version = 0;
  uint64_t reported_version = 0;
  (void)az_iot_connection_client__presence_twin_versions(
      TI(t).conn, &desired_version, &reported_version);
  uint8_t expected = 0;
  if (az_iot_connection_client__twin_push_desired(TI(t).conn) && desired_version != 0)
  {
    expected |= TWIN_PUSH_SECTION_DESIRED;
  }
  if (az_iot_connection_client__twin_push_reported(TI(t).conn) && reported_version != 0)
  {
    expected |= TWIN_PUSH_SECTION_REPORTED;
  }
  return expected;
}

/**
 * @brief Whether the pushes decoded for the current birth carried every expected section, each
 * at least at its birth-ack version (a push decoded before the birth-ack may predate it).
 */
static bool twin_push_received_for_current_birth(const az_iot_mqttv5_twin_client* t)
{
  uint8_t birth_nonce[AZ_IOT_CORRELATION_UUID_LEN];
  uint64_t desired_version = 0;
  uint64_t reported_version = 0;
  (void)az_iot_connection_client__presence_twin_versions(
      TI(t).conn, &desired_version, &reported_version);
  uint8_t expected = twin_push_sections_expected(t);
  uint8_t const received = TI(t).twin_push_wait.received_sections;
  bool const desired_current = (received & TWIN_PUSH_SECTION_DESIRED) != 0
      && TI(t).twin_push_wait.received_desired_version >= desired_version;
  bool const reported_current = (received & TWIN_PUSH_SECTION_REPORTED) != 0
      && TI(t).twin_push_wait.received_reported_version >= reported_version;
  uint8_t const current = (uint8_t)((desired_current ? TWIN_PUSH_SECTION_DESIRED : 0u)
                                    | (reported_current ? TWIN_PUSH_SECTION_REPORTED : 0u));
  return az_iot_connection_client__birth_nonce(TI(t).conn, birth_nonce) == AZ_IOT_OK
      && memcmp(TI(t).twin_push_wait.received_birth_nonce, birth_nonce, AZ_IOT_CORRELATION_UUID_LEN)
      == 0
      && (current & expected) == expected;
}

/**
 * @brief Record a decoded twin-push @p twin carrying @p sections (TWIN_PUSH_SECTION_* bits); once
 * every expected section arrived, cancel the wait and restart the schedule. A push after the wait
 * expired, or was overdue when this do_work() pass began, finds no wait: that miss stands.
 */
static void twin_push_wait_on_push(
    az_iot_mqttv5_twin_client* t,
    const az_iot_mqtt_message* msg,
    uint8_t sections,
    const az_iot_mqttv5_twin_state* twin)
{
  if (az_iot_connection_client__deadline_passed_before_pass(
          TI(t).conn, &TI(t).twin_push_wait.deadline))
  {
    return;
  }
  if (memcmp(
          TI(t).twin_push_wait.received_birth_nonce,
          msg->correlation_data,
          AZ_IOT_CORRELATION_UUID_LEN)
      != 0)
  {
    memcpy(
        TI(t).twin_push_wait.received_birth_nonce,
        msg->correlation_data,
        AZ_IOT_CORRELATION_UUID_LEN);
    TI(t).twin_push_wait.received_sections = 0;
    TI(t).twin_push_wait.received_desired_version = 0;
    TI(t).twin_push_wait.received_reported_version = 0;
  }
  TI(t).twin_push_wait.received_sections |= sections;
  if ((sections & TWIN_PUSH_SECTION_DESIRED) != 0
      && twin->desired.version > TI(t).twin_push_wait.received_desired_version)
  {
    TI(t).twin_push_wait.received_desired_version = twin->desired.version;
  }
  if ((sections & TWIN_PUSH_SECTION_REPORTED) != 0
      && twin->reported.version > TI(t).twin_push_wait.received_reported_version)
  {
    TI(t).twin_push_wait.received_reported_version = twin->reported.version;
  }
  if (az_iot_deadline__is_set(&TI(t).twin_push_wait.deadline)
      && twin_push_received_for_current_birth(t))
  {
    az_iot_deadline__clear(&TI(t).twin_push_wait.deadline);
    TI(t).twin_push_wait.missed_count = 0;
  }
}

/** @brief At CONNECTED: arm the wait unless no section is expected or every one already
 * arrived (a push decoded before the birth-ack). */
static void twin_push_wait_arm(az_iot_mqttv5_twin_client* t)
{
  if (twin_push_sections_expected(t) == 0)
  {
    return;
  }
  if (twin_push_received_for_current_birth(t))
  {
    TI(t).twin_push_wait.missed_count = 0;
    return;
  }
  uint32_t attempt = az_iot_defensive_timeout__next_attempt(TI(t).twin_push_wait.missed_count);
  uint32_t timeout_ms = az_iot_connection_client__defensive_timeout_ms(TI(t).conn, attempt);
  az_iot_deadline__arm_after_ms(&TI(t).twin_push_wait.deadline, az_iot_time_mono_ms(), timeout_ms);
}

/** @brief TICK: end the session when its twin-push is overdue; the miss escalates the
 * next wait. Runs from the connection's do_work(), outside adapter callbacks. Overdue once a
 * whole pass began past the deadline, so input queued by then has had a pass to be dispatched. */
static void twin_push_wait_check(void* user_ctx)
{
  az_iot_mqttv5_twin_client* t = (az_iot_mqttv5_twin_client*)user_ctx;
  if (!az_iot_connection_client__deadline_passed_before_pass(
          TI(t).conn, &TI(t).twin_push_wait.deadline))
  {
    return;
  }
  az_iot_deadline__clear(&TI(t).twin_push_wait.deadline);
  az_iot_defensive_timeout__count_miss(&TI(t).twin_push_wait.missed_count);
  AZ_IOT_LOG_WARN(
      AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "the expected twin-push did not arrive; reconnecting");
  (void)az_iot_connection_client__end_hub_session(TI(t).conn, AZ_IOT_ERR_TIMEOUT);
}

/** @brief NEXT_DEADLINE: lower @p remaining_ms to the twin-push deadline, so the connection wakes
 * for it. */
static void twin_push_wait_lower_remaining(void* user_ctx, uint64_t* remaining_ms)
{
  az_iot_mqttv5_twin_client* t = (az_iot_mqttv5_twin_client*)user_ctx;
  uint64_t left
      = az_iot_deadline__remaining_ms(&TI(t).twin_push_wait.deadline, az_iot_time_mono_ms());
  if (left < *remaining_ms)
  {
    *remaining_ms = left;
  }
}

/* ------------------------------------------------------------------------- */
/* publishing                                                                */
/* ------------------------------------------------------------------------- */

/** @brief Build `ih/{device_id}<suffix>`. */
static az_iot_result build_topic(
    az_iot_connection_client* conn,
    const char* suffix,
    char (*topic)[AZ_IOT_MQTTV5_TWIN_TOPIC_MAX],
    size_t* out_len)
{
  const char* device_id = az_iot_connection_client__device_id(conn);
  if (!device_id)
  {
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "the device id is not assigned yet");
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  const char* parts[] = { TWIN_TOPIC_ROOT, device_id, suffix };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(*topic), out_len, parts, 3) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN,
        "the '%s' topic did not fit AZ_IOT_MQTTV5_TWIN_TOPIC_MAX bytes",
        suffix);
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_OK;
}

/** @brief Encode TwinGet from @p opts. */
static bool encode_get_body(
    const az_iot_mqttv5_twin_get_options* opts,
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

/** @brief The desired-snapshot GET as of now: `if_not_match` is the delivered version, so a
 *         handler already current gets no payload back. */
static az_iot_mqttv5_twin_get_options snapshot_get_options(const az_iot_mqttv5_twin_client* t)
{
  az_iot_mqttv5_twin_get_options opts = az_iot_mqttv5_twin_get_options_default();
  opts.sections = AZ_IOT_MQTTV5_TWIN_SECTIONS_DESIRED;
  opts.if_not_match_desired
      = TI(t).desired_snapshot_required ? 0u : TI(t).desired_properties_device_version;
  return opts;
}

/**
 * @brief Tracker encode(): the request tracker entry @p entry_index holds, built at each send.
 * @return AZ_IOT_OK; any other error discards the request.
 */
static az_iot_result encode_twin_request(void* owner, size_t entry_index, az_iot_mqtt_message* out)
{
  az_iot_mqttv5_twin_client* t = (az_iot_mqttv5_twin_client*)owner;
  const az_iot_mqttv5_twin_request* request = request_of_entry(t, entry_index);
  if (request == NULL)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  az_iot_result r = build_topic(TI(t).conn, TWIN_OUTBOUND_SUFFIX, &TI(t).outbound_topic, NULL);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  const uint8_t* body = TI(t).get_body;
  size_t body_len = 0;
  if (request->kind == TWIN_PENDING_PATCH)
  {
    /* The encode buffer is the caller's only during the patch call. */
    if (!TI(t).submit_in_progress || entry_index != TI(t).submitting_entry)
    {
      return AZ_IOT_ERR_INTERNAL;
    }
    body = az_span_ptr(TI(t).encode_buffer);
    body_len = request->patch_length;
    TI(t).outbound_type.value = TWIN_TYPE_REPORTED_PATCH;
  }
  else
  {
    az_iot_mqttv5_twin_get_options const opts
        = request->kind == TWIN_PENDING_SNAPSHOT ? snapshot_get_options(t) : request->get_options;
    if (!encode_get_body(&opts, TI(t).get_body, sizeof(TI(t).get_body), &body_len))
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    TI(t).outbound_type.value = TWIN_TYPE_GET;
  }
  TI(t).outbound_type.key = TWIN_TYPE_KEY;

  memset(out, 0, sizeof(*out));
  out->topic = TI(t).outbound_topic;
  out->payload = body;
  out->payload_len = body_len;
  out->qos = AZ_IOT_MQTT_QOS_0; /* answered by a response message, not a PUBACK */
  out->content_type = TWIN_CONTENT_TYPE;
  out->user_properties = az_iot_mqtt_user_properties_from_array(&TI(t).outbound_type, 1);
  out->correlation_data = request->correlation_id;
  out->correlation_data_len = AZ_IOT_CORRELATION_UUID_LEN;
  return AZ_IOT_OK;
}

/** @brief Tracker on_sent(): a reported patch must be sent within its call. */
static void note_twin_request_sent(void* owner, size_t entry_index)
{
  az_iot_mqttv5_twin_request* request
      = request_of_entry((az_iot_mqttv5_twin_client*)owner, entry_index);
  if (request != NULL)
  {
    request->sent = true;
  }
}

/** @brief Report @p request's end to its callback; the request is already released. */
static void report_twin_request(
    const az_iot_mqttv5_twin_request* request,
    az_iot_result status,
    const az_iot_mqttv5_twin_state* twin,
    const az_iot_mqttv5_twin_patch_result* result)
{
  if (request->kind == TWIN_PENDING_GET && request->cb.get_cb)
  {
    request->cb.get_cb(status, twin, request->user_ctx);
  }
  else if (request->kind == TWIN_PENDING_PATCH && request->cb.patch_cb)
  {
    request->cb.patch_cb(status, result, request->user_ctx);
  }
}

/**
 * @brief Tracker on_complete(): the request ended unanswered. Expired is AZ_IOT_ERR_TIMEOUT;
 *        a lost session or a request that cannot be sent is AZ_IOT_ERR_NOT_CONNECTED. The one
 *        being submitted is reported by the submitting call instead.
 */
static void complete_twin_request(
    void* owner,
    size_t entry_index,
    az_iot_delivery_tracker_outcome outcome,
    const az_iot_publish_result* detail)
{
  (void)detail;
  az_iot_mqttv5_twin_client* t = (az_iot_mqttv5_twin_client*)owner;
  az_iot_mqttv5_twin_request* held = request_of_entry(t, entry_index);
  if (held == NULL)
  {
    return;
  }
  az_iot_mqttv5_twin_request const request = *held;
  memset(held, 0, sizeof(*held));
  if (TI(t).submit_in_progress && entry_index == TI(t).submitting_entry)
  {
    TI(t).submit_failure = outcome == AZ_IOT_DELIVERY_TRACKER_OUTCOME_EXPIRED
        ? AZ_IOT_ERR_TIMEOUT
        : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    return;
  }
  if (request.kind == TWIN_PENDING_SNAPSHOT)
  {
    /* Re-requested on the next desired event or reconnect, not on a timer. */
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "the desired-snapshot GET went unanswered");
    return;
  }
  report_twin_request(
      &request,
      outcome == AZ_IOT_DELIVERY_TRACKER_OUTCOME_EXPIRED ? AZ_IOT_ERR_TIMEOUT
                                                         : AZ_IOT_ERR_NOT_CONNECTED,
      NULL,
      NULL);
}

static const az_iot_delivery_tracker_profile k_twin_get_profile = {
  .type = TWIN_TYPE_GET,
  .confirmation = AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_RESPONSE,
  .on_session_loss = AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_RESEND,
  .priority = 0,
  .send_untracked_without_slot = true, /* QoS 0: never needs a PUBACK slot. */
  .encode = encode_twin_request,
  .on_complete = complete_twin_request,
  .on_sent = note_twin_request_sent,
};

/* Not resent: the service may have applied it. */
static const az_iot_delivery_tracker_profile k_twin_reported_patch_profile = {
  .type = TWIN_TYPE_REPORTED_PATCH,
  .confirmation = AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_RESPONSE,
  .on_session_loss = AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_FAIL,
  .priority = 0,
  .send_untracked_without_slot = true, /* QoS 0: never needs a PUBACK slot. */
  .encode = encode_twin_request,
  .on_complete = complete_twin_request,
  .on_sent = note_twin_request_sent,
};

/**
 * @brief Track @p request under a fresh correlation id. A reported patch must be sent now: one
 *        still queued is withdrawn.
 * @return AZ_IOT_OK; AZ_IOT_ERR_BUSY from a callback of another submission;
 *         AZ_IOT_ERR_NOT_ENOUGH_SPACE when it cannot be sent at all; AZ_IOT_ERR_TIMEOUT when its
 *         time limit passed before it was sent; for a reported patch,
 *         AZ_IOT_ERR_NOT_CONNECTED or AZ_IOT_ERR_BUSY when it could not be sent now.
 */
static az_iot_result submit_twin_request(
    az_iot_mqttv5_twin_client* t,
    const az_iot_mqttv5_twin_request* request)
{
  if (TI(t).submit_in_progress)
  {
    return AZ_IOT_ERR_BUSY;
  }
  size_t entry = 0;
  while (entry < AZ_IOT_MQTTV5_TWIN_TRACKED_MESSAGES
         && TI(t).requests[entry].kind != TWIN_PENDING_NONE)
  {
    ++entry;
  }
  if (entry == AZ_IOT_MQTTV5_TWIN_TRACKED_MESSAGES)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  az_iot_mqttv5_twin_request* held = &TI(t).requests[entry];
  *held = *request;
  az_iot_connection_client__gen_uuid(TI(t).conn, held->correlation_id);
  TI(t).submitting_entry = entry;
  TI(t).submit_failure = AZ_IOT_OK;
  TI(t).submit_in_progress = true;
  int const kind = request->kind;
  az_iot_connection_client* const conn = TI(t).conn;
  az_iot_delivery_tracker_binding const bound = az_iot_delivery_tracker__binding(&TI(t).tracker);
  az_iot_result const r = az_iot_delivery_tracker__submit_at(
      &TI(t).tracker,
      entry,
      kind == TWIN_PENDING_PATCH ? &k_twin_reported_patch_profile : &k_twin_get_profile,
      held->correlation_id,
      sizeof(held->correlation_id),
      TI(t).request_timeout_ms);
  /* A callback of an older request may have deinitialized (or freed) this client. */
  if (!az_iot_delivery_tracker__has_binding(&TI(t).tracker, bound))
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  TI(t).submit_in_progress = false;
  if (r != AZ_IOT_OK)
  {
    memset(held, 0, sizeof(*held));
    return r;
  }
  if (TI(t).submit_failure != AZ_IOT_OK)
  {
    return TI(t).submit_failure; /* Already released. */
  }
  if (kind == TWIN_PENDING_PATCH && !held->sent)
  {
    memset(held, 0, sizeof(*held));
    az_iot_delivery_tracker__cancel(&TI(t).tracker, entry);
    return az_iot_connection_client__is_connected(conn) ? AZ_IOT_ERR_BUSY
                                                        : AZ_IOT_ERR_NOT_CONNECTED;
  }
  return AZ_IOT_OK;
}

/**
 * @brief Whether the application may make a request: a free slot, and a device id to address
 *        it to. @p type names it in the log.
 */
static az_iot_result check_application_request_allowed(
    const az_iot_mqttv5_twin_client* t,
    const char* type)
{
  if (count_application_requests(t) >= AZ_IOT_TWIN_MAX_PENDING)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN,
        "refusing a '%s' -- AZ_IOT_TWIN_MAX_PENDING requests are in flight",
        type);
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (!az_iot_connection_client__device_id(TI(t).conn))
  {
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "the device id is not assigned yet");
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  return AZ_IOT_OK;
}

/**
 * @brief Issue the SDK's desired-snapshot GET when the handler is behind.
 *
 * At most one is in flight, built at each send from the delivered version.
 */
static void request_snapshot_if_behind(az_iot_mqttv5_twin_client* t)
{
  if (!TI(t).desired_handler || snapshot_in_flight(t)
      || (!TI(t).desired_snapshot_required
          && TI(t).desired_properties_service_version <= TI(t).desired_properties_device_version))
  {
    return;
  }
  if (!az_iot_connection_client__is_connected(TI(t).conn))
  {
    TI(t).desired_catch_up_pending = true; /* issued on hub CONNECTED */
    return;
  }

  az_iot_mqttv5_twin_request request;
  memset(&request, 0, sizeof(request));
  request.kind = TWIN_PENDING_SNAPSHOT;
  az_iot_result const r = submit_twin_request(t, &request);
  if (r == AZ_IOT_ERR_NOT_INITIALIZED)
  {
    return; /* A callback deinitialized the client. */
  }
  /* BUSY: from a callback of another request call; issued on a later TICK. Other failures are
   * re-evaluated on the next desired event. */
  TI(t).desired_catch_up_pending = r == AZ_IOT_ERR_BUSY;
}

/** @brief TICK: issue a snapshot an earlier call could not. @return Whether the client is still
 *         initialized: a callback the submission runs may deinitialize it. */
static bool request_pending_snapshot(az_iot_mqttv5_twin_client* t)
{
  if (!TI(t).desired_catch_up_pending || !az_iot_connection_client__is_connected(TI(t).conn))
  {
    return true;
  }
  az_iot_delivery_tracker_binding const bound = az_iot_delivery_tracker__binding(&TI(t).tracker);
  request_snapshot_if_behind(t);
  return az_iot_delivery_tracker__has_binding(&TI(t).tracker, bound);
}

/** @brief Session end: a twin-push wait already past its deadline is a miss. Requests are the
 *         tracker's: a GET is sent again, a reported patch fails. */
static void twin_push_wait_end_session(void* user_ctx)
{
  az_iot_mqttv5_twin_client* t = (az_iot_mqttv5_twin_client*)user_ctx;
  if (!t)
  {
    return;
  }
  /* IDLE or FAULTED reset the count afterwards. */
  if (az_iot_deadline__has_passed(&TI(t).twin_push_wait.deadline, az_iot_time_mono_ms()))
  {
    az_iot_defensive_timeout__count_miss(&TI(t).twin_push_wait.missed_count);
  }
  az_iot_deadline__clear(&TI(t).twin_push_wait.deadline);
}

/* ------------------------------------------------------------------------- */
/* desired delivery                                                          */
/* ------------------------------------------------------------------------- */

/** @brief Deliver @p section as a snapshot if it is newer than what the handler has. */
static void deliver_snapshot(
    az_iot_mqttv5_twin_client* t,
    const az_iot_mqttv5_twin_section* section)
{
  note_desired_service_version(t, section->version);
  if (!TI(t).desired_handler || !section->payload
      || (!TI(t).desired_snapshot_required
          && section->version <= TI(t).desired_properties_device_version))
  {
    return;
  }
  TI(t).desired_properties_device_version = section->version;
  TI(t).desired_snapshot_required = false;
  TI(t).desired_handler(
      AZ_IOT_MQTTV5_TWIN_DESIRED_SNAPSHOT,
      section->version,
      section->payload,
      section->payload_len,
      TI(t).desired_handler_ctx);
}

/* ------------------------------------------------------------------------- */
/* decoding                                                                  */
/* ------------------------------------------------------------------------- */

/** @brief Decode Section { 1 version, 2 payload }. */
static bool decode_section(const uint8_t* buf, size_t len, az_iot_mqttv5_twin_section* out)
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
    else if (field == TWIN_F_SECTION_VERSION || field == TWIN_F_SECTION_PAYLOAD)
    {
      ok = false; /* a known field on the wrong wire type */
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
static bool decode_get_response(const az_iot_mqtt_message* msg, az_iot_mqttv5_twin_state* twin)
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
static void on_twin_push(az_iot_mqttv5_twin_client* t, const az_iot_mqtt_message* msg)
{
  az_iot_mqttv5_twin_state twin;
  memset(&twin, 0, sizeof(twin));

  const uint8_t* buf = msg->payload;
  size_t len = msg->payload_len;
  size_t pos = 0;
  bool ok = true;
  bool desired_present = false;
  bool reported_present = false;
  while (ok && buf && pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      ok = false;
    }
    else if (field == TWIN_F_PUSH_DESIRED || field == TWIN_F_PUSH_REPORTED)
    {
      const uint8_t* sec = NULL;
      size_t sec_len = 0;
      desired_present |= (field == TWIN_F_PUSH_DESIRED);
      reported_present |= (field == TWIN_F_PUSH_REPORTED);
      /* A known section on any other wire type is malformed. */
      ok = wire == AZ_IOT_PROTO3_WIRE_LEN
          && az_iot_proto3_read_bytes(buf, len, &pos, &sec, &sec_len)
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
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "dropping a malformed twin-push");
    return;
  }
  twin_push_wait_on_push(
      t,
      msg,
      (uint8_t)((desired_present ? TWIN_PUSH_SECTION_DESIRED : 0u)
                | (reported_present ? TWIN_PUSH_SECTION_REPORTED : 0u)),
      &twin);

  if (reported_present)
  {
    TI(t).reported_properties_service_version = twin.reported.version;
  }
  if (reported_present && TI(t).reported_handler)
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
static void on_desired_patch(az_iot_mqttv5_twin_client* t, const az_iot_mqtt_message* msg)
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
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "dropping a malformed desired-patch");
    return;
  }

  note_desired_service_version(t, version);
  if (TI(t).desired_handler && has_payload && !TI(t).desired_snapshot_required
      && version > TI(t).desired_properties_device_version
      && version - TI(t).desired_properties_device_version == 1u)
  {
    TI(t).desired_properties_device_version = version;
    TI(t).desired_handler(
        AZ_IOT_MQTTV5_TWIN_DESIRED_PATCH, version, patch, patch_len, TI(t).desired_handler_ctx);
  }
  /* Still behind after an in-order patch if an earlier fetch was lost. */
  request_snapshot_if_behind(t);
}

/** @brief Response to the SDK's own snapshot GET. */
static void on_snapshot_response(az_iot_mqttv5_twin_client* t, const az_iot_mqtt_message* msg)
{
  az_iot_mqttv5_twin_state twin;
  if (!type_is(msg, TWIN_TYPE_GET_RESPONSE) || !decode_get_response(msg, &twin))
  {
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "dropping an unusable desired-snapshot response");
    return;
  }
  TI(t).reported_properties_service_version = twin.reported.version;
  deliver_snapshot(t, &twin.desired);
  /* Still behind if the service moved on while the GET was in flight. */
  request_snapshot_if_behind(t);
}

/** @brief Response to an application GET. Desired state is not delivered from it. */
static void on_get_response(
    az_iot_mqttv5_twin_client* t,
    const az_iot_mqttv5_twin_request* request,
    const az_iot_mqtt_message* msg)
{
  az_iot_mqttv5_twin_state twin;
  if (!decode_get_response(msg, &twin))
  {
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "dropping a malformed get-response");
    report_twin_request(request, AZ_IOT_ERR_PROTOCOL, NULL, NULL);
    return;
  }
  TI(t).reported_properties_service_version = twin.reported.version;
  note_desired_service_version(t, twin.desired.version);
  report_twin_request(request, AZ_IOT_OK, &twin, NULL);
  request_snapshot_if_behind(t);
}

/** @brief reported-patch-response: ReportedPatchResponse { 1 result, 2 version }. */
static void on_patch_response(
    az_iot_mqttv5_twin_client* t,
    const az_iot_mqttv5_twin_request* request,
    const az_iot_mqtt_message* msg)
{
  az_iot_mqttv5_twin_patch_result result;
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
      result.status = (az_iot_mqttv5_twin_patch_status)v;
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
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "dropping a malformed reported-patch-response");
    report_twin_request(request, AZ_IOT_ERR_PROTOCOL, NULL, NULL);
    return;
  }

  TI(t).reported_properties_service_version = result.version;
  report_twin_request(request, AZ_IOT_OK, NULL, &result);
}

static void adopt_session(az_iot_mqttv5_twin_client* t);

/** @brief Single handler for `ih/{id}/dev/twin`, routed on `type`. */
static void on_twin_inbound(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_mqttv5_twin_client* t = (az_iot_mqttv5_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic || !topic_is_inbound(t, msg->topic))
  {
    return;
  }
  adopt_session(t);

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

  int idx = find_request_by_correlation_id(t, msg);
  if (idx < 0)
  {
    return; /* stale or unknown */
  }
  /* Answered: released before its callback, so the callback may make a new request. */
  az_iot_mqttv5_twin_request const request = TI(t).requests[idx];
  memset(&TI(t).requests[idx], 0, sizeof(TI(t).requests[idx]));
  az_iot_delivery_tracker_binding const bound = az_iot_delivery_tracker__binding(&TI(t).tracker);
  az_iot_delivery_tracker__cancel(&TI(t).tracker, (size_t)idx);
  /* The cancel sends what the freed entry lets go; a callback there may deinitialize this client.
   */
  if (!az_iot_delivery_tracker__has_binding(&TI(t).tracker, bound))
  {
    return;
  }

  if (request.kind == TWIN_PENDING_SNAPSHOT)
  {
    on_snapshot_response(t, msg);
  }
  else if (type_is(msg, TWIN_TYPE_GET_RESPONSE) && request.kind == TWIN_PENDING_GET)
  {
    on_get_response(t, &request, msg);
  }
  else if (type_is(msg, TWIN_TYPE_PATCH_RESPONSE) && request.kind == TWIN_PENDING_PATCH)
  {
    on_patch_response(t, &request, msg);
  }
  else
  {
    /* Ours, but unreadable. The service sends one answer; do not wait for another. */
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN,
        "releasing a pending request whose response was not usable");
    report_twin_request(&request, AZ_IOT_ERR_PROTOCOL, NULL, NULL);
  }
}

/**
 * @brief Adopt the birth-ack baseline, once per session.
 *
 * Keyed on the birth nonce and run by whatever touches the new session first:
 * a twin-push can arrive before hub CONNECTED is announced, and must be applied
 * on top of the baseline, not overwritten by it. A desired version below what
 * the handler holds means the twin was replaced; patches cannot follow it.
 */
static void adopt_session(az_iot_mqttv5_twin_client* t)
{
  uint8_t birth_nonce[AZ_IOT_CORRELATION_UUID_LEN];
  uint64_t desired = 0;
  uint64_t reported = 0;
  if (az_iot_connection_client__presence_nonce(TI(t).conn, birth_nonce) != AZ_IOT_OK
      || (TI(t).birth_nonce_valid
          && memcmp(TI(t).birth_nonce, birth_nonce, AZ_IOT_CORRELATION_UUID_LEN) == 0)
      || az_iot_connection_client__presence_twin_versions(TI(t).conn, &desired, &reported)
          != AZ_IOT_OK)
  {
    return;
  }
  memcpy(TI(t).birth_nonce, birth_nonce, AZ_IOT_CORRELATION_UUID_LEN);
  TI(t).birth_nonce_valid = true;

  TI(t).reported_properties_service_version = reported;
  if (desired < TI(t).desired_properties_device_version)
  {
    TI(t).desired_snapshot_required = true;
  }
  TI(t).desired_properties_service_version = desired;
}

/**
 * @brief Hub CONNECTED: requests can now be published, so catch up.
 *
 * With `twin_push.push_desired` the service pushes the snapshot, so the birth-ack
 * fetch is skipped; a replaced twin not yet settled by a pushed snapshot, or a
 * gap a patch or probe showed before CONNECTED, is still fetched.
 */
static void on_hub_connected(az_iot_mqttv5_twin_client* t)
{
  adopt_session(t);
  if (!az_iot_connection_client__twin_push_desired(TI(t).conn) || TI(t).desired_snapshot_required
      || TI(t).desired_catch_up_pending)
  {
    request_snapshot_if_behind(t);
  }
}

static void on_connection_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_mqttv5_twin_client* t = (az_iot_mqttv5_twin_client*)user_ctx;
  if (!t || !event || event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }
  if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    az_iot_delivery_tracker_binding const bound = az_iot_delivery_tracker__binding(&TI(t).tracker);
    on_hub_connected(t);
    /* Its snapshot submission may run a callback that deinitializes the client. */
    if (!az_iot_delivery_tracker__has_binding(&TI(t).tracker, bound))
    {
      return;
    }
    /* Armed only here, not by init on a session already up: its push may be gone. */
    twin_push_wait_arm(t);
  }
  else if (event->state == AZ_IOT_CONN_STATE_IDLE || event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    az_iot_deadline__clear(&TI(t).twin_push_wait.deadline);
    TI(t).twin_push_wait.missed_count = 0;
  }
}

/** @brief Feature observer: the twin-push wait and state transitions; requests are the tracker's.
 */
static void on_connection_event(const az_iot_connection_feature_event* event, void* user_ctx)
{
  switch (event->kind)
  {
    case AZ_IOT_CONNECTION_FEATURE_EVENT_SESSION_END:
      twin_push_wait_end_session(user_ctx);
      break;
    case AZ_IOT_CONNECTION_FEATURE_EVENT_TICK:
      if (request_pending_snapshot((az_iot_mqttv5_twin_client*)user_ctx))
      {
        twin_push_wait_check(user_ctx);
      }
      break;
    case AZ_IOT_CONNECTION_FEATURE_EVENT_NEXT_DEADLINE:
      twin_push_wait_lower_remaining(user_ctx, event->remaining_ms);
      break;
    case AZ_IOT_CONNECTION_FEATURE_EVENT_STATE:
      on_connection_state(event->state, user_ctx);
      break;
    case AZ_IOT_CONNECTION_FEATURE_EVENT_PUBLISH_COMPLETED:
    case AZ_IOT_CONNECTION_FEATURE_EVENT_IDENTITY_CHANGED:
    case AZ_IOT_CONNECTION_FEATURE_EVENT_UNSETTLED_QUERY:
    default:
      break;
  }
}

/* ------------------------------------------------------------------------- */
/* binding                                                                   */
/* ------------------------------------------------------------------------- */

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_mqttv5_twin_client* client = (az_iot_mqttv5_twin_client*)owner;

  char topic[AZ_IOT_MQTTV5_TWIN_TOPIC_MAX];
  size_t topic_len = 0;
  az_iot_result r = build_topic(conn, TWIN_INBOUND_SUFFIX, &topic, &topic_len);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN,
        "cannot bind topics -- the inbound topic could not be built");
    return (r == AZ_IOT_ERR_NOT_CONNECTED) ? AZ_IOT_ERR_NOT_INITIALIZED : AZ_IOT_ERR_INTERNAL;
  }
  TI(client).inbound_topic_len = topic_len;

  r = az_iot_connection_client__register_inbound_handler(conn, topic, on_twin_inbound, client);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN,
        "could not register delivery for the twin topic (%s)",
        az_iot_result_to_string(r));
  }
  return r;
}

/** @brief Withdraw everything init registered; shared by the init unwind and deinit. */
static void withdraw_registrations(
    az_iot_connection_client* conn,
    az_iot_mqttv5_twin_client* client)
{
  az_iot_delivery_tracker__unbind(&TI(client).tracker);
  az_iot_connection_client__remove_feature_observer(conn, on_connection_event, client);
  az_iot_connection_client__unregister_feature_client_bind(conn, client);
  (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */
/* ------------------------------------------------------------------------- */

AZ_NODISCARD az_iot_result
az_iot_mqttv5_twin_client_init(az_iot_mqttv5_twin_client* client, az_iot_connection_client* conn)
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
  TI(client).request_timeout_ms = AZ_IOT_MQTTV5_TWIN_REQUEST_TIMEOUT_MS_DEFAULT;

  result = az_iot_connection_client__register_feature_client_bind(conn, client, bind_topics);
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__add_feature_observer(
        conn,
        AZ_IOT_CONNECTION_FEATURE_EVENT_STATE | AZ_IOT_CONNECTION_FEATURE_EVENT_SESSION_END
            | AZ_IOT_CONNECTION_FEATURE_EVENT_TICK | AZ_IOT_CONNECTION_FEATURE_EVENT_NEXT_DEADLINE,
        on_connection_event,
        client);
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_delivery_tracker__init(
        &TI(client).tracker,
        TI(client).tracker_entries,
        AZ_IOT_MQTTV5_TWIN_TRACKED_MESSAGES,
        0,
        NULL,
        0,
        client);
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_delivery_tracker__bind(&TI(client).tracker, conn);
  }

  if (result != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN,
        "init failed (%s); withdrawing partial registrations",
        az_iot_result_to_string(result));
    withdraw_registrations(conn, client);
    az_iot_connection_client__release_profile(conn);
    memset(client, 0, sizeof(*client));
    return result;
  }

  /* Observers are not told about a session already up; catch up on it. Its
   * push may have come before this client existed: waiting costs at most one
   * reconnect, not waiting could leave the session without desired patches. */
  if (az_iot_connection_client__is_connected(conn))
  {
    on_hub_connected(client);
    twin_push_wait_arm(client);
  }
  return AZ_IOT_OK;
}

void az_iot_mqttv5_twin_client_deinit(az_iot_mqttv5_twin_client* client)
{
  if (!client || !TI(client).conn)
  {
    return;
  }
  withdraw_registrations(TI(client).conn, client);
  az_iot_connection_client__release_profile(TI(client).conn);
  memset(client, 0, sizeof(*client));
}

AZ_NODISCARD az_iot_mqttv5_twin_get_options az_iot_mqttv5_twin_get_options_default(void)
{
  az_iot_mqttv5_twin_get_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.sections = AZ_IOT_MQTTV5_TWIN_SECTIONS_BOTH;
  return opts;
}

AZ_NODISCARD az_iot_result az_iot_mqttv5_twin_client_get(
    az_iot_mqttv5_twin_client* twin,
    az_iot_mqttv5_twin_get_callback cb,
    void* user_ctx)
{
  az_iot_mqttv5_twin_get_options opts = az_iot_mqttv5_twin_get_options_default();
  return az_iot_mqttv5_twin_client_get_with_options(twin, &opts, cb, user_ctx);
}

AZ_NODISCARD az_iot_result az_iot_mqttv5_twin_client_get_with_options(
    az_iot_mqttv5_twin_client* twin,
    const az_iot_mqttv5_twin_get_options* opts,
    az_iot_mqttv5_twin_get_callback cb,
    void* user_ctx)
{
  if (!twin || !opts || (unsigned)opts->sections < (unsigned)AZ_IOT_MQTTV5_TWIN_SECTIONS_DESIRED
      || (unsigned)opts->sections > (unsigned)AZ_IOT_MQTTV5_TWIN_SECTIONS_BOTH)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  uint8_t body[TWIN_GET_BODY_MAX];
  size_t body_len = 0;
  if (!encode_get_body(opts, body, sizeof(body), &body_len))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  az_iot_result const allowed = check_application_request_allowed(twin, TWIN_TYPE_GET);
  if (allowed != AZ_IOT_OK)
  {
    return allowed;
  }
  az_iot_mqttv5_twin_request request;
  memset(&request, 0, sizeof(request));
  request.kind = TWIN_PENDING_GET;
  request.get_options = *opts;
  request.cb.get_cb = cb;
  request.user_ctx = user_ctx;
  return submit_twin_request(twin, &request);
}

AZ_NODISCARD az_iot_result az_iot_mqttv5_twin_client_patch_reported(
    az_iot_mqttv5_twin_client* twin,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_mqttv5_twin_patch_ack_callback cb,
    void* user_ctx)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return az_iot_mqttv5_twin_client_patch_reported_if_match(
      twin, TI(twin).reported_properties_service_version, patch, patch_len, cb, user_ctx);
}

AZ_NODISCARD az_iot_result az_iot_mqttv5_twin_client_patch_reported_if_match(
    az_iot_mqttv5_twin_client* twin,
    uint64_t if_match,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_mqttv5_twin_patch_ack_callback cb,
    void* user_ctx)
{
  if (!twin || (patch_len > 0 && patch == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* From a callback run by another request call: the encode buffer may hold its patch. */
  if (TI(twin).submit_in_progress)
  {
    return AZ_IOT_ERR_BUSY;
  }
  uint8_t* buf = az_span_ptr(TI(twin).encode_buffer);
  size_t cap = (size_t)az_span_size(TI(twin).encode_buffer);
  if (cap == 0)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN,
        "refusing a patch -- no encode buffer; call "
        "az_iot_mqttv5_twin_client_set_encode_buffer()");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  /* ReportedPatch { 1 if_match, 2 payload }; proto3 omits a zero if_match. */
  size_t pos = 0;
  if ((if_match
       && !az_iot_proto3_write_varint_field(buf, cap, &pos, TWIN_F_REPORTED_IF_MATCH, if_match))
      || !az_iot_proto3_write_bytes_field(
          buf, cap, &pos, TWIN_F_REPORTED_PAYLOAD, patch, patch_len))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN, "the framed patch did not fit the encode buffer");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  az_iot_result const allowed = check_application_request_allowed(twin, TWIN_TYPE_REPORTED_PATCH);
  if (allowed != AZ_IOT_OK)
  {
    return allowed;
  }
  az_iot_mqttv5_twin_request request;
  memset(&request, 0, sizeof(request));
  request.kind = TWIN_PENDING_PATCH;
  request.patch_length = pos;
  request.cb.patch_cb = cb;
  request.user_ctx = user_ctx;
  return submit_twin_request(twin, &request);
}

az_iot_result az_iot_mqttv5_twin_client_set_desired_handler(
    az_iot_mqttv5_twin_client* twin,
    az_iot_mqttv5_twin_desired_callback cb,
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
  TI(twin).desired_snapshot_required = false;
  request_snapshot_if_behind(twin);
  return AZ_IOT_OK;
}

az_iot_result az_iot_mqttv5_twin_client_set_reported_handler(
    az_iot_mqttv5_twin_client* twin,
    az_iot_mqttv5_twin_reported_callback cb,
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

az_iot_result az_iot_mqttv5_twin_client_set_encode_buffer(
    az_iot_mqttv5_twin_client* twin,
    az_span buffer)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* From a callback run by another request call: that call may still read the buffer. */
  if (TI(twin).submit_in_progress)
  {
    return AZ_IOT_ERR_BUSY;
  }
  int32_t size = az_span_size(buffer);
  if (size < 0 || (size > 0 && !az_span_ptr(buffer)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Exactly the overhead is the valid minimum: it frames an empty patch. */
  if (size > 0 && size < AZ_IOT_MQTTV5_TWIN_ENCODE_OVERHEAD)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  TI(twin).encode_buffer = buffer;
  return AZ_IOT_OK;
}

az_iot_result az_iot_mqttv5_twin_client_set_request_timeout(
    az_iot_mqttv5_twin_client* twin,
    uint32_t timeout_ms)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  TI(twin).request_timeout_ms
      = timeout_ms ? timeout_ms : AZ_IOT_MQTTV5_TWIN_REQUEST_TIMEOUT_MS_DEFAULT;
  return AZ_IOT_OK;
}

az_iot_result az_iot_mqttv5_twin_client_do_work(az_iot_mqttv5_twin_client* twin)
{
  if (!twin || !TI(twin).conn)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_delivery_tracker__expire_overdue(&TI(twin).tracker);
  return AZ_IOT_OK;
}

void az_iot_mqttv5_twin_client__twin_push_force_timeout(az_iot_mqttv5_twin_client* twin)
{
  if (twin && az_iot_deadline__is_set(&TI(twin).twin_push_wait.deadline))
  {
    az_iot_deadline__expire_now(&TI(twin).twin_push_wait.deadline);
  }
}

bool az_iot_mqttv5_twin_client__twin_push_remaining(
    const az_iot_mqttv5_twin_client* twin,
    uint64_t* out_remaining_ms)
{
  if (!twin || !az_iot_deadline__is_set(&TI(twin).twin_push_wait.deadline))
  {
    return false;
  }
  if (out_remaining_ms)
  {
    *out_remaining_ms
        = az_iot_deadline__remaining_ms(&TI(twin).twin_push_wait.deadline, az_iot_time_mono_ms());
  }
  return true;
}
