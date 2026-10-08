// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_websocket.h
 * @brief MQTT over WebSockets (RFC 6455, subprotocol "mqtt"): a transport layer over another
 * transport.
 *
 * @code
 * az_mqtt_websocket ws;
 * az_mqtt_websocket_options ws_options = az_mqtt_websocket_options_default();
 * rc = az_mqtt_websocket_init(&ws, platform_transport, &ws_options);
 * client_options.transport = az_mqtt_websocket_get_transport(&ws);
 * @endcode
 */

#ifndef AZ_MQTT_WEBSOCKET_H
#define AZ_MQTT_WEBSOCKET_H

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/** @brief Longest request path. */
#define AZ_MQTT_WEBSOCKET_PATH_MAX 256

#ifndef AZ_MQTT_WEBSOCKET_SEND_CHUNK
/**
 * @brief Stack buffer frames are masked in on their way out, in bytes (at least 15): a packet
 * larger than it takes several sends on the transport below. None starts later than
 * AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS after the first, so a send waits at most twice that.
 */
#define AZ_MQTT_WEBSOCKET_SEND_CHUNK 512
#endif

/** @brief Path Azure IoT Hub serves MQTT over WebSockets on. */
#define AZ_MQTT_WEBSOCKET_PATH_IOT_HUB "/$iothub/websocket"

/** @brief WebSocket settings. */
typedef struct
{
  /** @brief Request path: '/' then printable ASCII, no space (empty: "/mqtt"). Used, not copied. */
  az_span path;
} az_mqtt_websocket_options;

/** @brief Defaults: path "/mqtt". */
AZ_NODISCARD AZ_INLINE az_mqtt_websocket_options az_mqtt_websocket_options_default(void)
{
  az_mqtt_websocket_options options;
  options.path = AZ_SPAN_EMPTY;
  return options;
}

/**
 * @brief A WebSocket over another transport (caller storage; fields are internal).
 *
 * The connect host and port are those of the server's WebSocket listener (typically 443 with
 * TLS, 80 without); TLS and an HTTP proxy are the lower transport's. The host must stay valid
 * until the connect completes.
 */
typedef struct
{
  struct
  {
    az_mqtt_transport base;
    az_mqtt_transport* lower;
    az_mqtt_websocket_options options;
    az_span host;
    az_mqtt_transport_error_fn error_callback;
    void* error_context;
    /** @brief Failure found after payload that was returned first; returned by the next receive. */
    az_result pending;
    uint64_t frame_left;
    uint32_t connect_attempts;
    uint32_t http[2];
    uint16_t port;
    uint8_t accept[28];
    uint8_t line[80];
    uint8_t header[10];
    uint8_t control[125];
    uint8_t stash[64];
    uint8_t stash_start;
    uint8_t stash_end;
    uint8_t line_size;
    uint8_t header_size;
    uint8_t control_size;
    uint8_t opcode;
    uint8_t stage;
    uint8_t flags;
    bool tls;
  } _internal;
} az_mqtt_websocket;

#ifndef AZ_MQTT_NO_WEBSOCKETS
/**
 * @brief Initialize @p websocket over @p transport, which it then drives: use it only through
 * az_mqtt_websocket_get_transport(). Both must outlive their use.
 *
 * @param options NULL: defaults. Copied; the path is used, not copied.
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG Invalid or too long path.
 */
AZ_NODISCARD az_result az_mqtt_websocket_init(
    az_mqtt_websocket* websocket,
    az_mqtt_transport* transport,
    az_mqtt_websocket_options const* options);
#else // Built without WebSocket support (AZ_MQTT_ENABLE_WEBSOCKETS=OFF).
AZ_NODISCARD AZ_INLINE az_result az_mqtt_websocket_init(
    az_mqtt_websocket* websocket,
    az_mqtt_transport* transport,
    az_mqtt_websocket_options const* options)
{
  (void)websocket;
  (void)transport;
  (void)options;
  return AZ_MQTT_ERROR_NOT_SUPPORTED;
}
#endif // AZ_MQTT_NO_WEBSOCKETS

/** @brief The WebSocket as a transport, e.g. for a client's options.transport. */
AZ_NODISCARD AZ_INLINE az_mqtt_transport* az_mqtt_websocket_get_transport(
    az_mqtt_websocket* websocket)
{
  return &websocket->_internal.base;
}

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT_WEBSOCKET_H
