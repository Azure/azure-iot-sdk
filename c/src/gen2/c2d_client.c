// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTT v5 C2D receiver.
 *
 *   Inbound    "ih/{device_id}/dev/c2d"
 *
 * No subscription of its own: the presence handshake already holds
 * "ih/{device_id}/dev/#", which covers this topic. Properties arrive as MQTT
 * v5 User Properties, already decoded by the adapter, and the content type
 * has its own field -- so there is nothing to percent-decode here.
 */
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/gen2/az_iot_c2d_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_C2D_TOPIC_MAX 192

#define CI(c) ((c)->_internal)

static void on_c2d(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen2_c2d_client* c2d = (az_iot_gen2_c2d_client*)user_ctx;
  if (!c2d || !msg || !CI(c2d).handler)
  {
    return;
  }

  az_iot_c2d_property properties[AZ_IOT_C2D_MAX_PROPERTIES];
  size_t count = 0;
  size_t seen = 0;
  for (size_t i = 0; i < msg->user_properties_count; ++i)
  {
    if (msg->user_properties[i].key == NULL)
    {
      continue;
    }
    seen++;
    if (count >= AZ_IOT_C2D_MAX_PROPERTIES)
    {
      continue;
    }
    properties[count].key = msg->user_properties[i].key;
    properties[count].value = msg->user_properties[i].value;
    count++;
  }
  if (seen > count)
  {
    /* The message still arrives, but say what was lost. */
    AZ_IOT_LOG_WARNF(
        "c2d: delivering only the first %d of %zu properties; raise "
        "AZ_IOT_C2D_MAX_PROPERTIES to see the rest",
        (int)AZ_IOT_C2D_MAX_PROPERTIES,
        seen);
  }

  az_iot_c2d_message out;
  out.payload = msg->payload;
  out.payload_len = msg->payload_len;
  out.properties = properties;
  out.properties_count = count;
  out.content_type = msg->content_type;
  CI(c2d).handler(&out, CI(c2d).handler_ctx);
}

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_gen2_c2d_client* client = (az_iot_gen2_c2d_client*)owner;

  const char* device_id = az_iot_connection_client__device_id(conn);
  if (!device_id)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  /* Exact topic: there is no wildcard on this path. */
  char prefix[AZ_IOT_C2D_TOPIC_MAX];
  const char* prefix_parts[] = { "ih/", device_id, "/dev/c2d" };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), NULL, prefix_parts, 3) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  return az_iot_connection_client__register_inbound_handler(conn, prefix, on_c2d, client);
}

az_iot_result az_iot_gen2_c2d_client_init(
    az_iot_gen2_c2d_client* client,
    az_iot_connection_client* conn)
{
  if (!client || !conn)
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
  CI(client).conn = conn;

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

void az_iot_gen2_c2d_client_destroy(az_iot_gen2_c2d_client* client)
{
  if (!client)
  {
    return;
  }
  az_iot_connection_client__unregister_feature_client_bind(CI(client).conn, client);
  az_iot_connection_client__release_profile(CI(client).conn);
  (void)az_iot_connection_client__remove_subscriptions_for(CI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(CI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_gen2_c2d_client_set_handler(
    az_iot_gen2_c2d_client* client,
    az_iot_c2d_handler_callback cb,
    void* user_ctx)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  CI(client).handler = cb;
  CI(client).handler_ctx = user_ctx;
  return AZ_IOT_OK;
}
