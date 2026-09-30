// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_transport_tls.c
 * @brief Server certificate validation and client identity, against an in-process peer.
 *
 * Built for whichever TLS backend the library uses (AZ_MQTT_TEST_BACKEND_*).
 * Every rejection is asserted against a server that is otherwise healthy, so a
 * pass means the check itself rejected the connection.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include "test_server.h"

typedef struct
{
  test_server* server;
  az_mqtt_transport* transport;
} fixture;

static int _setup(fixture* f, test_server_options const* o)
{
  memset(f, 0, sizeof(*f));
  f->server = test_server_start(o);
  if (f->server == NULL)
  {
    return -1;
  }
  f->transport = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  return (f->transport != NULL && az_result_succeeded(az_mqtt_transport_init(f->transport))) ? 0
                                                                                              : -1;
}

static void _teardown(fixture* f)
{
  if (f->transport != NULL)
  {
    az_mqtt_transport_close(f->transport);
    free(f->transport);
  }
  test_server_stop(f->server);
}

static az_mqtt_tls_options _trusting(fixture const* f)
{
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.ca_cert_path = az_span_create_from_str((char*)(uintptr_t)test_server_ca_path(f->server));
  return t;
}

static az_result _connect(fixture* f, char const* host, az_mqtt_tls_options const* tls)
{
  return az_mqtt_transport_connect(
      f->transport,
      az_span_create_from_str((char*)(uintptr_t)host),
      test_server_port(f->server),
      tls);
}

#if defined(AZ_MQTT_TEST_BACKEND_NONE)

static void tls_requested_without_backend_is_refused(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  fixture f;
  assert_int_equal(_setup(&f, &o), 0);
  az_mqtt_tls_options t = _trusting(&f);
  assert_int_equal(_connect(&f, "localhost", &t), AZ_MQTT_ERROR_NOT_SUPPORTED);
  assert_int_equal(test_server_accepted(f.server), 0);
  _teardown(&f);
}

int main(void)
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(tls_requested_without_backend_is_refused) };
  return cmocka_run_group_tests_name("transport_tls_none", tests, NULL, NULL);
}

#else

/**
 * @brief Whether an MQTT CONNECT gets a CONNACK over the connected transport.
 *
 * With TLS 1.3 the client finishes its handshake before the server has judged
 * the client certificate, so a refusal can surface on the first exchange.
 */
static bool _mqtt_exchange_works(fixture* f)
{
  static uint8_t connect_v5[] = { 0x10, 0x0E, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x05,
                                        0x02, 0x00, 0x3C, 0x00, 0x00, 0x01, 'c' };
  if (az_result_failed(az_mqtt_transport_send(
          f->transport, AZ_SPAN_FROM_BUFFER(connect_v5))))
  {
    return false;
  }
  uint8_t buf[8];
  az_span got = AZ_SPAN_EMPTY;
  az_result rc = az_mqtt_transport_receive(f->transport, AZ_SPAN_FROM_BUFFER(buf), 3000, &got);
  return az_result_succeeded(rc) && az_span_size(got) > 0 && buf[0] == 0x20;
}

static void _expect_rejected(test_server_options const* o, char const* host)
{
  fixture f;
  assert_int_equal(_setup(&f, o), 0);
  az_mqtt_tls_options t = _trusting(&f);
  assert_true(az_result_failed(_connect(&f, host, &t)));
  assert_int_equal(test_server_handshakes(f.server), 0);
  _teardown(&f);
}

static void trusted_server_by_dns_name_is_accepted(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  fixture f;
  assert_int_equal(_setup(&f, &o), 0);
  az_mqtt_tls_options t = _trusting(&f);
  assert_int_equal(_connect(&f, "localhost", &t), AZ_OK);
  assert_true(_mqtt_exchange_works(&f));
  _teardown(&f);
}

static void trusted_server_by_ip_address_is_accepted(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  fixture f;
  assert_int_equal(_setup(&f, &o), 0);
  az_mqtt_tls_options t = _trusting(&f);
  assert_int_equal(_connect(&f, "127.0.0.1", &t), AZ_OK);
  assert_true(_mqtt_exchange_works(&f));
  _teardown(&f);
}

static void untrusted_issuer_is_rejected(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.untrusted_ca = true;
  _expect_rejected(&o, "localhost");
}

static void host_name_not_in_certificate_is_rejected(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.san = "DNS:other.example";
  _expect_rejected(&o, "localhost");
}

static void ip_address_not_in_certificate_is_rejected(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.san = "DNS:localhost";
  _expect_rejected(&o, "127.0.0.1");
}

static void expired_certificate_is_rejected(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.expired = true;
  _expect_rejected(&o, "localhost");
}

static void missing_trust_anchor_never_trusts_the_server(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  fixture f;
  assert_int_equal(_setup(&f, &o), 0);
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  az_result rc = _connect(&f, "localhost", &t);
#if defined(AZ_MQTT_TEST_BACKEND_MBEDTLS)
  // No system store to fall back to: refused before any connection.
  assert_int_equal(rc, AZ_MQTT_ERROR_NOT_SUPPORTED);
  assert_int_equal(test_server_accepted(f.server), 0);
#else
  // The system store does not contain the test CA.
  assert_true(az_result_failed(rc));
  assert_int_equal(test_server_handshakes(f.server), 0);
#endif
  _teardown(&f);
}

static void half_a_client_identity_is_refused_before_connecting(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  fixture f;
  assert_int_equal(_setup(&f, &o), 0);

  az_mqtt_tls_options t = _trusting(&f);
  t.client_cert_path
      = az_span_create_from_str((char*)(uintptr_t)test_server_client_cert_path(f.server));
  assert_int_equal(_connect(&f, "localhost", &t), AZ_MQTT_ERROR_INVALID_CONFIG);

  t = _trusting(&f);
  t.client_key_path
      = az_span_create_from_str((char*)(uintptr_t)test_server_client_key_path(f.server));
  assert_int_equal(_connect(&f, "localhost", &t), AZ_MQTT_ERROR_INVALID_CONFIG);

  assert_int_equal(test_server_accepted(f.server), 0);
  _teardown(&f);
}

static void client_certificate_is_presented(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.require_client_cert = true;
  fixture f;
  assert_int_equal(_setup(&f, &o), 0);
  az_mqtt_tls_options t = _trusting(&f);
  t.client_cert_path
      = az_span_create_from_str((char*)(uintptr_t)test_server_client_cert_path(f.server));
  t.client_key_path
      = az_span_create_from_str((char*)(uintptr_t)test_server_client_key_path(f.server));
  assert_int_equal(_connect(&f, "localhost", &t), AZ_OK);
  assert_true(_mqtt_exchange_works(&f));
  assert_true(test_server_saw_client_cert(f.server));
  _teardown(&f);
}

static void server_requiring_a_client_certificate_refuses_one_without(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.require_client_cert = true;
  fixture f;
  assert_int_equal(_setup(&f, &o), 0);
  az_mqtt_tls_options t = _trusting(&f);
  if (az_result_succeeded(_connect(&f, "localhost", &t)))
  {
    assert_false(_mqtt_exchange_works(&f));
  }
  assert_int_equal(test_server_handshakes(f.server), 0);
  _teardown(&f);
}

static void reconnect_after_a_rejected_handshake_succeeds(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.san = "DNS:localhost";
  fixture f;
  assert_int_equal(_setup(&f, &o), 0);
  az_mqtt_tls_options t = _trusting(&f);
  // Same transport: the rejected handshake must leave nothing behind.
  assert_true(az_result_failed(_connect(&f, "127.0.0.1", &t)));
  assert_int_equal(_connect(&f, "localhost", &t), AZ_OK);
  assert_true(_mqtt_exchange_works(&f));
  az_mqtt_transport_close(f.transport);
  assert_int_equal(_connect(&f, "localhost", &t), AZ_OK);
  assert_true(_mqtt_exchange_works(&f));
  _teardown(&f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(trusted_server_by_dns_name_is_accepted),
    cmocka_unit_test(trusted_server_by_ip_address_is_accepted),
    cmocka_unit_test(untrusted_issuer_is_rejected),
    cmocka_unit_test(host_name_not_in_certificate_is_rejected),
    cmocka_unit_test(ip_address_not_in_certificate_is_rejected),
    cmocka_unit_test(expired_certificate_is_rejected),
    cmocka_unit_test(missing_trust_anchor_never_trusts_the_server),
    cmocka_unit_test(half_a_client_identity_is_refused_before_connecting),
    cmocka_unit_test(client_certificate_is_presented),
    cmocka_unit_test(server_requiring_a_client_certificate_refuses_one_without),
    cmocka_unit_test(reconnect_after_a_rejected_handshake_succeeds),
  };
  return cmocka_run_group_tests_name("transport_tls", tests, NULL, NULL);
}

#endif
