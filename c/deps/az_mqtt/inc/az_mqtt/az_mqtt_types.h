// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_types.h
 * @brief Types common to az_mqttv3 and az_mqttv5: result codes, packet types, QoS, client state.
 */

#ifndef AZ_MQTT_TYPES_H
#define AZ_MQTT_TYPES_H

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

// ──────────────────────── Error codes ────────────────────────

enum az_mqtt_result
{
  AZ_MQTT_ERROR_PROTOCOL = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 10),
  AZ_MQTT_ERROR_MALFORMED_PACKET = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 11),
  AZ_MQTT_ERROR_BUFFER_TOO_SMALL = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 12),
  AZ_MQTT_ERROR_TRANSPORT = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 13),
  AZ_MQTT_ERROR_TIMEOUT = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 14),
  AZ_MQTT_ERROR_NOT_CONNECTED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 15),
  AZ_MQTT_ERROR_INVALID_STATE = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 16),
  /** @brief The request needs a capability this build does not have (e.g. TLS without a backend). */
  AZ_MQTT_ERROR_NOT_SUPPORTED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 17),
  /** @brief The options are inconsistent (e.g. a client certificate without its key). */
  AZ_MQTT_ERROR_INVALID_CONFIG = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 18),
  /** @brief No PINGRESP (or any packet) within the keep-alive interval after a PINGREQ. */
  AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 19),
  /** @brief The server ended the session with a DISCONNECT packet. */
  AZ_MQTT_ERROR_SERVER_DISCONNECTED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 20),
};

// ──────────────────────── Packet Types ───────────────────────

typedef enum
{
  AZ_MQTT_PACKET_TYPE_CONNECT = 1,
  AZ_MQTT_PACKET_TYPE_CONNACK = 2,
  AZ_MQTT_PACKET_TYPE_PUBLISH = 3,
  AZ_MQTT_PACKET_TYPE_PUBACK = 4,
  AZ_MQTT_PACKET_TYPE_PUBREC = 5,
  AZ_MQTT_PACKET_TYPE_PUBREL = 6,
  AZ_MQTT_PACKET_TYPE_PUBCOMP = 7,
  AZ_MQTT_PACKET_TYPE_SUBSCRIBE = 8,
  AZ_MQTT_PACKET_TYPE_SUBACK = 9,
  AZ_MQTT_PACKET_TYPE_UNSUBSCRIBE = 10,
  AZ_MQTT_PACKET_TYPE_UNSUBACK = 11,
  AZ_MQTT_PACKET_TYPE_PINGREQ = 12,
  AZ_MQTT_PACKET_TYPE_PINGRESP = 13,
  AZ_MQTT_PACKET_TYPE_DISCONNECT = 14,
  AZ_MQTT_PACKET_TYPE_AUTH = 15,
} az_mqtt_packet_type;

// ──────────────────────── QoS ────────────────────────────────

typedef enum
{
  AZ_MQTT_QOS_AT_MOST_ONCE = 0,
  AZ_MQTT_QOS_AT_LEAST_ONCE = 1,
  AZ_MQTT_QOS_EXACTLY_ONCE = 2,
} az_mqtt_qos;

// ──────────────────────── Client state ───────────────────────

typedef enum
{
  AZ_MQTT_CLIENT_STATE_DISCONNECTED = 0,
  AZ_MQTT_CLIENT_STATE_CONNECTING,
  AZ_MQTT_CLIENT_STATE_CONNECTED,
} az_mqtt_client_state;

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT_TYPES_H
