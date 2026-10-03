// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "azure/iot/mqttv3/az_iot_telemetry_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_MQTTV3_TELEMETRY_TOPIC_MAX 512

AZ_NODISCARD az_iot_result az_iot_mqttv3_telemetry_client_init(
    az_iot_mqttv3_telemetry_client* client,
    az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    AZ_IOT_LOG_ERROR("mqttv3_telemetry: init: invalid arguments");
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
  client->_internal.conn = conn;
  return AZ_IOT_OK;
}

void az_iot_mqttv3_telemetry_client_deinit(az_iot_mqttv3_telemetry_client* client)
{
  if (client)
  {
    az_iot_connection_client__release_profile(client->_internal.conn);
    memset(client, 0, sizeof(*client));
  }
}

static az_iot_result build_topic(
    const char* device_id,
    const az_iot_telemetry_message* message,
    char* topic,
    size_t topic_size)
{
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, az_span_create((uint8_t*)topic, (int32_t)topic_size));
  az_iot_span_writer_append_str(&writer, "devices/");
  az_iot_span_writer_append_str(&writer, device_id);
  az_iot_span_writer_append_str(&writer, "/messages/events/");

  bool first = true;
  for (size_t i = 0; i < message->properties_count; ++i)
  {
    const az_iot_telemetry_property* property = &message->properties[i];
    if (!is_nonempty_cstr(property->key))
    {
      continue;
    }
    if (!first)
    {
      az_iot_span_writer_append_u8(&writer, (uint8_t)'&');
    }
    az_iot_span_writer_append_url_encoded(&writer, property->key);
    if (property->value)
    {
      az_iot_span_writer_append_u8(&writer, (uint8_t)'=');
      az_iot_span_writer_append_url_encoded(&writer, property->value);
    }
    first = false;
  }

  return az_iot_span_writer_end_str(&writer, NULL);
}

AZ_NODISCARD az_iot_result az_iot_mqttv3_telemetry_client_send(
    az_iot_mqttv3_telemetry_client* client,
    const az_iot_telemetry_message* message,
    az_iot_telemetry_send_callback callback,
    void* user_ctx)
{
  if (client == NULL || message == NULL || callback == NULL
      || (message->payload_len > 0 && message->payload == NULL)
      || (message->properties_count > 0 && message->properties == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->_internal.conn == NULL)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  const char* device_id = az_iot_connection_client__device_id(client->_internal.conn);
  if (device_id == NULL)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  char topic[AZ_IOT_MQTTV3_TELEMETRY_TOPIC_MAX];
  az_iot_result result = build_topic(device_id, message, topic, sizeof(topic));
  if (result != AZ_IOT_OK)
  {
    return result;
  }

  az_iot_mqtt_message mqtt_message = { 0 };
  mqtt_message.topic = topic;
  mqtt_message.payload = message->payload;
  mqtt_message.payload_len = message->payload_len;
  mqtt_message.qos = AZ_IOT_MQTT_QOS_1;
  return az_iot_connection_client__publish(
      client->_internal.conn, &mqtt_message, callback, user_ctx);
}
