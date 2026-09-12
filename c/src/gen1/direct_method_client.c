// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* IoT Hub Classic direct methods (MQTT v3.1.1).
 *
 *   Subscribe  "$iothub/methods/POST/#"
 *   Inbound    "$iothub/methods/POST/{methodName}/?$rid={rid}"
 *   Respond    "$iothub/methods/res/{status}/?$rid={rid}"
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/gen1/az_iot_direct_method_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/reconnect.h"
#include "internal/span_writer.h"

#define AZ_IOT_GEN1_DM_TOPIC_MAX 192
#define METHODS_REQUEST_PREFIX "$iothub/methods/POST/"
#define DM_MS_PER_SECOND 1000u

#define DI(d) ((d)->_internal)

/* Release every in-flight slot whose response deadline has passed.
 *
 * Without this an application that drops a request -- a handler that returns
 * without responding, including on its own error paths -- would hold that slot
 * for the life of the client, and once AZ_IOT_DM_MAX_INFLIGHT had leaked every
 * further invocation would be dropped.
 *
 * Feature clients get no periodic tick, so this runs whenever a method message
 * arrives, which is exactly when the capacity it frees is about to be needed.
 * Nothing goes on the wire: Classic has no abandon message, and by here the
 * service has stopped waiting for an answer anyway. */
static void requests_expire_stale(az_iot_gen1_direct_method_client* dm)
{
  uint64_t now = az_iot_time_mono_ms();
  for (size_t i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    az_iot_direct_method_request* r = &DI(dm).req_pool[i];
    if (!r->_internal.in_use || now < DI(dm).req_deadline_ms[i])
    {
      continue;
    }
    AZ_IOT_LOG_WARNF(
        "gen1_direct_method: '%s' was never answered within %u seconds; reclaiming its slot",
        r->_internal.method_name,
        (unsigned)DI(dm).response_deadline_seconds);
    r->_internal.in_use = false;
  }
}

/* Acquire a free slot from the bounded pool (NULL if full).
 *
 * A slot is returned by az_iot_gen1_direct_method_respond(), or reclaimed by
 * requests_expire_stale() once the invocation can no longer be answered
 * usefully. Running out means this many invocations arrived while earlier ones
 * were still legitimately in flight. */
static az_iot_direct_method_request* request_acquire(az_iot_gen1_direct_method_client* dm)
{
  for (size_t n = 0; n < AZ_IOT_DM_MAX_INFLIGHT; ++n)
  {
    size_t i = (DI(dm).next_slot + n) % AZ_IOT_DM_MAX_INFLIGHT;
    az_iot_direct_method_request* r = &DI(dm).req_pool[i];
    if (!r->_internal.in_use)
    {
      memset(r, 0, sizeof(*r));
      r->_internal.in_use = true;
      r->_internal.owner = dm;
      r->_internal.profile = AZ_IOT_CONNECTION_PROFILE_CLASSIC;
      DI(dm).req_deadline_ms[i]
          = az_iot_time_mono_ms() + ((uint64_t)DI(dm).response_deadline_seconds * DM_MS_PER_SECOND);
      DI(dm).next_slot = (i + 1u) % AZ_IOT_DM_MAX_INFLIGHT;
      return r;
    }
  }
  AZ_IOT_LOG_WARNF(
      "gen1_direct_method: dropping an invocation, all %d in-flight slots are taken by requests "
      "still inside their %u second response deadline. Answer them with "
      "az_iot_gen1_direct_method_respond(), or raise AZ_IOT_DM_MAX_INFLIGHT.",
      (int)AZ_IOT_DM_MAX_INFLIGHT,
      (unsigned)DI(dm).response_deadline_seconds);
  return NULL;
}

/* Parse "$iothub/methods/POST/<methodName>/?$rid=<rid>". */
static bool parse_method_topic(
    const char* topic,
    char* out_method,
    size_t method_cap,
    char* out_rid,
    size_t rid_cap)
{
  static const char k_prefix[] = METHODS_REQUEST_PREFIX;
  size_t prefix_len = sizeof(k_prefix) - 1;
  if (strncmp(topic, k_prefix, prefix_len) != 0)
  {
    return false;
  }
  const char* name = topic + prefix_len;
  const char* slash = strchr(name, '/');
  if (!slash || slash == name)
  {
    return false;
  }
  size_t name_len = (size_t)(slash - name);
  if (name_len + 1 > method_cap)
  {
    return false;
  }
  memcpy(out_method, name, name_len);
  out_method[name_len] = '\0';

  static const char k_rid_marker[] = "?$rid=";
  const char* rid_marker = strstr(slash, k_rid_marker);
  if (!rid_marker)
  {
    return false;
  }
  const char* rid_val = rid_marker + (sizeof(k_rid_marker) - 1);
  size_t rid_len = strlen(rid_val);
  if (rid_len == 0 || rid_len + 1 > rid_cap)
  {
    return false;
  }
  memcpy(out_rid, rid_val, rid_len);
  out_rid[rid_len] = '\0';
  return true;
}

static void on_method_invocation(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen1_direct_method_client* dm = (az_iot_gen1_direct_method_client*)user_ctx;
  if (!dm || !msg || !msg->topic || !DI(dm).handler)
  {
    return;
  }

  requests_expire_stale(dm);

  char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
  char rid[AZ_IOT_DM_RID_MAX];
  if (!parse_method_topic(msg->topic, method_name, sizeof(method_name), rid, sizeof(rid)))
  {
    AZ_IOT_LOG_WARNF("gen1_direct_method: dropping an unparsable request topic: %s", msg->topic);
    return;
  }

  az_iot_direct_method_request* req = request_acquire(dm);
  if (!req)
  {
    return;
  }
  memcpy(req->_internal.rid, rid, strlen(rid) + 1);
  memcpy(req->_internal.method_name, method_name, strlen(method_name) + 1);

  DI(dm).handler(req, method_name, msg->payload, msg->payload_len, DI(dm).handler_ctx);
}

az_iot_result az_iot_gen1_direct_method_client_init(
    az_iot_gen1_direct_method_client* client,
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
  DI(client).conn = conn;
  DI(client).response_deadline_seconds = AZ_IOT_GEN1_DM_RESPONSE_DEADLINE_SECONDS;

  /* No connect-time bind: unlike C2D, these topics carry no device id, so they
   * are known before the connection resolves. */
  result = az_iot_connection_client__register_inbound_handler(
      conn, METHODS_REQUEST_PREFIX, on_method_invocation, client);
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__add_subscription_on_connect(
        conn,
        METHODS_REQUEST_PREFIX "#",
        AZ_IOT_MQTT_QOS_0,
        client,
        AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
        NULL);
  }
  if (result != AZ_IOT_OK)
  {
    (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
    (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
    az_iot_connection_client__release_profile(conn);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_gen1_direct_method_client_destroy(az_iot_gen1_direct_method_client* client)
{
  if (!client)
  {
    return;
  }
  az_iot_connection_client__release_profile(DI(client).conn);
  (void)az_iot_connection_client__remove_subscriptions_for(DI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(DI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_gen1_direct_method_client_set_handler(
    az_iot_gen1_direct_method_client* client,
    az_iot_direct_method_handler_callback cb,
    void* user_ctx)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  DI(client).handler = cb;
  DI(client).handler_ctx = user_ctx;
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen1_direct_method_client_set_response_deadline(
    az_iot_gen1_direct_method_client* client,
    uint32_t seconds)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  DI(client).response_deadline_seconds
      = (seconds == 0u) ? AZ_IOT_GEN1_DM_RESPONSE_DEADLINE_SECONDS : seconds;
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen1_direct_method_respond(
    az_iot_direct_method_request* request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len)
{
  if (request == NULL || (payload_len > 0 && payload == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Responding twice used to publish a second answer on a slot that had already
   * been handed to another invocation, so the reply carried that invocation's
   * rid and answered the wrong call. The service also treats a duplicate rid as
   * an error. Catch it here rather than on the wire. */
  if (!request->_internal.in_use || request->_internal.owner == NULL)
  {
    AZ_IOT_LOG_ERROR("gen1_direct_method: respond() called on a request that was already answered");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (request->_internal.profile != AZ_IOT_CONNECTION_PROFILE_CLASSIC)
  {
    AZ_IOT_LOG_ERROR("gen1_direct_method: this request was delivered by the gen2 client");
    return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH;
  }

  az_iot_gen1_direct_method_client* dm
      = (az_iot_gen1_direct_method_client*)request->_internal.owner;

  char topic[AZ_IOT_GEN1_DM_TOPIC_MAX];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(topic));
  az_iot_span_writer_append_str(&writer, "$iothub/methods/res/");
  az_iot_span_writer_append_i32(&writer, (int32_t)status_code);
  az_iot_span_writer_append_str(&writer, "/?$rid=");
  az_iot_span_writer_append_str(&writer, request->_internal.rid);
  if (az_iot_span_writer_end_str(&writer, NULL) != AZ_IOT_OK)
  {
    request->_internal.in_use = false;
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  az_iot_mqtt_message out = { 0 };
  out.topic = topic;
  out.payload = payload;
  out.payload_len = payload_len;
  out.qos = AZ_IOT_MQTT_QOS_0;
  out.retain = false;

  az_iot_result r = az_iot_connection_client__publish(DI(dm).conn, &out, NULL, NULL);
  request->_internal.in_use = false;
  return r;
}
