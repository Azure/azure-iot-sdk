// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_transport_timeouts.c
 * @brief Send and blocking-connect bounds. Linked against a test build of the
 * library with 1 s AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS / _CONNECT_TIMEOUT_MS.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include "test_server.h"

#if AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS != 1000 || AZ_MQTT_TRANSPORT_CONNECT_TIMEOUT_MS != 1000
#error "Build against the short-timeout test library."
#endif

static int64_t _now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static az_span _str(char const* s) { return az_span_create_from_str((char*)(uintptr_t)s); }

static az_mqtt_transport* _transport_new(void)
{
  az_mqtt_transport* t = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  assert_non_null(t);
  assert_int_equal(az_mqtt_transport_init(t), AZ_OK);
  return t;
}

static void _transport_free(az_mqtt_transport* t)
{
  az_mqtt_transport_close(t);
  free(t);
}

static void a_send_the_peer_never_reads_times_out(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.tls = false;
  o.behavior = TEST_SERVER_STOP_READING;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt_transport* t = _transport_new();
  assert_int_equal(az_mqtt_transport_connect(t, _str("127.0.0.1"), test_server_port(s), NULL), AZ_OK);

  // CONNECT / CONNACK first: after that the peer stops reading.
  static uint8_t connect_v5[] = { 0x10, 0x0E, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x05,
                                  0x02, 0x00, 0x3C, 0x00, 0x00, 0x01, 'c' };
  assert_int_equal(az_mqtt_transport_send(t, AZ_SPAN_FROM_BUFFER(connect_v5)), AZ_OK);
  uint8_t ack[8];
  az_span got = AZ_SPAN_EMPTY;
  assert_int_equal(az_mqtt_transport_receive(t, AZ_SPAN_FROM_BUFFER(ack), 3000, &got), AZ_OK);
  assert_true(az_span_size(got) > 0);

  static uint8_t chunk[64 * 1024];
  az_result rc = AZ_OK;
  int64_t last_ok = _now_ms();
  for (int i = 0; i < 4096 && az_result_succeeded(rc); i++)
  {
    rc = az_mqtt_transport_send(t, AZ_SPAN_FROM_BUFFER(chunk));
    if (az_result_succeeded(rc))
    {
      last_ok = _now_ms();
    }
  }
  int64_t stuck = _now_ms() - last_ok;
  assert_int_equal(rc, AZ_MQTT_ERROR_TIMEOUT);
  assert_true(stuck >= 900 && stuck < 3000);
  // A partial packet may be on the wire: the transport refuses further use.
  assert_true(az_result_failed(az_mqtt_transport_send(t, AZ_SPAN_FROM_BUFFER(chunk))));

  _transport_free(t);
  test_server_stop(s);
}

static void a_blocking_connect_to_a_silent_tls_peer_is_bounded(void** state)
{
  (void)state;
#if defined(AZ_MQTT_TEST_BACKEND_NONE)
  skip();
#else
  test_server_options o = test_server_options_default();
  o.behavior = TEST_SERVER_SILENT;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt_transport* t = _transport_new();
  az_mqtt_tls_options tls = az_mqtt_tls_options_default();
  tls.ca_cert_path = _str(test_server_ca_path(s));

  int64_t t0 = _now_ms();
  assert_int_equal(
      az_mqtt_transport_connect(t, _str("127.0.0.1"), test_server_port(s), &tls),
      AZ_MQTT_ERROR_TIMEOUT);
  int64_t elapsed = _now_ms() - t0;
  assert_true(elapsed >= 900 && elapsed < 3000);

  _transport_free(t);
  test_server_stop(s);
#endif
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_send_the_peer_never_reads_times_out),
    cmocka_unit_test(a_blocking_connect_to_a_silent_tls_peer_is_bounded),
  };
  return cmocka_run_group_tests_name("transport_timeouts", tests, NULL, NULL);
}
