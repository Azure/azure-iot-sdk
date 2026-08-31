// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* DirectMethodClient (Phase 3.2).
 *
 * Supports both IoT Hub Classic (MQTT v3.1.1) and Hub-Next (MQTT v5).
 *
 * Classic:
 *   Subscribe  "$iothub/methods/POST/#"
 *   Inbound    "$iothub/methods/POST/{methodName}/?$rid={rid}"
 *   Respond    "$iothub/methods/res/{status}/?$rid={rid}"
 *
 * Next:
 *   Subscribe  "ih/{device_id}/dev/methods/+"
 *   Inbound    "ih/{device_id}/dev/methods/{methodName}" + correlation_data
 *   Respond    "ih/{device_id}/srv/methods/{methodName}/response" + correlation_data
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_direct_method_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/protocol_profile.h"
#include "internal/span_writer.h"

/* Method-name / rid / correlation bounds live in the public header (they size the
 * request handle). This one is response-topic scratch, internal to this TU. */
#define AZ_IOT_DM_RESP_TOPIC_MAX 192

/* Internal shorthand */
#define DI(d) ((d)->_internal)

/* Acquire a free request slot from the client's bounded pool (NULL if full).
 * Requests may outlive the handler (async respond), so they live in the
 * caller-allocated client struct instead of on the heap.
 *
 * A slot is returned to the pool only by az_iot_direct_method_respond(). An
 * application that drops a request -- a handler that returns without
 * responding, including on its own error paths -- keeps the slot forever, and
 * once AZ_IOT_DM_MAX_INFLIGHT of them have leaked every further invocation is
 * dropped. That used to happen in complete silence, which made it look like
 * the service had stopped delivering; say so instead. */
static az_iot_direct_method_request* dm_request_acquire(az_iot_direct_method_client* dm)
{
  for (size_t i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    az_iot_direct_method_request* r = &DI(dm).req_pool[i];
    if (!r->_internal.in_use)
    {
      memset(r, 0, sizeof(*r));
      r->_internal.in_use = true;
      r->_internal.owner = dm;
      return r;
    }
  }
  AZ_IOT_LOG_WARNF(
      "direct_method: dropping an invocation, all %d in-flight slots are taken. A slot is "
      "released by az_iot_direct_method_respond(); a handler that returns without responding "
      "leaks one.",
      (int)AZ_IOT_DM_MAX_INFLIGHT);
  return NULL;
}

/* Parse "$iothub/methods/POST/<methodName>/?$rid=<rid>" into out_method and
 * out_rid (NUL-terminated). Returns false on malformed input. */
static bool parse_method_topic_classic(
    const char* topic,
    char* out_method,
    size_t method_cap,
    char* out_rid,
    size_t rid_cap)
{
  static const char k_prefix[] = "$iothub/methods/POST/";
  size_t prefix_len = sizeof(k_prefix) - 1;
  if (strncmp(topic, k_prefix, prefix_len) != 0)
  {
    return false;
  }
  const char* name = topic + prefix_len;
  /* methodName is up to the next '/'. */
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

  /* After the slash we expect "?$rid=<value>" (the value runs to end). */
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

/* Parse Hub-Next topic "ih/{device_id}/dev/methods/{methodName}".
 * Returns false on malformed input. */
static bool parse_method_topic_next(const char* topic, char* out_method, size_t method_cap)
{
  /* Expected: "ih/<id>/dev/methods/<name>" */
  static const char k_prefix[] = "ih/";
  if (strncmp(topic, k_prefix, 3) != 0)
  {
    return false;
  }
  /* Find "/dev/methods/" after device_id */
  const char* dev_methods = strstr(topic + 3, "/dev/methods/");
  if (!dev_methods)
  {
    return false;
  }
  const char* name = dev_methods + 13; /* strlen("/dev/methods/") */
  size_t name_len = strlen(name);
  /* Strip trailing '/' if present */
  while (name_len > 0 && name[name_len - 1] == '/')
  {
    name_len--;
  }
  if (name_len == 0 || name_len + 1 > method_cap)
  {
    return false;
  }
  memcpy(out_method, name, name_len);
  out_method[name_len] = '\0';
  return true;
}

/* Inbound dispatch handler for Classic topics. */
static void on_method_invocation_classic(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_direct_method_client* dm = (az_iot_direct_method_client*)user_ctx;
  if (!dm || !msg || !msg->topic)
  {
    return;
  }
  if (!DI(dm).handler)
  {
    return;
  }

  char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
  char rid[AZ_IOT_DM_RID_MAX];
  if (!parse_method_topic_classic(msg->topic, method_name, sizeof(method_name), rid, sizeof(rid)))
  {
    AZ_IOT_LOG_WARNF("direct_method: dropping an unparsable request topic: %s", msg->topic);
    return;
  }

  az_iot_direct_method_request* req = dm_request_acquire(dm);
  if (!req)
  {
    return;
  }
  req->_internal.is_next = false;
  memcpy(req->_internal.rid, rid, strlen(rid) + 1);

  DI(dm).handler(req, method_name, msg->payload, msg->payload_len, DI(dm).handler_ctx);
}

/* Inbound dispatch handler for Hub-Next topics. */
static void on_method_invocation_next(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_direct_method_client* dm = (az_iot_direct_method_client*)user_ctx;
  if (!dm || !msg || !msg->topic)
  {
    return;
  }
  if (!DI(dm).handler)
  {
    return;
  }

  char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
  if (!parse_method_topic_next(msg->topic, method_name, sizeof(method_name)))
  {
    return;
  }

  az_iot_direct_method_request* req = dm_request_acquire(dm);
  if (!req)
  {
    return;
  }
  req->_internal.is_next = true;
  memcpy(req->_internal.method_name, method_name, strlen(method_name) + 1);

  /* Copy correlation data from inbound message if present */
  if (msg->correlation_data && msg->correlation_data_len > 0)
  {
    size_t copy_len = msg->correlation_data_len;
    if (copy_len > AZ_IOT_DM_CORR_DATA_MAX)
    {
      copy_len = AZ_IOT_DM_CORR_DATA_MAX;
    }
    memcpy(req->_internal.correlation_data, msg->correlation_data, copy_len);
    req->_internal.correlation_data_len = copy_len;
  }

  DI(dm).handler(req, method_name, msg->payload, msg->payload_len, DI(dm).handler_ctx);
}

az_iot_result az_iot_direct_method_client_init(
    az_iot_direct_method_client* client,
    az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  const az_iot_protocol_profile* profile = az_iot_connection_client__profile(conn);
  if (!profile)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  memset(client, 0, sizeof(*client));
  DI(client).conn = conn;

  if (profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
  {
    /* Hub-Next: subscribe to "ih/{device_id}/dev/methods/+" */
    const char* device_id = az_iot_connection_client__device_id(conn);
    if (!device_id)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_NOT_INITIALIZED;
    }

    /* Build topic prefix for inbound dispatch: "ih/{device_id}/dev/methods/" */
    char prefix[AZ_IOT_DM_RESP_TOPIC_MAX];
    const char* prefix_parts[] = { "ih/", device_id, "/dev/methods/" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), NULL, prefix_parts, 3)
        != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }

    az_iot_result r = az_iot_connection_client__register_inbound_handler(
        conn, prefix, on_method_invocation_next, client);
    if (r != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return r;
    }

    /* Build wildcard subscription: "ih/{device_id}/dev/methods/+" */
    char filter[AZ_IOT_DM_RESP_TOPIC_MAX];
    const char* filter_parts[] = { "ih/", device_id, "/dev/methods/+" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(filter), NULL, filter_parts, 3)
        != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }
    r = az_iot_connection_client__add_subscription_on_connect(
        conn, filter, AZ_IOT_MQTT_QOS_1, client, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }
  }
  else
  {
    /* Classic: subscribe to "$iothub/methods/POST/#" */
    if (!profile->methods_request_topic_prefix)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    az_iot_result r = az_iot_connection_client__register_inbound_handler(
        conn, profile->methods_request_topic_prefix, on_method_invocation_classic, client);
    if (r != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return r;
    }

    char filter[AZ_IOT_DM_RESP_TOPIC_MAX];
    const char* filter_parts[] = { profile->methods_request_topic_prefix, "#" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(filter), NULL, filter_parts, 2)
        != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }
    r = az_iot_connection_client__add_subscription_on_connect(
        conn, filter, AZ_IOT_MQTT_QOS_0, client, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }
  }

  return AZ_IOT_OK;
}

void az_iot_direct_method_client_destroy(az_iot_direct_method_client* client)
{
  if (!client)
  {
    return;
  }
  (void)az_iot_connection_client__remove_subscriptions_for(DI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(DI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_direct_method_client_set_handler(
    az_iot_direct_method_client* dm,
    az_iot_direct_method_handler_callback cb,
    void* user_ctx)
{
  if (dm == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  DI(dm).handler = cb;
  DI(dm).handler_ctx = user_ctx;
  return AZ_IOT_OK;
}

az_iot_result az_iot_direct_method_respond(
    az_iot_direct_method_request* request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len)
{
  if (request == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (payload_len > 0 && payload == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Responding twice used to publish a second answer on a slot that had
   * already been handed to another invocation, so the reply carried that
   * invocation's rid and answered the wrong call. The service also treats a
   * duplicate rid as an error. Catch it here rather than on the wire. */
  if (!request->_internal.in_use || request->_internal.owner == NULL)
  {
    AZ_IOT_LOG_ERROR("direct_method: respond() called on a request that was already answered");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_direct_method_client* dm = request->_internal.owner;

  if (request->_internal.is_next)
  {
    /* Hub-Next: respond on "ih/{device_id}/srv/methods/{methodName}/response".
     * A connection that never got a client id yields a NULL device id, and
     * the writer reports that instead of dereferencing it. */
    const char* device_id = az_iot_connection_client__device_id(DI(dm).conn);
    char topic[AZ_IOT_DM_RESP_TOPIC_MAX];
    const char* topic_parts[]
        = { "ih/", device_id, "/srv/methods/", request->_internal.method_name, "/response" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, topic_parts, 5) != AZ_IOT_OK)
    {
      request->_internal.in_use = false;
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    /* Build status property as user property */
    char status_str[12];
    az_iot_span_writer status_writer;
    az_iot_span_writer_init(&status_writer, AZ_SPAN_FROM_BUFFER(status_str));
    az_iot_span_writer_append_i32(&status_writer, (int32_t)status_code);
    if (az_iot_span_writer_end_str(&status_writer, NULL) != AZ_IOT_OK)
    {
      request->_internal.in_use = false;
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    az_iot_mqtt_user_property user_props[1];
    user_props[0].key = "status";
    user_props[0].value = status_str;

    az_iot_mqtt_message out = { 0 };
    out.topic = topic;
    out.payload = payload;
    out.payload_len = payload_len;
    out.qos = AZ_IOT_MQTT_QOS_1;
    out.retain = false;
    out.user_properties = user_props;
    out.user_properties_count = 1;
    out.correlation_data = request->_internal.correlation_data;
    out.correlation_data_len = request->_internal.correlation_data_len;

    az_iot_result r = az_iot_connection_client__publish(DI(dm).conn, &out, NULL, NULL);
    request->_internal.in_use = false;
    return r;
  }
  else
  {
    /* Classic: "$iothub/methods/res/{status}/?$rid={rid}" */
    char topic[AZ_IOT_DM_RESP_TOPIC_MAX];
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
}
