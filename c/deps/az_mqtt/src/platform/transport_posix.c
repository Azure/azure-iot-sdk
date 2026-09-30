// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file transport_posix.c
 * @brief POSIX sockets transport, with TLS through OpenSSL when AZ_MQTT_TLS_OPENSSL is defined.
 */

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE // inet_pton, ssize_t, recv under -std=c99
#endif

#include "az_mqtt_socket_posix.h"

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_span.h>
#include <azure/core/internal/az_precondition_internal.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#ifdef AZ_MQTT_TLS_OPENSSL
#include <arpa/inet.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/store.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <sys/socket.h>
#endif

// ──────────────────────── Platform-specific transport ────────

/** @brief Connection progress. */
typedef enum
{
  _TRANSPORT_IDLE = 0,
  _TRANSPORT_TCP,
  _TRANSPORT_TLS,
  _TRANSPORT_CONNECTED,
} _transport_state;

struct az_mqtt_transport
{
  int socket_fd;
  _az_mqtt_tcp_connect tcp;
  _transport_state state;
#ifdef AZ_MQTT_TLS_OPENSSL
  SSL_CTX* ssl_ctx;
  SSL* ssl;
#endif
  bool connected;
};

AZ_NODISCARD int32_t az_mqtt_transport_sizeof(void) { return (int32_t)sizeof(az_mqtt_transport); }

AZ_NODISCARD az_result az_mqtt_transport_init(az_mqtt_transport* transport)
{
  _az_PRECONDITION_NOT_NULL(transport);
  memset(transport, 0, sizeof(*transport));
  transport->socket_fd = -1;
  _az_mqtt_tcp_connect_init(&transport->tcp);
  transport->state = _TRANSPORT_IDLE;
  return AZ_OK;
}

#ifdef AZ_MQTT_TLS_OPENSSL
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

// ──────────────────────── OpenSSL socket BIO ─────────────────
//
// OpenSSL's socket BIO writes with write(), which raises SIGPIPE when the peer
// has reset the connection. This BIO uses send(MSG_NOSIGNAL) instead.

static BIO_METHOD* s_bio_method;
static pthread_once_t s_bio_once = PTHREAD_ONCE_INIT;

static int _bio_fd(BIO* bio) { return (int)(intptr_t)BIO_get_data(bio); }

static int _bio_write(BIO* bio, char const* data, int size)
{
  BIO_clear_retry_flags(bio);
  int32_t n = _az_mqtt_send_nosignal(_bio_fd(bio), (uint8_t const*)data, size);
  if (n == 0 && size > 0)
  {
    BIO_set_retry_write(bio);
    return -1;
  }
  return n;
}

static int _bio_read(BIO* bio, char* buffer, int size)
{
  BIO_clear_retry_flags(bio);
  ssize_t n = recv(_bio_fd(bio), buffer, (size_t)size, 0);
  if (n < 0 && _az_mqtt_would_block())
  {
    BIO_set_retry_read(bio);
  }
  return (int)n;
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
  BIO_METHOD* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "az_mqtt_socket");
  if (m != NULL
      && (BIO_meth_set_write(m, _bio_write) != 1 || BIO_meth_set_read(m, _bio_read) != 1
          || BIO_meth_set_ctrl(m, _bio_ctrl) != 1 || BIO_meth_set_create(m, _bio_create) != 1))
  {
    BIO_meth_free(m);
    m = NULL;
  }
  s_bio_method = m;
}

static BIO* _bio_new(int fd)
{
  if (pthread_once(&s_bio_once, _bio_method_init) != 0 || s_bio_method == NULL)
  {
    return NULL;
  }
  BIO* bio = BIO_new(s_bio_method);
  if (bio != NULL)
  {
    BIO_set_data(bio, (void*)(intptr_t)fd);
  }
  return bio;
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
  ERR_clear_error(); // End of input is reported as an error.
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
  ERR_clear_error();
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
    if (info != NULL && OSSL_STORE_INFO_get_type(info) == OSSL_STORE_INFO_PKEY)
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
    az_mqtt_transport* transport,
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

/**
 * @brief Map an OpenSSL I/O result to a wait: 1 retried after waiting, 0 timed out, -1 failed.
 */
static int _tls_wait(az_mqtt_transport* transport, int ret, int64_t deadline)
{
  int err = SSL_get_error(transport->ssl, ret);
  if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
  {
    return -1;
  }
  return _az_mqtt_wait_fd(
      transport->socket_fd,
      err == SSL_ERROR_WANT_READ ? _AZ_MQTT_WAIT_READ : _AZ_MQTT_WAIT_WRITE,
      _az_mqtt_remaining_ms(deadline));
}

/** @brief Drive the handshake until done, failed or @p deadline. */
static az_result _tls_handshake(az_mqtt_transport* transport, int64_t deadline)
{
  for (;;)
  {
    ERR_clear_error();
    int ret = SSL_connect(transport->ssl);
    if (ret == 1)
    {
      // SSL_VERIFY_PEER already aborts on a bad chain or name; checked again so
      // it stays so if the verify mode is ever relaxed.
      return (SSL_get_verify_result(transport->ssl) == X509_V_OK
              && SSL_get0_peer_certificate(transport->ssl) != NULL)
          ? AZ_OK
          : AZ_MQTT_ERROR_TRANSPORT;
    }
    int w = _tls_wait(transport, ret, deadline);
    if (w == 0)
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }
    if (w < 0)
    {
      return AZ_MQTT_ERROR_TRANSPORT;
    }
  }
}
#endif // AZ_MQTT_TLS_OPENSSL

// ──────────────────────── Connect ────────────────────────────

AZ_NODISCARD az_result az_mqtt_transport_connect_start(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  _az_PRECONDITION_NOT_NULL(transport);

  az_mqtt_transport_close(transport);

#ifdef AZ_MQTT_TLS_OPENSSL
  if (tls_options != NULL)
  {
    az_result rc = _check_tls_options(tls_options);
    if (az_result_succeeded(rc))
    {
      rc = _tls_prepare(transport, host, tls_options);
    }
    if (az_result_failed(rc))
    {
      az_mqtt_transport_close(transport);
      return rc;
    }
  }
#else
  if (tls_options != NULL)
  {
    // Built without TLS: never fall back to plaintext.
    return AZ_MQTT_ERROR_NOT_SUPPORTED;
  }
#endif

  az_result rc = _az_mqtt_tcp_connect_start(&transport->tcp, host, port);
  if (az_result_failed(rc))
  {
    az_mqtt_transport_close(transport);
    return rc;
  }
  transport->state = _TRANSPORT_TCP;
  return AZ_OK;
}

AZ_NODISCARD az_result
az_mqtt_transport_connect_poll(az_mqtt_transport* transport, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(transport);
  int64_t const deadline = _az_mqtt_deadline(timeout_ms);
  az_result rc = AZ_OK;

  if (transport->state == _TRANSPORT_TCP)
  {
    rc = _az_mqtt_tcp_connect_poll(&transport->tcp, _az_mqtt_remaining_ms(deadline));
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      return rc;
    }
    if (az_result_succeeded(rc))
    {
      transport->socket_fd = transport->tcp.fd;
      _az_mqtt_tcp_connect_init(&transport->tcp);
      transport->state = _TRANSPORT_CONNECTED;
#ifdef AZ_MQTT_TLS_OPENSSL
      if (transport->ssl != NULL)
      {
        BIO* bio = _bio_new(transport->socket_fd);
        if (bio == NULL)
        {
          rc = AZ_MQTT_ERROR_TRANSPORT;
        }
        else
        {
          SSL_set_bio(transport->ssl, bio, bio);
          transport->state = _TRANSPORT_TLS;
        }
      }
#endif
    }
  }

#ifdef AZ_MQTT_TLS_OPENSSL
  if (az_result_succeeded(rc) && transport->state == _TRANSPORT_TLS)
  {
    rc = _tls_handshake(transport, deadline);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      return rc;
    }
    if (az_result_succeeded(rc))
    {
      transport->state = _TRANSPORT_CONNECTED;
    }
  }
#endif

  if (az_result_succeeded(rc) && transport->state != _TRANSPORT_CONNECTED)
  {
    rc = AZ_MQTT_ERROR_INVALID_STATE;
  }
  if (az_result_failed(rc))
  {
    az_mqtt_transport_close(transport);
    return rc;
  }
  transport->connected = true;
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_transport_connect(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  az_result rc = az_mqtt_transport_connect_start(transport, host, port, tls_options);
  if (az_result_succeeded(rc))
  {
    rc = az_mqtt_transport_connect_poll(transport, AZ_MQTT_TRANSPORT_CONNECT_TIMEOUT_MS);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      az_mqtt_transport_close(transport);
    }
  }
  return rc;
}

// ──────────────────────── I/O ────────────────────────────────

AZ_NODISCARD az_result az_mqtt_transport_send(az_mqtt_transport* transport, az_span data)
{
  _az_PRECONDITION_NOT_NULL(transport);
  if (!transport->connected)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  int64_t const deadline = _az_mqtt_deadline(AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS);
  uint8_t const* ptr = az_span_ptr(data);
  int32_t remaining = az_span_size(data);

  while (remaining > 0)
  {
    int w;
#ifdef AZ_MQTT_TLS_OPENSSL
    if (transport->ssl != NULL)
    {
      ERR_clear_error();
      int n = SSL_write(transport->ssl, ptr, remaining);
      if (n > 0)
      {
        ptr += n;
        remaining -= n;
        continue;
      }
      w = _tls_wait(transport, n, deadline);
    }
    else
#endif
    {
      int32_t n = _az_mqtt_send_nosignal(transport->socket_fd, ptr, remaining);
      if (n > 0)
      {
        ptr += n;
        remaining -= n;
        continue;
      }
      w = n < 0 ? -1
                : _az_mqtt_wait_fd(
                    transport->socket_fd, _AZ_MQTT_WAIT_WRITE, _az_mqtt_remaining_ms(deadline));
    }
    if (w <= 0)
    {
      // A partial packet may be on the wire: the connection is unusable.
      transport->connected = false;
      return w == 0 ? AZ_MQTT_ERROR_TIMEOUT : AZ_MQTT_ERROR_TRANSPORT;
    }
  }
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_transport_receive(
    az_mqtt_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received)
{
  _az_PRECONDITION_NOT_NULL(transport);
  _az_PRECONDITION_NOT_NULL(out_received);

  *out_received = AZ_SPAN_EMPTY;
  if (!transport->connected)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  int64_t const deadline = _az_mqtt_deadline(timeout_ms);
  for (;;)
  {
    int w;
#ifdef AZ_MQTT_TLS_OPENSSL
    if (transport->ssl != NULL)
    {
      // Read first: decrypted bytes may already be buffered inside OpenSSL.
      ERR_clear_error();
      int n = SSL_read(transport->ssl, az_span_ptr(buffer), az_span_size(buffer));
      if (n > 0)
      {
        *out_received = az_span_slice(buffer, 0, n);
        return AZ_OK;
      }
      // A partial TLS record returns WANT_READ; wait for the rest within the deadline.
      w = _tls_wait(transport, n, deadline);
    }
    else
#endif
    {
      w = _az_mqtt_wait_fd(
          transport->socket_fd, _AZ_MQTT_WAIT_READ, _az_mqtt_remaining_ms(deadline));
      if (w > 0)
      {
        int32_t n = _az_mqtt_recv_nonblocking(
            transport->socket_fd, az_span_ptr(buffer), az_span_size(buffer));
        if (n > 0)
        {
          *out_received = az_span_slice(buffer, 0, n);
          return AZ_OK;
        }
        w = n < 0 ? -1 : 1;
      }
    }
    if (w == 0)
    {
      return AZ_OK; // Timed out: out_received stays empty.
    }
    if (w < 0)
    {
      transport->connected = false;
      return AZ_MQTT_ERROR_TRANSPORT;
    }
  }
}

void az_mqtt_transport_close(az_mqtt_transport* transport)
{
  if (transport == NULL)
  {
    return;
  }
#ifdef AZ_MQTT_TLS_OPENSSL
  if (transport->ssl != NULL)
  {
    if (transport->connected)
    {
      (void)SSL_shutdown(transport->ssl); // Best effort; non-blocking.
    }
    SSL_free(transport->ssl);
    transport->ssl = NULL;
  }
  if (transport->ssl_ctx != NULL)
  {
    SSL_CTX_free(transport->ssl_ctx);
    transport->ssl_ctx = NULL;
  }
#endif
  _az_mqtt_tcp_connect_cancel(&transport->tcp);
  if (transport->socket_fd >= 0)
  {
    close(transport->socket_fd);
    transport->socket_fd = -1;
  }
  transport->state = _TRANSPORT_IDLE;
  transport->connected = false;
}
