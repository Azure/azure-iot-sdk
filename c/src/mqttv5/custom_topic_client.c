// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "azure/iot/mqttv5/az_iot_custom_topic_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

/** @brief First topic level the SDK's own mqttv5 traffic uses; never a custom topic. */
#define RESERVED_FIRST_LEVEL "ih"

/**
 * @brief Append the literal @p length bytes at @p literal.
 *
 * @return false, appending nothing, when they cannot fit @p out_size (at most INT32_MAX), which
 *         also keeps @p length within the int32_t az_span_create() takes.
 */
static bool append_literal(
    az_iot_span_writer* writer,
    const char* literal,
    size_t length,
    size_t out_size);

/**
 * @brief Length of the well-formed UTF-8 sequence at @p s, or 0 if it is malformed.
 *
 * Rejects overlong forms, surrogates and code points above U+10FFFF.
 */
static size_t utf8_sequence_length(const uint8_t* s);

/**
 * @brief Whether @p topic can be published to a custom topic.
 *
 * Catches locally what the hub would refuse anyway, some of it by closing the connection.
 */
static bool is_valid_topic(const char* topic);

AZ_NODISCARD az_iot_result az_iot_mqttv5_custom_topic_client_init(
    az_iot_mqttv5_custom_topic_client* client,
    az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_MQTTV5_CUSTOM_TOPIC, "init: invalid arguments");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(client, 0, sizeof(*client));
  az_iot_result result
      = az_iot_connection_client__require_profile(conn, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  if (result != AZ_IOT_OK)
  {
    return result;
  }

  client->_internal.conn = conn;
  return AZ_IOT_OK;
}

void az_iot_mqttv5_custom_topic_client_deinit(az_iot_mqttv5_custom_topic_client* client)
{
  if (client)
  {
    az_iot_connection_client__release_profile(client->_internal.conn);
    memset(client, 0, sizeof(*client));
  }
}

AZ_NODISCARD az_iot_result az_iot_mqttv5_custom_topic_client_format_topic(
    const az_iot_mqttv5_custom_topic_client* client,
    const char* topic_template,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  if (client == NULL || topic_template == NULL || out == NULL || out_size == 0
      || out_size > INT32_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->_internal.conn == NULL)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  /* On a DPS connection the device id is settled only once assigned. */
  if (!az_iot_connection_client__is_connected(client->_internal.conn))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  const char* device_id = az_iot_connection_client__device_id(client->_internal.conn);
  if (!is_nonempty_cstr(device_id))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  static const char variable[] = AZ_IOT_MQTTV5_CUSTOM_TOPIC_DEVICE_ID_VARIABLE;
  const size_t variable_len = sizeof(variable) - 1;

  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, az_span_create((uint8_t*)out, (int32_t)out_size));
  const char* literal = topic_template;
  const char* cursor = topic_template;
  while (*cursor != '\0')
  {
    if (*cursor == '{' && strncmp(cursor, variable, variable_len) == 0)
    {
      if (!append_literal(&writer, literal, (size_t)(cursor - literal), out_size))
      {
        out[0] = '\0';
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
      }
      az_iot_span_writer_append_str(&writer, device_id);
      cursor += variable_len;
      literal = cursor;
    }
    else if (*cursor == '{' || *cursor == '}')
    {
      out[0] = '\0';
      return AZ_IOT_ERR_INVALID_ARG;
    }
    else
    {
      ++cursor;
    }
  }
  if (!append_literal(&writer, literal, (size_t)(cursor - literal), out_size))
  {
    out[0] = '\0';
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  return az_iot_span_writer_end_str(&writer, out_len);
}

AZ_NODISCARD az_iot_result az_iot_mqttv5_custom_topic_client_publish(
    az_iot_mqttv5_custom_topic_client* client,
    const az_iot_mqttv5_custom_topic_message* message,
    az_iot_mqttv5_custom_topic_publish_callback callback,
    void* user_ctx)
{
  if (client == NULL || message == NULL || callback == NULL
      || (message->payload_len > 0 && message->payload == NULL)
      || (message->user_properties_count > 0 && message->user_properties == NULL)
      || (message->qos != AZ_IOT_MQTT_QOS_0 && message->qos != AZ_IOT_MQTT_QOS_1))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->_internal.conn == NULL)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (!is_valid_topic(message->topic))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_MQTTV5_CUSTOM_TOPIC,
        "publish: invalid topic; see az_iot_mqttv5_custom_topic_message::topic");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  for (size_t i = 0; i < message->user_properties_count; ++i)
  {
    if (!is_nonempty_cstr(message->user_properties[i].key)
        || message->user_properties[i].value == NULL)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }

  az_iot_mqtt_message mqtt_message = { 0 };
  mqtt_message.topic = message->topic;
  mqtt_message.payload = message->payload;
  mqtt_message.payload_len = message->payload_len;
  mqtt_message.qos = message->qos;
  mqtt_message.content_type = message->content_type;
  mqtt_message.user_properties = message->user_properties;
  mqtt_message.user_properties_count = message->user_properties_count;
  mqtt_message.message_expiry_seconds = message->message_expiry_seconds;
  return az_iot_connection_client__publish(
      client->_internal.conn, client, &mqtt_message, callback, user_ctx);
}

static bool append_literal(
    az_iot_span_writer* writer,
    const char* literal,
    size_t length,
    size_t out_size)
{
  if (length >= out_size)
  {
    return false;
  }
  az_iot_span_writer_append_span(
      writer, az_span_create((uint8_t*)(uintptr_t)literal, (int32_t)length));
  return true;
}

static size_t utf8_sequence_length(const uint8_t* s)
{
  if (s[0] < 0x80)
  {
    return 1;
  }
  size_t len;
  uint32_t code_point;
  uint32_t minimum;
  if ((s[0] & 0xE0) == 0xC0)
  {
    len = 2;
    code_point = s[0] & 0x1Fu;
    minimum = 0x80;
  }
  else if ((s[0] & 0xF0) == 0xE0)
  {
    len = 3;
    code_point = s[0] & 0x0Fu;
    minimum = 0x800;
  }
  else if ((s[0] & 0xF8) == 0xF0)
  {
    len = 4;
    code_point = s[0] & 0x07u;
    minimum = 0x10000;
  }
  else
  {
    return 0;
  }
  for (size_t i = 1; i < len; ++i)
  {
    if ((s[i] & 0xC0) != 0x80)
    {
      return 0;
    }
    code_point = (code_point << 6) | (s[i] & 0x3Fu);
  }
  if (code_point < minimum || code_point > 0x10FFFF
      || (code_point >= 0xD800 && code_point <= 0xDFFF))
  {
    return 0;
  }
  return len;
}

static bool is_valid_topic(const char* topic)
{
  if (!is_nonempty_cstr(topic))
  {
    return false;
  }

  const size_t first_level_len = strcspn(topic, "/");
  if (first_level_len == sizeof(RESERVED_FIRST_LEVEL) - 1
      && memcmp(topic, RESERVED_FIRST_LEVEL, first_level_len) == 0)
  {
    return false;
  }

  size_t length = 0;
  size_t levels = 1;
  const uint8_t* cursor = (const uint8_t*)topic;
  while (*cursor != '\0')
  {
    if (*cursor == '+' || *cursor == '#')
    {
      return false;
    }
    if (*cursor == '/')
    {
      ++levels;
    }
    const size_t sequence = utf8_sequence_length(cursor);
    if (sequence == 0)
    {
      return false;
    }
    length += sequence;
    cursor += sequence;
    if (length > AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN)
    {
      return false;
    }
  }
  return levels <= AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEVELS;
}
