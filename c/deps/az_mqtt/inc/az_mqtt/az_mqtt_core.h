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

/** @brief Internal: called by the core when a session ends; see on_connection_closed. */
typedef void (*_az_mqtt_core_on_closed_fn)(az_mqtt_core* core, az_result reason);

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
    _az_mqtt_core_on_closed_fn on_closed;
    int64_t last_send_time_ms;
    int64_t last_receive_time_ms;
    int64_t ping_sent_time_ms;
    /** @brief Bytes buffered in receive_buffer. */
    int32_t recv_buf_pos;
    /** @brief Bumped whenever a session ends; guards against callbacks that reconnect. */
    uint32_t session_generation;
    az_mqtt_client_state state;
    uint16_t port;
    uint16_t next_packet_id;
    /** @brief Keep-alive in force (the server's, if it sent one). */
    uint16_t keep_alive_seconds;
    /** @brief A PINGREQ is awaiting its PINGRESP (or any other packet). */
    bool ping_outstanding;
  } _internal;
};

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT_CORE_H
