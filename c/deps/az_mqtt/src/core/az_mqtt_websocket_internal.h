// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_websocket_internal.h
 * @brief Internal: WebSocket client handshake and framing (RFC 6455), without I/O.
 */

#ifndef AZ_MQTT_WEBSOCKET_INTERNAL_H
#define AZ_MQTT_WEBSOCKET_INTERNAL_H

#include <az_mqtt/az_mqtt_types.h>
#include <az_mqtt/az_mqtt_websocket.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Room for the longest request _az_mqtt_websocket_request() builds. */
#define _AZ_MQTT_WEBSOCKET_REQUEST_MAX 768

/** @brief Longest frame header the client sends (2 + 8 length + 4 mask bytes). */
#define _AZ_MQTT_WEBSOCKET_HEADER_MAX 14

/** @brief Close status codes (RFC 6455 §7.4.1). */
#define _AZ_MQTT_WEBSOCKET_CLOSE_NORMAL 1000
#define _AZ_MQTT_WEBSOCKET_CLOSE_PROTOCOL_ERROR 1002
#define _AZ_MQTT_WEBSOCKET_CLOSE_NO_STATUS 1005

/** @brief Opcodes. */
enum
{
  _AZ_MQTT_WEBSOCKET_BINARY = 0x2,
  _AZ_MQTT_WEBSOCKET_CLOSE = 0x8,
  _AZ_MQTT_WEBSOCKET_PING = 0x9,
  _AZ_MQTT_WEBSOCKET_PONG = 0xA,
};

/** @brief Connection stage (_internal.stage). */
enum
{
  _AZ_MQTT_WEBSOCKET_IDLE, ///< No upgrade requested on this connection yet.
  _AZ_MQTT_WEBSOCKET_UPGRADING, ///< Request sent, reading the reply.
  _AZ_MQTT_WEBSOCKET_OPEN, ///< Upgraded: frames flow.
  _AZ_MQTT_WEBSOCKET_CLOSED, ///< A close frame was sent or received, or framing failed.
};

/** @brief What _az_mqtt_websocket_deframe() stopped at. */
typedef enum
{
  _AZ_MQTT_WEBSOCKET_EVENT_DATA, ///< All input consumed.
  _AZ_MQTT_WEBSOCKET_EVENT_PING, ///< A ping: answer with a pong of _az_mqtt_websocket_control().
  _AZ_MQTT_WEBSOCKET_EVENT_CLOSE, ///< A close frame; see _az_mqtt_websocket_close_code().
  _AZ_MQTT_WEBSOCKET_EVENT_ERROR, ///< A frame RFC 6455 does not allow.
} _az_mqtt_websocket_event;

/** @brief SHA-1 of @p data (FIPS 180-4); only for the handshake's accept value. */
void _az_mqtt_sha1(uint8_t const* data, size_t size, uint8_t out[20]);

/** @brief Whether @p path is a valid az_mqtt_websocket_options.path. */
AZ_NODISCARD bool _az_mqtt_websocket_path_is_valid(az_span path);

/**
 * @brief Write the upgrade request to @p buffer and reset @p ws for a new connection (stage
 * UPGRADING).
 *
 * @param host Server host name or address; port @p port, omitted from Host when the default for
 *        @p tls (443, 80).
 * @param nonce 16 random bytes for Sec-WebSocket-Key.
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG @p host is empty, too long, or has CR, LF, NUL or a space.
 */
AZ_NODISCARD az_result _az_mqtt_websocket_request(
    az_mqtt_websocket* ws,
    az_span host,
    uint16_t port,
    bool tls,
    uint8_t const nonce[16],
    az_span buffer,
    int32_t* out_size);

/**
 * @brief Parse the next bytes of the upgrade reply; in any number of pieces, never consuming past
 * its end (later bytes are frames).
 *
 * @param[out] out_status The reply's HTTP status once known; 0 if not HTTP.
 * @retval AZ_OK Upgraded (stage OPEN).
 * @retval AZ_MQTT_ERROR_TIMEOUT Incomplete: parse more.
 * @retval AZ_MQTT_ERROR_WEBSOCKET Refused (status not 101), invalid headers, or not HTTP.
 */
AZ_NODISCARD az_result _az_mqtt_websocket_reply_parse(
    az_mqtt_websocket* ws,
    az_span data,
    int32_t* out_consumed,
    uint16_t* out_status);

/**
 * @brief Write the header of a final frame of @p opcode, masked with @p mask, carrying
 * @p size payload bytes, to @p out (_AZ_MQTT_WEBSOCKET_HEADER_MAX bytes).
 * @return Header size.
 */
int32_t _az_mqtt_websocket_frame_header(
    uint8_t* out,
    uint8_t opcode,
    uint64_t size,
    uint8_t const mask[4]);

/** @brief Mask (or unmask) @p size bytes at payload offset @p offset with @p mask. */
void _az_mqtt_websocket_mask(uint8_t* data, int32_t size, uint8_t const mask[4], uint64_t offset);

/**
 * @brief Deframe received bytes: copy the binary payload in @p in to @p out (which may overlap,
 * at or before @p in), dropping frame headers and collecting control frames.
 *
 * Stops after a ping or close frame, or at an invalid frame: call again with the rest.
 *
 * @param[out] out_consumed Bytes of @p in used.
 * @param[out] out_produced Payload bytes written to @p out.
 */
_az_mqtt_websocket_event _az_mqtt_websocket_deframe(
    az_mqtt_websocket* ws,
    uint8_t const* in,
    int32_t in_size,
    uint8_t* out,
    int32_t* out_consumed,
    int32_t* out_produced);

/** @brief Payload of the last ping or close frame (at most 125 bytes). */
AZ_NODISCARD az_span _az_mqtt_websocket_control(az_mqtt_websocket* ws);

/** @brief Status code of the last close frame; 1005 if it had none. */
AZ_NODISCARD uint16_t _az_mqtt_websocket_close_code(az_mqtt_websocket const* ws);

#endif // AZ_MQTT_WEBSOCKET_INTERNAL_H
