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

#define METHODS_RESPONSE_PREFIX "$iothub/methods/res/"
#define METHODS_RESPONSE_RID_MARKER "/?$rid="
/* Longest decimal an int32 status can print, INT32_MIN included. */
#define AZ_IOT_GEN1_DM_STATUS_MAX 11

/* Derived from the parts the topic is built out of rather than fixed, because
 * AZ_IOT_DM_RID_MAX is a documented knob: at a fixed size, raising it far
 * enough produced a client that accepted invocations and could never answer
 * one. The two sizeof()s each carry a NUL, which covers the terminator with a
 * byte to spare. */
#define AZ_IOT_GEN1_DM_TOPIC_MAX                               \
  (sizeof(METHODS_RESPONSE_PREFIX) + AZ_IOT_GEN1_DM_STATUS_MAX \
   + sizeof(METHODS_RESPONSE_RID_MARKER) + AZ_IOT_DM_RID_MAX)

#define METHODS_REQUEST_PREFIX "$iothub/methods/POST/"
#define MS_PER_SECOND 1000u

#define DI(d) ((d)->_internal)

/* Release every in-flight slot whose response timeout has passed.
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
    az_iot_direct_method_slot* s = &DI(dm).req_pool[i];
    if (!s->_internal.in_use || now < DI(dm).req_expires_at_ms[i])
    {
      continue;
    }
    AZ_IOT_LOG_WARNF(
        "gen1_direct_method: '%s' was never answered within its response timeout; reclaiming its "
        "slot",
        s->_internal.method_name);
    s->_internal.in_use = false;
  }
}

/* Resolve a request to the slot it names, or NULL when it no longer refers to a
 * live invocation of this client.
 *
 * The sequence check is what makes a reused slot detectable: the request is a
 * value the pool cannot reach, so a slot reclaimed and handed to another
 * invocation carries a newer seq and the stale request stops matching. */
static az_iot_direct_method_slot* request_resolve(
    az_iot_gen1_direct_method_client* dm,
    az_iot_direct_method_request request,
    size_t* out_index)
{
  size_t i = (size_t)request._internal.slot;
  if (i >= AZ_IOT_DM_MAX_INFLIGHT || request._internal.seq == 0u)
  {
    return NULL;
  }
  az_iot_direct_method_slot* s = &DI(dm).req_pool[i];
  if (!s->_internal.in_use || s->_internal.seq != request._internal.seq)
  {
    return NULL;
  }
  *out_index = i;
  return s;
}

/* Acquire a free slot from the bounded pool.
 *
 * A slot is returned by az_iot_gen1_direct_method_respond(), or reclaimed by
 * requests_expire_stale() once the invocation can no longer be answered
 * usefully. Running out means this many invocations arrived while earlier ones
 * were still legitimately in flight.
 *
 * Returns false when the pool is full; otherwise fills @p out_request with the
 * value the application hands back to respond(). */
static bool request_acquire(
    az_iot_gen1_direct_method_client* dm,
    az_iot_direct_method_request* out_request,
    az_iot_direct_method_slot** out_slot)
{
  for (size_t n = 0; n < AZ_IOT_DM_MAX_INFLIGHT; ++n)
  {
    size_t i = (DI(dm).next_slot + n) % AZ_IOT_DM_MAX_INFLIGHT;
    az_iot_direct_method_slot* s = &DI(dm).req_pool[i];
    if (!s->_internal.in_use)
    {
      memset(s, 0, sizeof(*s));
      /* Skips 0 on wrap, so a zeroed request can never match a live slot. */
      DI(dm).next_seq++;
      if (DI(dm).next_seq == 0u)
      {
        DI(dm).next_seq = 1u;
      }
      s->_internal.seq = DI(dm).next_seq;
      s->_internal.in_use = true;
      DI(dm).req_expires_at_ms[i]
          = az_iot_time_mono_ms() + ((uint64_t)DI(dm).response_timeout_seconds * MS_PER_SECOND);
      DI(dm).next_slot = (i + 1u) % AZ_IOT_DM_MAX_INFLIGHT;

      memset(out_request, 0, sizeof(*out_request));
      out_request->_internal.slot = (uint32_t)i;
      out_request->_internal.seq = s->_internal.seq;
      out_request->_internal.profile = AZ_IOT_CONNECTION_PROFILE_CLASSIC;
      *out_slot = s;
      return true;
    }
  }
  AZ_IOT_LOG_WARNF(
      "gen1_direct_method: dropping an invocation, all %d in-flight slots are held by requests "
      "that have not been answered and have not yet timed out. Answer them with "
      "az_iot_gen1_direct_method_respond(), or raise AZ_IOT_DM_MAX_INFLIGHT.",
      (int)AZ_IOT_DM_MAX_INFLIGHT);
  return false;
}

/* Parse "$iothub/methods/POST/<methodName>/?$rid=<rid>".
 *
 * The prefix check below is unreachable through the public path -- every caller
 * arrives via a dispatch entry registered on METHODS_REQUEST_PREFIX itself --
 * and is kept as the guard on that contract rather than on the input. */
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

  az_iot_direct_method_request req;
  az_iot_direct_method_slot* slot = NULL;
  if (!request_acquire(dm, &req, &slot))
  {
    return;
  }
  memcpy(slot->_internal.rid, rid, strlen(rid) + 1);
  memcpy(slot->_internal.method_name, method_name, strlen(method_name) + 1);

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
  DI(client).response_timeout_seconds = AZ_IOT_GEN1_DM_RESPONSE_TIMEOUT_SECONDS;

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

az_iot_result az_iot_gen1_direct_method_client_set_response_timeout(
    az_iot_gen1_direct_method_client* client,
    uint32_t seconds)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  DI(client).response_timeout_seconds
      = (seconds == 0u) ? AZ_IOT_GEN1_DM_RESPONSE_TIMEOUT_SECONDS : seconds;
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen1_direct_method_respond(
    az_iot_gen1_direct_method_client* client,
    az_iot_direct_method_request request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len)
{
  if (client == NULL || (payload_len > 0 && payload == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (request._internal.profile != AZ_IOT_CONNECTION_PROFILE_CLASSIC)
  {
    AZ_IOT_LOG_ERROR("gen1_direct_method: this request was delivered by the gen2 client");
    return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH;
  }

  az_iot_gen1_direct_method_client* dm = client;

  /* Covers three cases at once, none of which may reach the wire: already
   * answered, reclaimed on timeout, and reclaimed then handed to another
   * invocation -- the last would otherwise publish under that call's rid. */
  size_t index = 0;
  az_iot_direct_method_slot* slot = request_resolve(dm, request, &index);
  if (slot == NULL)
  {
    AZ_IOT_LOG_ERROR("gen1_direct_method: respond() called on a request that is no longer live -- "
                     "it was already "
                     "answered, or its slot was reclaimed when the response timeout passed");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* The sweep only runs when a method message arrives, so checking the slot's
   * own expiry here is what makes the refusal hold on an otherwise idle device. */
  if (az_iot_time_mono_ms() >= DI(dm).req_expires_at_ms[index])
  {
    AZ_IOT_LOG_WARNF(
        "gen1_direct_method: '%s' was answered after its response timeout; sending nothing",
        slot->_internal.method_name);
    slot->_internal.in_use = false;
    return AZ_IOT_ERR_TIMEOUT;
  }

  char topic[AZ_IOT_GEN1_DM_TOPIC_MAX];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(topic));
  az_iot_span_writer_append_str(&writer, METHODS_RESPONSE_PREFIX);
  az_iot_span_writer_append_i32(&writer, (int32_t)status_code);
  az_iot_span_writer_append_str(&writer, METHODS_RESPONSE_RID_MARKER);
  az_iot_span_writer_append_str(&writer, slot->_internal.rid);
  /* Unreachable by construction: the buffer is sized from these same parts.
   * Kept because that is a property of two macros agreeing, not something the
   * writer itself enforces. */
  if (az_iot_span_writer_end_str(&writer, NULL) != AZ_IOT_OK)
  {
    slot->_internal.in_use = false;
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  az_iot_mqtt_message out = { 0 };
  out.topic = topic;
  out.payload = payload;
  out.payload_len = payload_len;
  out.qos = AZ_IOT_MQTT_QOS_0;
  out.retain = false;

  az_iot_result r = az_iot_connection_client__publish(DI(dm).conn, &out, NULL, NULL);
  slot->_internal.in_use = false;
  return r;
}
