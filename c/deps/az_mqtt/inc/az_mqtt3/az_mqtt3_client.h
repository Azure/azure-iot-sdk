// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt3_client.h
 * @brief MQTT 3.1.1 client API (az_mqttv3 library, on az_mqtt_core).
 *
 * Usage:
 *   1. Allocate az_mqtt3_client on the stack/static.
 *   2. Call az_mqtt3_client_init() with options and caller-owned buffers.
 *   3. Call az_mqtt3_client_connect() to establish the MQTT session.
 *   4. Use az_mqtt3_client_publish/subscribe/unsubscribe.
 *   5. Call az_mqtt3_client_process_loop() regularly to handle I/O and keepalive.
 *   6. Call az_mqtt3_client_disconnect() when done.
 *
 * Zero dynamic memory allocations. All buffers are provided by the caller.
 */

#ifndef AZ_MQTT3_CLIENT_H
#define AZ_MQTT3_CLIENT_H

#include <az_mqtt/az_mqtt_core.h>
#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>
#include <az_mqtt3/az_mqtt3_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <azure/core/_az_cfg_prefix.h>

// ──────────────────────── Callbacks ──────────────────────────

typedef struct az_mqtt3_client az_mqtt3_client;

/** @brief A CONNACK arrived (accepted or not). */
typedef void (*az_mqtt3_on_connack_fn)(az_mqtt3_client* client, az_mqtt3_connack_data const* connack);

/** @brief A PUBLISH arrived; its PUBACK / PUBREC is sent by the client. */
typedef void (*az_mqtt3_on_publish_received_fn)(
    az_mqtt3_client* client,
    az_mqtt3_publish_data const* publish);

/** @brief A SUBACK arrived. */
typedef void (*az_mqtt3_on_suback_fn)(az_mqtt3_client* client, az_mqtt3_suback_data const* suback);

/** @brief An UNSUBACK arrived. */
typedef void (*az_mqtt3_on_unsuback_fn)(az_mqtt3_client* client, az_mqtt3_ack_data const* unsuback);

/** @brief A PUBACK arrived (QoS 1 publish complete). */
typedef void (*az_mqtt3_on_puback_fn)(az_mqtt3_client* client, az_mqtt3_ack_data const* ack);

/**
 * @brief A QoS 2 exchange completed: PUBCOMP received (outgoing) or PUBREL
 * received and PUBCOMP sent (incoming).
 */
typedef void (*az_mqtt3_on_pubcomp_fn)(az_mqtt3_client* client, az_mqtt3_ack_data const* ack);

/**
 * @brief The session ended.
 *
 * @p reason is AZ_OK after az_mqtt3_client_disconnect(), otherwise why the
 * session ended (e.g. AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT, a transport error).
 * The transport is already closed when it runs, and the callback may call
 * az_mqtt3_client_connect() to reconnect.
 */
typedef void (*az_mqtt3_on_connection_closed_fn)(az_mqtt3_client* client, az_result reason);

// ──────────────────────── Client options ─────────────────────

typedef struct
{
  /** @brief Transport handle (caller-allocated). */
  az_mqtt_transport* transport;

  /** @brief TLS options (NULL for plain TCP). */
  az_mqtt_tls_options const* tls_options;

  /** @brief Broker hostname. */
  az_span hostname;

  /** @brief Broker port. */
  uint16_t port;

  /** @brief Buffer for encoding outgoing packets. */
  az_span send_buffer;

  /** @brief Buffer for receiving incoming data; holds the largest packet expected. */
  az_span receive_buffer;

  /** @brief CONNECT options (client_id, keep-alive, etc.). */
  az_mqtt3_connect_options connect_options;

  // Callbacks (all optional, set to NULL if not needed)
  az_mqtt3_on_connack_fn on_connack;
  az_mqtt3_on_publish_received_fn on_publish;
  az_mqtt3_on_suback_fn on_suback;
  az_mqtt3_on_unsuback_fn on_unsuback;
  az_mqtt3_on_puback_fn on_puback;
  az_mqtt3_on_pubcomp_fn on_pubcomp;
  /** @brief Optional. See az_mqtt3_on_connection_closed_fn. */
  az_mqtt3_on_connection_closed_fn on_connection_closed;

  /** @brief User context pointer (passthrough, not used by the library). */
  void* user_context;
} az_mqtt3_client_options;

// ──────────────────────── Client ─────────────────────────────

/** @brief MQTT 3.1.1 client. Fields are internal. */
struct az_mqtt3_client
{
  struct
  {
    /** @brief Connection, framing, keep-alive and session state (az_mqtt_core); first. */
    az_mqtt_core core;
    az_mqtt3_connect_options connect_options;
    az_mqtt3_on_connack_fn on_connack;
    az_mqtt3_on_publish_received_fn on_publish;
    az_mqtt3_on_suback_fn on_suback;
    az_mqtt3_on_unsuback_fn on_unsuback;
    az_mqtt3_on_puback_fn on_puback;
    az_mqtt3_on_pubcomp_fn on_pubcomp;
    az_mqtt3_on_connection_closed_fn on_connection_closed;
    void* user_context;
  } _internal;
};

// ──────────────────────── API ────────────────────────────────

/** @brief Initialize the client. */
AZ_NODISCARD az_result
az_mqtt3_client_init(az_mqtt3_client* client, az_mqtt3_client_options const* options);

/**
 * @brief Connect to the broker (TCP + optional TLS + MQTT CONNECT).
 *
 * Blocks until the CONNACK arrives; @p timeout_ms bounds the whole sequence
 * (AZ_MQTT_ERROR_TIMEOUT; -1: no bound). A refused CONNACK returns
 * AZ_MQTT_ERROR_NOT_CONNECTED after on_connack reports its return code.
 * Equivalent to az_mqtt3_client_connect_start() followed by
 * az_mqtt3_client_process_loop() until the state leaves CONNECTING.
 *
 * @retval AZ_MQTT_ERROR_INVALID_STATE Not DISCONNECTED.
 */
AZ_NODISCARD az_result az_mqtt3_client_connect(az_mqtt3_client* client, int32_t timeout_ms);

/**
 * @brief Start connecting without waiting.
 *
 * Returns once the TCP connect has started; only name resolution may block
 * (see az_mqtt_transport_connect_start()). az_mqtt3_client_process_loop() then
 * completes the TCP/TLS connect, sends CONNECT and handles the CONNACK, each call
 * waiting no longer than its own timeout. The state is CONNECTING until an
 * accepted CONNACK makes it CONNECTED (on_connack runs first).
 *
 * @p timeout_ms bounds the whole sequence (-1: no bound). If it expires, the
 * CONNACK refuses, or anything fails, the session ends: process_loop returns the
 * result (AZ_MQTT_ERROR_TIMEOUT, AZ_MQTT_ERROR_NOT_CONNECTED, ...) and
 * on_connection_closed reports it. az_mqtt3_client_disconnect() cancels.
 *
 * @retval AZ_OK Started.
 * @retval AZ_MQTT_ERROR_INVALID_STATE Not DISCONNECTED.
 */
AZ_NODISCARD az_result
az_mqtt3_client_connect_start(az_mqtt3_client* client, int32_t timeout_ms);

/**
 * @brief Run the I/O processing loop once.
 *
 * Handles every complete packet available (up to a bound), dispatches
 * callbacks, and sends PINGREQ when due. While CONNECTING, also progresses the
 * connect (see az_mqtt3_client_connect_start()). Returns early when keep-alive needs
 * attention, so a long @p timeout_ms never delays a PINGREQ.
 *
 * Any failure ends the session: the transport is closed, the state becomes
 * DISCONNECTED and on_connection_closed reports the same result.
 *
 * @param timeout_ms  Max time to wait for incoming data; -1 waits until keep-alive is due.
 * @retval AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT A PINGREQ got nothing back within the keep-alive.
 * @retval AZ_MQTT_ERROR_NOT_CONNECTED Called while disconnected.
 */
AZ_NODISCARD az_result az_mqtt3_client_process_loop(az_mqtt3_client* client, int32_t timeout_ms);

/**
 * @brief Publish a message.
 * @param[out] out_packet_id  Packet ID assigned (for QoS > 0). Can be NULL.
 */
AZ_NODISCARD az_result az_mqtt3_client_publish(
    az_mqtt3_client* client,
    az_mqtt3_publish_options const* options,
    uint16_t* out_packet_id);

/**
 * @brief Subscribe to topic(s).
 * @param[out] out_packet_id  Packet ID assigned. Can be NULL.
 */
AZ_NODISCARD az_result az_mqtt3_client_subscribe(
    az_mqtt3_client* client,
    az_mqtt3_subscription const* subscriptions,
    int32_t subscription_count,
    uint16_t* out_packet_id);

/**
 * @brief Unsubscribe from topic(s).
 * @param[out] out_packet_id  Packet ID assigned. Can be NULL.
 */
AZ_NODISCARD az_result az_mqtt3_client_unsubscribe(
    az_mqtt3_client* client,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t* out_packet_id);

/** @brief Send a DISCONNECT and close the transport. */
AZ_NODISCARD az_result az_mqtt3_client_disconnect(az_mqtt3_client* client);

/** @brief Get current client state. */
AZ_NODISCARD AZ_INLINE az_mqtt_client_state az_mqtt3_client_get_state(az_mqtt3_client const* client)
{
  return client->_internal.core._internal.state;
}

/** @brief Get the user context pointer. */
AZ_NODISCARD AZ_INLINE void* az_mqtt3_client_get_user_context(az_mqtt3_client const* client)
{
  return client->_internal.user_context;
}

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT3_CLIENT_H
