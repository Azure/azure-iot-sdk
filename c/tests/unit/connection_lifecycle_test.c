// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* ConnectionClient lifecycle unit tests: open/close/destroy/do_work argument
 * validation, state-machine edges, adapter ownership, and the observable
 * consequences of a CONNACK rejection.
 *
 * Everything is driven through the public API against the in-memory mock
 * adapter. Companion suites: connection_reconnect_test.c (retry/backoff) and
 * connection_dps_test.c (provisioning). */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "internal/connection_client_internal.h"

#include "support/connection_test_harness.h"

/* ------------------------------------------------------------------------- */
/* fixture                                                                   */
/* ------------------------------------------------------------------------- */

static int setup(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = az_iot_test_classic_options();
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  if (fx)
  {
    /* A registered factory is adopted by the client and freed from destroy();
     * an unregistered one is still ours. */
    bool adopted = (fx->client->factory_count > 0);
    az_iot_connection_client_destroy(&fx->client_storage);
    if (!adopted)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
    free(fx);
  }
  return 0;
}

/* Register the factory and open(); returns the mock adapter instance. */
static az_iot_mock_mqtt_client* open_to_connecting(az_iot_test_conn* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  return m;
}

/* Drive all the way to CONNECTED. */
static az_iot_mock_mqtt_client* open_to_connected(az_iot_test_conn* fx)
{
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
  return m;
}

/* ------------------------------------------------------------------------- */
/* open() argument + state validation                                        */
/* ------------------------------------------------------------------------- */

static void open_rejects_null_client(void** state)
{
  (void)state;
  assert_int_equal(az_iot_connection_client_open(NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void open_without_host_or_dps_is_rejected(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  /* No host, no dps.id_scope: there is nothing to connect to. */
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_INVALID_ARG);
  az_iot_connection_client_destroy(&c);
}

static void open_without_client_id_is_rejected(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  /* A direct hub connect needs an identity as well as an endpoint. */
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_INVALID_ARG);
  az_iot_connection_client_destroy(&c);
}

static void open_while_connecting_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connecting(fx);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_ALREADY_INITIALIZED);
}

static void open_while_connected_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connected(fx);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_ALREADY_INITIALIZED);
}

/* open() is still IDLE-only: a fault has to be acknowledged with close()
 * first, which is what returns the client to IDLE. See
 * close_from_faulted_returns_to_idle() and
 * open_after_close_from_faulted_starts_a_new_session(). */
static void open_from_faulted_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));

  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_ALREADY_INITIALIZED);
}

/* The adapter registry is keyed by MQTT version; a Classic (v3.1.1) session
 * cannot borrow a v5 factory. */
static void open_rejects_factory_of_the_wrong_version(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);

  az_iot_mqtt_factory* v5 = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(v5);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, v5), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_connection_client_destroy(&c);
}

static void open_connects_to_the_configured_endpoint(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "broker.example");
  assert_string_equal(c->connect.client_id, "ut-device");
  assert_int_equal(c->connect.port, 8883);
}

/* ------------------------------------------------------------------------- */
/* keep-alive and connect timeout                                            */
/* ------------------------------------------------------------------------- */

/* Open a throwaway client with the given options and hand back the CONNECT the
 * adapter recorded. The record is copied out by value before destroy(), which
 * frees the mock the call history lives in. */
static void connect_call_for(
    const az_iot_connection_client_options* opts,
    az_iot_mock_call* out_connect)
{
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, opts), AZ_IOT_OK);
  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, f), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(f);
  assert_non_null(m);
  const az_iot_mock_call* rec = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(rec);
  *out_connect = *rec;

  az_iot_connection_client_destroy(&c);
}

static void keep_alive_defaults_when_unset(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_int_equal(c.connect.keep_alive_seconds, AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS);
}

static void keep_alive_is_configurable(void** state)
{
  (void)state;
  /* IoT Hub derives its own timeout from this value (1.5x, capped at 1767 s),
   * so a device on a metered link has a real reason to raise it and a device
   * on a lossy one has a real reason to lower it. It used to be hardcoded. */
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.keep_alive_seconds = 120;
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_int_equal(c.connect.keep_alive_seconds, 120);
}

static void the_largest_useful_keep_alive_reaches_the_adapter(void** state)
{
  (void)state;
  /* 1177 s is the largest value IoT Hub does not clamp (1177 * 1.5 = 1765.5,
   * under the 1767 s server cap). The SDK must pass it through rather than
   * truncating it into a uint8 or its own smaller bound. */
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.keep_alive_seconds = 1177;
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_int_equal(c.connect.keep_alive_seconds, 1177);
}

static void connect_timeout_defaults_when_unset(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_int_equal(c.connect.connect_timeout_seconds, AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS);
}

static void connect_timeout_is_configurable(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.connect_timeout_seconds = 5;
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_int_equal(c.connect.connect_timeout_seconds, 5);
}

/* ------------------------------------------------------------------------- */
/* transport and proxy                                                       */
/* ------------------------------------------------------------------------- */

/* Nothing selected: the behaviour that predates the transport and proxy
 * options, so an application that never heard of them is unaffected. */
static void transport_defaults_to_tcp_on_8883_with_no_proxy(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.port = 0; /* derive */
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_int_equal(c.connect.transport, AZ_IOT_MQTT_TRANSPORT_TCP);
  assert_int_equal(c.connect.port, 8883);
  assert_string_equal(c.connect.proxy_host, "");
}

/* Selecting WebSockets has to move the port too. 8883 is not served over
 * WebSockets, and a device selects this transport precisely because the
 * network it is on will not pass 8883 at all. */
static void websockets_derive_port_443(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.port = 0;
  opts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_int_equal(c.connect.transport, AZ_IOT_MQTT_TRANSPORT_WEBSOCKET);
  assert_int_equal(c.connect.port, 443);
}

static void an_explicit_port_survives_the_transport_default(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  opts.port = 8443;
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_int_equal(c.connect.port, 8443);
}

static void the_proxy_reaches_the_adapter_whole(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.username = "device";
  opts.proxy.password = "s3cret";
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_string_equal(c.connect.proxy_host, "proxy.corp.example");
  assert_int_equal(c.connect.proxy_port, 3128);
  assert_string_equal(c.connect.proxy_username, "device");
  assert_string_equal(c.connect.proxy_password, "s3cret");
  /* The proxy does not change which broker the session targets. */
  assert_string_equal(c.connect.host, "broker.example");
}

static void the_websocket_path_reaches_the_adapter(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  opts.websocket_path = "/mqtt";
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_string_equal(c.connect.websocket_path, "/mqtt");
}

/* Stock options retry. A zeroed struct does not -- that is the caller's
 * choice -- but the function whose job is to supply sensible defaults must not
 * hand back a client for which every transient failure is terminal. */
static void the_default_options_enable_reconnection(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  az_iot_reconnection_policy expected = az_iot_reconnection_policy_get_default();

  assert_true(opts.reconnection_policy.initial_delay_ms > 0);
  assert_int_equal(opts.reconnection_policy.initial_delay_ms, expected.initial_delay_ms);
  assert_int_equal(opts.reconnection_policy.max_delay_ms, expected.max_delay_ms);
  assert_int_equal(opts.reconnection_policy.max_attempts, expected.max_attempts);
  assert_int_equal(opts.reconnection_policy.jitter_pct, expected.jitter_pct);
}

/* The field values are only half of it: they matter because they reach the
 * lifecycle. A client built from az_iot_connection_client_options_default()
 * must actually survive a refused CONNACK, or a future regression in wiring
 * the default policy through could still pass the comparison above. */
static void the_default_options_retry_a_refused_connack(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.host = "broker.example";
  opts.client_id = "ut-device";
  /* initial_delay_ms is deliberately NOT overridden: it is the field that
   * decides whether reconnection happens at all, so the test has to depend on
   * the default supplying it. Only the cap is shortened, which bounds the
   * computed delay (base = min(max_delay_ms, initial_delay_ms << n)) so the
   * retry deadline is reachable without a one-second wait. */
  opts.reconnection_policy.max_delay_ms = 20u;
  opts.reconnection_policy.jitter_pct = 0;

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_test_state_log log;
  memset(&log, 0, sizeof(log));
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&c, az_iot_test_on_state, &log), AZ_IOT_OK);

  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, f), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);

  az_iot_mock_mqtt_client* first = az_iot_mock_mqtt_factory_last_client(f);
  assert_non_null(first);
  assert_true(az_iot_mock_mqtt_client_inject_connected(first, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(&c, 0);
  (void)az_iot_connection_client_do_work(&c, 0);

  /* Retrying, not terminal. */
  assert_int_equal(az_iot_test_last_state(&log), AZ_IOT_CONN_STATE_RECONNECTING);
  assert_false(az_iot_test_saw_state(&log, AZ_IOT_CONN_STATE_FAULTED));

  /* And the retry is really issued once the backoff elapses. */
  az_iot_test_wait_ms(25u);
  (void)az_iot_connection_client_do_work(&c, 0);
  az_iot_mock_mqtt_client* second = az_iot_mock_mqtt_factory_last_client(f);
  assert_non_null(second);
  assert_true(az_iot_mock_mqtt_client_inject_connected(second, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&c, 0);
  assert_true(az_iot_connection_client__is_connected(&c));

  az_iot_connection_client_destroy(&c);
}

/* Opting out stays possible, and a zeroed struct keeps meaning "no retry". */
static void reconnection_can_still_be_disabled(void** state)
{
  (void)state;
  az_iot_connection_client_options zeroed = { 0 };
  assert_int_equal(zeroed.reconnection_policy.initial_delay_ms, 0);

  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.reconnection_policy.initial_delay_ms = 0;
  opts.host = "broker.example";
  opts.client_id = "ut-device";

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_test_state_log log;
  memset(&log, 0, sizeof(log));
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&c, az_iot_test_on_state, &log), AZ_IOT_OK);

  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, f), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(f);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(&c, 0);
  (void)az_iot_connection_client_do_work(&c, 0);

  assert_int_equal(az_iot_test_last_state(&log), AZ_IOT_CONN_STATE_FAULTED);
  az_iot_connection_client_destroy(&c);
}

/* ...but a clean peer DISCONNECT with retrying disabled is NOT a fault. It is
 * the end of a session, so the client settles in IDLE and is ready to be
 * opened again. Pinned because the header documents the two outcomes
 * separately, and describing them as one was wrong. */
static void a_peer_disconnect_without_retrying_settles_in_idle(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.reconnection_policy = az_iot_reconnection_policy_get_retry_disabled();
  opts.host = "broker.example";
  opts.client_id = "ut-device";

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_test_state_log log;
  memset(&log, 0, sizeof(log));
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&c, az_iot_test_on_state, &log), AZ_IOT_OK);

  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, f), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(f);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&c, 0);
  assert_true(az_iot_connection_client__is_connected(&c));

  /* The peer goes away. No retry is configured, but this is not a failure. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(&c, 0);
  (void)az_iot_connection_client_do_work(&c, 0);

  assert_int_equal(az_iot_test_last_state(&log), AZ_IOT_CONN_STATE_IDLE);
  assert_false(az_iot_test_saw_state(&log, AZ_IOT_CONN_STATE_FAULTED));

  /* IDLE means reopenable, which is the point of the distinction. */
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);
  az_iot_connection_client_destroy(&c);
}

/* The default options must not pin a port, or selecting WebSockets on top of
 * them would connect to 443's scheme on 8883's port. */
static void the_default_options_leave_the_port_to_the_transport(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  assert_int_equal(opts.port, 0);
  assert_int_equal(opts.transport, AZ_IOT_MQTT_TRANSPORT_TCP);
  assert_null(opts.proxy.host);
}

/* ------------------------------------------------------------------------- */
/* Classic CONNECT packet shape                                              */
/*                                                                           */
/* These pin what IoT Hub actually requires of the CONNECT. They all passed  */
/* the day they were written -- the point is that nothing would have noticed */
/* if they stopped passing, and the service rejects or misroutes a device    */
/* that gets any of them wrong.                                              */
/* ------------------------------------------------------------------------- */

static void the_username_carries_the_host_and_device_id(void** state)
{
  (void)state;
  /* IoT Hub expects "{iothub-hostname}/{device-id}/?api-version=...". The
   * hostname is how the service identifies the hub behind a shared gateway,
   * and the "/?" is what separates the identity from the query string -- a
   * username missing it is malformed, so the check has to include it. */
  static const char k_expected_prefix[] = "broker.example/ut-device/?";
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_int_equal(strncmp(c.username, k_expected_prefix, sizeof(k_expected_prefix) - 1), 0);
}

static void the_username_carries_an_api_version(void** state)
{
  (void)state;
  /* The service documents omitting api-version as a source of "unexpected
   * behaviour", and the SDK gets it from azure-sdk-for-c rather than building
   * it here -- so a dependency bump could drop it without anything failing. */
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_non_null(strstr(c.username, "api-version="));
}

static void the_model_id_is_announced_in_the_username(void** state)
{
  (void)state;
  /* Plug and Play model announcement. Device Update discovers a device by this
   * value, so losing it silently disables ADU on every device. */
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.model_id = "dtmi:azure:iot:deviceUpdateContractModel;2";
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_non_null(strstr(c.username, "model-id="));
  assert_non_null(strstr(c.username, "deviceUpdateContractModel"));
}

static void no_model_id_means_none_in_the_username(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_null(strstr(c.username, "model-id="));
}

static void an_empty_model_id_is_treated_as_none(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.model_id = "";
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_null(strstr(c.username, "model-id="));
}

static void the_connect_does_not_request_a_clean_session(void** state)
{
  (void)state;
  /* CleanSession 0 is what makes the C2D subscription survive a reconnect and
   * lets the hub deliver messages queued while the device was away. Asking for
   * a clean session would drop them, and the loss would be invisible. */
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_mock_call c;
  connect_call_for(&opts, &c);
  assert_false(c.connect.clean_start);
}

/* ------------------------------------------------------------------------- */
/* close()                                                                   */
/* ------------------------------------------------------------------------- */

static void close_rejects_null_client(void** state)
{
  (void)state;
  assert_int_equal(az_iot_connection_client_close(NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void close_while_connecting_disconnects_the_adapter(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_DISCONNECTING);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 1);
}

/* A CONNACK that lands after the application asked to close belongs to an
 * attempt it has already abandoned. Announcing CONNECTED for it would report a
 * session nobody asked for, and any code that publishes on CONNECTED would
 * write into a socket already being torn down. */
static void close_while_connecting_suppresses_a_late_connack(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_false(az_iot_connection_client__is_connected(fx->client));
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_CONNECTED));

  /* The close still completes normally once the disconnect lands. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);
}

/* The suppression must key on "the user asked to close", not on a stale state:
 * a normal CONNACK on a live attempt is still announced. */
static void a_connack_without_a_pending_close_still_connects(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_connection_client__is_connected(fx->client));
}

static void close_twice_is_idempotent(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);

  size_t transitions = fx->log.count;
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(fx->log.count, transitions);
}

/* After a fault the adapter is already gone, so there is nothing to
 * disconnect -- which is exactly why close() has to reach IDLE by itself here
 * rather than waiting for a transport event. */
static void close_from_faulted_returns_to_idle(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));

  /* FAULTED is settled, not a trap: close() acknowledges it and the client is
   * IDLE by the time the call returns -- there is no adapter left to wait for. */
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);
}

/* The recovery this makes possible: retry without destroying the client (and
 * therefore without rebuilding every attached feature client). */
static void open_after_close_from_faulted_starts_a_new_session(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* second = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(second);
  assert_true(az_iot_mock_mqtt_client_inject_connected(second, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* Closing twice from a fault is as idempotent as closing twice from a session. */
static void close_from_faulted_twice_is_idempotent(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_test_count_state(&fx->log, AZ_IOT_CONN_STATE_IDLE), 1);
}

/* ------------------------------------------------------------------------- */
/* destroy()                                                                 */
/* ------------------------------------------------------------------------- */

/* Register a factory the client will NOT free.
 *
 * destroy() calls every registered factory's destroy hook, and the mock's hook
 * frees the factory itself -- so a test that inspects the factory AFTER
 * destroy() would be reading freed memory. glibc happens to leave the bytes
 * looking like the values the assertions want, which is why this passes on a
 * plain Linux build; valgrind reports an invalid read, and the MSVC debug CRT
 * fills freed blocks with 0xDD so the assertion fails outright. The client
 * copies the struct, so detaching the hook on a local copy leaves the real
 * factory alive and owned by the test. */
static void register_without_adopting(az_iot_connection_client* c, az_iot_mqtt_factory* factory)
{
  az_iot_mqtt_factory borrowed = *factory;
  borrowed.destroy = NULL;
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(c, &borrowed), AZ_IOT_OK);
}

static void destroy_while_connected_destroys_the_adapter(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);

  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(factory);
  register_without_adopting(&c, factory);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&c, 0);

  /* The mock frees itself inside destroy(), so read the state we care about
   * from the factory: a destroyed client detaches itself from last_client. */
  az_iot_connection_client_destroy(&c);
  assert_null(az_iot_mock_mqtt_factory_last_client(factory));

  az_iot_mock_mqtt_factory_destroy(factory);
}

static void destroy_while_connecting_destroys_the_adapter(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);

  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(factory);
  register_without_adopting(&c, factory);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);
  assert_non_null(az_iot_mock_mqtt_factory_last_client(factory));

  az_iot_connection_client_destroy(&c);
  assert_null(az_iot_mock_mqtt_factory_last_client(factory));

  az_iot_mock_mqtt_factory_destroy(factory);
}

static void destroy_is_silent_on_the_state_callback(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connected(fx);

  size_t transitions = fx->log.count;
  az_iot_connection_client_destroy(fx->client);
  assert_int_equal(fx->log.count, transitions);

  /* Neutralize the fixture teardown: the client is already destroyed and the
   * factory was adopted (and freed) by it. */
  memset(&fx->client_storage, 0, sizeof(fx->client_storage));
  fx->factory = NULL;
}

static void destroy_tolerates_null(void** state)
{
  (void)state;
  az_iot_connection_client_destroy(NULL);
}

/* ------------------------------------------------------------------------- */
/* do_work()                                                                 */
/* ------------------------------------------------------------------------- */

static void do_work_rejects_null_client(void** state)
{
  (void)state;
  assert_int_equal(az_iot_connection_client_do_work(NULL, 0), AZ_IOT_ERR_INVALID_ARG);
}

static void do_work_before_open_touches_no_adapter(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
  assert_int_equal(fx->log.count, 0);
}

static void do_work_after_close_touches_no_adapter(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);

  /* The adapter is torn down on the way to IDLE; further pumping is inert. */
  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);
}

static void do_work_forwards_the_timeout_to_the_adapter(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  (void)az_iot_connection_client_do_work(fx->client, 250);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP);
  assert_non_null(c);
  assert_int_equal(c->timeout_ms, 250);
}

static void do_work_surfaces_the_adapter_pump_error(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  az_iot_mock_mqtt_client_set_next_result(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP, AZ_IOT_ERR_MQTT);
  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_ERR_MQTT);
}

/* ------------------------------------------------------------------------- */
/* reopen                                                                    */
/* ------------------------------------------------------------------------- */

static void reopen_after_close_starts_a_second_session(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* first = open_to_connected(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(first));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);

  /* Second open() must build a fresh adapter instance -- the first one was
   * destroyed on the way to IDLE. */
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* second = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(second);
  assert_true(az_iot_mock_mqtt_client_inject_connected(second, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_count_state(&fx->log, AZ_IOT_CONN_STATE_CONNECTED), 2);
}

/* ------------------------------------------------------------------------- */
/* state observer registry                                                   */
/* ------------------------------------------------------------------------- */

static void add_state_observer_rejects_null_client(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(NULL, az_iot_test_on_state, NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

static void state_callback_carries_the_failure_reason(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_IDENTITY_REJECTED);
}

/* The registry is what replaced the single callback slot, so the behaviour
 * worth pinning is that a second observer does not displace the first: both
 * are delivered. Under the old single-slot setter this test asserted the
 * opposite -- that registering again silently stopped the first from being
 * called -- which is exactly the limitation the registry removes. */
static void a_second_observer_does_not_displace_the_first(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_test_state_log second = { 0 };
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &second),
      AZ_IOT_OK);

  size_t before = fx->log.count;
  (void)open_to_connecting(fx);

  assert_true(fx->log.count > before);
  assert_true(second.count > 0);
  assert_int_equal(fx->log.count - before, second.count);
}

/* Registering the same (cb, user_ctx) pair twice must not consume a second
 * slot, and must not deliver the event twice: a caller that cannot easily tell
 * whether it has already subscribed should be able to just call again. */
static void adding_the_same_observer_twice_is_idempotent(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_test_state_log second = { 0 };
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &second),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &second),
      AZ_IOT_OK);

  (void)open_to_connecting(fx);

  /* One delivery per transition, not two. */
  assert_int_equal(second.count, fx->log.count);
}

/* The same callback with two different contexts is two distinct
 * subscriptions -- the pair is the identity, not the function pointer. */
static void one_callback_with_two_contexts_is_two_observers(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_test_state_log a = { 0 };
  az_iot_test_state_log b = { 0 };
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &a),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &b),
      AZ_IOT_OK);

  (void)open_to_connecting(fx);

  assert_true(a.count > 0);
  assert_int_equal(a.count, b.count);
}

static void a_removed_observer_stops_being_called(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_test_state_log second = { 0 };
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &second),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_remove_state_observer(fx->client, az_iot_test_on_state, &second),
      AZ_IOT_OK);

  size_t before = fx->log.count;
  (void)open_to_connecting(fx);

  /* The one still registered keeps working; the withdrawn one is silent. */
  assert_true(fx->log.count > before);
  assert_int_equal(second.count, 0);
}

/* Removing matches on the pair, so withdrawing one context must leave the
 * other subscription intact. */
static void removing_one_context_leaves_the_other_registered(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_test_state_log a = { 0 };
  az_iot_test_state_log b = { 0 };
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &a),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &b),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_remove_state_observer(fx->client, az_iot_test_on_state, &a),
      AZ_IOT_OK);

  (void)open_to_connecting(fx);

  assert_int_equal(a.count, 0);
  assert_true(b.count > 0);
}

static void removing_an_unregistered_observer_reports_not_found(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_test_state_log never_added = { 0 };
  assert_int_equal(
      az_iot_connection_client_remove_state_observer(
          fx->client, az_iot_test_on_state, &never_added),
      AZ_IOT_ERR_NOT_FOUND);
}

/* The application pool is bounded, and a full pool must SAY so rather than
 * silently dropping a subscription the caller believes it holds. The fixture
 * already occupies one slot. */
static void a_full_application_pool_is_reported(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_test_state_log logs[AZ_IOT_MAX_APP_STATE_OBSERVERS];
  memset(logs, 0, sizeof(logs));

  for (size_t i = 0; i + 1 < AZ_IOT_MAX_APP_STATE_OBSERVERS; ++i)
  {
    assert_int_equal(
        az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &logs[i]),
        AZ_IOT_OK);
  }
  assert_int_equal(
      az_iot_connection_client_add_state_observer(
          fx->client, az_iot_test_on_state, &logs[AZ_IOT_MAX_APP_STATE_OBSERVERS - 1]),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

/* The ordering guarantee the registry exists to provide: every feature-client
 * observer runs before any application observer, so by the time the
 * application is told about a transition, the feature clients have already
 * reacted to it. An application that rebuilds its own state on CONNECTED would
 * otherwise race the clients it depends on. */
static char g_dispatch_order[8];
static size_t g_dispatch_order_len;

static void record_order(char tag)
{
  if (g_dispatch_order_len < sizeof(g_dispatch_order) - 1)
  {
    g_dispatch_order[g_dispatch_order_len++] = tag;
  }
}

static void feature_observer(const az_iot_connection_state_event* event, void* user_ctx)
{
  (void)event;
  (void)user_ctx;
  record_order('f');
}

static void app_observer(const az_iot_connection_state_event* event, void* user_ctx)
{
  (void)event;
  (void)user_ctx;
  record_order('a');
}

static void feature_observers_are_dispatched_before_application_ones(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  memset(g_dispatch_order, 0, sizeof(g_dispatch_order));
  g_dispatch_order_len = 0;

  /* Registered application-first on purpose: if the pools were walked in
   * registration order rather than feature-pool-first, this would record "af"
   * and the test would fail. */
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, app_observer, NULL), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_state_observer(fx->client, feature_observer, NULL), AZ_IOT_OK);

  (void)open_to_connecting(fx);

  assert_true(g_dispatch_order_len >= 2);
  assert_int_equal(g_dispatch_order[0], 'f');
  assert_int_equal(g_dispatch_order[1], 'a');
}

/* The feature-client pool is separate storage, so an application that fills
 * its own pool must still leave every feature client able to attach. */
static void a_full_application_pool_does_not_block_a_feature_client(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_test_state_log logs[AZ_IOT_MAX_APP_STATE_OBSERVERS];
  memset(logs, 0, sizeof(logs));

  for (size_t i = 0; i + 1 < AZ_IOT_MAX_APP_STATE_OBSERVERS; ++i)
  {
    assert_int_equal(
        az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &logs[i]),
        AZ_IOT_OK);
  }
  assert_int_equal(
      az_iot_connection_client_add_state_observer(
          fx->client, az_iot_test_on_state, &logs[AZ_IOT_MAX_APP_STATE_OBSERVERS - 1]),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  assert_int_equal(
      az_iot_connection_client__add_state_observer(fx->client, feature_observer, NULL), AZ_IOT_OK);
}

/* Mutating the registry from inside a dispatch would rewrite the array being
 * walked. Both entry points refuse rather than corrupt it. close() from an
 * observer stays legal and is covered elsewhere. */
static az_iot_connection_client* g_reentrant_client;
static az_iot_result g_reentrant_add_result;
static az_iot_result g_reentrant_remove_result;

static void reentrant_observer(const az_iot_connection_state_event* event, void* user_ctx)
{
  (void)event;
  (void)user_ctx;
  g_reentrant_add_result = az_iot_connection_client_add_state_observer(
      g_reentrant_client, reentrant_observer, (void*)(uintptr_t)1);
  g_reentrant_remove_result = az_iot_connection_client_remove_state_observer(
      g_reentrant_client, reentrant_observer, NULL);
}

static void the_registry_cannot_be_mutated_from_inside_an_observer(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  g_reentrant_client = fx->client;
  g_reentrant_add_result = AZ_IOT_OK;
  g_reentrant_remove_result = AZ_IOT_OK;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, reentrant_observer, NULL),
      AZ_IOT_OK);

  (void)open_to_connecting(fx);

  assert_int_equal(g_reentrant_add_result, AZ_IOT_ERR_BUSY);
  assert_int_equal(g_reentrant_remove_result, AZ_IOT_ERR_BUSY);
}

/* ------------------------------------------------------------------------- */
/* traffic is gated on CONNECTED                                             */
/* ------------------------------------------------------------------------- */

static void publish_before_connected_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connecting(fx);

  az_iot_mqtt_message msg = { 0 };
  msg.topic = "devices/ut-device/messages/events/";
  msg.qos = AZ_IOT_MQTT_QOS_1;
  assert_int_equal(
      az_iot_connection_client__publish(fx->client, &msg, NULL, NULL), AZ_IOT_ERR_NOT_CONNECTED);
}

static void subscribe_before_connected_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connecting(fx);

  uint16_t pid = 0;
  assert_int_equal(
      az_iot_connection_client__subscribe(
          fx->client, "devices/ut-device/#", AZ_IOT_MQTT_QOS_1, &pid),
      AZ_IOT_ERR_NOT_CONNECTED);
}

static void publish_after_disconnect_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mqtt_message msg = { 0 };
  msg.topic = "devices/ut-device/messages/events/";
  msg.qos = AZ_IOT_MQTT_QOS_1;
  assert_int_equal(
      az_iot_connection_client__publish(fx->client, &msg, NULL, NULL), AZ_IOT_ERR_NOT_CONNECTED);
}

/* ------------------------------------------------------------------------- */
/* CONNACK rejection: what the core actually does with IDENTITY_REJECTED      */
/* ------------------------------------------------------------------------- */

/* The adapter contract singles out AZ_IOT_ERR_IDENTITY_REJECTED as the code
 * that should make the SDK re-provision instead of retrying a refused identity.
 * The core does NOT act on it today: it is retried like any other CONNACK
 * failure. These two tests pin the SHIPPING behaviour so that implementing
 * re-provisioning is a deliberate, test-visible change rather than a silent
 * one. See docs/test-coverage.md ("known gaps"). */
static void identity_rejection_faults_when_reconnect_is_disabled(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_IDENTITY_REJECTED);
}

static void connack_rejection_tears_the_adapter_down(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    /* open() */
    cmocka_unit_test(open_rejects_null_client),
    cmocka_unit_test(open_without_host_or_dps_is_rejected),
    cmocka_unit_test(open_without_client_id_is_rejected),
    cmocka_unit_test_setup_teardown(open_while_connecting_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(open_while_connected_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(open_from_faulted_is_rejected, setup, teardown),
    cmocka_unit_test(open_rejects_factory_of_the_wrong_version),
    cmocka_unit_test_setup_teardown(open_connects_to_the_configured_endpoint, setup, teardown),
    cmocka_unit_test(keep_alive_defaults_when_unset),
    cmocka_unit_test(keep_alive_is_configurable),
    cmocka_unit_test(the_largest_useful_keep_alive_reaches_the_adapter),
    cmocka_unit_test(connect_timeout_defaults_when_unset),
    cmocka_unit_test(connect_timeout_is_configurable),
    cmocka_unit_test(the_username_carries_the_host_and_device_id),
    cmocka_unit_test(the_username_carries_an_api_version),
    cmocka_unit_test(the_model_id_is_announced_in_the_username),
    cmocka_unit_test(no_model_id_means_none_in_the_username),
    cmocka_unit_test(an_empty_model_id_is_treated_as_none),
    cmocka_unit_test(the_connect_does_not_request_a_clean_session),
    /* close() */
    cmocka_unit_test(close_rejects_null_client),
    cmocka_unit_test_setup_teardown(
        close_while_connecting_disconnects_the_adapter, setup, teardown),
    cmocka_unit_test_setup_teardown(
        close_while_connecting_suppresses_a_late_connack, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_connack_without_a_pending_close_still_connects, setup, teardown),
    cmocka_unit_test_setup_teardown(close_twice_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(close_from_faulted_returns_to_idle, setup, teardown),
    cmocka_unit_test_setup_teardown(close_from_faulted_twice_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(
        open_after_close_from_faulted_starts_a_new_session, setup, teardown),
    /* destroy() */
    cmocka_unit_test(destroy_while_connected_destroys_the_adapter),
    cmocka_unit_test(destroy_while_connecting_destroys_the_adapter),
    cmocka_unit_test_setup_teardown(destroy_is_silent_on_the_state_callback, setup, teardown),
    cmocka_unit_test(destroy_tolerates_null),
    /* do_work() */
    cmocka_unit_test(do_work_rejects_null_client),
    cmocka_unit_test_setup_teardown(do_work_before_open_touches_no_adapter, setup, teardown),
    cmocka_unit_test_setup_teardown(do_work_after_close_touches_no_adapter, setup, teardown),
    cmocka_unit_test_setup_teardown(do_work_forwards_the_timeout_to_the_adapter, setup, teardown),
    cmocka_unit_test_setup_teardown(do_work_surfaces_the_adapter_pump_error, setup, teardown),
    /* reopen */
    cmocka_unit_test_setup_teardown(reopen_after_close_starts_a_second_session, setup, teardown),
    /* state observer registry */
    cmocka_unit_test(add_state_observer_rejects_null_client),
    cmocka_unit_test_setup_teardown(state_callback_carries_the_failure_reason, setup, teardown),
    cmocka_unit_test_setup_teardown(a_second_observer_does_not_displace_the_first, setup, teardown),
    cmocka_unit_test_setup_teardown(adding_the_same_observer_twice_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(
        one_callback_with_two_contexts_is_two_observers, setup, teardown),
    cmocka_unit_test_setup_teardown(a_removed_observer_stops_being_called, setup, teardown),
    cmocka_unit_test_setup_teardown(
        removing_one_context_leaves_the_other_registered, setup, teardown),
    cmocka_unit_test_setup_teardown(
        removing_an_unregistered_observer_reports_not_found, setup, teardown),
    cmocka_unit_test_setup_teardown(a_full_application_pool_is_reported, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_full_application_pool_does_not_block_a_feature_client, setup, teardown),
    cmocka_unit_test_setup_teardown(
        feature_observers_are_dispatched_before_application_ones, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_registry_cannot_be_mutated_from_inside_an_observer, setup, teardown),
    /* traffic gating */
    cmocka_unit_test_setup_teardown(publish_before_connected_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(subscribe_before_connected_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(publish_after_disconnect_is_rejected, setup, teardown),
    /* CONNACK rejection */
    cmocka_unit_test_setup_teardown(
        identity_rejection_faults_when_reconnect_is_disabled, setup, teardown),
    cmocka_unit_test_setup_teardown(connack_rejection_tears_the_adapter_down, setup, teardown),
    /* transport + proxy */
    cmocka_unit_test(transport_defaults_to_tcp_on_8883_with_no_proxy),
    cmocka_unit_test(websockets_derive_port_443),
    cmocka_unit_test(an_explicit_port_survives_the_transport_default),
    cmocka_unit_test(the_proxy_reaches_the_adapter_whole),
    cmocka_unit_test(the_websocket_path_reaches_the_adapter),
    cmocka_unit_test(the_default_options_leave_the_port_to_the_transport),
    cmocka_unit_test(the_default_options_enable_reconnection),
    cmocka_unit_test(the_default_options_retry_a_refused_connack),
    cmocka_unit_test(reconnection_can_still_be_disabled),
    cmocka_unit_test(a_peer_disconnect_without_retrying_settles_in_idle),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
