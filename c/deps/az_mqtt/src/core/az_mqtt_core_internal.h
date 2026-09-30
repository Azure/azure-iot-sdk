// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_core_internal.h
 * @brief Internal: session engine used by the az_mqttv3 and az_mqttv5 clients.
 *
 * The core owns the transport, packet framing, the receive buffer, keep-alive
 * and session state. The version client encodes and decodes packets and
 * reports them to the application.
 */

#ifndef AZ_MQTT_CORE_INTERNAL_H
#define AZ_MQTT_CORE_INTERNAL_H

#include <az_mqtt/az_mqtt_core.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>

/**
 * @brief Handle one received packet (body excludes the fixed header).
 *
 * @p core is the first member of the version client. May end the session
 * (_az_mqtt_core_close()); the core then stops using the receive buffer.
 */
typedef az_result (*_az_mqtt_core_dispatch_fn)(
    az_mqtt_core* core,
    az_mqtt_packet_type type,
    uint8_t flags,
    az_span body);

/**
 * @brief Start connecting: state CONNECTING, transport connect started.
 *
 * The CONNECT packet must already be encoded at the start of the send buffer;
 * it stays there until sent (nothing else uses the send buffer while
 * CONNECTING). _az_mqtt_core_process_loop() then completes the transport
 * connect, sends CONNECT and dispatches the CONNACK; its dispatch sets state
 * CONNECTED (and keep_alive_seconds) on acceptance. @p timeout_ms (-1: none)
 * bounds the whole sequence. The session is closed on failure.
 */
AZ_NODISCARD az_result _az_mqtt_core_connect_start(az_mqtt_core* core, int32_t timeout_ms);

/**
 * @brief Run the process loop until a started connect completes.
 *
 * @return AZ_OK once connected; AZ_MQTT_ERROR_NOT_CONNECTED if the CONNACK
 * refused or a callback ended the session; else the failure.
 */
AZ_NODISCARD az_result
_az_mqtt_core_connect_wait(az_mqtt_core* core, _az_mqtt_core_dispatch_fn dispatch);

/**
 * @brief Keep-alive, connect progress and dispatch of received packets; see
 * az_mqtt5_client_process_loop().
 */
AZ_NODISCARD az_result _az_mqtt_core_process_loop(
    az_mqtt_core* core,
    int32_t timeout_ms,
    _az_mqtt_core_dispatch_fn dispatch);

/**
 * @brief Send the send-buffer bytes an encoder wrote, @p remaining being what it left.
 *
 * Does not close on failure.
 */
AZ_NODISCARD az_result _az_mqtt_core_send(
    az_mqtt_core* core,
    az_span remaining);

/**
 * @brief End the session: close the transport, reset state and, if it was
 * connecting or connected, call on_closed with @p reason.
 */
void _az_mqtt_core_close(
    az_mqtt_core* core,
    az_result reason);

/**
 * @brief As _az_mqtt_core_send(), but closes the session if the send fails (a
 * partial packet may be on the wire).
 */
AZ_NODISCARD AZ_INLINE az_result _az_mqtt_core_send_request(az_mqtt_core* core, az_span remaining)
{
  az_result rc = _az_mqtt_core_send(core, remaining);
  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc);
  }
  return rc;
}

/** @brief Next non-zero packet identifier. */
AZ_INLINE uint16_t _az_mqtt_core_next_packet_id(az_mqtt_core* core)
{
  if (++core->_internal.next_packet_id == 0)
  {
    core->_internal.next_packet_id = 1;
  }
  return core->_internal.next_packet_id;
}

#endif // AZ_MQTT_CORE_INTERNAL_H
