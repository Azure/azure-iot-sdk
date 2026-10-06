// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_transport.h
 * @brief Transport abstraction: the byte stream a client runs over.
 *
 * A transport is an az_mqtt_transport_vtable implementation. The platform transport (TCP, TLS,
 * HTTP CONNECT proxy) is provided for POSIX (OpenSSL, mbedTLS) and Windows (Schannel); layers,
 * such as az_mqtt_websocket, wrap another transport.
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

/** @brief A transport; see az_mqtt_transport_vtable. */
typedef struct az_mqtt_transport az_mqtt_transport;

// ──────────────────────── Proxy options ──────────────────────

/** @brief Longest proxy host name. */
#define AZ_MQTT_PROXY_HOST_MAX 255

/** @brief Longest proxy user name and password, together. */
#define AZ_MQTT_PROXY_CREDENTIALS_MAX 255

/**
 * @brief HTTP proxy to tunnel the connection through (HTTP CONNECT).
 *
 * TLS, when used, runs inside the tunnel, end to end with the server: the proxy sees no
 * plaintext, and the server is verified against the host passed to connect, not the proxy.
 * Nothing is taken from the environment (e.g. https_proxy).
 */
typedef struct
{
  /** @brief Proxy host name or IP address (without brackets); empty: no proxy. */
  az_span host;
  /** @brief Proxy port; must not be 0. */
  uint16_t port;
  /**
   * @brief Sent with @p password as HTTP Basic authentication; empty: no authentication.
   * Basic credentials are readable by anyone on the path to the proxy.
   */
  az_span username;
  az_span password;
} az_mqtt_proxy_options;

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
 * @brief Longest az_mqtt_transport_send() waits for the peer to accept data (over WebSockets,
 * up to twice this; see AZ_MQTT_WEBSOCKET_SEND_CHUNK). On expiry the connection is unusable (a
 * partial packet may have been sent).
 */
#define AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS 30000
#endif

// ──────────────────────── Transport API ──────────────────────

/**
 * @brief Size in bytes of the platform transport, for caller allocation (pointer-aligned).
 */
AZ_NODISCARD int32_t az_mqtt_transport_sizeof(void);

/**
 * @brief Initialize a platform transport (caller storage of az_mqtt_transport_sizeof() bytes).
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
 * Bounded by AZ_MQTT_TRANSPORT_CONNECT_TIMEOUT_MS (AZ_MQTT_ERROR_TIMEOUT).
 *
 * @retval AZ_MQTT_ERROR_NOT_SUPPORTED TLS requested from a build without a TLS
 *         backend, or an option the backend cannot honour. Never downgrades.
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG Only one of client_cert_path / client_key_path set.
 * @retval AZ_MQTT_ERROR_NAME_RESOLUTION, AZ_MQTT_ERROR_CONNECTION_REFUSED,
 *         AZ_MQTT_ERROR_TLS_HANDSHAKE, AZ_MQTT_ERROR_TLS_VERIFY, AZ_MQTT_ERROR_TRANSPORT
 *         The native errors behind them go to az_mqtt_transport_set_error_callback(). With a
 *         proxy, name resolution and the TCP connect concern the proxy.
 * @retval AZ_MQTT_ERROR_PROXY, AZ_MQTT_ERROR_PROXY_AUTH The proxy did not open the tunnel.
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
 * Name resolution is the only step that may block.
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
 * @retval AZ_MQTT_ERROR_CONNECTION_CLOSED The peer closed or reset the connection.
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
 * @retval AZ_MQTT_ERROR_CONNECTION_CLOSED The peer closed or reset the connection.
 */
AZ_NODISCARD az_result az_mqtt_transport_receive(
    az_mqtt_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received);

/**
 * @brief Close the transport connection and release resources.
 *
 * Platform transport: a TLS session still usable is ended with close_notify first, without
 * waiting; native errors met doing so are reported. A connection that failed is closed as is.
 */
void az_mqtt_transport_close(az_mqtt_transport* transport);

/**
 * @brief End an established connection in an orderly way before az_mqtt_transport_close()
 * (e.g. a WebSocket close frame, then TLS close_notify). Best effort.
 */
void az_mqtt_transport_shutdown(az_mqtt_transport* transport);

/** @brief What az_mqtt_native_error.code is. */
typedef enum
{
  /** @brief errno (POSIX) or WSAGetLastError() (Windows). */
  AZ_MQTT_NATIVE_ERROR_SOCKET = 1,
  /** @brief getaddrinfo() result: EAI_* (POSIX) or WSA* (Windows). */
  AZ_MQTT_NATIVE_ERROR_NAME_RESOLUTION = 2,
  /**
   * @brief An OpenSSL error queue entry, an mbedTLS or PSA error code, or a Schannel
   * SECURITY_STATUS (Win32 error while loading a certificate file).
   */
  AZ_MQTT_NATIVE_ERROR_TLS = 3,
  /**
   * @brief OpenSSL X509_V_ERR_*, mbedTLS verification flags (MBEDTLS_X509_BADCERT_*), or
   * Schannel certificate chain policy error (CERT_E_*).
   */
  AZ_MQTT_NATIVE_ERROR_TLS_VERIFY = 4,
  /** @brief The HTTP proxy's reply status (e.g. 407); 0 if the reply was not valid HTTP. */
  AZ_MQTT_NATIVE_ERROR_PROXY = 5,
  /**
   * @brief WebSocket layer (az_mqtt_websocket): the HTTP status of a refused or invalid upgrade
   * reply (0: not HTTP), the status code of a close frame from the server other than 1000 (1005:
   * none), or 1002 for a frame it refused.
   */
  AZ_MQTT_NATIVE_ERROR_WEBSOCKET = 6,
} az_mqtt_native_error_source;

/** @brief One platform error behind a transport failure, for diagnostics. */
typedef struct
{
  az_mqtt_native_error_source source;
  int32_t code;
  /**
   * @brief What the failing call returns (e.g. AZ_MQTT_ERROR_TLS_VERIFY); for an address given up
   * for the next one, what that address alone would have returned. The first error with the
   * returned result is its cause. While closing (close and shutdown return nothing): what the
   * failing step below returned, else AZ_MQTT_ERROR_TRANSPORT.
   */
  az_result result;
  /**
   * @brief The az_mqtt_transport_connect_start() it belongs to: 1 for the transport's first,
   * then 2, ... Errors of one connect, and of the session it opened, share it.
   */
  uint32_t connect_attempt;
} az_mqtt_native_error;

/**
 * @brief Called for each native error, in the order they occur, before the failing call
 * returns.
 *
 * One failure may report several: each address that could not be connected, a socket error
 * under a TLS failure, every queued OpenSSL error, the certificate verification result. An
 * orderly close by the peer reports none. Runs inside the transport call: it must not call into
 * the transport or the client using it.
 */
typedef void (*az_mqtt_transport_error_fn)(az_mqtt_native_error const* error, void* context);

// ──────────────────────── Implementing a transport ───────────

/**
 * @brief A transport implementation: what the az_mqtt_transport_* functions of the same names
 * call, with the same contracts.
 *
 * Optional entries may be NULL: set_proxy (then only "no proxy" is accepted), set_error_callback,
 * shutdown. A layer over another transport forwards what it does not handle to it.
 */
typedef struct
{
  az_result (*connect_start)(
      az_mqtt_transport* transport,
      az_span host,
      uint16_t port,
      az_mqtt_tls_options const* tls_options);
  az_result (*connect_poll)(az_mqtt_transport* transport, int32_t timeout_ms);
  az_result (*send)(az_mqtt_transport* transport, az_span data);
  az_result (*receive)(
      az_mqtt_transport* transport,
      az_span buffer,
      int32_t timeout_ms,
      az_span* out_received);
  void (*shutdown)(az_mqtt_transport* transport);
  void (*close)(az_mqtt_transport* transport);
  az_result (*set_proxy)(az_mqtt_transport* transport, az_mqtt_proxy_options const* proxy);
  void (*set_error_callback)(
      az_mqtt_transport* transport,
      az_mqtt_transport_error_fn callback,
      void* context);
} az_mqtt_transport_vtable;

/**
 * @brief Base of every transport: an implementation's struct starts with it, and sets vtable
 * when initialized.
 */
struct az_mqtt_transport
{
  az_mqtt_transport_vtable const* vtable;
};

/**
 * @brief Connect through @p proxy from the next connect on (NULL, or an empty host: directly).
 * A connect already started keeps the proxy it started with.
 *
 * @p proxy and the spans in it are used, not copied: they must stay valid while set, and until
 * every connect started with them has completed or the transport is closed, even after
 * another proxy (or NULL) is set. With a proxy, the host passed to
 * az_mqtt_transport_connect_start() must stay valid until the connect completes. A connect
 * through a proxy never falls back to connecting directly.
 *
 * @retval AZ_MQTT_ERROR_INVALID_CONFIG Port 0; host or credentials too long, or containing CR,
 *         LF or NUL; user name containing ':'; or a password without a user name.
 * @retval AZ_MQTT_ERROR_NOT_SUPPORTED This transport has no proxy support.
 */
AZ_NODISCARD az_result
az_mqtt_transport_set_proxy(az_mqtt_transport* transport, az_mqtt_proxy_options const* proxy);

/**
 * @brief Set the callback for @p transport's native errors (NULL: none). Clients set it at
 * initialization; set it only on a transport used without a client.
 */
void az_mqtt_transport_set_error_callback(
    az_mqtt_transport* transport,
    az_mqtt_transport_error_fn callback,
    void* context);

/**
 * @brief Fill @p buffer with bytes from a cryptographically secure random source.
 *
 * Part of the platform port, like az_mqtt_transport_clock_ms(); used for WebSocket keys and
 * frame masks.
 *
 * @retval AZ_MQTT_ERROR_TRANSPORT No random source.
 */
AZ_NODISCARD az_result az_mqtt_transport_random(az_span buffer);

/**
 * @brief Monotonic clock in milliseconds, used for keep-alive and timeouts.
 *
 * Part of the platform port, like the functions above: it must never go
 * backwards (not wall-clock time) and must have millisecond resolution.
 */
AZ_NODISCARD int64_t az_mqtt_transport_clock_ms(void);

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT_TRANSPORT_H
