// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file tls_mbedtls.c
 * @brief Internal: TLS layer through mbedTLS (2.x, 3.x, 4.x), over another transport.
 */

#include "az_mqtt_io_layers_internal.h"

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_span.h>
#include <azure/core/internal/az_precondition_internal.h>

#include <stdint.h>
#include <string.h>

#include <mbedtls/version.h> // MBEDTLS_VERSION_MAJOR in 2.x, 3.x and 4.x
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#if MBEDTLS_VERSION_MAJOR >= 4
// mbedTLS 4 (ESP-IDF v6): randomness comes from PSA; ctr_drbg/entropy are gone.
#define _AZ_MQTT_MBEDTLS_LEGACY_RNG 0
#else
#define _AZ_MQTT_MBEDTLS_LEGACY_RNG 1
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#endif
#if defined(MBEDTLS_PSA_CRYPTO_C) || MBEDTLS_VERSION_MAJOR >= 4
#include <psa/crypto.h>
#endif

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
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
#if _AZ_MQTT_MBEDTLS_LEGACY_RNG
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
#endif
  mbedtls_x509_crt ca_chain;
  mbedtls_x509_crt client_cert;
  mbedtls_pk_context client_key;
  /** @brief Until when I/O waits on the layer below (0 or past: does not wait). */
  int64_t deadline;
  /** @brief Why the transport below failed during the last mbedTLS call (AZ_OK: it did not). */
  az_result lower_failure;
  /** @brief The transport below had nothing to give (or no room) during the last mbedTLS call. */
  bool lower_idle;
  bool use_tls;
  /** @brief Contexts are initialized (and own resources, e.g. mutexes) until freed. */
  bool tls_contexts_ready;
  uint8_t stage;
  /** @brief close_notify sent (or tried) on this connection. */
  bool closing;
} _tls_transport;

#define _T(t) ((_tls_transport*)(t))
#define _LOWER_T(transport) (&(transport)->lower->base)

// ──────────────────────── I/O over the transport below ───────

static int _tls_send(void* ctx, unsigned char const* data, size_t size)
{
  _tls_transport* const transport = (_tls_transport*)ctx;
  int32_t const chunk = size > (size_t)INT32_MAX ? INT32_MAX : (int32_t)size;
  int32_t sent = 0;
  az_result const rc = _az_mqtt_io_layer_send_some(
      transport->lower,
      az_span_create((uint8_t*)(uintptr_t)data, chunk),
      _az_mqtt_io_layer_remaining(transport->deadline),
      &sent);
  if (az_result_failed(rc))
  {
    transport->lower_failure = rc;
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  if (sent == 0)
  {
    transport->lower_idle = true; // No room before the deadline.
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  }
  return (int)sent;
}

static int _tls_recv(void* ctx, unsigned char* buffer, size_t size)
{
  _tls_transport* const transport = (_tls_transport*)ctx;
  int32_t const chunk = size > (size_t)INT32_MAX ? INT32_MAX : (int32_t)size;
  az_span received;
  az_result const rc = az_mqtt_transport_receive(
      _LOWER_T(transport),
      az_span_create(buffer, chunk),
      _az_mqtt_io_layer_remaining(transport->deadline),
      &received);
  if (az_result_failed(rc))
  {
    transport->lower_failure = rc;
    return rc == AZ_MQTT_ERROR_CONNECTION_CLOSED ? 0 : MBEDTLS_ERR_NET_RECV_FAILED;
  }
  if (az_span_size(received) == 0)
  {
    transport->lower_idle = true; // Nothing before the deadline.
    return MBEDTLS_ERR_SSL_WANT_READ;
  }
  return az_span_size(received);
}

/** @brief Initialize every TLS context; called per connect, undone by _tls_contexts_free(). */
static void _tls_contexts_init(_tls_transport* transport)
{
  transport->tls_contexts_ready = true;
  mbedtls_ssl_init(&transport->ssl);
  mbedtls_ssl_config_init(&transport->conf);
#if _AZ_MQTT_MBEDTLS_LEGACY_RNG
  mbedtls_entropy_init(&transport->entropy);
  mbedtls_ctr_drbg_init(&transport->ctr_drbg);
#endif
  mbedtls_x509_crt_init(&transport->ca_chain);
  mbedtls_x509_crt_init(&transport->client_cert);
  mbedtls_pk_init(&transport->client_key);
  transport->use_tls = false;
}

/**
 * @brief Release every TLS context. Free-only, so a transport that is closed
 * and then discarded holds nothing (init() and close() have no deinit pair).
 */
static void _tls_contexts_free(_tls_transport* transport)
{
  if (!transport->tls_contexts_ready)
  {
    return;
  }
  transport->tls_contexts_ready = false;
  transport->use_tls = false;
  mbedtls_ssl_free(&transport->ssl);
  mbedtls_ssl_config_free(&transport->conf);
  mbedtls_x509_crt_free(&transport->ca_chain);
  mbedtls_x509_crt_free(&transport->client_cert);
  mbedtls_pk_free(&transport->client_key);
#if _AZ_MQTT_MBEDTLS_LEGACY_RNG
  mbedtls_ctr_drbg_free(&transport->ctr_drbg);
  mbedtls_entropy_free(&transport->entropy);
#endif
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
#if MBEDTLS_VERSION_MAJOR >= 4 || defined(MBEDTLS_USE_PSA_CRYPTO)
#define _AZ_MQTT_MBEDTLS_PSA_KEYS 1
#else
#define _AZ_MQTT_MBEDTLS_PSA_KEYS 0
#endif

static az_result _check_tls_options(az_mqtt_tls_options const* tls_options)
{
  az_result rc = az_mqtt_tls_options_check(tls_options);
  if (az_result_failed(rc))
  {
    return rc;
  }
  if (az_span_size(tls_options->client_key_uri) > 0)
  {
    return AZ_MQTT_ERROR_NOT_SUPPORTED; // OSSL_STORE URIs are OpenSSL; use client_key_psa_id.
  }
  if (tls_options->client_key_psa_id != 0 && !_AZ_MQTT_MBEDTLS_PSA_KEYS)
  {
    return AZ_MQTT_ERROR_NOT_SUPPORTED; // mbedTLS 3.x built without MBEDTLS_USE_PSA_CRYPTO.
  }
  if (az_span_size(tls_options->ca_cert_path) == 0 && az_span_size(tls_options->ca_cert_pem) == 0
      && tls_options->configure == NULL)
  {
    // mbedTLS has no system trust store; never connect without a trust anchor.
    return AZ_MQTT_ERROR_NOT_SUPPORTED;
  }
  return AZ_OK;
}

/**
 * @brief Run @p parse over @p pem as mbedTLS wants it: NUL-terminated, length
 * including the NUL. A span without one is copied (and the copy wiped).
 */
static int _parse_pem(
    az_span pem,
    int (*parse)(void* target, unsigned char const* buf, size_t len),
    void* target)
{
  size_t const size = (size_t)az_span_size(pem);
  uint8_t const* ptr = az_span_ptr(pem);
  if (size > 0 && ptr[size - 1] == '\0')
  {
    return parse(target, ptr, size);
  }
  unsigned char* copy = (unsigned char*)mbedtls_calloc(1, size + 1);
  if (copy == NULL)
  {
    return -1;
  }
  memcpy(copy, ptr, size);
  int ret = parse(target, copy, size + 1);
  mbedtls_platform_zeroize(copy, size + 1);
  mbedtls_free(copy);
  return ret;
}

static int _parse_crt(void* chain, unsigned char const* buf, size_t len)
{
  return mbedtls_x509_crt_parse((mbedtls_x509_crt*)chain, buf, len);
}

static int _parse_key(void* transport_ptr, unsigned char const* buf, size_t len)
{
  _tls_transport* transport = (_tls_transport*)transport_ptr;
#if MBEDTLS_VERSION_MAJOR == 3
  return mbedtls_pk_parse_key(
      &transport->client_key, buf, len, NULL, 0, mbedtls_ctr_drbg_random, &transport->ctr_drbg);
#else
  return mbedtls_pk_parse_key(&transport->client_key, buf, len, NULL, 0);
#endif
}

/**
 * @brief Build the TLS configuration and session for @p host; the socket is attached later.
 */
/** @brief Report @p ret (an mbedTLS or PSA error); AZ_MQTT_ERROR_TRANSPORT. */
static az_result _tls_setup_failure(_tls_transport* transport, int ret)
{
  _az_mqtt_io_layer_report(
      &transport->errors, AZ_MQTT_NATIVE_ERROR_TLS, ret, AZ_MQTT_ERROR_TRANSPORT);
  return AZ_MQTT_ERROR_TRANSPORT;
}

static az_result _tls_prepare(
    _tls_transport* transport,
    az_span host,
    az_mqtt_tls_options const* tls_options)
{
  int ret = 0;
#if defined(MBEDTLS_PSA_CRYPTO_C) || MBEDTLS_VERSION_MAJOR >= 4
  // Required by mbedTLS 4 and by 3.x TLS 1.3; idempotent.
  psa_status_t const status = psa_crypto_init();
  if (status != PSA_SUCCESS)
  {
    return _tls_setup_failure(transport, (int)status);
  }
#endif
#if _AZ_MQTT_MBEDTLS_LEGACY_RNG
  static const char pers[] = "az_mqtt_mbedtls";
  ret = mbedtls_ctr_drbg_seed(
      &transport->ctr_drbg,
      mbedtls_entropy_func,
      &transport->entropy,
      (const unsigned char*)pers,
      sizeof(pers) - 1);
  if (ret != 0)
  {
    return _tls_setup_failure(transport, ret);
  }
#endif
  ret = mbedtls_ssl_config_defaults(
      &transport->conf,
      MBEDTLS_SSL_IS_CLIENT,
      MBEDTLS_SSL_TRANSPORT_STREAM,
      MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
  {
    return _tls_setup_failure(transport, ret);
  }
#if _AZ_MQTT_MBEDTLS_LEGACY_RNG
  mbedtls_ssl_conf_rng(&transport->conf, mbedtls_ctr_drbg_random, &transport->ctr_drbg);
#endif

  char path[256];
  az_result rc = AZ_OK;
  if (az_span_size(tls_options->ca_cert_path) > 0)
  {
    rc = _span_to_cstr(tls_options->ca_cert_path, path, (int32_t)sizeof(path));
    ret = az_result_succeeded(rc) ? mbedtls_x509_crt_parse_file(&transport->ca_chain, path) : 0;
  }
  else if (az_span_size(tls_options->ca_cert_pem) > 0)
  {
    ret = _parse_pem(tls_options->ca_cert_pem, _parse_crt, &transport->ca_chain);
  }
  if (az_result_failed(rc) || ret != 0)
  {
    return az_result_failed(rc) ? rc : _tls_setup_failure(transport, ret);
  }
  if (az_span_size(tls_options->ca_cert_path) > 0 || az_span_size(tls_options->ca_cert_pem) > 0)
  {
    mbedtls_ssl_conf_ca_chain(&transport->conf, &transport->ca_chain, NULL);
  }
  // Otherwise `configure` installs trust; with none, the handshake fails.
  mbedtls_ssl_conf_authmode(&transport->conf, MBEDTLS_SSL_VERIFY_REQUIRED);

  bool has_cert = true;
  if (az_span_size(tls_options->client_cert_path) > 0)
  {
    rc = _span_to_cstr(tls_options->client_cert_path, path, (int32_t)sizeof(path));
    ret = az_result_succeeded(rc) ? mbedtls_x509_crt_parse_file(&transport->client_cert, path) : 0;
  }
  else if (az_span_size(tls_options->client_cert_pem) > 0)
  {
    ret = _parse_pem(tls_options->client_cert_pem, _parse_crt, &transport->client_cert);
  }
  else
  {
    has_cert = false;
  }

  if (az_result_succeeded(rc) && ret == 0 && has_cert)
  {
    // az_mqtt_tls_options_check() guaranteed exactly one key source.
    if (az_span_size(tls_options->client_key_path) > 0)
    {
      rc = _span_to_cstr(tls_options->client_key_path, path, (int32_t)sizeof(path));
#if MBEDTLS_VERSION_MAJOR == 3
      ret = az_result_succeeded(rc) ? mbedtls_pk_parse_keyfile(
                &transport->client_key, path, NULL, mbedtls_ctr_drbg_random, &transport->ctr_drbg)
                                    : 0;
#else // 2.x and 4.x take no RNG
      ret = az_result_succeeded(rc) ? mbedtls_pk_parse_keyfile(&transport->client_key, path, NULL)
                                    : 0;
#endif
    }
    else if (az_span_size(tls_options->client_key_pem) > 0)
    {
      ret = _parse_pem(tls_options->client_key_pem, _parse_key, transport);
    }
    else
    {
#if MBEDTLS_VERSION_MAJOR >= 4
      ret = mbedtls_pk_wrap_psa(&transport->client_key, (mbedtls_svc_key_id_t)tls_options->client_key_psa_id);
#elif _AZ_MQTT_MBEDTLS_PSA_KEYS
      ret = mbedtls_pk_setup_opaque(&transport->client_key, (psa_key_id_t)tls_options->client_key_psa_id);
#else
      ret = -1; // Unreachable: _check_tls_options() refused it.
#endif
    }
    if (az_result_succeeded(rc) && ret == 0)
    {
      ret = mbedtls_ssl_conf_own_cert(&transport->conf, &transport->client_cert, &transport->client_key);
    }
  }
  if (az_result_failed(rc) || ret != 0)
  {
    return az_result_failed(rc) ? rc : _tls_setup_failure(transport, ret);
  }

  if (tls_options->configure != NULL)
  {
    rc = tls_options->configure(&transport->conf, tls_options->configure_context);
    if (az_result_failed(rc))
    {
      return rc;
    }
    // Whatever the hook did, the server is verified.
    mbedtls_ssl_conf_authmode(&transport->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  }

  ret = mbedtls_ssl_setup(&transport->ssl, &transport->conf);
  if (ret != 0)
  {
    return _tls_setup_failure(transport, ret);
  }

  // Sets both SNI and the name the certificate is verified against.
  char host_str[256];
  rc = _span_to_cstr(host, host_str, (int32_t)sizeof(host_str));
  if (az_result_failed(rc))
  {
    return rc;
  }
  ret = mbedtls_ssl_set_hostname(&transport->ssl, host_str);
  if (ret != 0)
  {
    return _tls_setup_failure(transport, ret);
  }
  transport->use_tls = true;
  return AZ_OK;
}

/**
 * @brief Classify a failed mbedTLS call (@p ret) and report its native error. Failures below were
 * reported there (during the handshake, as AZ_MQTT_ERROR_TLS_HANDSHAKE).
 */
static az_result _tls_failure(_tls_transport* transport, int ret, bool handshake)
{
  _az_mqtt_io_layer_errors const* const sink = &transport->errors;
  // A certificate whose (extended) key usage does not allow it fails as a bad certificate.
  uint32_t const flags = handshake ? mbedtls_ssl_get_verify_result(&transport->ssl) : 0;
  if (handshake
      && (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
          || (ret == MBEDTLS_ERR_SSL_BAD_CERTIFICATE && flags != 0 && flags != UINT32_MAX)))
  {
    _az_mqtt_io_layer_report(
        sink, AZ_MQTT_NATIVE_ERROR_TLS_VERIFY, (int32_t)flags, AZ_MQTT_ERROR_TLS_VERIFY);
    _az_mqtt_io_layer_report(sink, AZ_MQTT_NATIVE_ERROR_TLS, ret, AZ_MQTT_ERROR_TLS_VERIFY);
    return AZ_MQTT_ERROR_TLS_VERIFY;
  }
  if (!handshake
      && (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_SSL_CONN_EOF))
  {
    return AZ_MQTT_ERROR_CONNECTION_CLOSED; // Orderly: no native error.
  }
  az_result rc = AZ_MQTT_ERROR_TLS_HANDSHAKE;
  if (!handshake)
  {
    rc = az_result_failed(transport->lower_failure)
        ? transport->lower_failure
        : (ret == MBEDTLS_ERR_NET_CONN_RESET ? AZ_MQTT_ERROR_CONNECTION_CLOSED
                                             : AZ_MQTT_ERROR_TRANSPORT);
  }
  _az_mqtt_io_layer_report(sink, AZ_MQTT_NATIVE_ERROR_TLS, ret, rc);
  return rc;
}

/** @brief Most retries in a row without waiting on the layer below (mbedTLS made progress). */
#define _IMMEDIATE_RETRIES_MAX 64

/** @brief Run before each mbedTLS I/O call. */
static void _before_call(_tls_transport* transport)
{
  transport->lower_failure = AZ_OK;
  transport->lower_idle = false;
}

/** @brief What to do after an mbedTLS call returned @p ret (not success). */
typedef enum
{
  _FAIL, ///< A failure: see _tls_failure().
  _AGAIN, ///< Call again now (a ticket was consumed, or a record without data; nothing waited).
  _WAIT, ///< The layer below had nothing (or no room): call again while time is left.
} _next;

static _next _after_call(_tls_transport* transport, int ret, int* in_out_immediate)
{
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
  bool const ticket = ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET;
#else
  bool const ticket = false;
#endif
  bool const want = ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE;
  if (az_result_failed(transport->lower_failure) || (!ticket && !want))
  {
    return _FAIL;
  }
  // mbedTLS returns WANT_READ after a record without application data (e.g. a TLS 1.3 session
  // ticket) with more possibly waiting below: only an idle layer below means waiting.
  if ((ticket || !transport->lower_idle) && ++*in_out_immediate <= _IMMEDIATE_RETRIES_MAX)
  {
    return _AGAIN;
  }
  *in_out_immediate = 0;
  return _WAIT;
}

/** @brief Drive the handshake until done, failed or @p deadline. */
static az_result _tls_handshake(_tls_transport* transport, int64_t deadline)
{
  transport->deadline = deadline;
  int immediate = 0;
  for (;;)
  {
    _before_call(transport);
    int const ret = mbedtls_ssl_handshake(&transport->ssl);
    if (ret == 0)
    {
      // VERIFY_REQUIRED already fails the handshake; checked again so it stays so.
      uint32_t const flags = mbedtls_ssl_get_verify_result(&transport->ssl);
      if (flags == 0)
      {
        return AZ_OK;
      }
      _az_mqtt_io_layer_report(
          &transport->errors,
          AZ_MQTT_NATIVE_ERROR_TLS_VERIFY,
          (int32_t)flags,
          AZ_MQTT_ERROR_TLS_VERIFY);
      return AZ_MQTT_ERROR_TLS_VERIFY;
    }
    _next const next = _after_call(transport, ret, &immediate);
    if (next == _FAIL)
    {
      return _tls_failure(transport, ret, true);
    }
    if (next == _WAIT && _az_mqtt_io_layer_remaining(deadline) == 0)
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }
  }
}

// ──────────────────────── Transport ──────────────────────────

/**
 * @brief Send close_notify, once per connection, if the session is up: without waiting on the
 * layer below. Native errors it meets are reported (from below, with their result; mbedTLS's,
 * with the failure below or AZ_MQTT_ERROR_TRANSPORT).
 */
static void _send_close_notify(_tls_transport* transport)
{
  if (transport->stage != _OPEN || !transport->use_tls || transport->closing)
  {
    return;
  }
  transport->closing = true;
  transport->deadline = 0; // Never wait.
  _before_call(transport);
  int const ret = mbedtls_ssl_close_notify(&transport->ssl);
  int immediate = _IMMEDIATE_RETRIES_MAX; // One attempt.
  if (ret != 0 && _after_call(transport, ret, &immediate) == _FAIL)
  {
    _az_mqtt_io_layer_report(
        &transport->errors,
        AZ_MQTT_NATIVE_ERROR_TLS,
        ret,
        az_result_failed(transport->lower_failure) ? transport->lower_failure
                                                   : AZ_MQTT_ERROR_TRANSPORT);
  }
}

static void _close(az_mqtt_transport* t)
{
  _send_close_notify(_T(t));
  _tls_contexts_free(_T(t));
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
    az_result rc = _check_tls_options(tls_options);
    if (az_result_succeeded(rc))
    {
      _tls_contexts_init(transport);
      rc = _tls_prepare(transport, host, tls_options);
    }
    if (az_result_failed(rc))
    {
      _close(t);
      return rc;
    }
    mbedtls_ssl_set_bio(&transport->ssl, transport, _tls_send, _tls_recv, NULL);
  }
  az_result const rc = az_mqtt_transport_connect_start(_LOWER_T(transport), host, port, NULL);
  if (az_result_failed(rc))
  {
    _tls_contexts_free(transport);
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
    transport->stage = transport->use_tls ? _HANDSHAKE : _OPEN;
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
  if (!transport->use_tls)
  {
    return az_mqtt_transport_send(_LOWER_T(transport), data);
  }
  transport->deadline = _az_mqtt_io_layer_deadline(AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS);
  uint8_t const* ptr = az_span_ptr(data);
  int32_t remaining = az_span_size(data);
  int immediate = 0;
  while (remaining > 0)
  {
    _before_call(transport);
    int const n = mbedtls_ssl_write(&transport->ssl, ptr, (size_t)remaining);
    if (n > 0)
    {
      ptr += n;
      remaining -= n;
      continue;
    }
    _next const next = _after_call(transport, n, &immediate);
    if (next == _AGAIN || (next == _WAIT && _az_mqtt_io_layer_remaining(transport->deadline) > 0))
    {
      continue;
    }
    // A partial packet may be on the wire: the connection is unusable.
    transport->stage = _IDLE;
    return next == _WAIT ? AZ_MQTT_ERROR_TIMEOUT : _tls_failure(transport, n, false);
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
  if (!transport->use_tls)
  {
    return az_mqtt_transport_receive(_LOWER_T(transport), buffer, timeout_ms, out_received);
  }
  transport->deadline = _az_mqtt_io_layer_deadline(timeout_ms);
  int immediate = 0;
  for (;;)
  {
    // Read first: decrypted bytes may already be buffered inside mbedTLS.
    _before_call(transport);
    int const n
        = mbedtls_ssl_read(&transport->ssl, az_span_ptr(buffer), (size_t)az_span_size(buffer));
    if (n > 0)
    {
      *out_received = az_span_slice(buffer, 0, n);
      return AZ_OK;
    }
    _next const next = _after_call(transport, n, &immediate);
    if (next == _FAIL)
    {
      transport->stage = _IDLE;
      return _tls_failure(transport, n, false);
    }
    if (next == _WAIT && _az_mqtt_io_layer_remaining(transport->deadline) == 0)
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
