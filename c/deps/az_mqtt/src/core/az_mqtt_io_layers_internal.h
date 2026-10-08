// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/**
 * @file az_mqtt_io_layers_internal.h
 * @brief Internal: the layers the platform transport is stacked from (TLS over HTTP CONNECT
 * proxy over socket; see az_mqtt_transport_init()), and what layers share.
 */

#ifndef AZ_MQTT_IO_LAYERS_INTERNAL_H
#define AZ_MQTT_IO_LAYERS_INTERNAL_H

#include "../platform/az_mqtt_http_connect.h"

#include <az_mqtt/az_mqtt_transport.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

// ──────────────────────── Native errors of a layer ───────────

/**
 * @brief Where a layer's native errors, and those of the transport below it, go.
 *
 * Errors from below are stamped with this layer's connect_attempt, and, while phase_result is
 * set (not AZ_OK), with that result: the one the layer returns for any failure below it then.
 */
typedef struct
{
  az_mqtt_transport_error_fn callback;
  void* context;
  uint32_t connect_attempt;
  az_result phase_result;
} _az_mqtt_io_layer_errors;

/** @brief Route @p lower's native errors through @p errors. */
void _az_mqtt_io_layer_errors_attach(_az_mqtt_io_layer_errors* errors, az_mqtt_transport* lower);

/** @brief Report one native error of the layer itself. */
void _az_mqtt_io_layer_report(
    _az_mqtt_io_layer_errors const* errors,
    az_mqtt_native_error_source source,
    int32_t code,
    az_result result);

/** @brief Milliseconds left until @p deadline_ms (-1: none), 0 if passed. */
int32_t _az_mqtt_io_layer_remaining(int64_t deadline_ms);

/** @brief Absolute deadline @p timeout_ms from now; -1 for a negative timeout. */
int64_t _az_mqtt_io_layer_deadline(int32_t timeout_ms);

// ──────────────────────── Internal layer ─────────────────────

/** @brief What internal layers offer those above them, besides az_mqtt_transport_vtable. */
typedef struct
{
  /**
   * @brief Send what can be sent of @p data within @p timeout_ms (0: without waiting); resumable.
   * @param[out] out_sent Bytes sent; 0 if none could be sent in time.
   * @retval AZ_OK Sent *@p out_sent bytes (possibly none).
   * @retval other The connection failed (native errors reported); it is unusable.
   */
  az_result (*send_some)(
      az_mqtt_transport* transport,
      az_span data,
      int32_t timeout_ms,
      int32_t* out_sent);
} _az_mqtt_io_layer_ops;

/** @brief Base of the layers below TLS (proxy, socket): a transport with _az_mqtt_io_layer_ops. */
typedef struct
{
  az_mqtt_transport base; ///< Must be first.
  _az_mqtt_io_layer_ops const* ops;
} _az_mqtt_io_layer;

/** @brief See _az_mqtt_io_layer_ops.send_some. */
AZ_NODISCARD az_result _az_mqtt_io_layer_send_some(
    _az_mqtt_io_layer* layer,
    az_span data,
    int32_t timeout_ms,
    int32_t* out_sent);

// ──────────────────────── Socket (platform) ──────────────────

/** @brief Size of the socket layer (TCP; refuses TLS options and proxies). */
AZ_NODISCARD int32_t _az_mqtt_socket_transport_sizeof(void);

/** @brief Initialize the socket layer in @p storage (_az_mqtt_socket_transport_sizeof() bytes). */
AZ_NODISCARD az_result _az_mqtt_socket_transport_init(_az_mqtt_io_layer* storage);

// ──────────────────────── TLS (platform) ─────────────────────

/** @brief Size of the TLS layer; 0 in builds without a TLS backend. */
AZ_NODISCARD int32_t _az_mqtt_tls_transport_sizeof(void);

/**
 * @brief TLS over @p lower: connect_start()'s tls_options are this layer's (NULL: passes bytes
 * through); @p lower gets none.
 */
AZ_NODISCARD az_result
_az_mqtt_tls_transport_init(az_mqtt_transport* transport, _az_mqtt_io_layer* lower);

// ──────────────────────── HTTP CONNECT proxy (portable) ──────

#ifndef AZ_MQTT_NO_PROXY
/** @brief HTTP CONNECT proxy layer (az_mqtt_transport_set_proxy()). Fields are internal. */
typedef struct
{
  _az_mqtt_io_layer layer; ///< Must be first.
  _az_mqtt_io_layer* lower;
  _az_mqtt_io_layer_errors errors;
  /** @brief Set with set_proxy(); NULL: connect directly. */
  az_mqtt_proxy_options const* proxy;
  /** @brief The current connect's (kept until the next connect_start()). */
  az_mqtt_proxy_options const* active;
  az_span host;
  _az_mqtt_http_reply reply;
  /** @brief CONNECT request bytes sent so far. */
  int32_t sent;
  uint16_t port;
  uint8_t stage;
  /** @brief Bytes after the CONNECT reply: the tunnelled stream's first. */
  uint16_t stash_start;
  uint16_t stash_end;
  uint8_t stash[256];
} _az_mqtt_proxy_transport;

AZ_NODISCARD az_result
_az_mqtt_proxy_transport_init(_az_mqtt_proxy_transport* proxy, _az_mqtt_io_layer* lower);
#endif // AZ_MQTT_NO_PROXY

#endif // AZ_MQTT_IO_LAYERS_INTERNAL_H
