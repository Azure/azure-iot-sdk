// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_ssl_posix.c
 * @brief POSIX TLS integration test for AZ_MQTT_T(client).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <azure/core/az_span.h>

#include "test_common.h"

#include <string.h>
#include <time.h>

#ifndef E2E_CA_CERT_PATH
#error "E2E_CA_CERT_PATH must be defined by CMake for TLS tests"
#endif

static az_mqtt_e2e_fixture s_fixture;

static bool s_connack_received;
static int s_connack_reason;

static void on_connack(AZ_MQTT_T(client)* client, AZ_MQTT_T(connack_data) const* connack)
{
  (void)client;
  s_connack_received = true;
  s_connack_reason = AZ_MQTT_TEST_CONNACK_CODE(connack);
}

static void reset_state(void)
{
  s_connack_received = false;
  s_connack_reason = -1;
}

static az_result init_tls_client(AZ_MQTT_T(client)* client, az_span client_id)
{
  az_mqtt_e2e_fixture_reset(&s_fixture);

  static az_mqtt_tls_options tls_opts;
  tls_opts = az_mqtt_tls_options_default();
  tls_opts.ca_cert_path = AZ_SPAN_FROM_STR(E2E_CA_CERT_PATH);

  az_mqtt_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = client_id;
  params.hostname = AZ_SPAN_FROM_STR("localhost");
  params.port = 8883;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = &tls_opts;
  params.on_connack = on_connack;

  return az_mqtt_e2e_init_client(&s_fixture, client, &params);
}

static void test_tls_connect_and_disconnect_posix(void** state)
{
  (void)state;
  reset_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_tls_client(&client, AZ_SPAN_FROM_STR("test-tls-posix-01"));
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT_TEST_CONNACK_ACCEPTED);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

/** @brief connect_start + process_loop(0): the TLS handshake spans several calls. */
static void test_tls_started_connect_posix(void** state)
{
  (void)state;
  reset_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_tls_client(&client, AZ_SPAN_FROM_STR("test-tls-posix-02"));
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect_start)(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  int calls = 0;
  while (az_result_succeeded(rc)
         && AZ_MQTT_T(client_get_state)(&client) == AZ_MQTT_CLIENT_STATE_CONNECTING && calls < 1000)
  {
    rc = AZ_MQTT_T(client_process_loop)(&client, 0);
    calls++;
    struct timespec const pause = { 0, 5 * 1000 * 1000 };
    nanosleep(&pause, NULL);
  }
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  assert_true(calls > 1);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT_TEST_CONNACK_ACCEPTED);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_tls_connect_and_disconnect_posix),
    cmocka_unit_test(test_tls_started_connect_posix),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
