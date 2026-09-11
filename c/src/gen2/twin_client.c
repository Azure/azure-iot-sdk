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
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/gen2/az_iot_twin_client.h"

#include "internal/connection_client_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_TWIN_TOPIC_MAX 192

/* The pending slot kind values stored in _internal.pending[].kind */
#define TWIN_PENDING_NONE 0
#define TWIN_PENDING_GET 1
#define TWIN_PENDING_PATCH 2

/* Longest decimal request id this client mints, plus the NUL. */
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

/* Decode the decimal request id carried in correlation data. Returns 0 when the
 * message carries none, which matches no slot because this client never mints
 * request id 0. */
static uint32_t rid_from_correlation_data(const az_iot_mqtt_message* msg)
{
  if (!msg->correlation_data || msg->correlation_data_len == 0)
  {
    return 0;
  }
  char rid_buf[TWIN_RID_BUF];
  size_t n = msg->correlation_data_len < sizeof(rid_buf) - 1 ? msg->correlation_data_len
                                                             : sizeof(rid_buf) - 1;
  memcpy(rid_buf, msg->correlation_data, n);
  rid_buf[n] = '\0';
  return (uint32_t)strtoul(rid_buf, NULL, 10);
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

/* Complete every pending request with an error and release its slot.
 *
 * Called when the MQTT session ends. The responses these requests were waiting
 * for would have arrived on that session, so they can never come now; holding
 * the slots would leak one per outage until AZ_IOT_TWIN_MAX_PENDING is gone and
 * the client refuses every further request.
 *
 * The slot is released BEFORE the callback runs so a callback that re-issues
 * its request immediately can claim it. */
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

    int kind = TI(t).pending[i].kind;
    az_iot_twin_get_callback get_cb = TI(t).pending[i].cb.get_cb;
    az_iot_twin_patch_ack_callback patch_cb = TI(t).pending[i].cb.patch_cb;
    void* ctx = TI(t).pending[i].user_ctx;

    TI(t).pending[i].in_use = false;
    TI(t).pending[i].kind = TWIN_PENDING_NONE;

    if (kind == TWIN_PENDING_GET && get_cb)
    {
      get_cb(AZ_IOT_ERR_NOT_CONNECTED, NULL, 0, ctx);
    }
    else if (kind == TWIN_PENDING_PATCH && patch_cb)
    {
      patch_cb(AZ_IOT_ERR_NOT_CONNECTED, 0, ctx);
    }
  }
}

/* ------------------------------------------------------------------------- */
/* dispatch handlers                                                         */
/* ------------------------------------------------------------------------- */

/* Inbound on "ih/{device_id}/dev/twin/get/response". */
static void on_twin_get_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t || !msg)
  {
    return;
  }

  int idx = find_pending_by_rid(t, rid_from_correlation_data(msg));
  if (idx < 0)
  {
    return; /* stale or unknown request id */
  }

  if (TI(t).pending[idx].kind == TWIN_PENDING_GET)
  {
    az_iot_twin_get_callback cb = TI(t).pending[idx].cb.get_cb;
    void* ctx = TI(t).pending[idx].user_ctx;
    TI(t).pending[idx].in_use = false;
    TI(t).pending[idx].kind = TWIN_PENDING_NONE;
    if (cb)
    {
      cb(AZ_IOT_OK, msg->payload, msg->payload_len, ctx);
    }
  }
  else
  {
    TI(t).pending[idx].in_use = false;
  }
}

/* Inbound on "ih/{device_id}/dev/twin/reported/response". */
static void on_twin_reported_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t || !msg)
  {
    return;
  }

  int idx = find_pending_by_rid(t, rid_from_correlation_data(msg));
  if (idx < 0)
  {
    return;
  }

  if (TI(t).pending[idx].kind == TWIN_PENDING_PATCH)
  {
    az_iot_twin_patch_ack_callback cb = TI(t).pending[idx].cb.patch_cb;
    void* ctx = TI(t).pending[idx].user_ctx;
    TI(t).pending[idx].in_use = false;
    TI(t).pending[idx].kind = TWIN_PENDING_NONE;
    if (cb)
    {
      /* This acknowledgement arrives on its own topic and does not carry a
       * reported version yet, so there is nothing to report but the status. */
      cb(AZ_IOT_OK, 0, ctx);
    }
  }
  else
  {
    TI(t).pending[idx].in_use = false;
  }
}

/* Inbound on "ih/{device_id}/dev/twin/desired". */
static void on_twin_desired(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_twin_client* t = (az_iot_gen2_twin_client*)user_ctx;
  if (!t || !msg || !TI(t).desired_handler)
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
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  /* Exact topics: there is no wildcard on any of these paths. */
  static const char* const k_suffixes[]
      = { "/dev/twin/get/response", "/dev/twin/reported/response", "/dev/twin/desired" };
  static const az_iot_inbound_handler_callback k_handlers[]
      = { on_twin_get_response, on_twin_reported_response, on_twin_desired };

  for (size_t i = 0; i < sizeof(k_suffixes) / sizeof(k_suffixes[0]); ++i)
  {
    char topic[AZ_IOT_TWIN_TOPIC_MAX];
    const char* parts[] = { "ih/", device_id, k_suffixes[i] };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, parts, 3) != AZ_IOT_OK)
    {
      return AZ_IOT_ERR_INTERNAL;
    }
    az_iot_result r
        = az_iot_connection_client__register_inbound_handler(conn, topic, k_handlers[i], client);
    if (r != AZ_IOT_OK)
    {
      return r;
    }
  }

  /* No SUBSCRIBEs on this path: the presence handshake already holds
   * ih/{device_id}/dev/#, which covers all three of these topics. */
  return AZ_IOT_OK;
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
    az_iot_connection_client__unregister_feature_client_bind(conn, client);
    (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
    (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
    az_iot_connection_client__release_profile(conn);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_gen2_twin_client_destroy(az_iot_gen2_twin_client* client)
{
  if (!client)
  {
    return;
  }
  (void)az_iot_connection_client__unregister_session_end_handler(TI(client).conn, client);
  az_iot_connection_client__unregister_feature_client_bind(TI(client).conn, client);
  az_iot_connection_client__release_profile(TI(client).conn);
  (void)az_iot_connection_client__remove_subscriptions_for(TI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(TI(client).conn, client);
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
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  const char* parts[] = { "ih/", device_id, suffix };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(*topic), NULL, parts, 3) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_OK;
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
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  char topic[AZ_IOT_TWIN_TOPIC_MAX];
  az_iot_result r = build_request_topic(twin, "/srv/twin/get", &topic);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  uint32_t rid = next_rid(twin);

  /* Correlation data: the request id as ASCII. */
  char corr_buf[TWIN_RID_BUF];
  size_t corr_len = 0;
  az_iot_span_writer corr_writer;
  az_iot_span_writer_init(&corr_writer, AZ_SPAN_FROM_BUFFER(corr_buf));
  az_iot_span_writer_append_u32(&corr_writer, rid);
  if (az_iot_span_writer_end_str(&corr_writer, &corr_len) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
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

  r = az_iot_connection_client__publish(TI(twin).conn, &out, NULL, NULL);
  if (r != AZ_IOT_OK)
  {
    TI(twin).pending[idx].in_use = false;
    TI(twin).pending[idx].kind = TWIN_PENDING_NONE;
  }
  return r;
}

az_iot_result az_iot_gen2_twin_client_patch_reported(
    az_iot_gen2_twin_client* twin,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_twin_patch_ack_callback cb,
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
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  char topic[AZ_IOT_TWIN_TOPIC_MAX];
  az_iot_result r = build_request_topic(twin, "/srv/twin/reported", &topic);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  uint32_t rid = next_rid(twin);

  char corr_buf[TWIN_RID_BUF];
  size_t corr_len = 0;
  az_iot_span_writer corr_writer;
  az_iot_span_writer_init(&corr_writer, AZ_SPAN_FROM_BUFFER(corr_buf));
  az_iot_span_writer_append_u32(&corr_writer, rid);
  if (az_iot_span_writer_end_str(&corr_writer, &corr_len) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
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

  r = az_iot_connection_client__publish(TI(twin).conn, &out, NULL, NULL);
  if (r != AZ_IOT_OK)
  {
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
