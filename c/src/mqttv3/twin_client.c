// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv3 hub (MQTT v3.1.1) twin client.
 *
 *   GET            : $iothub/twin/GET/?$rid=<n>
 *   PATCH reported : $iothub/twin/PATCH/properties/reported/?$rid=<n>
 *   Response       : $iothub/twin/res/<status>/?$rid=<n>[&$version=<v>]
 *   Desired        : $iothub/twin/PATCH/properties/desired/?$version=<v>
 *
 * Requests correlate on the `$rid` the device mints and the service echoes in
 * the response topic. These topics carry no device id, so they are known before
 * the connection resolves and no connect-time bind is needed.
 */
#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv3/az_iot_twin_client.h"

#include "internal/connection_client_internal.h"
#include "internal/delivery_tracker.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define TWIN_RESPONSE_PREFIX "$iothub/twin/res/"
#define TWIN_DESIRED_PREFIX "$iothub/twin/PATCH/properties/desired/"
#define TWIN_GET_TOPIC_PREFIX "$iothub/twin/GET/?$rid="
#define TWIN_PATCH_TOPIC_PREFIX "$iothub/twin/PATCH/properties/reported/?$rid="

/* Query-string keys carried on the inbound topics. */
#define TWIN_QUERY_RID "$rid"
#define TWIN_QUERY_VERSION "$version"

/* Request kinds, held in _internal.requests[].kind. */
#define TWIN_PENDING_NONE 0
#define TWIN_PENDING_GET 1
#define TWIN_PENDING_PATCH 2

/* Status codes carried on the twin response topic, per the IoT Hub twin
 * documentation. The values happen to coincide with HTTP status codes, but the
 * service contract does not state that they are HTTP codes, so nothing here
 * depends on that: they are declared as twin constants rather than borrowed
 * from an HTTP header. */
#define TWIN_STATUS_SUCCESS_MIN 200
#define TWIN_STATUS_SUCCESS_LIMIT 300 /* exclusive upper bound */
#define TWIN_STATUS_BAD_REQUEST 400
#define TWIN_STATUS_NOT_FOUND 404
#define TWIN_STATUS_THROTTLED 429

/* Internal shorthand to access _internal fields */
#define TI(t) ((t)->_internal)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Tracker entry of the request carrying @p rid, or -1. */
static int find_request_by_rid(const az_iot_mqttv3_twin_client* t, uint32_t rid)
{
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(t).requests[i].kind != TWIN_PENDING_NONE && TI(t).requests[i].rid == rid)
    {
      return i;
    }
  }
  return -1;
}

/* View a NUL-terminated run of topic bytes as a span, without copying. */
static az_span span_over(const char* text, size_t len)
{
  return az_span_create((uint8_t*)(uintptr_t)text, (int32_t)len);
}

/* Parse a whole span as a decimal uint32.
 *
 * az_span_atou32 rejects an embedded non-digit and anything that would
 * overflow, and the span is length-counted, so a value such as "1x" or one past
 * UINT32_MAX cannot decode into a live request id the way a prefix-tolerant
 * strtoul() would. */
static bool span_to_u32(az_span text, uint32_t* out)
{
  return az_span_size(text) > 0 && az_span_atou32(text, out) == AZ_OK;
}

static bool span_to_u64(az_span text, uint64_t* out)
{
  return az_span_size(text) > 0 && az_span_atou64(text, out) == AZ_OK;
}

/* Find "<key>=<value>" inside a query string (starting at '?') and hand back
 * the value as a span over the topic itself. Returns false if absent.
 *
 * A span rather than a copy into scratch: the value is then parsed at its exact
 * length, so an over-long value is rejected as malformed instead of being
 * silently truncated into a different -- possibly live -- request id. */
static bool query_value(const char* qs, const char* key, az_span* out)
{
  if (!qs || !key)
  {
    return false;
  }
  size_t key_len = strlen(key);
  const char* p = qs;
  while (p && *p)
  {
    /* skip past '?' or '&' */
    if (*p == '?' || *p == '&')
    {
      p++;
    }
    if (strncmp(p, key, key_len) == 0 && p[key_len] == '=')
    {
      const char* val = p + key_len + 1;
      const char* end = strchr(val, '&');
      *out = span_over(val, end ? (size_t)(end - val) : strlen(val));
      return true;
    }
    p = strchr(p, '&');
  }
  return false;
}

/* Read the "<status>" component of a response topic, which runs from the end of
 * the response prefix to the '/' or '?' that follows it.
 *
 * Parsed at its exact length rather than accumulated digit by digit: an inbound
 * topic is untrusted text, and a long digit run multiplied into a signed int is
 * undefined behaviour once it overflows. A value that does not fit is reported
 * as unreadable, the same as a non-numeric one. */
static bool parse_status(const char* topic, const char* qmark, int* out_status)
{
  const char* status_str = topic + sizeof(TWIN_RESPONSE_PREFIX) - 1;
  const char* end = status_str;
  while (end < qmark && *end != '/')
  {
    end++;
  }

  uint32_t value = 0;
  if (!span_to_u32(span_over(status_str, (size_t)(end - status_str)), &value)
      || value > (uint32_t)INT_MAX)
  {
    return false;
  }
  *out_status = (int)value;
  return true;
}

/* Map the service status on a twin response onto an az_iot result.
 *
 * The documented set is 200 (GET success), 204 (PATCH success), 400 (malformed
 * reported-properties JSON), 429 (throttled) and 5xx (server errors). Each one
 * the caller can act on differently gets its own code:
 *
 *   400 -> INVALID_ARG   the patch this device sent was not accepted; resending
 *                        it unchanged will fail again.
 *   404 -> NOT_FOUND     undocumented for twin but cheap to distinguish.
 *   429 -> BUSY          back off and retry. Deliberately NOT NOT_SUPPORTED,
 *                        which is what a full pending table returns -- a caller
 *                        must be able to tell "the service is throttling me"
 *                        from "I have too many requests in flight locally".
 *   else -> MQTT         server errors and anything unrecognised. */
static az_iot_result status_to_result(int status)
{
  if (status >= TWIN_STATUS_SUCCESS_MIN && status < TWIN_STATUS_SUCCESS_LIMIT)
  {
    return AZ_IOT_OK;
  }
  if (status == TWIN_STATUS_BAD_REQUEST)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (status == TWIN_STATUS_NOT_FOUND)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  if (status == TWIN_STATUS_THROTTLED)
  {
    return AZ_IOT_ERR_BUSY;
  }
  return AZ_IOT_ERR_MQTT;
}

/* ------------------------------------------------------------------------- */
/* completing a request                                                      */
/* ------------------------------------------------------------------------- */

/* Hand @p request's outcome to its callback; the request is already released, so a callback
 * that re-issues it can take its entry.
 *
 * The GET payload is withheld on failure: az_iot_twin_get_callback documents twin_payload as NULL
 * when status is not AZ_IOT_OK, and a rejected GET carries a service error description rather
 * than a twin document. A patch version only means something when the update was applied. */
static void report_twin_request(
    const az_iot_mqttv3_twin_request* request,
    az_iot_result status,
    const uint8_t* payload,
    size_t payload_len,
    uint64_t version)
{
  bool const ok = (status == AZ_IOT_OK);
  if (request->kind == TWIN_PENDING_GET && request->cb.get_cb)
  {
    request->cb.get_cb(status, ok ? payload : NULL, ok ? payload_len : 0, request->user_ctx);
  }
  else if (request->kind == TWIN_PENDING_PATCH && request->cb.patch_cb)
  {
    request->cb.patch_cb(status, ok ? version : 0, request->user_ctx);
  }
}

/* ------------------------------------------------------------------------- */
/* dispatch handlers                                                         */
/* ------------------------------------------------------------------------- */

static void on_twin_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_mqttv3_twin_client* t = (az_iot_mqttv3_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic)
  {
    return;
  }

  /* Topic: "$iothub/twin/res/<status>/?$rid=<n>[&$version=<v>]". */
  if (strncmp(msg->topic, TWIN_RESPONSE_PREFIX, sizeof(TWIN_RESPONSE_PREFIX) - 1) != 0)
  {
    return;
  }
  const char* qmark = strchr(msg->topic, '?');
  if (!qmark)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV3_TWIN,
        "dropping a response topic with no query string: %s",
        msg->topic);
    return;
  }

  int status = 0;
  if (!parse_status(msg->topic, qmark, &status))
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV3_TWIN,
        "dropping a response with an unreadable status: %s",
        msg->topic);
    return;
  }

  az_span rid_text;
  uint32_t rid = 0;
  if (!query_value(qmark, TWIN_QUERY_RID, &rid_text) || !span_to_u32(rid_text, &rid))
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV3_TWIN,
        "dropping a response whose request id is missing or not a number: %s",
        msg->topic);
    return;
  }

  /* A reported-properties acknowledgement carries the new version of that
   * section: "$iothub/twin/res/204/?$rid=1&$version=6". An application that
   * tracks it can tell an applied update from a lost one. */
  uint64_t version = 0;
  az_span version_text;
  if (query_value(qmark, TWIN_QUERY_VERSION, &version_text))
  {
    (void)span_to_u64(version_text, &version);
  }

  int idx = find_request_by_rid(t, rid);
  if (idx < 0)
  {
    return; /* stale or unknown rid */
  }

  /* Answered: released before its callback, so the callback may make a new request. */
  az_iot_mqttv3_twin_request const request = TI(t).requests[idx];
  memset(&TI(t).requests[idx], 0, sizeof(TI(t).requests[idx]));
  az_iot_delivery_tracker__cancel(&TI(t).tracker, (size_t)idx);
  report_twin_request(&request, status_to_result(status), msg->payload, msg->payload_len, version);
}

static void on_twin_desired(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_mqttv3_twin_client* t = (az_iot_mqttv3_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic || !TI(t).desired_handler)
  {
    return;
  }

  /* Topic: "$iothub/twin/PATCH/properties/desired/?$version=<v>". */
  uint64_t version = 0;
  const char* qmark = strchr(msg->topic, '?');
  if (qmark)
  {
    az_span version_text;
    if (query_value(qmark, TWIN_QUERY_VERSION, &version_text))
    {
      (void)span_to_u64(version_text, &version);
    }
  }
  TI(t).desired_handler(msg->payload, msg->payload_len, version, TI(t).desired_handler_ctx);
}

/* ------------------------------------------------------------------------- */
/* request plumbing                                                          */
/* ------------------------------------------------------------------------- */

/* Mint the next request id, skipping 0 so it never collides with the value an
 * unparsable response decodes to. */
static uint32_t next_rid(az_iot_mqttv3_twin_client* t)
{
  uint32_t rid = TI(t).next_rid++;
  if (TI(t).next_rid == 0)
  {
    TI(t).next_rid = 1;
  }
  return rid;
}

/* Withdraw everything _init() registered. Shared by the partial-init unwind and
 * _deinit() so the two can never drift apart. */
static void withdraw_registrations(
    az_iot_connection_client* conn,
    az_iot_mqttv3_twin_client* client)
{
  az_iot_delivery_tracker__unbind(&TI(client).tracker);
  (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
}

/* Build a request topic carrying the request id. */
static az_iot_result build_request_topic(
    const char* prefix,
    uint32_t rid,
    char (*topic)[AZ_IOT_MQTTV3_TWIN_TOPIC_MAX])
{
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(*topic));
  az_iot_span_writer_append_str(&writer, prefix);
  az_iot_span_writer_append_u32(&writer, rid);
  if (az_iot_span_writer_end_str(&writer, NULL) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV3_TWIN,
        "'%s<rid>' did not fit AZ_IOT_MQTTV3_TWIN_TOPIC_MAX bytes",
        prefix);
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_OK;
}

/* The request tracker entry @p entry_index holds, or NULL. */
static az_iot_mqttv3_twin_request* request_of_entry(
    az_iot_mqttv3_twin_client* t,
    size_t entry_index)
{
  if (entry_index >= AZ_IOT_TWIN_MAX_PENDING
      || TI(t).requests[entry_index].kind == TWIN_PENDING_NONE)
  {
    return NULL;
  }
  return &TI(t).requests[entry_index];
}

/* Tracker encode(): the request tracker entry @p entry_index holds, built at each send. Any
 * error discards it. */
static az_iot_result encode_twin_request(void* owner, size_t entry_index, az_iot_mqtt_message* out)
{
  az_iot_mqttv3_twin_client* t = (az_iot_mqttv3_twin_client*)owner;
  const az_iot_mqttv3_twin_request* request = request_of_entry(t, entry_index);
  if (request == NULL)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  bool const patch = request->kind == TWIN_PENDING_PATCH;
  /* The patch is the caller's only during the patch call. */
  if (patch && (!TI(t).submit_in_progress || entry_index != TI(t).submitting_entry))
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  az_iot_result const r = build_request_topic(
      patch ? TWIN_PATCH_TOPIC_PREFIX : TWIN_GET_TOPIC_PREFIX, request->rid, &TI(t).outbound_topic);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  memset(out, 0, sizeof(*out));
  out->topic = TI(t).outbound_topic;
  out->payload = patch ? request->patch : NULL;
  out->payload_len = patch ? request->patch_len : 0;
  out->qos = AZ_IOT_MQTT_QOS_0;
  return AZ_IOT_OK;
}

/* Tracker on_sent(): a reported patch must be sent within its call. */
static void note_twin_request_sent(void* owner, size_t entry_index)
{
  az_iot_mqttv3_twin_request* request
      = request_of_entry((az_iot_mqttv3_twin_client*)owner, entry_index);
  if (request != NULL)
  {
    request->sent = true;
  }
}

/* Tracker on_complete(): the request ended unanswered. Expired is AZ_IOT_ERR_TIMEOUT; a lost
 * session or a request that cannot be sent is AZ_IOT_ERR_NOT_CONNECTED. The one being submitted
 * is reported by the submitting call instead. */
static void complete_twin_request(
    void* owner,
    size_t entry_index,
    az_iot_delivery_tracker_outcome outcome,
    const az_iot_publish_result* detail)
{
  (void)detail;
  az_iot_mqttv3_twin_client* t = (az_iot_mqttv3_twin_client*)owner;
  az_iot_mqttv3_twin_request* held = request_of_entry(t, entry_index);
  if (held == NULL)
  {
    return;
  }
  az_iot_mqttv3_twin_request const request = *held;
  memset(held, 0, sizeof(*held));
  bool const expired = outcome == AZ_IOT_DELIVERY_TRACKER_OUTCOME_EXPIRED;
  if (TI(t).submit_in_progress && entry_index == TI(t).submitting_entry)
  {
    TI(t).submit_failure = expired ? AZ_IOT_ERR_TIMEOUT : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    return;
  }
  report_twin_request(
      &request, expired ? AZ_IOT_ERR_TIMEOUT : AZ_IOT_ERR_NOT_CONNECTED, NULL, 0, 0);
}

static const az_iot_delivery_tracker_profile k_twin_get_profile = {
  .type = "twin GET",
  .confirmation = AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_RESPONSE,
  .on_session_loss = AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_RESEND,
  .priority = 0,
  .send_untracked_without_slot = false,
  .encode = encode_twin_request,
  .on_complete = complete_twin_request,
  .on_sent = note_twin_request_sent,
};

/* Not resent: the service may have applied it. */
static const az_iot_delivery_tracker_profile k_twin_reported_patch_profile = {
  .type = "twin reported patch",
  .confirmation = AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_RESPONSE,
  .on_session_loss = AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_FAIL,
  .priority = 0,
  .send_untracked_without_slot = false,
  .encode = encode_twin_request,
  .on_complete = complete_twin_request,
  .on_sent = note_twin_request_sent,
};

/* Track @p request under a fresh request id in a free entry. A reported patch must be sent now:
 * one still queued is withdrawn. Returns as az_iot_mqttv3_twin_client_patch_reported(). */
static az_iot_result submit_twin_request(
    az_iot_mqttv3_twin_client* t,
    const az_iot_mqttv3_twin_request* request)
{
  if (TI(t).submit_in_progress)
  {
    return AZ_IOT_ERR_BUSY;
  }
  size_t entry = 0;
  while (entry < AZ_IOT_TWIN_MAX_PENDING && TI(t).requests[entry].kind != TWIN_PENDING_NONE)
  {
    ++entry;
  }
  if (entry == AZ_IOT_TWIN_MAX_PENDING)
  {
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_MQTTV3_TWIN,
        "refusing a request -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  az_iot_mqttv3_twin_request* held = &TI(t).requests[entry];
  *held = *request;
  held->rid = next_rid(t);
  uint8_t correlation_id[sizeof(held->rid)];
  memcpy(correlation_id, &held->rid, sizeof(correlation_id));
  TI(t).submitting_entry = entry;
  TI(t).submit_failure = AZ_IOT_OK;
  TI(t).submit_in_progress = true;
  az_iot_result const r = az_iot_delivery_tracker__submit_at(
      &TI(t).tracker,
      entry,
      request->kind == TWIN_PENDING_PATCH ? &k_twin_reported_patch_profile : &k_twin_get_profile,
      correlation_id,
      sizeof(correlation_id),
      TI(t).request_timeout_ms);
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
  held->patch = NULL;
  held->patch_len = 0;
  if (request->kind == TWIN_PENDING_PATCH && !held->sent)
  {
    memset(held, 0, sizeof(*held));
    az_iot_delivery_tracker__cancel(&TI(t).tracker, entry);
    return az_iot_connection_client__is_connected(TI(t).conn) ? AZ_IOT_ERR_BUSY
                                                              : AZ_IOT_ERR_NOT_CONNECTED;
  }
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */
/* ------------------------------------------------------------------------- */

AZ_NODISCARD az_iot_result
az_iot_mqttv3_twin_client_init(az_iot_mqttv3_twin_client* client, az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_result result
      = az_iot_connection_client__require_profile(conn, AZ_IOT_CONNECTION_PROFILE_MQTT_V3);
  if (result != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return result;
  }

  memset(client, 0, sizeof(*client));
  TI(client).conn = conn;
  TI(client).next_rid = 1;
  TI(client).request_timeout_ms = AZ_IOT_MQTTV3_TWIN_REQUEST_TIMEOUT_MS_DEFAULT;

  result = az_iot_connection_client__register_inbound_handler(
      conn, TWIN_RESPONSE_PREFIX, on_twin_response, client);
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__register_inbound_handler(
        conn, TWIN_DESIRED_PREFIX, on_twin_desired, client);
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__add_subscription_on_connect(
        conn,
        TWIN_RESPONSE_PREFIX "#",
        AZ_IOT_MQTT_QOS_0,
        client,
        AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
        NULL);
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__add_subscription_on_connect(
        conn,
        TWIN_DESIRED_PREFIX "#",
        AZ_IOT_MQTT_QOS_0,
        client,
        AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
        NULL);
  }
  /* Requests go through the tracker: a GET is sent again after a lost session, a patch fails,
   * and both time out. Bound last: it must not outlive a failed init. */
  if (result == AZ_IOT_OK)
  {
    result = az_iot_delivery_tracker__init(
        &TI(client).tracker,
        TI(client).tracker_entries,
        AZ_IOT_TWIN_MAX_PENDING,
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
        AZ_IOT_LOG_COMPONENT_MQTTV3_TWIN,
        "init failed (%s); withdrawing partial registrations",
        az_iot_result_to_string(result));
    withdraw_registrations(conn, client);
    az_iot_connection_client__release_profile(conn);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_mqttv3_twin_client_deinit(az_iot_mqttv3_twin_client* client)
{
  if (!client || !TI(client).conn)
  {
    return;
  }
  withdraw_registrations(TI(client).conn, client);
  az_iot_connection_client__release_profile(TI(client).conn);
  memset(client, 0, sizeof(*client));
}

AZ_NODISCARD az_iot_result az_iot_mqttv3_twin_client_get(
    az_iot_mqttv3_twin_client* twin,
    az_iot_twin_get_callback cb,
    void* user_ctx)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_mqttv3_twin_request request;
  memset(&request, 0, sizeof(request));
  request.kind = TWIN_PENDING_GET;
  request.cb.get_cb = cb;
  request.user_ctx = user_ctx;
  return submit_twin_request(twin, &request);
}

AZ_NODISCARD az_iot_result az_iot_mqttv3_twin_client_patch_reported(
    az_iot_mqttv3_twin_client* twin,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_twin_patch_complete_callback cb,
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
  az_iot_mqttv3_twin_request request;
  memset(&request, 0, sizeof(request));
  request.kind = TWIN_PENDING_PATCH;
  request.patch = patch;
  request.patch_len = patch_len;
  request.cb.patch_cb = cb;
  request.user_ctx = user_ctx;
  return submit_twin_request(twin, &request);
}

az_iot_result az_iot_mqttv3_twin_client_set_request_timeout(
    az_iot_mqttv3_twin_client* twin,
    uint32_t timeout_ms)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  TI(twin).request_timeout_ms
      = timeout_ms ? timeout_ms : AZ_IOT_MQTTV3_TWIN_REQUEST_TIMEOUT_MS_DEFAULT;
  return AZ_IOT_OK;
}

az_iot_result az_iot_mqttv3_twin_client_set_desired_handler(
    az_iot_mqttv3_twin_client* twin,
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
