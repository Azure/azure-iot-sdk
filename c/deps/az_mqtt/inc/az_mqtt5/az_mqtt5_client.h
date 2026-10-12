// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_client.h
 * @brief MQTT 5.0 client API (az_mqttv5 library, on az_mqtt_core).
 *
 * Usage:
 *   1. Allocate az_mqtt5_client on the stack/static.
 *   2. Call az_mqtt5_client_init() with options and caller-owned buffers.
 *   3. Call az_mqtt5_client_connect() to establish the MQTT session.
 *   4. Use az_mqtt5_client_publish/subscribe/unsubscribe.
 *   5. Call az_mqtt5_client_process_loop() regularly to handle I/O and keepalive.
 *   6. Call az_mqtt5_client_disconnect() when done.
 *
 * Zero dynamic memory allocations. All buffers are provided by the caller.
 */

#ifndef AZ_MQTT5_CLIENT_H
#define AZ_MQTT5_CLIENT_H

#include <az_mqtt/az_mqtt_core.h>
#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>
#include <az_mqtt5/az_mqtt5_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <azure/core/_az_cfg_prefix.h>

// ──────────────────────── Callbacks ──────────────────────────

/** Forward declaration. */
typedef struct az_mqtt5_client az_mqtt5_client;

/**
 * @brief Called when a PUBLISH is received from the broker.
 */
typedef void (*az_mqtt5_on_publish_received_fn)(
    az_mqtt5_client* client,
    az_mqtt5_publish_data const* publish);

/**
 * @brief Called when a CONNACK is received; after any resends of a resumed session.
 */
typedef void (*az_mqtt5_on_connack_fn)(az_mqtt5_client* client, az_mqtt5_connack_data const* connack);

/**
 * @brief Called when a SUBACK is received for a SUBSCRIBE in flight.
 */
typedef void (*az_mqtt5_on_suback_fn)(az_mqtt5_client* client, az_mqtt5_suback_data const* suback);

/**
 * @brief Called when an UNSUBACK is received for an UNSUBSCRIBE in flight.
 */
typedef void (*az_mqtt5_on_unsuback_fn)(az_mqtt5_client* client, az_mqtt5_suback_data const* unsuback);

/**
 * @brief A QoS 1 PUBLISH in flight ended: PUBACK received (ack->status AZ_OK), or dropped
 * unacknowledged (ack->status says why; see az_mqtt5_client_options.inflight_message_buffer).
 */
typedef void (*az_mqtt5_on_puback_fn)(az_mqtt5_client* client, az_mqtt5_ack_data const* ack);

/**
 * @brief A QoS 2 exchange ended: PUBCOMP received; or a PUBREC with a reason code
 * of 0x80 or more (failed: no PUBREL is sent, @p ack is the PUBREC); or, for an
 * inbound one held in an in-flight entry, PUBREL received and PUBCOMP sent; or an outgoing one
 * was dropped unacknowledged (ack->status says why). ack->incoming tells the direction.
 */
typedef void (*az_mqtt5_on_pubcomp_fn)(az_mqtt5_client* client, az_mqtt5_ack_data const* ack);

/**
 * @brief Called when the broker sends a DISCONNECT.
 */
typedef void (*az_mqtt5_on_disconnect_fn)(az_mqtt5_client* client, az_mqtt5_disconnect_data const* disc);

/**
 * @brief Called whenever the client leaves CONNECTING or CONNECTED for DISCONNECTED.
 *
 * @p reason is AZ_OK after az_mqtt5_client_disconnect(), otherwise why the
 * session ended (e.g. AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT,
 * AZ_MQTT_ERROR_SERVER_DISCONNECTED, a transport error). The transport is
 * already closed when it runs, and the callback may call
 * az_mqtt5_client_connect() to reconnect.
 */
typedef void (*az_mqtt5_on_connection_closed_fn)(az_mqtt5_client* client, az_result reason);

/**
 * @brief Called for each native transport error (see az_mqtt_transport_error_fn), before the
 * failing call returns. It must not call into the client or its transport.
 */
typedef void (*az_mqtt5_on_transport_error_fn)(
    az_mqtt5_client* client,
    az_mqtt_native_error const* error);

// ──────────────────────── Client options ─────────────────────


typedef struct
{
  /** @brief Transport handle (caller-allocated). */
  az_mqtt_transport* transport;

  /** @brief TLS options (NULL for plain TCP). */
  az_mqtt_tls_options const* tls_options;

  /**
   * @brief HTTP proxy to connect through (NULL: none); see az_mqtt_transport_set_proxy().
   * Used, not copied: it must stay valid for the client's lifetime.
   */
  az_mqtt_proxy_options const* proxy_options;

  /** @brief Broker hostname. */
  az_span hostname;

  /** @brief Broker port. */
  uint16_t port;

  /** @brief Buffer for encoding outgoing packets. */
  az_span send_buffer;

  /** @brief Buffer for receiving incoming data; holds the largest packet expected. */
  az_span receive_buffer;

  /** @brief CONNECT options (client_id, keepalive, etc.). */
  az_mqtt5_connect_options connect_options;

  /**
   * @brief In-flight entries (az_mqtt_inflight_entry[]; at most UINT16_MAX used).
   *
   * Each QoS 1/2 PUBLISH, SUBSCRIBE and UNSUBSCRIBE holds an entry until acknowledged, and each
   * inbound QoS 2 PUBLISH until its PUBREL. A request with no free entry fails with
   * AZ_MQTT_ERROR_FLOW_CONTROL; an inbound QoS 2 PUBLISH with none is not delivered, and the
   * session ends with AZ_MQTT_ERROR_FLOW_CONTROL (after a best-effort DISCONNECT with Quota
   * exceeded). With connect_options.receive_maximum below the entry count, requests leave that
   * many entries for inbound QoS 2, so a server within it never ends the session this way.
   * May be empty if only QoS 0 is published and nothing is subscribed.
   * Acknowledgements for packet identifiers not in flight are ignored.
   *
   * When a connection ends, SUBSCRIBE and UNSUBSCRIBE in flight are abandoned; PUBLISH exchanges
   * are kept until the next accepted CONNACK (see inflight_message_buffer).
   */
  az_span inflight_control_buffer;

  // Callbacks (all optional, set to NULL if not needed)
  az_mqtt5_on_connack_fn on_connack;
  az_mqtt5_on_publish_received_fn on_publish;
  az_mqtt5_on_suback_fn on_suback;
  az_mqtt5_on_unsuback_fn on_unsuback;
  az_mqtt5_on_puback_fn on_puback;
  az_mqtt5_on_pubcomp_fn on_pubcomp;
  az_mqtt5_on_disconnect_fn on_disconnect;

  /** @brief User context pointer (passthrough, not used by the library). */
  void* user_context;

  /**
   * @brief User properties of a received packet (az_mqtt5_user_property[]), for every packet type:
   * one packet is decoded at a time. A callback's arrays point into it, valid until it returns or
   * reconnects (as its spans into receive_buffer). Extra ones are dropped. May be empty.
   */
  az_span decode_user_properties;
  /**
   * @brief Subscription identifiers (PUBLISH) or reason codes (SUBACK, UNSUBACK) of a received
   * packet (int32_t[]), shared like decode_user_properties. Extra ones are dropped. May be empty.
   */
  az_span decode_codes;
  /** @brief Optional. See az_mqtt5_on_connection_closed_fn. */
  az_mqtt5_on_connection_closed_fn on_connection_closed;
  /** @brief Optional. See az_mqtt5_on_transport_error_fn. */
  az_mqtt5_on_transport_error_fn on_transport_error;

  /**
   * @brief Copies of the QoS 1/2 PUBLISH awaiting acknowledgement (caller storage), resent when
   * the server resumes the session, as MQTT requires.
   *
   * Required for QoS 1/2 PUBLISH when the session outlives the connection (Clean Start 0 and a
   * Session Expiry Interval above 0): without it they fail with AZ_MQTT_ERROR_INVALID_CONFIG, and
   * with a Topic Alias with AZ_MQTT_ERROR_NOT_SUPPORTED. Otherwise unused: a device with clean
   * sessions can leave it empty to save the memory.
   *
   * If not empty, it must hold at least send_buffer + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD bytes (else
   * initialization fails with AZ_MQTT_ERROR_INVALID_CONFIG). Each stored PUBLISH takes its size +
   * AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD until PUBACK or PUBREC; with no room left, a PUBLISH fails
   * with AZ_MQTT_ERROR_OUT_OF_STORAGE.
   *
   * On the next accepted CONNACK, before on_connack:
   * - Session Present: each PUBREL is resent, then each stored PUBLISH, oldest first, with DUP, its
   *   packet identifier and the Message Expiry Interval left, within the new Receive Maximum (the
   *   rest as acknowledgements free room). Dropped instead: one whose Message Expiry Interval
   *   elapsed (AZ_MQTT_ERROR_MESSAGE_EXPIRED; MQTT itself would resend it), or a PUBLISH or PUBREL
   *   over the new Maximum Packet Size (AZ_MQTT_ERROR_PACKET_TOO_LARGE).
   * - Otherwise each outgoing QoS 1/2 exchange is dropped (AZ_MQTT_ERROR_SESSION_NOT_RESUMED).
   *
   * Each drop is logged and reported to on_puback (QoS 1) or on_pubcomp (QoS 2) with that status.
   * Kept in memory only: not across a restart.
   */
  az_span inflight_message_buffer;
} az_mqtt5_client_options;

// ──────────────────────── Client ─────────────────────────────

/** @brief MQTT 5.0 client. Fields are internal. */
struct az_mqtt5_client
{
  struct
  {
    /** @brief Connection, framing, keep-alive and session state (az_mqtt_core); first. */
    az_mqtt_core core;
    az_mqtt5_connect_options connect_options;
    az_span decode_user_properties;
    az_span decode_codes;
    az_mqtt5_on_connack_fn on_connack;
    az_mqtt5_on_publish_received_fn on_publish;
    az_mqtt5_on_suback_fn on_suback;
    az_mqtt5_on_unsuback_fn on_unsuback;
    az_mqtt5_on_puback_fn on_puback;
    az_mqtt5_on_pubcomp_fn on_pubcomp;
    az_mqtt5_on_disconnect_fn on_disconnect;
    az_mqtt5_on_connection_closed_fn on_connection_closed;
    az_mqtt5_on_transport_error_fn on_transport_error;
    void* user_context;
    /** @brief From the accepted CONNACK (MQTT 5.0 defaults when absent); see also the core. */
    uint16_t server_receive_maximum;
    uint16_t server_topic_alias_maximum;
    uint8_t server_maximum_qos;
    bool server_retain_available;
  } _internal;
};

// ──────────────────────── API ────────────────────────────────

/**
 * @brief Initialize the client.
 *
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG, AZ_MQTT_ERROR_NOT_SUPPORTED options->proxy_options was
 *         refused (az_mqtt_transport_set_proxy()); the client is not initialized.
 */
AZ_NODISCARD az_result az_mqtt5_client_init(az_mqtt5_client* client, az_mqtt5_client_options const* options);

/**
 * @brief Connect to the broker (TCP + optional TLS + MQTT CONNECT).
 *
 * Blocks until the CONNACK arrives; @p timeout_ms bounds the whole sequence
 * (AZ_MQTT_ERROR_TIMEOUT; -1: no bound). A refused CONNACK returns
 * AZ_MQTT_ERROR_NOT_CONNECTED after on_connack reports its reason code.
 * Equivalent to az_mqtt5_client_connect_start() followed by
 * az_mqtt5_client_process_loop() until the state leaves CONNECTING.
 *
 * @retval AZ_MQTT_ERROR_INVALID_STATE Not DISCONNECTED.
 * @retval AZ_ERROR_ARG CONNECT cannot be encoded (az_mqtt5_codec_encode_connect()).
 */
AZ_NODISCARD az_result az_mqtt5_client_connect(az_mqtt5_client* client, int32_t timeout_ms);

/**
 * @brief Start connecting without waiting.
 *
 * Returns once the TCP connect has started; only name resolution may block
 * (see az_mqtt_transport_connect_start()). az_mqtt5_client_process_loop() then
 * completes the TCP/TLS connect, sends CONNECT and handles the CONNACK, each call
 * waiting for the peer no longer than its own timeout. Sending CONNECT, like
 * every send, is bounded by AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS (over WebSockets,
 * twice that) instead; it waits only if CONNECT exceeds the socket send buffer.
 * The state is CONNECTING until an accepted CONNACK makes it CONNECTED
 * (on_connack runs first).
 *
 * @p timeout_ms bounds the whole sequence (-1: no bound). If it expires, the
 * CONNACK refuses, or anything fails, the session ends: process_loop returns the
 * result (AZ_MQTT_ERROR_TIMEOUT, AZ_MQTT_ERROR_NOT_CONNECTED, ...) and
 * on_connection_closed reports it. az_mqtt5_client_disconnect() cancels.
 *
 * @retval AZ_OK Started.
 * @retval AZ_MQTT_ERROR_INVALID_STATE Not DISCONNECTED.
 * @retval AZ_ERROR_ARG CONNECT cannot be encoded (az_mqtt5_codec_encode_connect()).
 */
AZ_NODISCARD az_result
az_mqtt5_client_connect_start(az_mqtt5_client* client, int32_t timeout_ms);

/**
 * @brief Run the I/O processing loop once.
 *
 * Handles every complete packet available (up to a bound), dispatches
 * callbacks, and sends PINGREQ when due. While CONNECTING, also progresses the
 * connect (see az_mqtt5_client_connect_start()). Returns early when keep-alive needs
 * attention, so a long @p timeout_ms never delays a PINGREQ.
 *
 * Any failure but AZ_MQTT_ERROR_NOT_CONNECTED and AZ_MQTT_ERROR_INVALID_STATE ends the session:
 * the transport is closed, the state becomes DISCONNECTED and on_connection_closed reports the
 * same result.
 *
 * @param timeout_ms  Max time to wait for incoming data (or connect progress);
 *                    -1 waits until keep-alive is due. Sends are bounded by
 *                    AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS (over WebSockets, twice that), not
 *                    by this.
 * @retval AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT A PINGREQ got nothing back within the keep-alive.
 * @retval AZ_MQTT_ERROR_NOT_CONNECTED Called while disconnected.
 * @retval AZ_MQTT_ERROR_INVALID_STATE Called from a callback of a received packet of this session
 *         (that packet is still being handled): nothing done; the session continues.
 */
AZ_NODISCARD az_result az_mqtt5_client_process_loop(az_mqtt5_client* client, int32_t timeout_ms);

/**
 * @brief Publish a message.
 *
 * QoS 1/2 holds an in-flight entry until PUBACK / PUBCOMP (see options.inflight_control_buffer),
 * and a stored copy until PUBACK / PUBREC (see options.inflight_message_buffer).
 *
 * @param[out] out_packet_id  Packet ID assigned (for QoS > 0). Can be NULL.
 * @retval AZ_MQTT_ERROR_FLOW_CONTROL No free entry, the server's Receive Maximum is reached, or
 *         (QoS 1/2) an earlier PUBLISH still awaits its resend after a resume.
 * @retval AZ_MQTT_ERROR_OUT_OF_STORAGE, AZ_MQTT_ERROR_INVALID_CONFIG See inflight_message_buffer.
 * @retval AZ_ERROR_ARG Topic with a wildcard ('+', '#'), not valid UTF-8, or empty without
 *         topic_alias; response_topic with a wildcard or not valid UTF-8.
 * @retval AZ_MQTT_ERROR_NOT_SUPPORTED QoS above the server's Maximum QoS, retain without
 *         Retain Available, a Topic Alias above its Topic Alias Maximum, or (QoS 1/2) one on a
 *         session that outlives the connection.
 * @retval AZ_MQTT_ERROR_PACKET_TOO_LARGE Over the server's Maximum Packet Size.
 */
AZ_NODISCARD az_result az_mqtt5_client_publish(
    az_mqtt5_client* client,
    az_mqtt5_publish_options const* options,
    uint16_t* out_packet_id);

/**
 * @brief Subscribe to topic(s). Holds an in-flight entry until SUBACK.
 * @param[out] out_packet_id  Packet ID assigned. Can be NULL.
 * @retval AZ_MQTT_ERROR_FLOW_CONTROL No free in-flight entry.
 * @retval AZ_ERROR_ARG No or over 65,535 Topic Filters, or one empty, with a misplaced wildcard or
 *         not valid UTF-8.
 * @retval AZ_MQTT_ERROR_PACKET_TOO_LARGE Over the server's Maximum Packet Size.
 */
AZ_NODISCARD az_result az_mqtt5_client_subscribe(
    az_mqtt5_client* client,
    az_mqtt5_subscription const* subscriptions,
    int32_t subscription_count,
    uint16_t* out_packet_id);

/**
 * @brief Unsubscribe from topic(s). Holds an in-flight entry until UNSUBACK.
 * @param[out] out_packet_id  Packet ID assigned. Can be NULL.
 * @retval AZ_MQTT_ERROR_FLOW_CONTROL No free in-flight entry.
 * @retval AZ_ERROR_ARG No or over 65,535 Topic Filters, or one empty, with a misplaced wildcard or
 *         not valid UTF-8.
 * @retval AZ_MQTT_ERROR_PACKET_TOO_LARGE Over the server's Maximum Packet Size.
 */
AZ_NODISCARD az_result az_mqtt5_client_unsubscribe(
    az_mqtt5_client* client,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t* out_packet_id);

/**
 * @brief Send a DISCONNECT and close the transport.
 */
AZ_NODISCARD az_result az_mqtt5_client_disconnect(
    az_mqtt5_client* client,
    az_mqtt5_reason_code reason_code);

/**
 * @brief Get current client state.
 */
AZ_NODISCARD AZ_INLINE az_mqtt_client_state az_mqtt5_client_get_state(az_mqtt5_client const* client)
{
  return client->_internal.core._internal.state;
}

/**
 * @brief Get the user context pointer.
 */
AZ_NODISCARD AZ_INLINE void* az_mqtt5_client_get_user_context(az_mqtt5_client const* client)
{
  return client->_internal.user_context;
}

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT5_CLIENT_H
