// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTT v5 twin client.
 *
 *   GET            : ih/{device_id}/srv/twin/get + correlation_data
 *   PATCH reported : ih/{device_id}/srv/twin/reported + correlation_data
 *   GET response   : ih/{device_id}/dev/twin/get/response + correlation_data
 *   Reported ack   : ih/{device_id}/dev/twin/reported/response + correlation_data
 *   Desired        : ih/{device_id}/dev/twin/desired
 *
 * Requests correlate on MQTT v5 correlation data carrying the decimal request
 * id, rather than the `$rid` Classic puts in the topic. No subscription of its
 * own: the presence handshake already holds "ih/{device_id}/dev/#", which
 * covers all three inbound topics.
 */
#include <stdbool.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/gen2/az_iot_twin_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_TWIN_TOPIC_MAX 192

/* Topic space. The inbound prefix is what the handlers measure a delivered
 * topic against; the suffixes are the exact leaves under it. */
#define TWIN_TOPIC_ROOT "ih/"
#define TWIN_INBOUND_PREFIX "/dev/twin/"
#define TWIN_INBOUND_GET_RESPONSE "get/response"
#define TWIN_INBOUND_REPORTED_RESPONSE "reported/response"
#define TWIN_INBOUND_DESIRED "desired"
#define TWIN_OUTBOUND_GET "/srv/twin/get"
#define TWIN_OUTBOUND_REPORTED "/srv/twin/reported"

/* The pending slot kind values stored in _internal.pending[].kind */
#define TWIN_PENDING_NONE 0
#define TWIN_PENDING_GET 1
#define TWIN_PENDING_PATCH 2

/* Longest decimal request id this client mints, plus the NUL. Matches the
 * AZ_IOT_*_BUF naming used for the connection client's scratch bounds. */
#define TWIN_RID_BUF 16

/* Internal shorthand to access _internal fields */
#define TI(t) ((t)->_internal)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Find the pending slot matching a given request-id. Returns slot index or -1. */
static int find_pending_by_rid(az_iot_gen2_twin_client* t, uint32_t rid)
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

/* Decode the decimal request id carried in correlation data.
 *
 * Parsed straight off the wire bytes at their exact length: az_span_atou32
 * rejects an embedded non-digit and anything past UINT32_MAX, so neither "1x"
 * nor an over-long run of digits can be truncated or wrapped into a live
 * request id. Returns false when the message carries no usable correlator. */
static bool rid_from_correlation_data(const az_iot_mqtt_message* msg, uint32_t* out_rid)
{
  if (!msg->correlation_data || msg->correlation_data_len == 0)
  {
    return false;
  }
  az_span text = az_span_create(
      (uint8_t*)(uintptr_t)msg->correlation_data, (int32_t)msg->correlation_data_len);
  return az_span_atou32(text, out_rid) == AZ_OK;
}

/* True when @p topic is exactly the inbound topic ending in @p suffix.
 *
 * Inbound handlers are registered by prefix and the dispatch table takes the
 * longest match, so a message published to a longer topic under the same root
 * -- ".../dev/twin/desired/extra" -- still arrives here. The protocol has three
 * exact leaves and nothing below them, so anything longer is not part of it.
 * prefix_len is 0 until bind_topics() runs, which rejects everything. */
static bool topic_is(az_iot_gen2_twin_client* t, const char* topic, const char* suffix)
{
  size_t prefix_len = TI(t).inbound_prefix_len;
  return prefix_len > 0 && strlen(topic) > prefix_len && strcmp(topic + prefix_len, suffix) == 0;
}

/* Mint the next request id, skipping 0 so it never collides with the value a
 * message carrying no correlation data decodes to. */
static uint32_t next_rid(az_iot_gen2_twin_client* t)
{
  uint32_t rid = TI(t).next_rid++;
  if (TI(t).next_rid == 0)
  {
    TI(t).next_rid = 1;
  }
  return rid;
}

/* ------------------------------------------------------------------------- */
/* completing a pending request                                              */
/* ------------------------------------------------------------------------- */

/* Release slot @p idx and hand the GET outcome to its callback.
 *
 * The slot is released BEFORE the callback runs so a callback that re-issues
 * its request immediately can claim it. */
static void invoke_get_cb(
    az_iot_gen2_twin_client* t,
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

/* Release slot @p idx and hand the patch outcome to its callback. */
static void invoke_patch_cb(
    az_iot_gen2_twin_client* t,
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
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
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

/* Resolve the pending slot a response belongs to, or -1.
 *
 * A slot whose kind does not match the topic the response arrived on is NOT
 * claimed and NOT released: correlation data is the only thing tying the two
 * together, so a get-response quoting a pending patch's id is a malformed
 * message, and cancelling that patch would drop its real acknowledgement and
 * leave the caller waiting forever. */
static int match_pending(az_iot_gen2_twin_client* t, const az_iot_mqtt_message* msg, int kind)
{
  uint32_t rid = 0;
  if (!rid_from_correlation_data(msg, &rid))
  {
    AZ_IOT_LOG_WARN("gen2_twin: dropping a response with missing or malformed correlation data");
    return -1;
  }

  int idx = find_pending_by_rid(t, rid);
  if (idx < 0)
  {
    return -1; /* stale or unknown request id */
  }
  if (TI(t).pending[idx].kind != kind)
  {
    AZ_IOT_LOG_WARNF(
        "gen2_twin: ignoring a response on the wrong topic for request id %u; leaving it pending",
        (unsigned)rid);
    return -1;
  }
  return idx;
}

/* Inbound on "ih/{device_id}/dev/twin/get/response". */
static void on_twin_get_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic || !topic_is(t, msg->topic, TWIN_INBOUND_GET_RESPONSE))
  {
    return;
  }

  int idx = match_pending(t, msg, TWIN_PENDING_GET);
  if (idx >= 0)
  {
    invoke_get_cb(t, idx, AZ_IOT_OK, msg->payload, msg->payload_len);
  }
}

/* Inbound on "ih/{device_id}/dev/twin/reported/response". */
static void on_twin_reported_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic || !topic_is(t, msg->topic, TWIN_INBOUND_REPORTED_RESPONSE))
  {
    return;
  }

  int idx = match_pending(t, msg, TWIN_PENDING_PATCH);
  if (idx >= 0)
  {
    /* This acknowledgement arrives on its own topic and does not carry a
     * reported version yet, so there is nothing to report but the status. */
    invoke_patch_cb(t, idx, AZ_IOT_OK, 0);
  }
}

/* Inbound on "ih/{device_id}/dev/twin/desired". */
static void on_twin_desired(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic || !topic_is(t, msg->topic, TWIN_INBOUND_DESIRED))
  {
    return;
  }
  if (!TI(t).desired_handler)
  {
    return;
  }

  /* The version is not carried on this path yet, so report 0 -- the same value
   * Classic reports when the service sends no $version. */
  TI(t).desired_handler(msg->payload, msg->payload_len, 0, TI(t).desired_handler_ctx);
}

/* ------------------------------------------------------------------------- */
/* connect-time topic binding                                                */
/* ------------------------------------------------------------------------- */

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_gen2_twin_client* client = (az_iot_gen2_twin_client*)owner;

  const char* device_id = az_iot_connection_client__device_id(conn);
  if (!device_id)
  {
    AZ_IOT_LOG_ERROR("gen2_twin: cannot bind topics -- the connection has no assigned device id");
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  static const char* const k_suffixes[]
      = { TWIN_INBOUND_GET_RESPONSE, TWIN_INBOUND_REPORTED_RESPONSE, TWIN_INBOUND_DESIRED };
  static const az_iot_inbound_handler_callback k_handlers[]
      = { on_twin_get_response, on_twin_reported_response, on_twin_desired };

  char prefix[AZ_IOT_TWIN_TOPIC_MAX];
  const char* prefix_parts[] = { TWIN_TOPIC_ROOT, device_id, TWIN_INBOUND_PREFIX };
  size_t prefix_len = 0;
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), &prefix_len, prefix_parts, 3)
      != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("gen2_twin: the inbound topic prefix did not fit AZ_IOT_TWIN_TOPIC_MAX bytes");
    return AZ_IOT_ERR_INTERNAL;
  }
  TI(client).inbound_prefix_len = prefix_len;

  for (size_t i = 0; i < sizeof(k_suffixes) / sizeof(k_suffixes[0]); ++i)
  {
    char topic[AZ_IOT_TWIN_TOPIC_MAX];
    const char* parts[] = { prefix, k_suffixes[i] };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, parts, 2) != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERRORF(
          "gen2_twin: the '%s' topic did not fit AZ_IOT_TWIN_TOPIC_MAX bytes", k_suffixes[i]);
      return AZ_IOT_ERR_INTERNAL;
    }
    az_iot_result r
        = az_iot_connection_client__register_inbound_handler(conn, topic, k_handlers[i], client);
    if (r != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERRORF(
          "gen2_twin: could not register delivery for '%s' (%s)",
          k_suffixes[i],
          az_iot_result_to_string(r));
      return r;
    }
  }

  /* No SUBSCRIBEs on this path: the presence handshake already holds
   * ih/{device_id}/dev/#, which covers all three of these topics. */
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
  TI(client).next_rid = 1;

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

/* Build "ih/{device_id}/srv/twin/<suffix>" into @p topic. */
static az_iot_result build_request_topic(
    az_iot_gen2_twin_client* twin,
    const char* suffix,
    char (*topic)[AZ_IOT_TWIN_TOPIC_MAX])
{
  const char* device_id = az_iot_connection_client__device_id(TI(twin).conn);
  if (!device_id)
  {
    AZ_IOT_LOG_WARN("gen2_twin: refusing a request -- the device id is not assigned yet");
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  const char* parts[] = { TWIN_TOPIC_ROOT, device_id, suffix };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(*topic), NULL, parts, 3) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF("gen2_twin: the '%s' topic did not fit AZ_IOT_TWIN_TOPIC_MAX bytes", suffix);
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_OK;
}

/* Write the request id as the ASCII correlation data the service echoes back. */
static az_iot_result build_correlation_data(
    uint32_t rid,
    char (*buf)[TWIN_RID_BUF],
    size_t* out_len)
{
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(*buf));
  az_iot_span_writer_append_u32(&writer, rid);
  if (az_iot_span_writer_end_str(&writer, out_len) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("gen2_twin: the request id did not fit TWIN_RID_BUF bytes");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_OK;
}

/* QoS 1 acknowledgement for a twin request.
 *
 * A rejected PUBLISH does not strand the caller: the pending slot is completed
 * with AZ_IOT_ERR_NOT_CONNECTED when the session ends, which is also when a
 * rejected QoS 1 publish leaves the request unanswerable. Reported here so the
 * cause is visible rather than surfacing later as an unexplained timeout. */
static void on_publish_ack(az_iot_result status, void* user_ctx)
{
  const char* what = (const char*)user_ctx;
  if (status != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen2_twin: the service did not acknowledge a %s publish (%s); the request stays pending "
        "until it is answered or the session ends",
        what ? what : "twin",
        az_iot_result_to_string(status));
  }
}

az_iot_result az_iot_gen2_twin_client_get(
    az_iot_gen2_twin_client* twin,
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
    AZ_IOT_LOG_WARN("gen2_twin: refusing a GET -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  char topic[AZ_IOT_TWIN_TOPIC_MAX];
  az_iot_result r = build_request_topic(twin, TWIN_OUTBOUND_GET, &topic);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  uint32_t rid = next_rid(twin);
  char corr_buf[TWIN_RID_BUF];
  size_t corr_len = 0;
  r = build_correlation_data(rid, &corr_buf, &corr_len);
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
  out.qos = AZ_IOT_MQTT_QOS_1;
  out.correlation_data = (const uint8_t*)corr_buf;
  out.correlation_data_len = corr_len;

  r = az_iot_connection_client__publish(TI(twin).conn, &out, on_publish_ack, (void*)"GET");
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen2_twin: the GET publish was refused (%s); releasing its pending slot",
        az_iot_result_to_string(r));
    TI(twin).pending[idx].in_use = false;
    TI(twin).pending[idx].kind = TWIN_PENDING_NONE;
  }
  return r;
}

az_iot_result az_iot_gen2_twin_client_patch_reported(
    az_iot_gen2_twin_client* twin,
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
        "gen2_twin: refusing a patch -- AZ_IOT_TWIN_MAX_PENDING requests are in flight");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  char topic[AZ_IOT_TWIN_TOPIC_MAX];
  az_iot_result r = build_request_topic(twin, TWIN_OUTBOUND_REPORTED, &topic);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  uint32_t rid = next_rid(twin);
  char corr_buf[TWIN_RID_BUF];
  size_t corr_len = 0;
  r = build_correlation_data(rid, &corr_buf, &corr_len);
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
  out.qos = AZ_IOT_MQTT_QOS_1;
  out.correlation_data = (const uint8_t*)corr_buf;
  out.correlation_data_len = corr_len;

  r = az_iot_connection_client__publish(
      TI(twin).conn, &out, on_publish_ack, (void*)"reported-properties");
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen2_twin: the patch publish was refused (%s); releasing its pending slot",
        az_iot_result_to_string(r));
    TI(twin).pending[idx].in_use = false;
    TI(twin).pending[idx].kind = TWIN_PENDING_NONE;
  }
  return r;
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
