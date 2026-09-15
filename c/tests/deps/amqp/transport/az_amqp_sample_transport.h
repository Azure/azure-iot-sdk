// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief A reference transport adapter used by the AMQP samples.
 *
 * @details This header defines the contract for a small, self-contained #az_amqp_transport backend
 * that the samples plug into the AMQP client. It demonstrates the integration boundary: an embedder
 * supplies a vtable plus backing storage and exposes whatever their event loop needs to wait on
 * (here, a pollable socket). The accompanying implementation uses each platform's *native* stack:
 *   - **Windows**: Winsock + **Schannel** (the OS TLS provider). No OpenSSL dependency.
 *   - **Linux/macOS**: BSD sockets + OpenSSL.
 *   - **ESP32 (ESP-IDF)**: lwIP + mbedTLS.
 *
 * Because the AMQP core only ever calls through #az_amqp_transport, swapping backends does not
 * change a single line of sample AMQP code. The backing #az_amqp_sample_transport is caller-
 * allocated so the samples allocate nothing on the heap. Fields are visible and managed by the
 * adapter implementation.
 */

#ifndef AZ_AMQP_SAMPLE_TRANSPORT_H
#define AZ_AMQP_SAMPLE_TRANSPORT_H

#include <azure/amqp/az_amqp_transport.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Caller-allocated backing storage for the sample transport. Fields are adapter-managed.
 *
 * @details `void*` is used for the TLS handles so this header pulls in neither OpenSSL nor Schannel
 * types: on Windows the handles hold Schannel `CredHandle`/`CtxtHandle` state, on Linux an OpenSSL
 * `SSL_CTX*`/`SSL*`.
 */
typedef struct
{
  void* tls_context; ///< Schannel credential / OpenSSL SSL_CTX. Adapter-managed.
  void* tls_session; ///< Schannel security context / OpenSSL SSL. Adapter-managed.
  int64_t file_descriptor; ///< The connected socket (a SOCKET on Windows). Adapter-managed.
  az_amqp_transport_options options; ///< The connection parameters. Adapter-managed.
  int phase; ///< Internal connect/handshake progress. Adapter-managed.
  int32_t last_error_status; ///< The most recent native error code. Adapter-managed.
  uint8_t last_error_message[128]; ///< Storage for the most recent native error text. Adapter-managed.
  /// HTTP CONNECT tunnel state, used only when AZ_IOT_E2E_SERVICE_PROXY_HOST is
  /// set in the environment. Adapter-managed.
  uint8_t proxy_request[512]; ///< The CONNECT request being sent.
  size_t proxy_request_len; ///< Its length, 0 when no proxy is in use.
  size_t proxy_request_sent; ///< How much of it has gone out.
  uint8_t proxy_response[512]; ///< The proxy's status line + headers, as they arrive.
  size_t proxy_response_len; ///< How much of it has arrived.
} az_amqp_sample_transport;

/**
 * @brief Initializes an #az_amqp_transport backed by @p storage from the given @p options.
 *
 * @param[out] out_transport The transport handed to #az_amqp_connection_init.
 * @param[in,out] storage Caller-allocated backing storage; must outlive @p out_transport.
 * @param[in] options Host, port, and TLS material for the connection.
 * @return #AZ_OK on success; #AZ_ERROR_ARG on a `NULL` argument.
 */
AZ_NODISCARD az_result az_amqp_sample_transport_init(
    az_amqp_transport* out_transport,
    az_amqp_sample_transport* storage,
    az_amqp_transport_options const* options);

/**
 * @brief Returns the underlying socket so the sample's event loop can wait on it (for example with
 * `select`/`poll`, or `WSAPoll` on Windows).
 *
 * @param[in] storage The backing storage previously initialized by #az_amqp_sample_transport_init.
 * @return The socket handle as an `int64_t`, or `-1` if not yet connected.
 */
int64_t az_amqp_sample_transport_get_socket(az_amqp_sample_transport const* storage);

#endif // AZ_AMQP_SAMPLE_TRANSPORT_H
