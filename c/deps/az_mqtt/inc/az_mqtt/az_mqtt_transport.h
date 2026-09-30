// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_transport.h
 * @brief Platform transport abstraction for TCP/TLS connections.
 *
 * Implement these functions for your platform. Implementations for Linux (POSIX + OpenSSL)
 * and Windows (Winsock + Schannel/OpenSSL) are provided.
 */

#ifndef AZ_MQTT_TRANSPORT_H
#define AZ_MQTT_TRANSPORT_H

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/_az_cfg_prefix.h>

// ──────────────────────── Transport handle ───────────────────

/**
 * @brief Opaque transport handle. Each platform provides its own struct definition.
 */
typedef struct az_mqtt_transport az_mqtt_transport;

// ──────────────────────── TLS options ────────────────────────

/**
 * @brief Called with the backend's TLS configuration before the handshake.
 *
 * @p native_config is an `SSL_CTX*` (OpenSSL) or `mbedtls_ssl_config*`
 * (mbedTLS), already holding what az_mqtt_tls_options asked for. Use it for
 * what the options do not cover, e.g. `esp_crt_bundle_attach()` on ESP-IDF.
 * A failure aborts the connect with it.
 *
 * The hook is trusted code: with the native configuration it can change how
 * the server is verified (a certificate bundle does so through a verify
 * callback). Afterwards the transport re-requires verification and checks its
 * result, which catches a hook that switches verification off, but not one
 * that installs a permissive verify callback.
 */
typedef az_result (*az_mqtt_tls_configure_fn)(void* native_config, void* context);

/**
 * @brief TLS settings. The server certificate chain and host name (or IP
 * address) are always verified; there is no option to turn that off.
 *
 * Each item comes from at most one source (file path or in-memory PEM); a
 * client certificate needs exactly one key source. Anything else fails with
 * AZ_MQTT_ERROR_INVALID_CONFIG; a source the backend cannot use fails with
 * AZ_MQTT_ERROR_NOT_SUPPORTED. Nothing is ever silently ignored.
 */
typedef struct
{
  /**
   * @brief Path to CA certificate file (PEM). With neither this nor
   * ca_cert_pem, OpenSSL and Schannel use the system store; mbedTLS has none
   * and needs `configure` to install trust (else AZ_MQTT_ERROR_NOT_SUPPORTED).
   */
  az_span ca_cert_path;

  /** @brief Client certificate file (PEM) for mutual TLS. */
  az_span client_cert_path;

  /** @brief Client private key file (PEM) for mutual TLS. */
  az_span client_key_path;

  /** @brief CA certificates, PEM in memory (OpenSSL, mbedTLS). */
  az_span ca_cert_pem;

  /** @brief Client certificate (chain), PEM in memory (OpenSSL, mbedTLS). */
  az_span client_cert_pem;

  /** @brief Client private key, PEM in memory (OpenSSL, mbedTLS). */
  az_span client_key_pem;

  /**
   * @brief Client key held by PSA and never exported (mbedTLS): an HSM,
   * secure element or, on ESP-IDF v6, the Digital Signature peripheral. 0 = none.
   */
  uint32_t client_key_psa_id;

  /**
   * @brief Client key loaded through OpenSSL's OSSL_STORE and providers, e.g.
   * "pkcs11:object=device;type=private", "tpm2:...", "file:/path/key.pem".
   */
  az_span client_key_uri;

  /** @brief Optional; see az_mqtt_tls_configure_fn. */
  az_mqtt_tls_configure_fn configure;
  void* configure_context;
} az_mqtt_tls_options;

AZ_NODISCARD AZ_INLINE az_mqtt_tls_options az_mqtt_tls_options_default(void)
{
  az_mqtt_tls_options opts;
  opts.ca_cert_path = AZ_SPAN_EMPTY;
  opts.client_cert_path = AZ_SPAN_EMPTY;
  opts.client_key_path = AZ_SPAN_EMPTY;
  opts.ca_cert_pem = AZ_SPAN_EMPTY;
  opts.client_cert_pem = AZ_SPAN_EMPTY;
  opts.client_key_pem = AZ_SPAN_EMPTY;
  opts.client_key_psa_id = 0;
  opts.client_key_uri = AZ_SPAN_EMPTY;
  opts.configure = NULL;
  opts.configure_context = NULL;
  return opts;
}

/**
 * @brief Backend-independent consistency check of @p o.
 *
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG Two sources for one item, a client
 *         certificate without exactly one key source, or a key without a certificate.
 */
AZ_NODISCARD AZ_INLINE az_result az_mqtt_tls_options_check(az_mqtt_tls_options const* o)
{
  bool const cert_path = az_span_size(o->client_cert_path) > 0;
  bool const cert_pem = az_span_size(o->client_cert_pem) > 0;
  int const keys = (az_span_size(o->client_key_path) > 0) + (az_span_size(o->client_key_pem) > 0)
      + (o->client_key_psa_id != 0) + (az_span_size(o->client_key_uri) > 0);
  if ((az_span_size(o->ca_cert_path) > 0 && az_span_size(o->ca_cert_pem) > 0)
      || (cert_path && cert_pem) || keys > 1 || (cert_path || cert_pem) != (keys == 1))
  {
    return AZ_MQTT_ERROR_INVALID_CONFIG;
  }
  return AZ_OK;
}

// ──────────────────────── Limits ─────────────────────────────

// These bounds apply to the POSIX transports (OpenSSL, mbedTLS, no TLS). The
// Windows (Schannel) transport still connects synchronously and is not bounded
// by them.

#ifndef AZ_MQTT_TRANSPORT_CONNECT_TIMEOUT_MS
/** @brief Bound of az_mqtt_transport_connect() (TCP connect + TLS handshake). */
#define AZ_MQTT_TRANSPORT_CONNECT_TIMEOUT_MS 30000
#endif

#ifndef AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS
/**
 * @brief Longest one resolved address is tried while others remain, so an
 * unreachable first address (e.g. broken IPv6) falls back to the next.
 */
#define AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS 2000
#endif

#ifndef AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS
/**
 * @brief Longest az_mqtt_transport_send() waits for the peer to accept data.
 * On expiry the connection is unusable (a partial packet may have been sent).
 */
#define AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS 30000
#endif

// ──────────────────────── Transport API ──────────────────────

/**
 * @brief Get the size in bytes of the platform-specific transport struct.
 * Use this to stack-allocate the transport: uint8_t buf[az_mqtt_transport_sizeof()]; then cast.
 */
AZ_NODISCARD int32_t az_mqtt_transport_sizeof(void);

/**
 * @brief Initialize a transport handle (already allocated by the caller).
 */
AZ_NODISCARD az_result az_mqtt_transport_init(az_mqtt_transport* transport);

/**
 * @brief Connect to a host over TCP, optionally with TLS.
 *
 * @param transport   Initialized transport handle.
 * @param host        Null-terminated hostname string (as az_span).
 * @param port        Destination port (e.g. 1883 or 8883).
 * @param tls_options TLS settings. Pass NULL for plain TCP.
 *
 * Bounded by AZ_MQTT_TRANSPORT_CONNECT_TIMEOUT_MS (AZ_MQTT_ERROR_TIMEOUT) on POSIX.
 *
 * @retval AZ_MQTT_ERROR_NOT_SUPPORTED TLS requested from a build without a TLS
 *         backend, or an option the backend cannot honour. Never downgrades.
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG Only one of client_cert_path / client_key_path set.
 */
AZ_NODISCARD az_result az_mqtt_transport_connect(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options);

/**
 * @brief Start a connect without waiting for it: resolves @p host and begins
 * the TCP connect. Drive it with az_mqtt_transport_connect_poll().
 *
 * Name resolution is the only step that may block. Windows (Schannel) completes
 * the whole connect here, synchronously and without the connect bound, and
 * az_mqtt_transport_connect_poll() returns at once.
 *
 * @param tls_options Same as az_mqtt_transport_connect(); read only during this call.
 */
AZ_NODISCARD az_result az_mqtt_transport_connect_start(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options);

/**
 * @brief Progress a started connect (TCP, then TLS handshake) for up to @p timeout_ms.
 *
 * @retval AZ_OK Connected.
 * @retval AZ_MQTT_ERROR_TIMEOUT Not done yet; call again.
 * @retval other Failed; the transport is closed.
 */
AZ_NODISCARD az_result
az_mqtt_transport_connect_poll(az_mqtt_transport* transport, int32_t timeout_ms);

/**
 * @brief Send bytes over the transport.
 * @return AZ_OK on success, or an error.
 */
AZ_NODISCARD az_result az_mqtt_transport_send(az_mqtt_transport* transport, az_span data);

/**
 * @brief Receive bytes from the transport.
 *
 * @param transport    Transport handle.
 * @param buffer       Buffer to read into.
 * @param timeout_ms   Timeout in milliseconds. 0 = non-blocking, -1 = block forever.
 * @param out_received Output: the sub-span of buffer that was filled.
 * @return AZ_OK on success (out_received size can be 0 on timeout), or an error.
 */
AZ_NODISCARD az_result az_mqtt_transport_receive(
    az_mqtt_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received);

/**
 * @brief Close the transport connection and release resources.
 */
void az_mqtt_transport_close(az_mqtt_transport* transport);

/**
 * @brief Monotonic clock in milliseconds, used for keep-alive and timeouts.
 *
 * Part of the platform port, like the functions above: it must never go
 * backwards (not wall-clock time) and must have millisecond resolution.
 */
AZ_NODISCARD int64_t az_mqtt_transport_clock_ms(void);

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT_TRANSPORT_H
