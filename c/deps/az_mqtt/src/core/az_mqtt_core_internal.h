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

#include <stdbool.h>
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
 * @retval AZ_MQTT_ERROR_PACKET_TOO_LARGE Over the server's Maximum Packet Size; nothing sent.
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

/**
 * @brief Have the transport's native errors logged and passed to on_transport_error. Call once
 * the transport is set.
 */
void _az_mqtt_core_register_transport_errors(az_mqtt_core* core);

/** @brief What an in-flight entry holds. PUBLISH_QOS1 to PUBREL are what Receive Maximum counts. */
typedef enum
{
  _AZ_MQTT_INFLIGHT_FREE = 0,
  /** @brief PUBLISH QoS 1 sent; awaiting PUBACK. */
  _AZ_MQTT_INFLIGHT_PUBLISH_QOS1,
  /** @brief PUBLISH QoS 2 sent; awaiting PUBREC. */
  _AZ_MQTT_INFLIGHT_PUBLISH_QOS2,
  /** @brief PUBREL sent; awaiting PUBCOMP. */
  _AZ_MQTT_INFLIGHT_PUBREL,
  _AZ_MQTT_INFLIGHT_SUBSCRIBE,
  _AZ_MQTT_INFLIGHT_UNSUBSCRIBE,
  /** @brief PUBLISH QoS 2 received, PUBREC sent; awaiting PUBREL. Server's packet identifier. */
  _AZ_MQTT_INFLIGHT_INBOUND_QOS2,
} _az_mqtt_inflight_kind;

/** @brief az_mqtt_inflight_entry mark of an outgoing QoS 1/2 exchange of an earlier connection. */
typedef enum
{
  _AZ_MQTT_INFLIGHT_MARK_NONE = 0,
  /** @brief The session resumed: to be resent. */
  _AZ_MQTT_INFLIGHT_MARK_RESEND,
  /** @brief The session did not resume: to be reported dropped. */
  _AZ_MQTT_INFLIGHT_MARK_STALE,
} _az_mqtt_inflight_mark;

/**
 * @brief Use @p entries (az_mqtt_inflight_entry[]; at most UINT16_MAX used) and @p messages
 * (stored PUBLISH packets); all free. Entries in use stay first, in reservation order. Call
 * once send_buffer is set.
 *
 * @param keep_messages The session outlives the connection: QoS 1/2 PUBLISH need @p messages.
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG @p messages is neither empty nor at least
 * send_buffer + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD bytes.
 */
AZ_NODISCARD az_result _az_mqtt_core_inflight_init(
    az_mqtt_core* core,
    az_span entries,
    az_span messages,
    bool keep_messages);

/**
 * @brief Reserve a free entry for an outgoing request, with a packet identifier no
 * other outgoing request holds.
 *
 * @param publish_limit Fail if this many outgoing QoS 1/2 PUBLISH exchanges are
 * incomplete on this connection (the server's Receive Maximum); UINT16_MAX for none.
 * @retval AZ_MQTT_ERROR_FLOW_CONTROL No free entry, @p publish_limit reached, or (for a PUBLISH)
 * an earlier one still awaits its resend: none overtakes it.
 */
AZ_NODISCARD az_result _az_mqtt_core_inflight_reserve_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t publish_limit,
    az_mqtt_inflight_entry** out_entry);

/** @brief The entry of @p kind holding @p packet_id, or NULL. */
AZ_NODISCARD az_mqtt_inflight_entry* _az_mqtt_core_inflight_find_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t packet_id);

/** @brief Free the entry of @p kind holding @p packet_id; whether there was one. */
AZ_NODISCARD bool _az_mqtt_core_inflight_release_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t packet_id);

/**
 * @brief Track an inbound QoS 2 PUBLISH until its PUBREL.
 *
 * @param[out] out_is_duplicate Whether @p packet_id already awaits PUBREL (do not
 * deliver it again). Without a free entry nothing is tracked, so a resent
 * duplicate would be delivered again.
 */
void _az_mqtt_core_inflight_track_inbound_qos2(
    az_mqtt_core* core,
    uint16_t packet_id,
    bool* out_is_duplicate);

/** @brief Encode a PUBREL (success) for @p packet_id at the start of @p dest. */
typedef az_result (*_az_mqtt_core_encode_pubrel_fn)(az_span* dest, uint16_t packet_id);

/**
 * @brief An outgoing QoS 1/2 exchange (@p qos1: QoS 1) ended without acknowledgement, because
 * of @p status. Its entry is already free.
 */
typedef void (*_az_mqtt_core_publish_dropped_fn)(
    az_mqtt_core* core,
    uint16_t packet_id,
    bool qos1,
    az_result status);

/**
 * @brief Where to encode a QoS 1/2 PUBLISH: the send buffer, or, if keep_messages, the free part
 * of inflight_message_buffer (at most send_buffer's size).
 *
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG keep_messages without inflight_message_buffer.
 * @retval AZ_MQTT_ERROR_OUT_OF_STORAGE No room left.
 */
AZ_NODISCARD az_result _az_mqtt_core_publish_buffer(az_mqtt_core* core, az_span* out_buffer);

/**
 * @brief Send a QoS 1/2 PUBLISH encoded into @p buffer (from _az_mqtt_core_publish_buffer();
 * @p remaining: what the encoder left), storing it if it is in inflight_message_buffer.
 *
 * As _az_mqtt_core_send_tracked_request(); an encoding that ran out of room in a partly used
 * inflight_message_buffer fails with AZ_MQTT_ERROR_OUT_OF_STORAGE.
 *
 * @param deadline_ms When it expires (-1: never); then dropped instead of resent.
 * @param expiry_offset Offset in the packet of the 4-byte Message Expiry Interval to set to the
 * time left when resent; 0: none.
 */
AZ_NODISCARD az_result _az_mqtt_core_send_publish(
    az_mqtt_core* core,
    az_mqtt_inflight_entry* entry,
    az_result encode_result,
    az_span buffer,
    az_span remaining,
    int64_t deadline_ms,
    uint32_t expiry_offset);

/** @brief A QoS 2 PUBLISH got its PUBREC: @p entry awaits PUBCOMP; its stored copy is freed. */
void _az_mqtt_core_inflight_to_pubrel(az_mqtt_core* core, az_mqtt_inflight_entry* entry);

/**
 * @brief Resume or discard the exchanges of earlier connections, on an accepted CONNACK.
 *
 * Without @p session_present each outgoing QoS 1/2 exchange is reported to @p dropped
 * (AZ_MQTT_ERROR_SESSION_NOT_RESUMED) and every entry freed. With it, each PUBREL is resent (one
 * over the server's Maximum Packet Size dropped instead: AZ_MQTT_ERROR_PACKET_TOO_LARGE), then
 * each stored PUBLISH (_az_mqtt_core_inflight_resend_due()). Stops if the session ends.
 *
 * @return A send failure (the session is closed); AZ_OK otherwise.
 */
AZ_NODISCARD az_result _az_mqtt_core_inflight_resume(
    az_mqtt_core* core,
    bool session_present,
    _az_mqtt_core_encode_pubrel_fn encode_pubrel,
    uint16_t publish_limit,
    _az_mqtt_core_publish_dropped_fn dropped);

/**
 * @brief Resend the stored PUBLISH still awaiting it, oldest first (DUP set), while fewer than
 * @p publish_limit exchanges are incomplete on this connection; the rest wait for the next call.
 * One expired, over the server's Maximum Packet Size, or not stored is dropped instead and
 * reported to @p dropped. Stops if the session ends.
 *
 * @return A send failure (the session is closed); AZ_OK otherwise.
 */
AZ_NODISCARD az_result _az_mqtt_core_inflight_resend_due(
    az_mqtt_core* core,
    uint16_t publish_limit,
    _az_mqtt_core_publish_dropped_fn dropped);

/** @brief Free @p entry and its stored PUBLISH, keeping the others in reservation order. */
void _az_mqtt_core_inflight_free_entry(az_mqtt_core* core, az_mqtt_inflight_entry* entry);

/**
 * @brief Send a request encoded into the send buffer (@p encode_result: the
 * encoder's result; @p remaining: what it left of the buffer).
 *
 * On an encoding failure, or a packet over the server's Maximum Packet Size
 * (AZ_MQTT_ERROR_PACKET_TOO_LARGE), frees @p entry (may be NULL) and sends
 * nothing. A send failure closes the session, as _az_mqtt_core_send_request().
 */
AZ_NODISCARD az_result _az_mqtt_core_send_tracked_request(
    az_mqtt_core* core,
    az_mqtt_inflight_entry* entry,
    az_result encode_result,
    az_span remaining);

#endif // AZ_MQTT_CORE_INTERNAL_H
