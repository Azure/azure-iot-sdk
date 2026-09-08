// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* IoT Hub Next / Event Grid direct methods (MQTT v5).
 *
 *   Inbound    "ih/{device_id}/dev/methods/{methodName}" + correlation data
 *   Respond    "ih/{device_id}/srv/methods/{methodName}/response",
 *              status as a user property, correlation data echoed back
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/gen2/az_iot_direct_method_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_GEN2_DM_TOPIC_MAX 192

#define DI(d) ((d)->_internal)

/* Acquire a free slot from the bounded pool (NULL if full). A slot is returned
 * only by az_iot_gen2_direct_method_respond(); a handler that returns without
 * responding leaks one, and once all of them have leaked every further
 * invocation is dropped -- say so rather than going quiet. */
static az_iot_direct_method_request* request_acquire(az_iot_gen2_direct_method_client* dm)
{
  for (size_t i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    az_iot_direct_method_request* r = &DI(dm).req_pool[i];
    if (!r->_internal.in_use)
    {
      memset(r, 0, sizeof(*r));
      r->_internal.in_use = true;
      r->_internal.owner = dm;
      r->_internal.profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
      return r;
    }
  }
  AZ_IOT_LOG_WARNF(
      "gen2_direct_method: dropping an invocation, all %d in-flight slots are taken. A slot is "
      "released by az_iot_gen2_direct_method_respond(); a handler that returns without responding "
      "leaks one.",
      (int)AZ_IOT_DM_MAX_INFLIGHT);
  return NULL;
}

/* Parse "ih/{device_id}/dev/methods/{methodName}". */
static bool parse_method_topic(const char* topic, char* out_method, size_t method_cap)
{
  if (strncmp(topic, "ih/", 3) != 0)
  {
    return false;
  }
  const char* dev_methods = strstr(topic + 3, "/dev/methods/");
  if (!dev_methods)
  {
    return false;
  }
  const char* name = dev_methods + (sizeof("/dev/methods/") - 1);
  size_t name_len = strlen(name);
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

static void on_method_invocation(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_direct_method_client* dm = (az_iot_gen2_direct_method_client*)user_ctx;
  if (!dm || !msg || !msg->topic || !DI(dm).handler)
  {
    return;
  }

  char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
  if (!parse_method_topic(msg->topic, method_name, sizeof(method_name)))
  {
    AZ_IOT_LOG_WARNF("gen2_direct_method: dropping an unparsable request topic: %s", msg->topic);
    return;
  }

  az_iot_direct_method_request* req = request_acquire(dm);
  if (!req)
  {
    return;
  }
  memcpy(req->_internal.method_name, method_name, strlen(method_name) + 1);

  /* The response has to carry this back verbatim or the service cannot pair it
   * with the invocation. */
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

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_gen2_direct_method_client* client = (az_iot_gen2_direct_method_client*)owner;

  const char* device_id = az_iot_connection_client__device_id(conn);
  if (!device_id)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  char prefix[AZ_IOT_GEN2_DM_TOPIC_MAX];
  const char* prefix_parts[] = { "ih/", device_id, "/dev/methods/" };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), NULL, prefix_parts, 3) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  /* No SUBSCRIBE: ih/{device_id}/dev/# from the presence handshake already
   * covers this. */
  return az_iot_connection_client__register_inbound_handler(
      conn, prefix, on_method_invocation, client);
}

az_iot_result az_iot_gen2_direct_method_client_init(
    az_iot_gen2_direct_method_client* client,
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
  DI(client).conn = conn;

  result = az_iot_connection_client__register_feature_client_bind(conn, client, bind_topics);
  if (result != AZ_IOT_OK)
  {
    az_iot_connection_client__release_profile(conn);
    (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
    az_iot_connection_client__unregister_feature_client_bind(conn, client);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_gen2_direct_method_client_destroy(az_iot_gen2_direct_method_client* client)
{
  if (!client)
  {
    return;
  }
  az_iot_connection_client__unregister_feature_client_bind(DI(client).conn, client);
  az_iot_connection_client__release_profile(DI(client).conn);
  (void)az_iot_connection_client__remove_subscriptions_for(DI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(DI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_gen2_direct_method_client_set_handler(
    az_iot_gen2_direct_method_client* client,
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

az_iot_result az_iot_gen2_direct_method_respond(
    az_iot_direct_method_request* request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len)
{
  if (request == NULL || (payload_len > 0 && payload == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!request->_internal.in_use || request->_internal.owner == NULL)
  {
    AZ_IOT_LOG_ERROR("gen2_direct_method: respond() called on a request that was already answered");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (request->_internal.profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    AZ_IOT_LOG_ERROR("gen2_direct_method: this request was delivered by the gen1 client");
    return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH;
  }

  az_iot_gen2_direct_method_client* dm
      = (az_iot_gen2_direct_method_client*)request->_internal.owner;

  /* Same answer bind_topics gives: a connection that never got a device id has
   * nothing to build a topic from, and saying NOT_SUPPORTED would point at the
   * topic writer rather than at the connection. */
  const char* device_id = az_iot_connection_client__device_id(DI(dm).conn);
  if (!device_id)
  {
    request->_internal.in_use = false;
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  char topic[AZ_IOT_GEN2_DM_TOPIC_MAX];
  const char* topic_parts[]
      = { "ih/", device_id, "/srv/methods/", request->_internal.method_name, "/response" };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, topic_parts, 5) != AZ_IOT_OK)
  {
    request->_internal.in_use = false;
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

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
