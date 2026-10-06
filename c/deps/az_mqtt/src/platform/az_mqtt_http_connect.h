// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_http_connect.h
 * @brief Internal: HTTP CONNECT request and reply (RFC 9110 §9.3.6), and an HTTP/1.x reply
 * parser shared with the WebSocket handshake; without I/O.
 */
#ifndef AZ_MQTT_HTTP_CONNECT_H
#define AZ_MQTT_HTTP_CONNECT_H

#include <az_mqtt/az_mqtt_transport.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>

/** @brief Room for the longest request _az_mqtt_http_connect_request() builds. */
#define _AZ_MQTT_HTTP_CONNECT_REQUEST_MAX 1024

/** @brief Longest reply accepted (status line and headers). */
#define _AZ_MQTT_HTTP_REPLY_MAX 8192

/**
 * @brief Sends up to @p size bytes of @p data without blocking.
 * @return Bytes sent (> 0), 0 if none can be sent now, -1 on error.
 */
typedef int32_t (*_az_mqtt_http_connect_send_fn)(void* context, uint8_t const* data, int32_t size);

/** @brief Reply parsing state. */
typedef struct
{
  uint32_t received;
  uint16_t status;
  uint8_t state;
  uint8_t position;
} _az_mqtt_http_reply;

/** @brief What a byte fed to _az_mqtt_http_reply_step() was. */
typedef enum
{
  _AZ_MQTT_HTTP_MORE, ///< Part of the status line, or a line end.
  _AZ_MQTT_HTTP_HEADER_BYTE, ///< A byte of a header line (field name, colon or value).
  _AZ_MQTT_HTTP_HEADER_END, ///< The end of a header line.
  _AZ_MQTT_HTTP_END, ///< The end of the headers (the reply's last byte).
  _AZ_MQTT_HTTP_MALFORMED, ///< Not HTTP/1.x, a control byte, or longer than _AZ_MQTT_HTTP_REPLY_MAX.
} _az_mqtt_http_event;

void _az_mqtt_http_reply_init(_az_mqtt_http_reply* reply);

#ifndef AZ_MQTT_NO_WEBSOCKETS
/**
 * @brief Parse the next byte of a reply's status line and headers; reply->status holds the
 * status once the status line is read (0 after _AZ_MQTT_HTTP_MALFORMED). Accepts LF as well as
 * CRLF line ends. Not to be called again after _AZ_MQTT_HTTP_END or _AZ_MQTT_HTTP_MALFORMED
 * without _az_mqtt_http_reply_init().
 */
_az_mqtt_http_event _az_mqtt_http_reply_step(_az_mqtt_http_reply* reply, uint8_t c);
#endif

#ifndef AZ_MQTT_NO_PROXY

/** @brief Validate @p proxy (see az_mqtt_transport_set_proxy()); NULL or an empty host is valid. */
AZ_NODISCARD az_result _az_mqtt_http_connect_check(az_mqtt_proxy_options const* proxy);

/**
 * @brief Write the CONNECT request for @p host:@p port, with @p proxy's credentials, to @p buffer.
 *
 * The request holds the credentials: clear @p buffer after use.
 *
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG @p host is empty, too long, or has CR, LF, NUL or a space.
 */
AZ_NODISCARD az_result _az_mqtt_http_connect_request(
    az_mqtt_proxy_options const* proxy,
    az_span host,
    uint16_t port,
    az_span buffer,
    int32_t* out_size);


/**
 * @brief Send the rest of the CONNECT request (see _az_mqtt_http_connect_request()) with
 * @p send; *@p in_out_sent counts what was sent and is kept across calls. The request is
 * rebuilt for each call and cleared afterwards, so the credentials are not kept.
 *
 * @retval AZ_OK All sent.
 * @retval AZ_MQTT_ERROR_TIMEOUT @p send could send no more now: call again when it can.
 * @retval AZ_MQTT_ERROR_PROXY @p send failed.
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG @p host cannot be put in a request.
 */
AZ_NODISCARD az_result _az_mqtt_http_connect_send_request(
    az_mqtt_proxy_options const* proxy,
    az_span host,
    uint16_t port,
    int32_t* in_out_sent,
    _az_mqtt_http_connect_send_fn send,
    void* context);

/**
 * @brief Parse the next bytes of the CONNECT reply.
 *
 * Takes the reply in any number of pieces, never consumes past its end, skips 1xx interim
 * replies, and accepts LF as well as CRLF line ends.
 *
 * @param[out] out_consumed How many bytes of @p data belong to the reply.
 * @retval AZ_OK The reply is complete, with a 2xx status: the tunnel is open.
 * @retval AZ_MQTT_ERROR_TIMEOUT Incomplete: parse more.
 * @retval AZ_MQTT_ERROR_PROXY_AUTH Status 407.
 * @retval AZ_MQTT_ERROR_PROXY Another status (reply->status), or not HTTP or too long (status 0).
 */
AZ_NODISCARD az_result _az_mqtt_http_connect_reply_parse(
    _az_mqtt_http_reply* reply,
    az_span data,
    int32_t* out_consumed);

#else // Built without proxy support (AZ_MQTT_ENABLE_PROXY=OFF): any proxy is refused.

AZ_NODISCARD AZ_INLINE az_result _az_mqtt_http_connect_check(az_mqtt_proxy_options const* proxy)
{
  return proxy == NULL || az_span_size(proxy->host) == 0 ? AZ_OK : AZ_MQTT_ERROR_NOT_SUPPORTED;
}

#endif // AZ_MQTT_NO_PROXY

#endif // AZ_MQTT_HTTP_CONNECT_H
