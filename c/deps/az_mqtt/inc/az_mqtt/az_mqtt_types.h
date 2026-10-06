// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_types.h
 * @brief Types common to az_mqttv3 and az_mqttv5: result codes, packet types, QoS, client state.
 */

#ifndef AZ_MQTT_TYPES_H
#define AZ_MQTT_TYPES_H

#include <azure/core/az_log.h>
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
  /** @brief A transport failure with no more specific code. */
  AZ_MQTT_ERROR_TRANSPORT = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 13),
  AZ_MQTT_ERROR_TIMEOUT = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 14),
  AZ_MQTT_ERROR_NOT_CONNECTED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 15),
  AZ_MQTT_ERROR_INVALID_STATE = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 16),
  /**
   * @brief The request needs a capability this build, or the server, does not have (e.g. TLS
   * without a backend; an MQTT 5.0 server's Maximum QoS, Retain Available or Topic Alias Maximum).
   */
  AZ_MQTT_ERROR_NOT_SUPPORTED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 17),
  /** @brief The options are inconsistent (e.g. a client certificate without its key). */
  AZ_MQTT_ERROR_INVALID_CONFIG = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 18),
  /** @brief No PINGRESP (or any packet) within the keep-alive interval after a PINGREQ. */
  AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 19),
  /** @brief The server ended the session with a DISCONNECT packet. */
  AZ_MQTT_ERROR_SERVER_DISCONNECTED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 20),
  /**
   * @brief No in-flight entry is free, the MQTT 5.0 server's Receive Maximum is reached, or an
   * earlier PUBLISH still awaits its resend. Nothing was sent; retry after an acknowledgement.
   * As the reason a session ended: an inbound QoS 2 PUBLISH found no free in-flight entry; it was
   * neither delivered nor acknowledged (MQTT 5.0: DISCONNECT with Quota exceeded was sent).
   */
  AZ_MQTT_ERROR_FLOW_CONTROL = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 21),
  /**
   * @brief The packet exceeds the MQTT 5.0 server's Maximum Packet Size. Nothing was sent; for
   * an acknowledgement, the session is closed with this reason.
   */
  AZ_MQTT_ERROR_PACKET_TOO_LARGE = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 22),
  /** @brief The host name could not be resolved. */
  AZ_MQTT_ERROR_NAME_RESOLUTION = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 23),
  /** @brief The server refused the TCP connection (the last address tried did). */
  AZ_MQTT_ERROR_CONNECTION_REFUSED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 24),
  /** @brief The TLS handshake failed for a reason other than certificate verification. */
  AZ_MQTT_ERROR_TLS_HANDSHAKE = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 25),
  /** @brief The server certificate chain or host name did not verify. */
  AZ_MQTT_ERROR_TLS_VERIFY = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 26),
  /** @brief The peer closed or reset an established connection. */
  AZ_MQTT_ERROR_CONNECTION_CLOSED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 27),
  /** @brief The HTTP proxy did not open the tunnel: a non-2xx reply, a malformed one, or none. */
  AZ_MQTT_ERROR_PROXY = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 28),
  /** @brief The HTTP proxy requires other credentials (407 Proxy Authentication Required). */
  AZ_MQTT_ERROR_PROXY_AUTH = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 29),
  /**
   * @brief The server refused the WebSocket upgrade or answered it invalidly, or sent a frame
   * RFC 6455 does not allow.
   */
  AZ_MQTT_ERROR_WEBSOCKET = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 30),
  /**
   * @brief No room in inflight_message_buffer for the PUBLISH now. Nothing was sent; retry after
   * an acknowledgement frees some.
   */
  AZ_MQTT_ERROR_OUT_OF_STORAGE = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 31),
  /** @brief A PUBLISH was dropped unacknowledged: its expiry interval elapsed before a resend. */
  AZ_MQTT_ERROR_MESSAGE_EXPIRED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 32),
  /**
   * @brief A QoS 1/2 exchange was dropped unacknowledged: the server did not resume the session,
   * or the client kept no copy to resend (the session was not set to outlive the connection).
   */
  AZ_MQTT_ERROR_SESSION_NOT_RESUMED = _az_RESULT_MAKE_ERROR(_az_FACILITY_IOT_MQTT, 33),
};

// ──────────────────────── Log classifications ────────────────

/**
 * @brief az_log classifications (azure/core/az_log.h). Messages never contain credentials,
 * keys, certificates, topics or payloads.
 */
enum az_mqtt_log_classification
{
  /** @brief Connect started (host and port), session closed (result and native error). */
  AZ_LOG_MQTT_CONNECTION = _az_LOG_MAKE_CLASSIFICATION(_az_FACILITY_IOT_MQTT, 16),
  /** @brief Packet sent or received: type and length. */
  AZ_LOG_MQTT_PACKET = _az_LOG_MAKE_CLASSIFICATION(_az_FACILITY_IOT_MQTT, 17),
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
