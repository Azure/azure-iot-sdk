// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_types.h
 * @brief MQTT 5.0 packet and option types.
 */

#ifndef AZ_MQTT5_TYPES_H
#define AZ_MQTT5_TYPES_H

#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

// ──────────────────────── Reason Codes ───────────────────────

/** @brief MQTT 5.0 reason codes. */
typedef enum
{
  AZ_MQTT5_REASON_SUCCESS = 0x00,
  AZ_MQTT5_REASON_NORMAL_DISCONNECTION = 0x00,
  AZ_MQTT5_REASON_GRANTED_QOS_0 = 0x00,
  AZ_MQTT5_REASON_GRANTED_QOS_1 = 0x01,
  AZ_MQTT5_REASON_GRANTED_QOS_2 = 0x02,
  AZ_MQTT5_REASON_DISCONNECT_WITH_WILL = 0x04,
  AZ_MQTT5_REASON_NO_MATCHING_SUBSCRIBERS = 0x10,
  AZ_MQTT5_REASON_NO_SUBSCRIPTION_EXISTED = 0x11,
  AZ_MQTT5_REASON_CONTINUE_AUTHENTICATION = 0x18,
  AZ_MQTT5_REASON_RE_AUTHENTICATE = 0x19,
  AZ_MQTT5_REASON_UNSPECIFIED_ERROR = 0x80,
  AZ_MQTT5_REASON_MALFORMED_PACKET = 0x81,
  AZ_MQTT5_REASON_PROTOCOL_ERROR = 0x82,
  AZ_MQTT5_REASON_IMPLEMENTATION_SPECIFIC_ERROR = 0x83,
  AZ_MQTT5_REASON_UNSUPPORTED_PROTOCOL_VERSION = 0x84,
  AZ_MQTT5_REASON_CLIENT_IDENTIFIER_NOT_VALID = 0x85,
  AZ_MQTT5_REASON_BAD_USER_NAME_OR_PASSWORD = 0x86,
  AZ_MQTT5_REASON_NOT_AUTHORIZED = 0x87,
  AZ_MQTT5_REASON_SERVER_UNAVAILABLE = 0x88,
  AZ_MQTT5_REASON_SERVER_BUSY = 0x89,
  AZ_MQTT5_REASON_BANNED = 0x8A,
  AZ_MQTT5_REASON_SERVER_SHUTTING_DOWN = 0x8B,
  AZ_MQTT5_REASON_BAD_AUTHENTICATION_METHOD = 0x8C,
  AZ_MQTT5_REASON_KEEP_ALIVE_TIMEOUT = 0x8D,
  AZ_MQTT5_REASON_SESSION_TAKEN_OVER = 0x8E,
  AZ_MQTT5_REASON_TOPIC_FILTER_INVALID = 0x8F,
  AZ_MQTT5_REASON_TOPIC_NAME_INVALID = 0x90,
  AZ_MQTT5_REASON_PACKET_IDENTIFIER_IN_USE = 0x91,
  AZ_MQTT5_REASON_PACKET_IDENTIFIER_NOT_FOUND = 0x92,
  AZ_MQTT5_REASON_RECEIVE_MAXIMUM_EXCEEDED = 0x93,
  AZ_MQTT5_REASON_TOPIC_ALIAS_INVALID = 0x94,
  AZ_MQTT5_REASON_PACKET_TOO_LARGE = 0x95,
  AZ_MQTT5_REASON_MESSAGE_RATE_TOO_HIGH = 0x96,
  AZ_MQTT5_REASON_QUOTA_EXCEEDED = 0x97,
  AZ_MQTT5_REASON_ADMINISTRATIVE_ACTION = 0x98,
  AZ_MQTT5_REASON_PAYLOAD_FORMAT_INVALID = 0x99,
  AZ_MQTT5_REASON_RETAIN_NOT_SUPPORTED = 0x9A,
  AZ_MQTT5_REASON_QOS_NOT_SUPPORTED = 0x9B,
  AZ_MQTT5_REASON_USE_ANOTHER_SERVER = 0x9C,
  AZ_MQTT5_REASON_SERVER_MOVED = 0x9D,
  AZ_MQTT5_REASON_SHARED_SUBSCRIPTIONS_NOT_SUPPORTED = 0x9E,
  AZ_MQTT5_REASON_CONNECTION_RATE_EXCEEDED = 0x9F,
  AZ_MQTT5_REASON_MAXIMUM_CONNECT_TIME = 0xA0,
  AZ_MQTT5_REASON_SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED = 0xA1,
  AZ_MQTT5_REASON_WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED = 0xA2,
} az_mqtt5_reason_code;

// ──────────────────────── Property IDs ───────────────────────

typedef enum
{
  AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR = 0x01,
  AZ_MQTT5_PROPERTY_MESSAGE_EXPIRY_INTERVAL = 0x02,
  AZ_MQTT5_PROPERTY_CONTENT_TYPE = 0x03,
  AZ_MQTT5_PROPERTY_RESPONSE_TOPIC = 0x08,
  AZ_MQTT5_PROPERTY_CORRELATION_DATA = 0x09,
  AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER = 0x0B,
  AZ_MQTT5_PROPERTY_SESSION_EXPIRY_INTERVAL = 0x11,
  AZ_MQTT5_PROPERTY_ASSIGNED_CLIENT_IDENTIFIER = 0x12,
  AZ_MQTT5_PROPERTY_SERVER_KEEP_ALIVE = 0x13,
  AZ_MQTT5_PROPERTY_AUTHENTICATION_METHOD = 0x15,
  AZ_MQTT5_PROPERTY_AUTHENTICATION_DATA = 0x16,
  AZ_MQTT5_PROPERTY_REQUEST_PROBLEM_INFORMATION = 0x17,
  AZ_MQTT5_PROPERTY_WILL_DELAY_INTERVAL = 0x18,
  AZ_MQTT5_PROPERTY_REQUEST_RESPONSE_INFORMATION = 0x19,
  AZ_MQTT5_PROPERTY_RESPONSE_INFORMATION = 0x1A,
  AZ_MQTT5_PROPERTY_SERVER_REFERENCE = 0x1C,
  AZ_MQTT5_PROPERTY_REASON_STRING = 0x1F,
  AZ_MQTT5_PROPERTY_RECEIVE_MAXIMUM = 0x21,
  AZ_MQTT5_PROPERTY_TOPIC_ALIAS_MAXIMUM = 0x22,
  AZ_MQTT5_PROPERTY_TOPIC_ALIAS = 0x23,
  AZ_MQTT5_PROPERTY_MAXIMUM_QOS = 0x24,
  AZ_MQTT5_PROPERTY_RETAIN_AVAILABLE = 0x25,
  AZ_MQTT5_PROPERTY_USER_PROPERTY = 0x26,
  AZ_MQTT5_PROPERTY_MAXIMUM_PACKET_SIZE = 0x27,
  AZ_MQTT5_PROPERTY_WILDCARD_SUBSCRIPTION_AVAILABLE = 0x28,
  AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER_AVAILABLE = 0x29,
  AZ_MQTT5_PROPERTY_SHARED_SUBSCRIPTION_AVAILABLE = 0x2A,
} az_mqtt5_property_id;

// ──────────────────────── User Property ──────────────────────

typedef struct
{
  az_span key;
  az_span value;
} az_mqtt5_user_property;

// ──────────────────────── Subscription Options ───────────────

typedef enum
{
  AZ_MQTT5_RETAIN_HANDLING_SEND_AT_SUBSCRIBE = 0,
  AZ_MQTT5_RETAIN_HANDLING_SEND_IF_NEW = 1,
  AZ_MQTT5_RETAIN_HANDLING_DO_NOT_SEND = 2,
} az_mqtt5_retain_handling;

typedef struct
{
  az_span topic_filter;
  az_mqtt_qos qos;
  bool no_local;
  bool retain_as_published;
  az_mqtt5_retain_handling retain_handling;
} az_mqtt5_subscription;

// ──────────────────────── CONNECT options ────────────────────

typedef struct
{
  az_span content_type;
  az_span response_topic;
  az_span correlation_data;
  az_span payload;
  az_span topic;
  az_mqtt_qos qos;
  bool retain;
  uint32_t will_delay_interval;
  uint32_t message_expiry_interval;
  uint8_t payload_format_indicator;
  az_mqtt5_user_property* user_properties;
  int32_t user_property_count;
} az_mqtt5_will_options;

typedef struct
{
  az_span client_id;
  az_span username;
  az_span password;
  uint16_t keep_alive_seconds;
  bool clean_start;
  uint32_t session_expiry_interval;
  uint16_t receive_maximum;
  uint32_t maximum_packet_size;
  uint16_t topic_alias_maximum;
  bool request_response_information;
  bool request_problem_information;
  az_mqtt5_user_property* user_properties;
  int32_t user_property_count;
  az_span authentication_method;
  az_span authentication_data;
  az_mqtt5_will_options* will;
} az_mqtt5_connect_options;

// ──────────────────────── CONNACK data ───────────────────────

typedef struct
{
  bool session_present;
  az_mqtt5_reason_code reason_code;
  uint32_t session_expiry_interval;
  uint16_t receive_maximum;
  uint8_t maximum_qos;
  bool retain_available;
  uint32_t maximum_packet_size;
  az_span assigned_client_identifier;
  uint16_t topic_alias_maximum;
  az_span reason_string;
  bool wildcard_subscription_available;
  bool subscription_identifier_available;
  bool shared_subscription_available;
  uint16_t server_keep_alive;
  /** @brief Whether the CONNACK carried Server Keep Alive (which then overrides the client's). */
  bool server_keep_alive_present;
  az_span response_information;
  az_span server_reference;
  az_span authentication_method;
  az_span authentication_data;
  az_mqtt5_user_property* user_properties;
  int32_t user_property_count;
  int32_t user_property_capacity;
} az_mqtt5_connack_data;

// ──────────────────────── PUBLISH data ───────────────────────

typedef struct
{
  az_span topic;
  az_span payload;
  az_mqtt_qos qos;
  bool retain;
  bool dup;
  uint16_t packet_id;
  uint8_t payload_format_indicator;
  uint32_t message_expiry_interval;
  uint16_t topic_alias;
  az_span response_topic;
  az_span correlation_data;
  az_span content_type;
  int32_t* subscription_identifiers;
  int32_t subscription_identifier_count;
  int32_t subscription_identifier_capacity; /**< Entries in @p subscription_identifiers; extras are dropped. */
  az_mqtt5_user_property* user_properties;
  int32_t user_property_count;
  int32_t user_property_capacity; /**< Entries in @p user_properties; extras are dropped. */
} az_mqtt5_publish_data;

// ──────────────────────── Publish options (for sending) ──────

typedef struct
{
  az_span topic;
  az_span payload;
  az_mqtt_qos qos;
  bool retain;
  uint8_t payload_format_indicator;
  uint32_t message_expiry_interval;
  uint16_t topic_alias;
  az_span response_topic;
  az_span correlation_data;
  az_span content_type;
  az_mqtt5_user_property* user_properties;
  int32_t user_property_count;
} az_mqtt5_publish_options;

// ──────────────────────── ACK data ───────────────────────────

typedef struct
{
  uint16_t packet_id;
  az_mqtt5_reason_code reason_code;
  az_span reason_string;
  az_mqtt5_user_property* user_properties;
  int32_t user_property_count;
  int32_t user_property_capacity;
} az_mqtt5_ack_data;

// ──────────────────────── SUBACK / UNSUBACK ──────────────────

typedef struct
{
  uint16_t packet_id;
  /** @brief One per topic filter, in order. */
  az_mqtt5_reason_code* reason_codes;
  /** @brief Entries stored; never exceeds reason_code_capacity (extra codes are dropped). */
  int32_t reason_code_count;
  int32_t reason_code_capacity;
  az_span reason_string;
  az_mqtt5_user_property* user_properties;
  int32_t user_property_count;
  int32_t user_property_capacity;
} az_mqtt5_suback_data;

// ──────────────────────── DISCONNECT data ────────────────────

typedef struct
{
  az_mqtt5_reason_code reason_code;
  uint32_t session_expiry_interval;
  az_span reason_string;
  az_span server_reference;
  az_mqtt5_user_property* user_properties;
  int32_t user_property_count;
  int32_t user_property_capacity;
} az_mqtt5_disconnect_data;

// ──────────────────────── AUTH data ──────────────────────────

typedef struct
{
  az_mqtt5_reason_code reason_code;
  az_span authentication_method;
  az_span authentication_data;
  az_span reason_string;
  az_mqtt5_user_property* user_properties;
  int32_t user_property_count;
  int32_t user_property_capacity;
} az_mqtt5_auth_data;

// ──────────────────────── Init helpers ───────────────────────

AZ_NODISCARD AZ_INLINE az_mqtt5_connect_options az_mqtt5_connect_options_default(void)
{
  az_mqtt5_connect_options opts;
  opts.client_id = AZ_SPAN_EMPTY;
  opts.username = AZ_SPAN_EMPTY;
  opts.password = AZ_SPAN_EMPTY;
  opts.keep_alive_seconds = 60;
  opts.clean_start = true;
  opts.session_expiry_interval = 0;
  opts.receive_maximum = 65535;
  opts.maximum_packet_size = 0; // 0 = no limit
  opts.topic_alias_maximum = 0;
  opts.request_response_information = false;
  opts.request_problem_information = true;
  opts.user_properties = NULL;
  opts.user_property_count = 0;
  opts.authentication_method = AZ_SPAN_EMPTY;
  opts.authentication_data = AZ_SPAN_EMPTY;
  opts.will = NULL;
  return opts;
}

AZ_NODISCARD AZ_INLINE az_mqtt5_publish_options az_mqtt5_publish_options_default(void)
{
  az_mqtt5_publish_options opts;
  opts.topic = AZ_SPAN_EMPTY;
  opts.payload = AZ_SPAN_EMPTY;
  opts.qos = AZ_MQTT_QOS_AT_MOST_ONCE;
  opts.retain = false;
  opts.payload_format_indicator = 0;
  opts.message_expiry_interval = 0;
  opts.topic_alias = 0;
  opts.response_topic = AZ_SPAN_EMPTY;
  opts.correlation_data = AZ_SPAN_EMPTY;
  opts.content_type = AZ_SPAN_EMPTY;
  opts.user_properties = NULL;
  opts.user_property_count = 0;
  return opts;
}

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT5_TYPES_H
