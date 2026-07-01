// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief The pluggable, non-blocking I/O + TLS transport interface used by the AMQP client.
 *
 * @details The AMQP client never calls a socket or TLS API directly. Instead it drives an
 * #az_amqp_transport: a small table of non-blocking functions that an integrator supplies for their
 * platform. This mirrors the pluggable-stream design of the `http-c` library and keeps the AMQP
 * core free of platform dependencies and free of dynamic allocation. The reference adapter shipped
 * with the samples uses each platform's *native* stack:
 *   - **Windows**: Winsock + **Schannel** (the OS TLS provider) — no OpenSSL dependency.
 *   - **Linux/macOS**: BSD sockets + OpenSSL.
 *   - **ESP32 (ESP-IDF)**: lwIP + mbedTLS.
 *
 * An integrator typically:
 *   1. Defines a backing structure for their TLS/socket state.
 *   2. Implements the #az_amqp_transport_vtable callbacks over that structure.
 *   3. Writes a factory that fills an #az_amqp_transport via #az_amqp_transport_init from an
 *      #az_amqp_transport_options describing the host, port, and TLS material.
 *
 * All callbacks are non-blocking. When a callback cannot finish because the socket would block, it
 * returns #AZ_AMQP_TRANSPORT_STATUS_WANT_READ or #AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE; the AMQP
 * connection propagates that as an #az_amqp_io_interest to the caller's event loop. When a callback
 * returns #AZ_AMQP_TRANSPORT_STATUS_ERROR, the optional #az_amqp_transport_vtable.get_last_error
 * lets the client retrieve the backend's native code for an #az_amqp_error_detail.
 *
 * @note The transport object exposes its fields directly (no hidden `_internal` wrapper). The
 * #az_amqp_transport.vtable and #az_amqp_transport.impl fields are managed by #az_amqp_transport_init
 * and read by the AMQP core; do not modify them afterward.
 */

#ifndef _az_AMQP_TRANSPORT_H
#define _az_AMQP_TRANSPORT_H

#include <azure/amqp/az_amqp_common.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/**
 * @brief The outcome of a non-blocking transport operation.
 *
 * @remark These are intentionally *not* #az_result values: want-read / want-write are normal,
 * expected outcomes of a non-blocking byte pump rather than errors, so modeling them as a distinct
 * status keeps caller code (and the AMQP core) branch-clear.
 */
typedef enum
{
  /// The operation made progress. For `read`/`write`, `*out_bytes` reports how many bytes moved
  /// (which may be 0 for `open`/`process`/`close` steps that completed a phase without I/O).
  AZ_AMQP_TRANSPORT_STATUS_OK = 0,

  /// The operation cannot proceed until the underlying transport is readable. The caller should
  /// wait for readability and invoke the same step again.
  AZ_AMQP_TRANSPORT_STATUS_WANT_READ = 1,

  /// The operation cannot proceed until the underlying transport is writable. The caller should
  /// wait for writability and invoke the same step again.
  AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE = 2,

  /// The peer performed an orderly shutdown of the connection.
  AZ_AMQP_TRANSPORT_STATUS_CLOSED = 3,

  /// An unrecoverable error occurred (connection refused, TLS handshake failure, reset, etc.).
  /// Retrieve native detail via #az_amqp_transport_vtable.get_last_error if implemented.
  AZ_AMQP_TRANSPORT_STATUS_ERROR = 4,
} az_amqp_transport_status;

/**
 * @brief Configuration consumed by a transport factory to establish a connection to the AMQP peer.
 *
 * @details Every field is a caller-owned #az_span; the transport implementation must not retain
 * pointers beyond what it copies into its own storage. Certificate material is provided in PEM
 * format. Leave the TLS fields empty for a plaintext connection (`tls_enabled == false`). On
 * Windows the Schannel adapter accepts PEM and imports it into a transient certificate store.
 */
typedef struct
{
  az_span host_name; ///< The DNS name or IP literal of the AMQP peer, e.g. `myns.servicebus.windows.net`.
  uint16_t port; ///< The TCP port. Conventionally 5671 for AMQP-over-TLS, 5672 for plaintext.

  bool tls_enabled; ///< When `true`, the transport must negotiate TLS before any AMQP bytes flow.

  struct
  {
    /// PEM-encoded trusted root certificate(s) used to validate the peer. When empty, the
    /// implementation should fall back to the platform's default trust store (the system store on
    /// Windows/Schannel, the OpenSSL default paths on Linux).
    az_span trusted_roots;

    /// PEM-encoded client certificate chain for mutual TLS. Empty to disable.
    az_span client_certificate;

    /// PEM-encoded private key matching #client_certificate. Empty to disable.
    az_span client_private_key;

    /// Server Name Indication to present during the handshake. When empty, the implementation
    /// should use #host_name.
    az_span server_name_indication;
  } tls; ///< TLS material; ignored when #tls_enabled is `false`.
} az_amqp_transport_options;

// Forward declaration: a transport instance is passed by pointer to each of its own callbacks.
typedef struct az_amqp_transport az_amqp_transport;

/**
 * @brief The set of non-blocking callbacks an integrator implements to back an #az_amqp_transport.
 *
 * @details All functions receive the owning #az_amqp_transport so the implementation can recover
 * its backing state through #az_amqp_transport_get_impl. None of them may block.
 */
typedef struct
{
  /**
   * @brief Begins establishing the underlying connection (DNS resolution + TCP connect, and, for a
   * TLS transport, arming the handshake). Must not block.
   *
   * @return #AZ_AMQP_TRANSPORT_STATUS_OK when the connect/handshake has at least started without
   * error, or WANT_READ / WANT_WRITE, or ERROR.
   */
  az_amqp_transport_status (*open)(az_amqp_transport* transport);

  /**
   * @brief Advances any in-progress connection or TLS handshake. Called repeatedly until it returns
   * #AZ_AMQP_TRANSPORT_STATUS_OK (fully connected and, if applicable, the handshake has completed).
   */
  az_amqp_transport_status (*process)(az_amqp_transport* transport);

  /**
   * @brief Reads up to `az_span_size(destination)` bytes into @p destination without blocking.
   *
   * @param[out] out_bytes The number of bytes actually read (0 when WANT_READ/WANT_WRITE).
   * @return OK when one or more bytes were read; WANT_READ when no data is available yet;
   * WANT_WRITE if a TLS renegotiation needs to write first; CLOSED on orderly shutdown; ERROR.
   */
  az_amqp_transport_status (*read)(az_amqp_transport* transport, az_span destination, size_t* out_bytes);

  /**
   * @brief Writes up to `az_span_size(source)` bytes from @p source without blocking.
   *
   * @param[out] out_bytes The number of bytes actually written (may be fewer than provided; 0 when
   * WANT_WRITE/WANT_READ).
   * @return OK when one or more bytes were written; WANT_WRITE when the send buffer is full;
   * WANT_READ if a TLS renegotiation needs to read first; ERROR.
   */
  az_amqp_transport_status (*write)(az_amqp_transport* transport, az_span source, size_t* out_bytes);

  /**
   * @brief Begins (and, on subsequent calls, completes) an orderly shutdown of the transport,
   * releasing any resources owned by the backing implementation.
   *
   * @return OK once the transport is fully closed; WANT_READ/WANT_WRITE while a TLS close-notify is
   * still in flight.
   */
  az_amqp_transport_status (*close)(az_amqp_transport* transport);

  /**
   * @brief __[nullable]__ Returns the backend's most recent native error so the AMQP client can
   * populate an #az_amqp_error_detail. Optional: set to `NULL` if the backend has no such detail.
   *
   * @param[out] out_status The backend's native code (errno / WSAGetLastError / OpenSSL error /
   * Schannel SECURITY_STATUS). Set to `0` if none.
   * @param[out] out_message A short, human-readable summary owned by the transport, valid until the
   * next transport call. Set to an empty span if none.
   */
  void (*get_last_error)(az_amqp_transport* transport, int32_t* out_status, az_span* out_message);
} az_amqp_transport_vtable;

/**
 * @brief A pluggable transport: an integrator-supplied vtable plus an opaque backing pointer.
 *
 * @details This object is caller-allocated (stack or static). It is filled by #az_amqp_transport_init
 * and then handed to #az_amqp_connection_init. The fields are library-managed: the AMQP client only
 * ever calls through #vtable, recovering backend state via #impl.
 */
struct az_amqp_transport
{
  az_amqp_transport_vtable const* vtable; ///< The integrator's non-blocking callbacks. Library-managed.
  void* impl; ///< Opaque pointer to the integrator's backing state. Library-managed.
};

/**
 * @brief Initializes an #az_amqp_transport from a vtable and a backing implementation pointer.
 *
 * @details Intended to be called by a transport factory, not by application code directly.
 *
 * @param[out] transport The transport to initialize. Must not be `NULL`.
 * @param[in] vtable The non-blocking callbacks. Must not be `NULL` and must outlive @p transport.
 * @param[in] impl __[nullable]__ Opaque backing pointer handed back to every callback.
 * @return #AZ_OK on success; #AZ_ERROR_ARG if @p transport or @p vtable was `NULL`.
 */
AZ_NODISCARD az_result az_amqp_transport_init(
    az_amqp_transport* transport,
    az_amqp_transport_vtable const* vtable,
    void* impl);

/**
 * @brief Returns the opaque backing pointer supplied to #az_amqp_transport_init.
 *
 * @details A transport implementation calls this from inside its own callbacks to recover its state.
 *
 * @param[in] transport The transport instance. Must not be `NULL`.
 * @return The backing pointer (may be `NULL` if none was provided).
 */
AZ_NODISCARD void* az_amqp_transport_get_impl(az_amqp_transport const* transport);

#include <azure/core/_az_cfg_suffix.h>

#endif // _az_AMQP_TRANSPORT_H
