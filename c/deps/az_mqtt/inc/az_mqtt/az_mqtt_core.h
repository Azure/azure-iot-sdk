// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_core.h
 * @brief Session state shared by az_mqtt3_client and az_mqtt5_client (az_mqtt_core library).
 *
 * Embedded in both clients. Its fields are internal: use the version client APIs.
 */

#ifndef AZ_MQTT_CORE_H
#define AZ_MQTT_CORE_H

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

typedef struct az_mqtt_core az_mqtt_core;

/** @brief Bytes inflight_message_buffer needs per stored PUBLISH, besides the packet. */
#define AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD 18

/**
 * @brief One entry of a client's `inflight_control_buffer`: a request awaiting acknowledgement.
 * Fields are internal.
 */
typedef struct
{
  struct
  {
    uint16_t packet_id;
    uint8_t kind;
    /** @brief From an earlier connection: 0, or a _az_mqtt_inflight_mark (resend or drop). */
    uint8_t mark;
  } _internal;
} az_mqtt_inflight_entry;

/** @brief Internal: called by the core when a session ends; see on_connection_closed. */
typedef void (*_az_mqtt_core_on_closed_fn)(az_mqtt_core* core, az_result reason);

/** @brief Internal: called by the core for each native transport error; see on_transport_error. */
typedef void (*_az_mqtt_core_on_transport_error_fn)(
    az_mqtt_core* core,
    az_mqtt_native_error const* error);

/** @brief Connection, framing, keep-alive and session state of one client. Internal. */
struct az_mqtt_core
{
  struct
  {
    az_mqtt_transport* transport;
    az_span send_buffer;
    az_span receive_buffer;
    az_span hostname;
    az_mqtt_tls_options const* tls_options;
    /** @brief For logging; the transport uses it (az_mqtt_transport_set_proxy()). */
    az_mqtt_proxy_options const* proxy;
    _az_mqtt_core_on_closed_fn on_closed;
    _az_mqtt_core_on_transport_error_fn on_transport_error;
    /** @brief In-flight requests: az_mqtt_inflight_entry[] (caller storage), at most UINT16_MAX. */
    az_span inflight_control_buffer;
    /** @brief Stored PUBLISH packets awaiting acknowledgement (caller storage), oldest first. */
    az_span inflight_message_buffer;
    /** @brief Bytes of inflight_message_buffer in use. */
    int32_t inflight_message_used;
    int64_t last_send_time_ms;
    int64_t last_receive_time_ms;
    /** @brief CONNECTED: when the outstanding PINGREQ was sent. CONNECTING: connect deadline (-1: none). */
    int64_t timer_ms;
    /** @brief Bytes buffered in receive_buffer. */
    int32_t recv_buf_pos;
    /** @brief Bumped whenever a session ends; guards against callbacks that reconnect. */
    uint32_t session_generation;
    /** @brief Server Maximum Packet Size (mqttv5 CONNACK); 0: none. Reset when a session ends. */
    uint32_t server_maximum_packet_size;
    az_mqtt_client_state state;
    uint16_t port;
    uint16_t next_packet_id;
    /** @brief In-flight entries outgoing requests leave for inbound QoS 2 (mqttv5); 0: none. */
    uint16_t inbound_reserve;
    /** @brief Keep-alive in force (the server's, if it sent one). */
    uint16_t keep_alive_seconds;
    /** @brief A PINGREQ is awaiting its PINGRESP (or any other packet). */
    bool ping_outstanding;
    /** @brief CONNECTING: the transport is up and CONNECT was sent. */
    bool connect_sent;
    /** @brief The session outlives the connection: QoS 1/2 PUBLISH are stored for resending. */
    bool keep_messages;
  } _internal;
};

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT_CORE_H
