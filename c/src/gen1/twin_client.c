// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* IoT Hub Classic (MQTT v3.1.1) twin client.
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
#include "azure/iot/gen1/az_iot_twin_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_TWIN_TOPIC_MAX 192

#define TWIN_RESPONSE_PREFIX "$iothub/twin/res/"
#define TWIN_DESIRED_PREFIX "$iothub/twin/PATCH/properties/desired/"
#define TWIN_GET_TOPIC_PREFIX "$iothub/twin/GET/?$rid="
#define TWIN_PATCH_TOPIC_PREFIX "$iothub/twin/PATCH/properties/reported/?$rid="

/* Query-string keys carried on the inbound topics. */
#define TWIN_QUERY_RID "$rid"
#define TWIN_QUERY_VERSION "$version"

/* The pending slot kind values stored in _internal.pending[].kind */
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

/* Find the pending slot matching a given request-id. Returns slot index or -1. */
static int find_pending_by_rid(az_iot_gen1_twin_client* t, uint32_t rid)
{
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(t).pending[i].in_use && TI(t).pending[i].rid == rid)
    {
      return i;
    }
  }
  return -1;
}

/* Allocate an unused pending slot. Returns slot index or -1 if full. */
static int alloc_pending(az_iot_gen1_twin_client* t)
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
/* completing a pending request                                              */
/* ------------------------------------------------------------------------- */

/* Release slot @p idx and hand the GET outcome to its callback.
 *
 * The slot is released BEFORE the callback runs so a callback that re-issues
 * its request immediately can claim it.
 *
 * The payload is withheld on failure: az_iot_twin_get_callback documents
 * twin_payload as NULL when status is not AZ_IOT_OK, and a rejected GET carries
 * a service error description rather than a twin document. */
static void invoke_get_cb(
    az_iot_gen1_twin_client* t,
    int idx,
    az_iot_result status,
    const uint8_t* payload,
    size_t payload_len)
{
  az_iot_twin_get_callback cb = TI(t).pending[idx].cb.get_cb;
  void* ctx = TI(t).pending[idx].user_ctx;
  TI(t).pending[idx].in_use = false;
  TI(t).pending[idx].kind = TWIN_PENDING_NONE;
  if (cb)
  {
    bool ok = (status == AZ_IOT_OK);
    cb(status, ok ? payload : NULL, ok ? payload_len : 0, ctx);
  }
}

/* Release slot @p idx and hand the patch outcome to its callback.
 *
 * A version only means something when the update was applied. If the service
 * ever attached one to a non-2xx response, reporting it would let a caller
 * record a version for a patch that was rejected. */
static void invoke_patch_cb(
    az_iot_gen1_twin_client* t,
    int idx,
    az_iot_result status,
    uint64_t version)
{
  az_iot_twin_patch_complete_callback cb = TI(t).pending[idx].cb.patch_cb;
  void* ctx = TI(t).pending[idx].user_ctx;
  TI(t).pending[idx].in_use = false;
  TI(t).pending[idx].kind = TWIN_PENDING_NONE;
  if (cb)
  {
    cb(status, (status == AZ_IOT_OK) ? version : 0, ctx);
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
  az_iot_gen1_twin_client* t = (az_iot_gen1_twin_client*)user_ctx;
  if (!t)
  {
    return;
  }

  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (!TI(t).pending[i].in_use)
    {
      continue;
    }
    if (TI(t).pending[i].kind == TWIN_PENDING_GET)
    {
      invoke_get_cb(t, i, AZ_IOT_ERR_NOT_CONNECTED, NULL, 0);
    }
    else
    {
      invoke_patch_cb(t, i, AZ_IOT_ERR_NOT_CONNECTED, 0);
    }
  }
}

/* ------------------------------------------------------------------------- */
/* dispatch handlers                                                         */
/* ------------------------------------------------------------------------- */

static void on_twin_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen1_twin_client* t = (az_iot_gen1_twin_client*)user_ctx;
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
    AZ_IOT_LOG_WARNF("gen1_twin: dropping a response topic with no query string: %s", msg->topic);
    return;
  }

  int status = 0;
  if (!parse_status(msg->topic, qmark, &status))
  {
    AZ_IOT_LOG_WARNF("gen1_twin: dropping a response with an unreadable status: %s", msg->topic);
    return;
  }

  az_span rid_text;
  uint32_t rid = 0;
  if (!query_value(qmark, TWIN_QUERY_RID, &rid_text) || !span_to_u32(rid_text, &rid))
  {
    AZ_IOT_LOG_WARNF(
        "gen1_twin: dropping a response whose request id is missing or not a number: %s",
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

  int idx = find_pending_by_rid(t, rid);
  if (idx < 0)
  {
    return; /* stale or unknown rid */
  }

  az_iot_result r = status_to_result(status);
  if (TI(t).pending[idx].kind == TWIN_PENDING_GET)
  {
    invoke_get_cb(t, idx, r, msg->payload, msg->payload_len);
  }
  else
  {
    invoke_patch_cb(t, idx, r, version);
  }
}

static void on_twin_desired(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen1_twin_client* t = (az_iot_gen1_twin_client*)user_ctx;
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
static uint32_t next_rid(az_iot_gen1_twin_client* t)
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
static void withdraw_registrations(az_iot_connection_client* conn, az_iot_gen1_twin_client* client)
{
  (void)az_iot_connection_client__unregister_session_end_handler(conn, client);
  (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
}

/* Build a request topic carrying the request id. */
static az_iot_result build_request_topic(
    const char* prefix,
    uint32_t rid,
    char (*topic)[AZ_IOT_TWIN_TOPIC_MAX])
{
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(*topic));
  az_iot_span_writer_append_str(&writer, prefix);
  az_iot_span_writer_append_u32(&writer, rid);
  if (az_iot_span_writer_end_str(&writer, NULL) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF("gen1_twin: '%s<rid>' did not fit AZ_IOT_TWIN_TOPIC_MAX bytes", prefix);
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_gen1_twin_client_init(
    az_iot_gen1_twin_client* client,
    az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_result result
      = az_iot_connection_client__require_profile(conn, AZ_IOT_CONNECTION_PROFILE_CLASSIC);
  if (result != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return result;
  }

  memset(client, 0, sizeof(*client));
  TI(client).conn = conn;
  TI(client).next_rid = 1;

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
        "gen1_twin: init failed (%s); withdrawing partial registrations",
        az_iot_result_to_string(result));
    withdraw_registrations(conn, client);
    az_iot_connection_client__release_profile(conn);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_gen1_twin_client_deinit(az_iot_gen1_twin_client* client)
{
  if (!client || !TI(client).conn)
  {
    return;
  }
  withdraw_registrations(TI(client).conn, client);
  az_iot_connection_client__release_profile(TI(client).conn);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_gen1_twin_client_get(
    az_iot_gen1_twin_client* twin,
    az_iot_twin_get_callback cb,
    void* user_ctx)
{
  if (!twin)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  int idx = alloc_pending(twin);
  if (idx < 0)
  {
    AZ_IOT_LOG_WARN("gen1_twin: refusing a GET -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  uint32_t rid = next_rid(twin);

  char topic[AZ_IOT_TWIN_TOPIC_MAX];
  az_iot_result r = build_request_topic(TWIN_GET_TOPIC_PREFIX, rid, &topic);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* Reserve the slot before publish. */
  TI(twin).pending[idx].in_use = true;
  TI(twin).pending[idx].rid = rid;
  TI(twin).pending[idx].kind = TWIN_PENDING_GET;
  TI(twin).pending[idx].cb.get_cb = cb;
  TI(twin).pending[idx].user_ctx = user_ctx;

  az_iot_mqtt_message out = { 0 };
  out.topic = topic;
  out.qos = AZ_IOT_MQTT_QOS_0;

  r = az_iot_connection_client__publish(TI(twin).conn, &out, NULL, NULL);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen1_twin: the GET publish was refused (%s); releasing its pending slot",
        az_iot_result_to_string(r));
    TI(twin).pending[idx].in_use = false;
    TI(twin).pending[idx].kind = TWIN_PENDING_NONE;
  }
  return r;
}

az_iot_result az_iot_gen1_twin_client_patch_reported(
    az_iot_gen1_twin_client* twin,
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

  int idx = alloc_pending(twin);
  if (idx < 0)
  {
    AZ_IOT_LOG_WARN(
        "gen1_twin: refusing a patch -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  uint32_t rid = next_rid(twin);

  char topic[AZ_IOT_TWIN_TOPIC_MAX];
  az_iot_result r = build_request_topic(TWIN_PATCH_TOPIC_PREFIX, rid, &topic);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  TI(twin).pending[idx].in_use = true;
  TI(twin).pending[idx].rid = rid;
  TI(twin).pending[idx].kind = TWIN_PENDING_PATCH;
  TI(twin).pending[idx].cb.patch_cb = cb;
  TI(twin).pending[idx].user_ctx = user_ctx;

  az_iot_mqtt_message out = { 0 };
  out.topic = topic;
  out.payload = patch;
  out.payload_len = patch_len;
  out.qos = AZ_IOT_MQTT_QOS_0;

  r = az_iot_connection_client__publish(TI(twin).conn, &out, NULL, NULL);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen1_twin: the patch publish was refused (%s); releasing its pending slot",
        az_iot_result_to_string(r));
    TI(twin).pending[idx].in_use = false;
    TI(twin).pending[idx].kind = TWIN_PENDING_NONE;
  }
  return r;
}

az_iot_result az_iot_gen1_twin_client_set_desired_handler(
    az_iot_gen1_twin_client* twin,
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
