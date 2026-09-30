// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_connect.c
 * @brief Integration tests for connection and state transition behaviors.
 *
 * Requires a running broker on localhost:1883 (plain TCP).
 * Launch with: test/start_broker.ps1 (Windows) or test/start_broker.sh (Linux)
 *
 * Test matrix (API behavior -> test):
 *  - connect() success state transition -> test_connect_and_disconnect
 *  - connect() invalid state when already connected -> test_connect_when_already_connected
 *  - connect() failure on unreachable port -> test_connect_failure
 *  - reconnect with clean_start=false session resume -> test_reconnect_with_session
 *  - process_loop() while disconnected -> test_process_loop_while_disconnected
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// cmocka must be included after the standard headers above

#include <cmocka.h>

#include <azure/core/az_span.h>

#include "test_common.h"

#include <string.h>

static az_mqtt_e2e_fixture s_fixture;

// ──────────────────────── Callback tracking ──────────────────

static bool s_connack_received;
static int s_connack_reason;
static bool s_connack_session_present;

static void reset_callback_state(void)
{
  s_connack_received = false;
  s_connack_reason = -1;
  s_connack_session_present = false;
}

static void on_connack(AZ_MQTT_T(client)* client, AZ_MQTT_T(connack_data) const* connack)
{
  (void)client;
  s_connack_received = true;
  s_connack_reason = AZ_MQTT_TEST_CONNACK_CODE(connack);
  s_connack_session_present = connack->session_present;
}

// ──────────────────────── Helper: init a client ──────────────

static az_result init_client(AZ_MQTT_T(client)* client, az_span client_id)
{
  az_mqtt_e2e_fixture_reset(&s_fixture);

  az_mqtt_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = client_id;
  params.hostname = AZ_SPAN_FROM_STR("localhost");
  params.port = 1883;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = NULL;
  params.on_connack = on_connack;

  return az_mqtt_e2e_init_client(&s_fixture, client, &params);
}

// ──────────────────────── Tests ──────────────────────────────

/**
 * @brief Test: connect to broker with clean start, verify CONNACK, then disconnect.
 */
static void test_connect_and_disconnect(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-connect-01"));
  assert_int_equal(rc, AZ_OK);

  // Connect (5 second timeout)
  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  // Verify CONNACK
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT_TEST_CONNACK_ACCEPTED);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&client), AZ_MQTT_CLIENT_STATE_CONNECTED);

  // Clean start = true, so no previous session
  assert_false(s_connack_session_present);

  // Disconnect
  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);

  // Disconnecting an already disconnected client is idempotent.
  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

/**
 * @brief Test: calling connect while connected returns INVALID_STATE.
 */
static void test_connect_when_already_connected(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-connect-dup-01"));
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_MQTT_ERROR_INVALID_STATE);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

/**
 * @brief Test: connect, disconnect, reconnect with clean_start=false to verify session.
 */
static void test_reconnect_with_session(void** state)
{
  (void)state;
  reset_callback_state();
  az_span client_id = AZ_SPAN_FROM_STR("test-session-01");

  // First connection: clean start
  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, client_id);
  assert_int_equal(rc, AZ_OK);

#if AZ_MQTT_TEST_VERSION == 5
  // Override session expiry to keep the session
  client._internal.connect_options.session_expiry_interval = 300;
#else
  // MQTT 3.1.1 has no session expiry: a Clean Session 1 session ends at
  // disconnect. Clear any old session with one clean connect, then keep the
  // next one with Clean Session 0.
  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
  s_connack_received = false;
  rc = init_client(&client, client_id);
  assert_int_equal(rc, AZ_OK);
  client._internal.connect_options.clean_session = false;
#endif

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT_TEST_CONNACK_ACCEPTED);
  assert_false(s_connack_session_present);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);

  // Second connection: resume session
  s_connack_received = false;
  rc = init_client(&client, client_id);
  assert_int_equal(rc, AZ_OK);

  client._internal.connect_options.AZ_MQTT_TEST_CLEAN = false;
#if AZ_MQTT_TEST_VERSION == 5
  client._internal.connect_options.session_expiry_interval = 300;
#endif

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);
  assert_true(s_connack_received);
  assert_int_equal(s_connack_reason, AZ_MQTT_TEST_CONNACK_ACCEPTED);
  assert_true(s_connack_session_present);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

/**
 * @brief Test: connecting to a non-existent host should fail with a transport error.
 */
static void test_connect_failure(void** state)
{
  (void)state;
  reset_callback_state();

  az_mqtt_e2e_fixture_reset(&s_fixture);

  az_mqtt_transport* transport = (az_mqtt_transport*)s_fixture.transport_buf.bytes;
  az_result rc = az_mqtt_transport_init(transport);
  assert_int_equal(rc, AZ_OK);

  AZ_MQTT_T(connect_options) connect_opts = AZ_MQTT_T(connect_options_default)();
  connect_opts.client_id = AZ_SPAN_FROM_STR("test-fail-01");

  AZ_MQTT_T(client_options) opts;
  memset(&opts, 0, sizeof(opts));
  opts.transport = transport;
  opts.send_buffer = AZ_SPAN_FROM_BUFFER(s_fixture.send_buf);
  opts.receive_buffer = AZ_SPAN_FROM_BUFFER(s_fixture.recv_buf);
  opts.connect_options = connect_opts;
  opts.hostname = AZ_SPAN_FROM_STR("127.0.0.1");
  opts.port = 19999; // Nothing is listening here
  opts.tls_options = NULL;

  AZ_MQTT_T(client) client;
  rc = AZ_MQTT_T(client_init)(&client, &opts);
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect)(&client, 3000);
  assert_true(az_result_failed(rc));
  assert_int_equal(AZ_MQTT_T(client_get_state)(&client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
}

/**
 * @brief Test: process_loop on a disconnected client returns NOT_CONNECTED.
 */
static void test_process_loop_while_disconnected(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-loop-disc-01"));
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_process_loop)(&client, 10);
  assert_int_equal(rc, AZ_MQTT_ERROR_NOT_CONNECTED);
}

// ──────────────────────── Main ───────────────────────────────

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_connect_and_disconnect),
    cmocka_unit_test(test_connect_when_already_connected),
    cmocka_unit_test(test_reconnect_with_session),
    cmocka_unit_test(test_connect_failure),
    cmocka_unit_test(test_process_loop_while_disconnected),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
