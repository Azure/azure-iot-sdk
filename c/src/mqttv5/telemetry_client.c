// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>

#include "azure/iot/mqttv5/az_iot_telemetry_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_MQTTV5_TELEMETRY_TOPIC_MAX 512

AZ_NODISCARD az_iot_result az_iot_mqttv5_telemetry_client_init(
    az_iot_mqttv5_telemetry_client* client,
    az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_MQTTV5_TELEMETRY, "init: invalid arguments");
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

void az_iot_mqttv5_telemetry_client_deinit(az_iot_mqttv5_telemetry_client* client)
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

/** @brief What telemetry_property_next() reads: the message and its resolved content type. */
typedef struct telemetry_properties
{
  const az_iot_telemetry_message* message;
  const char* content_type;
} telemetry_properties;

/** @brief Fills @p out from NUL-terminated @p key and @p value. */
static void set_view(az_iot_mqtt_user_property_view* out, const char* key, const char* value)
{
  out->key = key;
  out->key_len = strlen(key);
  out->value = value;
  out->value_len = strlen(value);
}

/**
 * @brief az_iot_mqtt_user_properties_next_fn for telemetry: `type`, `content-type`, then every
 * application property in order. Cursor 0 and 1 are the two SDK properties; 2 + i is application
 * property i.
 */
static az_iot_result telemetry_property_next(
    const void* source,
    size_t source_len,
    size_t* cursor,
    az_iot_mqtt_user_property_view* out)
{
  (void)source_len;
  const telemetry_properties* t = (const telemetry_properties*)source;
  if (*cursor == 0)
  {
    *cursor = 1;
    set_view(out, "type", "telemetry:1");
    return AZ_IOT_OK;
  }
  if (*cursor == 1)
  {
    *cursor = 2;
    set_view(out, "content-type", t->content_type);
    return AZ_IOT_OK;
  }
  while (*cursor - 2 < t->message->properties_count)
  {
    const az_iot_telemetry_property* property = &t->message->properties[*cursor - 2];
    (*cursor)++;
    /* $.ct and $.cid travel in their native v5 fields; a copy under their own
     * name would say the same thing in a spelling the service does not read.
     * Every other system property has no v5 equivalent, so it travels verbatim
     * -- which is what az_iot_message.h promises. */
    if (!is_nonempty_cstr(property->key) || strcmp(property->key, AZ_IOT_MSG_PROP_CONTENT_TYPE) == 0
        || strcmp(property->key, AZ_IOT_MSG_PROP_CORRELATION_ID) == 0)
    {
      continue;
    }
    set_view(out, property->key, property->value ? property->value : "");
    return AZ_IOT_OK;
  }
  return AZ_IOT_ERR_NOT_FOUND;
}

AZ_NODISCARD az_iot_result az_iot_mqttv5_telemetry_client_send(
    az_iot_mqttv5_telemetry_client* client,
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

  /* No device id yet: a provisioning connection has not been assigned one. */
  const char* device_id = az_iot_connection_client__device_id(client->_internal.conn);
  if (device_id == NULL)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  char topic[AZ_IOT_MQTTV5_TELEMETRY_TOPIC_MAX];
  az_iot_result result = build_topic(device_id, topic, sizeof(topic));
  if (result != AZ_IOT_OK)
  {
    return result;
  }

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

  /* The mqttv5 hub reads the correlation id only from native Correlation Data;
   * a $.cid user property would reach routing as an application property. */
  const char* correlation_id = NULL;
  for (size_t i = 0; i < message->properties_count; ++i)
  {
    if (message->properties[i].key
        && strcmp(message->properties[i].key, AZ_IOT_MSG_PROP_CORRELATION_ID) == 0
        && is_nonempty_cstr(message->properties[i].value))
    {
      correlation_id = message->properties[i].value;
      break;
    }
  }
  size_t correlation_id_len = correlation_id ? strlen(correlation_id) : 0;
  if (correlation_id_len > UINT16_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  telemetry_properties properties = { message, content_type };
  az_iot_mqtt_message mqtt_message = { 0 };
  mqtt_message.topic = topic;
  mqtt_message.payload = message->payload;
  mqtt_message.payload_len = message->payload_len;
  mqtt_message.qos = AZ_IOT_MQTT_QOS_1;
  mqtt_message.user_properties.next = telemetry_property_next;
  mqtt_message.user_properties.source = &properties;
  mqtt_message.content_type = content_type;
  mqtt_message.correlation_data = (const uint8_t*)correlation_id;
  mqtt_message.correlation_data_len = correlation_id_len;
  return az_iot_connection_client__publish(
      client->_internal.conn, client, &mqtt_message, callback, user_ctx);
}
