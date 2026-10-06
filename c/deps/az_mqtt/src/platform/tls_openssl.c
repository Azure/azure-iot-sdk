// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file tls_openssl.c
 * @brief Internal: TLS layer through OpenSSL, over another transport.
 */

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE // inet_pton under -std=c99
#endif

#include "az_mqtt_io_layers_internal.h"

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_span.h>
#include <azure/core/internal/az_precondition_internal.h>

#include <stdint.h>
#include <string.h>

#include <arpa/inet.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/store.h>
#include <openssl/x509v3.h>
#include <pthread.h>

/** @brief _tls_transport.stage. */
enum
{
  _IDLE, ///< Not connecting.
  _LOWER, ///< The transport below connects.
  _HANDSHAKE, ///< TLS handshake running.
  _OPEN, ///< Connected (TLS, or bytes passed through).
};

typedef struct
{
  az_mqtt_transport base; ///< Must be first.
  _az_mqtt_io_layer* lower;
  _az_mqtt_io_layer_errors errors;
  SSL_CTX* ssl_ctx;
  SSL* ssl; ///< NULL: no TLS for this connect.
  /** @brief Until when the BIO waits on the layer below (0 or past: does not wait). */
  int64_t deadline;
  /** @brief Why the transport below failed during the last OpenSSL call (AZ_OK: it did not). */
  az_result lower_failure;
  uint8_t stage;
  /** @brief close_notify sent (or tried) on this connection. */
  bool closing;
} _tls_transport;

#define _T(t) ((_tls_transport*)(t))
#define _LOWER_T(transport) (&(transport)->lower->base)

// ──────────────────────── BIO over the transport below ───────

static BIO_METHOD* s_bio_method;
static pthread_once_t s_bio_once = PTHREAD_ONCE_INIT;

static int _bio_write(BIO* bio, char const* data, int size)
{
  _tls_transport* const transport = (_tls_transport*)BIO_get_data(bio);
  BIO_clear_retry_flags(bio);
  int32_t sent = 0;
  az_result const rc = _az_mqtt_io_layer_send_some(
      transport->lower,
      az_span_create((uint8_t*)(uintptr_t)data, size),
      _az_mqtt_io_layer_remaining(transport->deadline),
      &sent);
  if (az_result_failed(rc))
  {
    transport->lower_failure = rc;
    return -1;
  }
  if (sent == 0)
  {
    BIO_set_retry_write(bio); // Nothing could be sent before the deadline.
    return -1;
  }
  return sent;
}

static int _bio_read(BIO* bio, char* buffer, int size)
{
  _tls_transport* const transport = (_tls_transport*)BIO_get_data(bio);
  BIO_clear_retry_flags(bio);
  az_span received;
  az_result const rc = az_mqtt_transport_receive(
      _LOWER_T(transport),
      az_span_create((uint8_t*)buffer, size),
      _az_mqtt_io_layer_remaining(transport->deadline),
      &received);
  if (az_result_failed(rc))
  {
    transport->lower_failure = rc;
    return rc == AZ_MQTT_ERROR_CONNECTION_CLOSED ? 0 : -1;
  }
  if (az_span_size(received) == 0)
  {
    BIO_set_retry_read(bio); // Nothing before the deadline.
    return -1;
  }
  return az_span_size(received);
}

static long _bio_ctrl(BIO* bio, int cmd, long num, void* ptr)
{
  (void)bio;
  (void)num;
  (void)ptr;
  return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

static int _bio_create(BIO* bio)
{
  BIO_set_init(bio, 1);
  return 1;
}

static void _bio_method_init(void)
{
  BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "az_mqtt_transport");
  if (m != NULL
      && (BIO_meth_set_write(m, _bio_write) != 1 || BIO_meth_set_read(m, _bio_read) != 1
          || BIO_meth_set_ctrl(m, _bio_ctrl) != 1 || BIO_meth_set_create(m, _bio_create) != 1))
  {
    BIO_meth_free(m);
    m = NULL;
  }
  s_bio_method = m;
}

static BIO* _bio_new(_tls_transport* transport)
{
  if (pthread_once(&s_bio_once, _bio_method_init) != 0 || s_bio_method == NULL)
  {
    return NULL;
  }
  BIO* bio = BIO_new(s_bio_method);
  if (bio != NULL)
  {
    BIO_set_data(bio, transport);
  }
  return bio;
}

// Helper: copy az_span to null-terminated char buffer on the stack.
static az_result _span_to_cstr(az_span src, char* buf, int32_t buf_size)
{
  int32_t len = az_span_size(src);
  if (len >= buf_size)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  memcpy(buf, az_span_ptr(src), (size_t)len);
  buf[len] = '\0';
  return AZ_OK;
}

// ──────────────────────── TLS setup ──────────────────────────

/**
 * @brief Reject TLS options this backend cannot honour, before any socket work.
 */
static az_result _check_tls_options(az_mqtt_tls_options const* tls_options)
{
  az_result rc = az_mqtt_tls_options_check(tls_options);
  if (az_result_succeeded(rc) && tls_options->client_key_psa_id != 0)
  {
    rc = AZ_MQTT_ERROR_NOT_SUPPORTED; // PSA keys are an mbedTLS feature; use client_key_uri.
  }
  return rc;
}

/** @brief Drop the end-of-input error PEM_read_bio_X509() leaves after the last certificate. */
static void _clear_pem_end(void)
{
  unsigned long const e = ERR_peek_last_error();
  if (ERR_GET_LIB(e) == ERR_LIB_PEM && ERR_GET_REASON(e) == PEM_R_NO_START_LINE)
  {
    ERR_clear_error();
  }
}

/** @brief A read-only memory BIO over @p pem. */
static BIO* _pem_bio(az_span pem)
{
  return BIO_new_mem_buf(az_span_ptr(pem), (int)az_span_size(pem));
}

/** @brief Add every certificate in @p pem to the trust store; at least one is required. */
static az_result _load_ca_pem(SSL_CTX* ctx, az_span pem)
{
  BIO* bio = _pem_bio(pem);
  X509_STORE* store = SSL_CTX_get_cert_store(ctx);
  int added = 0;
  X509* cert;
  while (bio != NULL && (cert = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL)
  {
    added += X509_STORE_add_cert(store, cert) == 1;
    X509_free(cert);
  }
  if (added > 0)
  {
    _clear_pem_end(); // Otherwise kept: why nothing was added.
  }
  BIO_free(bio);
  return added > 0 ? AZ_OK : AZ_MQTT_ERROR_TRANSPORT;
}

/** @brief Use the first certificate in @p pem as ours and the rest as its chain. */
static az_result _load_cert_pem(SSL_CTX* ctx, az_span pem)
{
  BIO* bio = _pem_bio(pem);
  X509* leaf = bio != NULL ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
  bool ok = leaf != NULL && SSL_CTX_use_certificate(ctx, leaf) == 1;
  X509_free(leaf);
  X509* extra;
  while (ok && (extra = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL)
  {
    ok = SSL_CTX_add0_chain_cert(ctx, extra) == 1; // Takes ownership on success.
    if (!ok)
    {
      X509_free(extra);
    }
  }
  if (ok)
  {
    _clear_pem_end();
  }
  BIO_free(bio);
  return ok ? AZ_OK : AZ_MQTT_ERROR_TRANSPORT;
}

static az_result _load_key_pem(SSL_CTX* ctx, az_span pem)
{
  BIO* bio = _pem_bio(pem);
  EVP_PKEY* key = bio != NULL ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL) : NULL;
  bool ok = key != NULL && SSL_CTX_use_PrivateKey(ctx, key) == 1;
  EVP_PKEY_free(key);
  BIO_free(bio);
  return ok ? AZ_OK : AZ_MQTT_ERROR_TRANSPORT;
}

/** @brief Load a private key through OSSL_STORE (any provider: pkcs11, tpm2, file, ...). */
static az_result _load_key_uri(SSL_CTX* ctx, az_span uri)
{
  char uri_str[1024];
  az_result rc = _span_to_cstr(uri, uri_str, (int32_t)sizeof(uri_str));
  if (az_result_failed(rc))
  {
    return rc;
  }
  OSSL_STORE_CTX* store = OSSL_STORE_open(uri_str, NULL, NULL, NULL, NULL);
  EVP_PKEY* key = NULL;
  while (store != NULL && key == NULL && !OSSL_STORE_eof(store))
  {
    OSSL_STORE_INFO* info = OSSL_STORE_load(store);
    if (info == NULL)
    {
      break; // A loader error does not set eof: retrying would not end.
    }
    if (OSSL_STORE_INFO_get_type(info) == OSSL_STORE_INFO_PKEY)
    {
      key = OSSL_STORE_INFO_get1_PKEY(info);
    }
    OSSL_STORE_INFO_free(info);
  }
  OSSL_STORE_close(store);
  bool ok = key != NULL && SSL_CTX_use_PrivateKey(ctx, key) == 1;
  EVP_PKEY_free(key);
  return ok ? AZ_OK : AZ_MQTT_ERROR_TRANSPORT;
}

/**
 * @brief Pin the peer identity the certificate must match.
 *
 * An IP literal is matched against iPAddress SANs and gets no SNI (RFC 6066);
 * anything else is matched as a DNS name and sent as SNI.
 */
static bool _tls_set_peer_identity(SSL* ssl, char* host)
{
  X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
  unsigned char addr[16];
  if (inet_pton(AF_INET, host, addr) == 1 || inet_pton(AF_INET6, host, addr) == 1)
  {
    return X509_VERIFY_PARAM_set1_ip_asc(param, host) == 1;
  }
  X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
  return X509_VERIFY_PARAM_set1_host(param, host, 0) == 1
      && SSL_set_tlsext_host_name(ssl, host) == 1;
}

/**
 * @brief Build the TLS context and session for @p host; the socket is attached later.
 */
static az_result _tls_prepare(
    _tls_transport* transport,
    az_span host,
    az_mqtt_tls_options const* tls_options)
{
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  transport->ssl_ctx = ctx;
  if (ctx == NULL || SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  char path[256];
  az_result rc = AZ_OK;
  if (az_span_size(tls_options->ca_cert_path) > 0)
  {
    rc = _span_to_cstr(tls_options->ca_cert_path, path, (int32_t)sizeof(path));
    if (az_result_succeeded(rc) && SSL_CTX_load_verify_locations(ctx, path, NULL) != 1)
    {
      rc = AZ_MQTT_ERROR_TRANSPORT;
    }
  }
  else if (az_span_size(tls_options->ca_cert_pem) > 0)
  {
    rc = _load_ca_pem(ctx, tls_options->ca_cert_pem);
  }
  else if (SSL_CTX_set_default_verify_paths(ctx) != 1)
  {
    rc = AZ_MQTT_ERROR_TRANSPORT;
  }
  if (az_result_failed(rc))
  {
    return rc;
  }

  bool has_cert = true;
  if (az_span_size(tls_options->client_cert_path) > 0)
  {
    rc = _span_to_cstr(tls_options->client_cert_path, path, (int32_t)sizeof(path));
    if (az_result_succeeded(rc) && SSL_CTX_use_certificate_chain_file(ctx, path) != 1)
    {
      rc = AZ_MQTT_ERROR_TRANSPORT;
    }
  }
  else if (az_span_size(tls_options->client_cert_pem) > 0)
  {
    rc = _load_cert_pem(ctx, tls_options->client_cert_pem);
  }
  else
  {
    has_cert = false;
  }

  if (az_result_succeeded(rc) && has_cert)
  {
    // az_mqtt_tls_options_check() guaranteed exactly one key source.
    if (az_span_size(tls_options->client_key_path) > 0)
    {
      rc = _span_to_cstr(tls_options->client_key_path, path, (int32_t)sizeof(path));
      if (az_result_succeeded(rc) && SSL_CTX_use_PrivateKey_file(ctx, path, SSL_FILETYPE_PEM) != 1)
      {
        rc = AZ_MQTT_ERROR_TRANSPORT;
      }
    }
    else if (az_span_size(tls_options->client_key_pem) > 0)
    {
      rc = _load_key_pem(ctx, tls_options->client_key_pem);
    }
    else
    {
      rc = _load_key_uri(ctx, tls_options->client_key_uri);
    }
    if (az_result_succeeded(rc) && SSL_CTX_check_private_key(ctx) != 1)
    {
      rc = AZ_MQTT_ERROR_TRANSPORT;
    }
  }
  if (az_result_failed(rc))
  {
    return rc;
  }

  if (tls_options->configure != NULL)
  {
    rc = tls_options->configure(ctx, tls_options->configure_context);
    if (az_result_failed(rc))
    {
      return rc;
    }
  }

  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

  transport->ssl = SSL_new(ctx);
  if (transport->ssl == NULL)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  char host_str[256];
  rc = _span_to_cstr(host, host_str, (int32_t)sizeof(host_str));
  if (az_result_failed(rc))
  {
    return rc;
  }
  return _tls_set_peer_identity(transport->ssl, host_str) ? AZ_OK : AZ_MQTT_ERROR_TRANSPORT;
}

/** @brief Report each queued OpenSSL error, oldest first, as part of @p result; clear the queue. */
static void _report_tls_queue(_tls_transport* transport, az_result result)
{
  unsigned long e;
  while ((e = ERR_get_error()) != 0)
  {
    _az_mqtt_io_layer_report(&transport->errors, AZ_MQTT_NATIVE_ERROR_TLS, (int32_t)e, result);
  }
}

/**
 * @brief Classify a failed OpenSSL call (@p ssl_error from SSL_get_error()) and report its
 * native errors. Failures below were reported there (during the handshake, as
 * AZ_MQTT_ERROR_TLS_HANDSHAKE).
 */
static az_result _tls_failure(_tls_transport* transport, int ssl_error, bool handshake)
{
  az_result rc;
  if (handshake && SSL_get_verify_result(transport->ssl) != X509_V_OK)
  {
    rc = AZ_MQTT_ERROR_TLS_VERIFY;
    _az_mqtt_io_layer_report(
        &transport->errors,
        AZ_MQTT_NATIVE_ERROR_TLS_VERIFY,
        (int32_t)SSL_get_verify_result(transport->ssl),
        rc);
  }
  else if (handshake)
  {
    rc = AZ_MQTT_ERROR_TLS_HANDSHAKE;
  }
  else if (az_result_failed(transport->lower_failure))
  {
    rc = transport->lower_failure;
  }
  else
  {
    unsigned long const library_error = ERR_peek_last_error();
    bool const closed = ssl_error == SSL_ERROR_ZERO_RETURN
        || (ssl_error == SSL_ERROR_SYSCALL && library_error == 0)
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
        || ERR_GET_REASON(library_error) == SSL_R_UNEXPECTED_EOF_WHILE_READING
#endif
        ;
    rc = closed ? AZ_MQTT_ERROR_CONNECTION_CLOSED : AZ_MQTT_ERROR_TRANSPORT;
  }
  _report_tls_queue(transport, rc);
  return rc;
}

/** @brief Whether OpenSSL only waits for bytes from below (the deadline passed). */
static bool _waiting(_tls_transport* transport, int ret)
{
  int const err = SSL_get_error(transport->ssl, ret);
  return (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
      && az_result_succeeded(transport->lower_failure);
}

/** @brief Drive the handshake until done, failed or @p deadline. */
static az_result _tls_handshake(_tls_transport* transport, int64_t deadline)
{
  transport->deadline = deadline;
  for (;;)
  {
    ERR_clear_error();
    transport->lower_failure = AZ_OK;
    int const ret = SSL_connect(transport->ssl);
    if (ret == 1)
    {
      // SSL_VERIFY_PEER already aborts on a bad chain or name; checked again so
      // it stays so if the verify mode is ever relaxed.
      long const verify = SSL_get_verify_result(transport->ssl);
      if (verify == X509_V_OK && SSL_get0_peer_certificate(transport->ssl) != NULL)
      {
        return AZ_OK;
      }
      if (verify != X509_V_OK)
      {
        _az_mqtt_io_layer_report(
            &transport->errors,
            AZ_MQTT_NATIVE_ERROR_TLS_VERIFY,
            (int32_t)verify,
            AZ_MQTT_ERROR_TLS_VERIFY);
      }
      return AZ_MQTT_ERROR_TLS_VERIFY;
    }
    if (!_waiting(transport, ret))
    {
      return _tls_failure(transport, SSL_get_error(transport->ssl, ret), true);
    }
    if (_az_mqtt_io_layer_remaining(deadline) == 0)
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }
  }
}

/** @brief Free the TLS session (not the connection below). */
static void _tls_free(_tls_transport* transport)
{
  SSL_free(transport->ssl); // Frees the BIO.
  transport->ssl = NULL;
  SSL_CTX_free(transport->ssl_ctx);
  transport->ssl_ctx = NULL;
}

// ──────────────────────── Transport ──────────────────────────

/**
 * @brief Send close_notify, once per connection, if the session is up: without waiting on the
 * layer below. Native errors it meets are reported (from below, with their result; OpenSSL's,
 * with the failure below or AZ_MQTT_ERROR_TRANSPORT).
 */
static void _send_close_notify(_tls_transport* transport)
{
  if (transport->stage != _OPEN || transport->ssl == NULL || transport->closing)
  {
    return;
  }
  transport->closing = true;
  transport->deadline = 0; // Never wait.
  ERR_clear_error();
  transport->lower_failure = AZ_OK;
  int const ret = SSL_shutdown(transport->ssl);
  if (ret < 0 && !_waiting(transport, ret))
  {
    _report_tls_queue(
        transport,
        az_result_failed(transport->lower_failure) ? transport->lower_failure
                                                   : AZ_MQTT_ERROR_TRANSPORT);
  }
  ERR_clear_error();
}

static void _close(az_mqtt_transport* t)
{
  _send_close_notify(_T(t));
  _tls_free(_T(t));
  _T(t)->stage = _IDLE;
  az_mqtt_transport_close(_LOWER_T(_T(t)));
}

static az_result _connect_start(
    az_mqtt_transport* t,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  _tls_transport* const transport = _T(t);
  _close(t); // A connect replaces the connection.
  transport->closing = false;
  transport->errors.connect_attempt++;
  if (tls_options != NULL)
  {
    ERR_clear_error(); // Only errors from here on belong to this connect.
    az_result rc = _check_tls_options(tls_options);
    if (az_result_succeeded(rc))
    {
      rc = _tls_prepare(transport, host, tls_options);
    }
    BIO* const bio = az_result_succeeded(rc) ? _bio_new(transport) : NULL;
    if (bio != NULL)
    {
      SSL_set_bio(transport->ssl, bio, bio);
    }
    else if (az_result_succeeded(rc))
    {
      rc = AZ_MQTT_ERROR_TRANSPORT;
    }
    if (az_result_failed(rc))
    {
      _report_tls_queue(transport, rc);
      _close(t);
      return rc;
    }
  }
  az_result const rc = az_mqtt_transport_connect_start(_LOWER_T(transport), host, port, NULL);
  if (az_result_failed(rc))
  {
    _tls_free(transport);
    return rc;
  }
  transport->stage = _LOWER;
  return AZ_OK;
}

static az_result _connect_poll(az_mqtt_transport* t, int32_t timeout_ms)
{
  _tls_transport* const transport = _T(t);
  int64_t const deadline = _az_mqtt_io_layer_deadline(timeout_ms);
  if (transport->stage == _OPEN || transport->stage == _IDLE)
  {
    return transport->stage == _OPEN ? AZ_OK : AZ_MQTT_ERROR_INVALID_STATE;
  }
  if (transport->stage == _LOWER)
  {
    az_result const rc = az_mqtt_transport_connect_poll(_LOWER_T(transport), timeout_ms);
    if (az_result_failed(rc))
    {
      if (rc != AZ_MQTT_ERROR_TIMEOUT)
      {
        _close(t);
      }
      return rc;
    }
    transport->stage = transport->ssl != NULL ? _HANDSHAKE : _OPEN;
  }
  if (transport->stage == _HANDSHAKE)
  {
    transport->errors.phase_result = AZ_MQTT_ERROR_TLS_HANDSHAKE;
    az_result const rc = _tls_handshake(transport, deadline);
    transport->errors.phase_result = AZ_OK;
    if (az_result_failed(rc))
    {
      if (rc != AZ_MQTT_ERROR_TIMEOUT)
      {
        _close(t);
      }
      return rc;
    }
    transport->stage = _OPEN;
  }
  return AZ_OK;
}

static az_result _send(az_mqtt_transport* t, az_span data)
{
  _tls_transport* const transport = _T(t);
  if (transport->stage != _OPEN)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  if (transport->ssl == NULL)
  {
    return az_mqtt_transport_send(_LOWER_T(transport), data);
  }
  transport->deadline = _az_mqtt_io_layer_deadline(AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS);
  uint8_t const* ptr = az_span_ptr(data);
  int32_t remaining = az_span_size(data);
  while (remaining > 0)
  {
    ERR_clear_error();
    transport->lower_failure = AZ_OK;
    int const n = SSL_write(transport->ssl, ptr, remaining);
    if (n > 0)
    {
      ptr += n;
      remaining -= n;
      continue;
    }
    bool const waiting = _waiting(transport, n);
    if (waiting && _az_mqtt_io_layer_remaining(transport->deadline) > 0)
    {
      continue;
    }
    // A partial packet may be on the wire: the connection is unusable.
    transport->stage = _IDLE;
    return waiting ? AZ_MQTT_ERROR_TIMEOUT
                   : _tls_failure(transport, SSL_get_error(transport->ssl, n), false);
  }
  return AZ_OK;
}

static az_result
_receive(az_mqtt_transport* t, az_span buffer, int32_t timeout_ms, az_span* out_received)
{
  _tls_transport* const transport = _T(t);
  *out_received = AZ_SPAN_EMPTY;
  if (transport->stage != _OPEN)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  if (transport->ssl == NULL)
  {
    return az_mqtt_transport_receive(_LOWER_T(transport), buffer, timeout_ms, out_received);
  }
  transport->deadline = _az_mqtt_io_layer_deadline(timeout_ms);
  for (;;)
  {
    // Read first: decrypted bytes may already be buffered inside OpenSSL.
    ERR_clear_error();
    transport->lower_failure = AZ_OK;
    int const n = SSL_read(transport->ssl, az_span_ptr(buffer), az_span_size(buffer));
    if (n > 0)
    {
      *out_received = az_span_slice(buffer, 0, n);
      return AZ_OK;
    }
    if (!_waiting(transport, n))
    {
      transport->stage = _IDLE;
      return _tls_failure(transport, SSL_get_error(transport->ssl, n), false);
    }
    if (_az_mqtt_io_layer_remaining(transport->deadline) == 0)
    {
      return AZ_OK; // Timed out (a partial record waits for the next call).
    }
  }
}

static void _shutdown(az_mqtt_transport* t)
{
  _send_close_notify(_T(t));
  az_mqtt_transport_shutdown(_LOWER_T(_T(t)));
}

static az_result _set_proxy(az_mqtt_transport* t, az_mqtt_proxy_options const* proxy)
{
  return az_mqtt_transport_set_proxy(_LOWER_T(_T(t)), proxy);
}

static void
_set_error_callback(az_mqtt_transport* t, az_mqtt_transport_error_fn callback, void* context)
{
  _T(t)->errors.callback = callback;
  _T(t)->errors.context = context;
}

static az_mqtt_transport_vtable const _vtable = {
  _connect_start, _connect_poll, _send,       _receive,
  _shutdown,      _close,        _set_proxy, _set_error_callback,
};

int32_t _az_mqtt_tls_transport_sizeof(void) { return (int32_t)sizeof(_tls_transport); }

az_result _az_mqtt_tls_transport_init(az_mqtt_transport* transport, _az_mqtt_io_layer* lower)
{
  _az_PRECONDITION_NOT_NULL(transport);
  _az_PRECONDITION_NOT_NULL(lower);
  _tls_transport* const tls = _T(transport);
  memset(tls, 0, sizeof(*tls));
  tls->base.vtable = &_vtable;
  tls->lower = lower;
  _az_mqtt_io_layer_errors_attach(&tls->errors, &lower->base);
  return AZ_OK;
}
