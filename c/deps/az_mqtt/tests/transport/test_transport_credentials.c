// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_transport_credentials.c
 * @brief In-memory PEM, key custody (PSA key id / OSSL_STORE URI) and the
 * `configure` hook, against the in-process peer.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include "test_server.h"

#if defined(AZ_MQTT_TEST_BACKEND_OPENSSL)
#include <openssl/ssl.h>
#elif defined(AZ_MQTT_TEST_BACKEND_MBEDTLS)
#include <mbedtls/build_info.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/psa_util.h>
#include <psa/crypto.h>
#endif

#if !defined(AZ_MQTT_TEST_BACKEND_NONE)

typedef struct
{
  test_server* server;
  az_mqtt_transport* transport;
  char ca[8192];
  char cert[8192];
  char key[8192];
} fixture;

static size_t _read_file(char const* path, char* buf, size_t cap)
{
  FILE* f = fopen(path, "rb");
  assert_non_null(f);
  size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = '\0';
  return n;
}

/** @brief PEM span without the terminating NUL (the common case). */
static az_span _pem(char* s) { return az_span_create((uint8_t*)s, (int32_t)strlen(s)); }

static az_span _str(char const* s) { return az_span_create_from_str((char*)(uintptr_t)s); }

static void _setup(fixture* f, test_server_options const* o)
{
  memset(f, 0, sizeof(*f));
  f->server = test_server_start(o);
  assert_non_null(f->server);
  (void)_read_file(test_server_ca_path(f->server), f->ca, sizeof(f->ca));
  (void)_read_file(test_server_client_cert_path(f->server), f->cert, sizeof(f->cert));
  (void)_read_file(test_server_client_key_path(f->server), f->key, sizeof(f->key));
  f->transport = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  assert_non_null(f->transport);
  assert_int_equal(az_mqtt_transport_init(f->transport), AZ_OK);
}

static void _teardown(fixture* f)
{
  az_mqtt_transport_close(f->transport);
  free(f->transport);
  test_server_stop(f->server);
}

static az_result _connect(fixture* f, az_mqtt_tls_options const* t)
{
  return az_mqtt_transport_connect(f->transport, _str("localhost"), test_server_port(f->server), t);
}

/** @brief CONNECT gets a CONNACK (a refused client certificate can surface only here with TLS 1.3). */
static bool _exchange(fixture* f)
{
  static uint8_t connect_v5[] = { 0x10, 0x0E, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x05,
                                  0x02, 0x00, 0x3C, 0x00, 0x00, 0x01, 'c' };
  if (az_result_failed(az_mqtt_transport_send(f->transport, AZ_SPAN_FROM_BUFFER(connect_v5))))
  {
    return false;
  }
  uint8_t buf[8];
  az_span got = AZ_SPAN_EMPTY;
  return az_result_succeeded(
             az_mqtt_transport_receive(f->transport, AZ_SPAN_FROM_BUFFER(buf), 3000, &got))
      && az_span_size(got) > 0 && buf[0] == 0x20;
}

static test_server_options _mutual(void)
{
  test_server_options o = test_server_options_default();
  o.require_client_cert = true;
  return o;
}

// ──────────────────────── In-memory PEM ──────────────────────

static void ca_from_memory_is_trusted(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  fixture f;
  _setup(&f, &o);
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.ca_cert_pem = _pem(f.ca);
  assert_int_equal(_connect(&f, &t), AZ_OK);
  assert_true(_exchange(&f));

  // With its NUL included (mbedTLS then parses in place).
  t.ca_cert_pem = az_span_create((uint8_t*)f.ca, (int32_t)strlen(f.ca) + 1);
  assert_int_equal(_connect(&f, &t), AZ_OK);
  assert_true(_exchange(&f));
  _teardown(&f);
}

static void untrusted_server_is_rejected_with_ca_from_memory(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.untrusted_ca = true;
  fixture f;
  _setup(&f, &o);
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.ca_cert_pem = _pem(f.ca);
  assert_true(az_result_failed(_connect(&f, &t)));
  assert_int_equal(test_server_handshakes(f.server), 0);
  _teardown(&f);
}

static void client_identity_from_memory_is_presented(void** state)
{
  (void)state;
  test_server_options o = _mutual();
  fixture f;
  _setup(&f, &o);
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.ca_cert_pem = _pem(f.ca);
  t.client_cert_pem = _pem(f.cert);
  t.client_key_pem = _pem(f.key);
  assert_int_equal(_connect(&f, &t), AZ_OK);
  assert_true(_exchange(&f));
  assert_true(test_server_saw_client_cert(f.server));
  _teardown(&f);
}

static void conflicting_sources_are_refused_before_connecting(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  fixture f;
  _setup(&f, &o);
  az_mqtt_tls_options t;

  t = az_mqtt_tls_options_default(); // Two CA sources.
  t.ca_cert_pem = _pem(f.ca);
  t.ca_cert_path = _str(test_server_ca_path(f.server));
  assert_int_equal(_connect(&f, &t), AZ_MQTT_ERROR_INVALID_CONFIG);

  t = az_mqtt_tls_options_default(); // Certificate without a key.
  t.ca_cert_pem = _pem(f.ca);
  t.client_cert_pem = _pem(f.cert);
  assert_int_equal(_connect(&f, &t), AZ_MQTT_ERROR_INVALID_CONFIG);

  t.client_key_pem = _pem(f.key); // Two key sources.
  t.client_key_path = _str(test_server_client_key_path(f.server));
  assert_int_equal(_connect(&f, &t), AZ_MQTT_ERROR_INVALID_CONFIG);

  t = az_mqtt_tls_options_default(); // Key without a certificate.
  t.ca_cert_pem = _pem(f.ca);
  t.client_key_pem = _pem(f.key);
  assert_int_equal(_connect(&f, &t), AZ_MQTT_ERROR_INVALID_CONFIG);

  assert_int_equal(test_server_accepted(f.server), 0);
  _teardown(&f);
}

// ──────────────────────── configure hook ─────────────────────

typedef struct
{
  char const* ca_path;
  bool disable_verification;
  int calls;
#if defined(AZ_MQTT_TEST_BACKEND_MBEDTLS)
  mbedtls_x509_crt ca;
#endif
} hook_state;

static az_result _hook(void* native, void* context)
{
  hook_state* h = (hook_state*)context;
  h->calls++;
#if defined(AZ_MQTT_TEST_BACKEND_OPENSSL)
  SSL_CTX* ctx = (SSL_CTX*)native;
  if (h->ca_path != NULL && SSL_CTX_load_verify_locations(ctx, h->ca_path, NULL) != 1)
  {
    return AZ_ERROR_ARG;
  }
  if (h->disable_verification)
  {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  }
#else
  mbedtls_ssl_config* conf = (mbedtls_ssl_config*)native;
  if (h->ca_path != NULL)
  {
    if (mbedtls_x509_crt_parse_file(&h->ca, h->ca_path) != 0)
    {
      return AZ_ERROR_ARG;
    }
    mbedtls_ssl_conf_ca_chain(conf, &h->ca, NULL);
  }
  if (h->disable_verification)
  {
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
  }
#endif
  return AZ_OK;
}

static void _hook_init(hook_state* h)
{
  memset(h, 0, sizeof(*h));
#if defined(AZ_MQTT_TEST_BACKEND_MBEDTLS)
  mbedtls_x509_crt_init(&h->ca);
#endif
}

static void _hook_free(hook_state* h)
{
#if defined(AZ_MQTT_TEST_BACKEND_MBEDTLS)
  mbedtls_x509_crt_free(&h->ca);
#else
  (void)h;
#endif
}

static void the_configure_hook_can_install_trust(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  fixture f;
  _setup(&f, &o);
  hook_state h;
  _hook_init(&h);
  h.ca_path = test_server_ca_path(f.server);
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.configure = _hook;
  t.configure_context = &h;
  assert_int_equal(_connect(&f, &t), AZ_OK);
  assert_true(_exchange(&f));
  assert_int_equal(h.calls, 1);
  _hook_free(&h);
  _teardown(&f);
}

static void a_failing_configure_hook_aborts_before_connecting(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  fixture f;
  _setup(&f, &o);
  hook_state h;
  _hook_init(&h);
  h.ca_path = "/nonexistent/ca.pem";
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.configure = _hook;
  t.configure_context = &h;
  assert_int_equal(_connect(&f, &t), AZ_ERROR_ARG);
  assert_int_equal(test_server_accepted(f.server), 0);
  _hook_free(&h);
  _teardown(&f);
}

static void a_configure_hook_turning_verification_off_is_caught(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.untrusted_ca = true;
  fixture f;
  _setup(&f, &o);
  hook_state h;
  _hook_init(&h);
  h.ca_path = test_server_ca_path(f.server);
  h.disable_verification = true;
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.configure = _hook;
  t.configure_context = &h;
  assert_true(az_result_failed(_connect(&f, &t)));
  assert_int_equal(test_server_handshakes(f.server), 0);
  _hook_free(&h);
  _teardown(&f);
}

// ──────────────────────── Key custody ────────────────────────

static void a_key_uri_is_used_or_refused(void** state)
{
  (void)state;
  test_server_options o = _mutual();
  fixture f;
  _setup(&f, &o);
  char uri[256];
  snprintf(uri, sizeof(uri), "file:%s", test_server_client_key_path(f.server));
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.ca_cert_pem = _pem(f.ca);
  t.client_cert_pem = _pem(f.cert);
  t.client_key_uri = _str(uri);
#if defined(AZ_MQTT_TEST_BACKEND_OPENSSL)
  // OSSL_STORE: the same path a pkcs11/tpm2 provider URI takes.
  assert_int_equal(_connect(&f, &t), AZ_OK);
  assert_true(_exchange(&f));
  assert_true(test_server_saw_client_cert(f.server));
#else
  assert_int_equal(_connect(&f, &t), AZ_MQTT_ERROR_NOT_SUPPORTED);
  assert_int_equal(test_server_accepted(f.server), 0);
#endif
  _teardown(&f);
}

static void a_psa_key_is_used_or_refused(void** state)
{
  (void)state;
  test_server_options o = _mutual();
  fixture f;
  _setup(&f, &o);
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.ca_cert_pem = _pem(f.ca);
  t.client_cert_pem = _pem(f.cert);
#if defined(AZ_MQTT_TEST_BACKEND_MBEDTLS) \
    && (MBEDTLS_VERSION_MAJOR >= 4 || defined(MBEDTLS_USE_PSA_CRYPTO))
  // Import the key into PSA and hand over only its id, as an HSM, secure
  // element or ESP32 DS driver would.
  assert_int_equal(psa_crypto_init(), PSA_SUCCESS);
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
#if MBEDTLS_VERSION_MAJOR >= 4
  assert_int_equal(
      mbedtls_pk_parse_key(&pk, (unsigned char const*)f.key, strlen(f.key) + 1, NULL, 0), 0);
#else
  assert_int_equal(
      mbedtls_pk_parse_key(
          &pk,
          (unsigned char const*)f.key,
          strlen(f.key) + 1,
          NULL,
          0,
          mbedtls_psa_get_random,
          MBEDTLS_PSA_RANDOM_STATE),
      0);
#endif
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  assert_int_equal(mbedtls_pk_get_psa_attributes(&pk, PSA_KEY_USAGE_SIGN_HASH, &attr), 0);
  mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
  assert_int_equal(mbedtls_pk_import_into_psa(&pk, &attr, &id), 0);
  mbedtls_pk_free(&pk);
  t.client_key_psa_id = (uint32_t)id;
  assert_int_equal(_connect(&f, &t), AZ_OK);
  assert_true(_exchange(&f));
  assert_true(test_server_saw_client_cert(f.server));
  (void)psa_destroy_key(id);
#else
  // OpenSSL has no PSA; mbedTLS 3.x without MBEDTLS_USE_PSA_CRYPTO (the default) neither.
  t.client_key_psa_id = 1;
  assert_int_equal(_connect(&f, &t), AZ_MQTT_ERROR_NOT_SUPPORTED);
  assert_int_equal(test_server_accepted(f.server), 0);
#endif
  _teardown(&f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(ca_from_memory_is_trusted),
    cmocka_unit_test(untrusted_server_is_rejected_with_ca_from_memory),
    cmocka_unit_test(client_identity_from_memory_is_presented),
    cmocka_unit_test(conflicting_sources_are_refused_before_connecting),
    cmocka_unit_test(the_configure_hook_can_install_trust),
    cmocka_unit_test(a_failing_configure_hook_aborts_before_connecting),
    cmocka_unit_test(a_configure_hook_turning_verification_off_is_caught),
    cmocka_unit_test(a_key_uri_is_used_or_refused),
    cmocka_unit_test(a_psa_key_is_used_or_refused),
  };
  return cmocka_run_group_tests_name("transport_credentials", tests, NULL, NULL);
}

#else

int main(void) { return 0; } // No TLS backend: covered by test_transport_tls.

#endif
