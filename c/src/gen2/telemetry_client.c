// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>

#include "azure/iot/gen2/az_iot_telemetry_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_GEN2_TELEMETRY_TOPIC_MAX 512
#define AZ_IOT_GEN2_TELEMETRY_MAX_USER_PROPERTIES 16

az_iot_result az_iot_gen2_telemetry_client_init(
    az_iot_gen2_telemetry_client* client,
    az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    AZ_IOT_LOG_ERROR("gen2_telemetry_client_init: invalid arguments");
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
  client->_internal.conn = conn;
  return AZ_IOT_OK;
}

void az_iot_gen2_telemetry_client_destroy(az_iot_gen2_telemetry_client* client)
{
  if (client)
  {
    az_iot_connection_client__release_profile(client->_internal.conn);
    memset(client, 0, sizeof(*client));
  }
}

static az_iot_result build_topic(const char* device_id, char* topic, size_t topic_size)
{
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, az_span_create((uint8_t*)topic, (int32_t)topic_size));
  az_iot_span_writer_append_str(&writer, "ih/");
  az_iot_span_writer_append_str(&writer, device_id);
  az_iot_span_writer_append_str(&writer, "/srv/telemetry");
  return az_iot_span_writer_end_str(&writer, NULL);
}

az_iot_result az_iot_gen2_telemetry_client_send(
    az_iot_gen2_telemetry_client* client,
    const az_iot_telemetry_message* message,
    az_iot_telemetry_send_callback callback,
    void* user_ctx)
{
  if (client == NULL || message == NULL || (message->payload_len > 0 && message->payload == NULL)
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

  char topic[AZ_IOT_GEN2_TELEMETRY_TOPIC_MAX];
  az_iot_result result = build_topic(device_id, topic, sizeof(topic));
  if (result != AZ_IOT_OK)
  {
    return result;
  }

  az_iot_mqtt_user_property user_properties[AZ_IOT_GEN2_TELEMETRY_MAX_USER_PROPERTIES];
  size_t property_count = 0;
  user_properties[property_count++]
      = (az_iot_mqtt_user_property){ .key = "type", .value = "telemetry:1" };

  const char* content_type = "application/json";
  for (size_t i = 0; i < message->properties_count; ++i)
  {
    if (message->properties[i].key
        && strcmp(message->properties[i].key, AZ_IOT_MSG_PROP_CONTENT_TYPE) == 0
        && message->properties[i].value)
    {
      content_type = message->properties[i].value;
      break;
    }
  }
  user_properties[property_count++]
      = (az_iot_mqtt_user_property){ .key = "content-type", .value = content_type };

  for (size_t i = 0; i < message->properties_count; ++i)
  {
    const az_iot_telemetry_property* property = &message->properties[i];
    if (!is_nonempty_cstr(property->key))
    {
      continue;
    }
    /* Already on the wire twice over as the native Content Type and the
     * content-type user property; a third copy under its own name would say
     * the same thing in a spelling the service does not read. Every other
     * system property has no v5 equivalent, so it travels verbatim -- which is
     * what az_iot_message.h promises and what gen2 c2d hands back unchanged. */
    if (strcmp(property->key, AZ_IOT_MSG_PROP_CONTENT_TYPE) == 0)
    {
      continue;
    }
    if (property_count >= AZ_IOT_GEN2_TELEMETRY_MAX_USER_PROPERTIES)
    {
      AZ_IOT_LOG_WARNF(
          "gen2_telemetry: '%s' and any properties after it were not sent; the message needs more "
          "than the %d user properties this client can carry, two of which are the type and "
          "content-type it adds. Send fewer, or raise AZ_IOT_GEN2_TELEMETRY_MAX_USER_PROPERTIES.",
          property->key,
          (int)AZ_IOT_GEN2_TELEMETRY_MAX_USER_PROPERTIES);
      break;
    }
    user_properties[property_count++] = (az_iot_mqtt_user_property){
      .key = property->key,
      .value = property->value ? property->value : "",
    };
  }

  az_iot_mqtt_message mqtt_message = { 0 };
  mqtt_message.topic = topic;
  mqtt_message.payload = message->payload;
  mqtt_message.payload_len = message->payload_len;
  mqtt_message.qos = AZ_IOT_MQTT_QOS_1;
  mqtt_message.user_properties = user_properties;
  mqtt_message.user_properties_count = property_count;
  mqtt_message.content_type = content_type;
  return az_iot_connection_client__publish(
      client->_internal.conn, &mqtt_message, callback, user_ctx);
}
