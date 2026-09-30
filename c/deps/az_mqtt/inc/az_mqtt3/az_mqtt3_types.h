// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt3_types.h
 * @brief MQTT 3.1.1 packet and option types.
 */

#ifndef AZ_MQTT3_TYPES_H
#define AZ_MQTT3_TYPES_H

#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/** @brief CONNACK return codes. */
typedef enum
{
  AZ_MQTT3_CONNACK_ACCEPTED = 0x00,
  AZ_MQTT3_CONNACK_UNACCEPTABLE_PROTOCOL_VERSION = 0x01,
  AZ_MQTT3_CONNACK_IDENTIFIER_REJECTED = 0x02,
  AZ_MQTT3_CONNACK_SERVER_UNAVAILABLE = 0x03,
  AZ_MQTT3_CONNACK_BAD_USER_NAME_OR_PASSWORD = 0x04,
  AZ_MQTT3_CONNACK_NOT_AUTHORIZED = 0x05,
} az_mqtt3_connack_return_code;

/** @brief SUBACK return codes. */
typedef enum
{
  AZ_MQTT3_SUBACK_GRANTED_QOS_0 = 0x00,
  AZ_MQTT3_SUBACK_GRANTED_QOS_1 = 0x01,
  AZ_MQTT3_SUBACK_GRANTED_QOS_2 = 0x02,
  AZ_MQTT3_SUBACK_FAILURE = 0x80,
} az_mqtt3_suback_return_code;

/** @brief One SUBSCRIBE topic filter. */
typedef struct
{
  az_span topic_filter;
  az_mqtt_qos qos;
} az_mqtt3_subscription;

/** @brief Will message, sent by the server if the connection ends without DISCONNECT. */
typedef struct
{
  az_span topic;
  az_span payload;
  az_mqtt_qos qos;
  bool retain;
} az_mqtt3_will_options;

/** @brief CONNECT options. */
typedef struct
{
  az_span client_id;
  /** @brief Empty: not sent. */
  az_span username;
  /** @brief Empty: not sent. */
  az_span password;
  /** @brief 0 disables keep-alive. */
  uint16_t keep_alive_seconds;
  bool clean_session;
  /** @brief NULL: no will. */
  az_mqtt3_will_options const* will;
} az_mqtt3_connect_options;

/** @brief Received CONNACK. */
typedef struct
{
  bool session_present;
  az_mqtt3_connack_return_code return_code;
} az_mqtt3_connack_data;

/** @brief Received PUBLISH. Spans point into the receive buffer: valid during the callback only. */
typedef struct
{
  az_span topic;
  az_span payload;
  az_mqtt_qos qos;
  bool retain;
  bool dup;
  /** @brief 0 for QoS 0. */
  uint16_t packet_id;
} az_mqtt3_publish_data;

/** @brief PUBLISH to send. */
typedef struct
{
  az_span topic;
  az_span payload;
  az_mqtt_qos qos;
  bool retain;
} az_mqtt3_publish_options;

/** @brief Received PUBACK, PUBREC, PUBREL, PUBCOMP or UNSUBACK: a packet identifier only. */
typedef struct
{
  uint16_t packet_id;
} az_mqtt3_ack_data;

/** @brief Received SUBACK. */
typedef struct
{
  uint16_t packet_id;
  /**
   * @brief One byte per topic filter, in order (az_mqtt3_suback_return_code).
   * Points into the receive buffer: valid during the callback only.
   */
  az_span return_codes;
} az_mqtt3_suback_data;

/** @brief Defaults: keep-alive 60 s, clean session, no credentials, no will. */
AZ_NODISCARD AZ_INLINE az_mqtt3_connect_options az_mqtt3_connect_options_default(void)
{
  az_mqtt3_connect_options opts;
  opts.client_id = AZ_SPAN_EMPTY;
  opts.username = AZ_SPAN_EMPTY;
  opts.password = AZ_SPAN_EMPTY;
  opts.keep_alive_seconds = 60;
  opts.clean_session = true;
  opts.will = NULL;
  return opts;
}

/** @brief Defaults: QoS 0, not retained, empty topic and payload. */
AZ_NODISCARD AZ_INLINE az_mqtt3_publish_options az_mqtt3_publish_options_default(void)
{
  az_mqtt3_publish_options opts;
  opts.topic = AZ_SPAN_EMPTY;
  opts.payload = AZ_SPAN_EMPTY;
  opts.qos = AZ_MQTT_QOS_AT_MOST_ONCE;
  opts.retain = false;
  return opts;
}

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT3_TYPES_H
