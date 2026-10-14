// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv3 hub C2D receiver.
 *
 *   Subscribe  "devices/{device_id}/messages/devicebound/#"
 *   Inbound    "devices/{device_id}/messages/devicebound/{property-bag}"
 *
 * The bag is handed to the application as a view into the topic; it is
 * percent-decoded on demand by the readers in az_iot_message.h, into buffers
 * the application provides.
 */
#include <stdbool.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv3/az_iot_c2d_client.h"

#include "internal/connection_client_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_C2D_TOPIC_MAX 192

#define CI(c) ((c)->_internal)

/** @brief Hand the message to the application with a view of its property bag. */
static void on_c2d(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_mqttv3_c2d_client* c2d = (az_iot_mqttv3_c2d_client*)user_ctx;
  if (!c2d || !msg || !CI(c2d).handler)
  {
    return;
  }

  az_iot_c2d_message out;
  memset(&out, 0, sizeof(out));
  out.payload = msg->payload;
  out.payload_len = msg->payload_len;
  if (msg->topic != NULL)
  {
    static const char k_marker[] = "/messages/devicebound/";
    const char* bag = strstr(msg->topic, k_marker);
    if (bag != NULL)
    {
      bag += sizeof(k_marker) - 1;
      out.properties._internal.bag = bag;
      out.properties._internal.bag_len = strlen(bag);
    }
  }
  CI(c2d).handler(&out, CI(c2d).handler_ctx);
}

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_mqttv3_c2d_client* client = (az_iot_mqttv3_c2d_client*)owner;

  const char* device_id = az_iot_connection_client__device_id(conn);
  if (!device_id)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  /* Dispatch prefix carries no wildcard -- prefix match handles the bag. */
  char prefix[AZ_IOT_C2D_TOPIC_MAX];
  const char* prefix_parts[] = { "devices/", device_id, "/messages/devicebound/" };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), NULL, prefix_parts, 3) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_iot_result result
      = az_iot_connection_client__register_inbound_handler(conn, prefix, on_c2d, client);
  if (result != AZ_IOT_OK)
  {
    return result;
  }

  char filter[AZ_IOT_C2D_TOPIC_MAX];
  const char* filter_parts[] = { "devices/", device_id, "/messages/devicebound/#" };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(filter), NULL, filter_parts, 3) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  return az_iot_connection_client__add_subscription_on_connect(
      conn, filter, AZ_IOT_MQTT_QOS_1, client, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL);
}

AZ_NODISCARD az_iot_result
az_iot_mqttv3_c2d_client_init(az_iot_mqttv3_c2d_client* client, az_iot_connection_client* conn)
{
  if (!client || !conn)
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
  CI(client).conn = conn;

  result = az_iot_connection_client__register_feature_client_bind(conn, client, bind_topics);
  if (result != AZ_IOT_OK)
  {
    az_iot_connection_client__release_profile(conn);
    (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
    (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
    az_iot_connection_client__unregister_feature_client_bind(conn, client);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_mqttv3_c2d_client_deinit(az_iot_mqttv3_c2d_client* client)
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

az_iot_result az_iot_mqttv3_c2d_client_set_handler(
    az_iot_mqttv3_c2d_client* client,
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
