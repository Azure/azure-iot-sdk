// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Real-stack reconnect integration test.
 *
 * The fast, deterministic connection_client_test drives the core reconnect
 * state machine with the in-memory mock adapter. This test complements it by
 * running the SAME state machine over the GENUINE Paho adapter with the
 * in-process test proxy inducing a real network drop: it proves the core
 * reconnects automatically in response to real adapter events, not just
 * injected ones.
 *
 * A classic v3.1.1 connection client with no certificate_provider connects in
 * plaintext (the adapter only enables TLS when the provider supplies material),
 * so it can talk to a local test broker through the proxy.
 *
 * Whether this runs is decided at build time by AZ_IOT_BUILD_CONFORMANCE_TESTS,
 * the same option that registers the conformance suites, because it needs the
 * same reachable broker. If it was registered, the broker address is a promise
 * the caller has already made, so a missing one fails rather than skips.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "az_iot_test_proxy.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_connection_client.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static const char* g_host = "localhost";
static uint16_t g_port = 1883;

#if defined(_WIN32)
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
static void sleep_ms(unsigned ms)
{
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)((ms % 1000u) * 1000000UL);
  nanosleep(&ts, NULL);
}
#endif

#define STATE_LOG_MAX 64

typedef struct state_log
{
  size_t count;
  az_iot_connection_state states[STATE_LOG_MAX];
} state_log;

static void on_state(az_iot_connection_state state, az_iot_result reason, void* ctx)
{
  (void)reason;
  state_log* s = (state_log*)ctx;
  if (s->count < STATE_LOG_MAX)
  {
    s->states[s->count++] = state;
  }
}

static int count_state(const state_log* s, az_iot_connection_state want)
{
  int n = 0;
  for (size_t i = 0; i < s->count; ++i)
  {
    if (s->states[i] == want)
    {
      ++n;
    }
  }
  return n;
}

/* Pump do_work in 50 ms slices until `want` has been recorded at least `target`
 * times or the slice budget is exhausted. Returns non-zero on success. */
static int pump_until_count(
    az_iot_connection_client* c,
    const state_log* s,
    az_iot_connection_state want,
    int target,
    int max_slices)
{
  for (int i = 0; i < max_slices; ++i)
  {
    if (count_state(s, want) >= target)
    {
      return 1;
    }
    (void)az_iot_connection_client_do_work(c, 50);
    /* do_work returns immediately while RECONNECTING (no active client to pump),
     * so sleep to let wall-clock cross the reconnect delay. */
    sleep_ms(20);
  }
  return count_state(s, want) >= target;
}

static void reconnect_after_real_drop(void** state)
{
  (void)state;

  /* Test proxy passthrough in front of the real broker. */
  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  uint16_t proxy_port = 0;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, &proxy_port), 0);
  assert_int_not_equal(proxy_port, 0);

  az_iot_connection_client* client = (az_iot_connection_client*)calloc(1, sizeof(*client));
  assert_non_null(client);

  /* Classic v3.1.1, plaintext (no certificate_provider), pointed at the proxy,
   * with reconnect armed on a short delay. */
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.host = "127.0.0.1";
  opts.port = proxy_port;
  opts.client_id = "az-iot-recon-it";
  opts.reconnection_policy.initial_delay_ms = 200;
  opts.reconnection_policy.max_delay_ms = 200;
  opts.reconnection_policy.max_attempts = 5;
  opts.reconnection_policy.jitter_pct = 0;
  assert_int_equal(az_iot_connection_client_init(client, &opts), AZ_IOT_OK);

  state_log log = { 0 };
  (void)az_iot_connection_client_set_state_callback(client, on_state, &log);

  az_iot_mqtt_factory* factory = az_iot_paho_factory_create_v3_1_1();
  assert_non_null(factory);
  /* The client adopts the factory and frees it from destroy(); do not free it
   * here. */
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(client, factory), AZ_IOT_OK);

  /* First connection must come up (<= 5 s). */
  assert_int_equal(az_iot_connection_client_open(client), AZ_IOT_OK);
  assert_true(pump_until_count(client, &log, AZ_IOT_CONN_STATE_CONNECTED, 1, 100));

  /* Induce a real network drop; the adapter reports DISCONNECTED and the core
   * must move to RECONNECTING then reconnect on its own. */
  az_iot_test_proxy_drop_now(proxy);
  assert_true(pump_until_count(client, &log, AZ_IOT_CONN_STATE_RECONNECTING, 1, 100));

  /* A SECOND CONNECTED proves the automatic reconnect succeeded (<= 10 s). */
  assert_true(pump_until_count(client, &log, AZ_IOT_CONN_STATE_CONNECTED, 2, 200));

  (void)az_iot_connection_client_close(client);
  az_iot_connection_client_destroy(client);
  free(client);
  az_iot_test_proxy_stop(proxy);
}

int main(void)
{
  const char* host = getenv("AZ_IOT_MQTT_BROKER_HOST");
  const char* port = getenv("AZ_IOT_MQTT_BROKER_PORT");

  if (!host || !*host)
  {
    fprintf(
        stderr,
        "reconnect-integration: AZ_IOT_MQTT_BROKER_HOST is unset or empty, but this test was "
        "built with AZ_IOT_BUILD_CONFORMANCE_TESTS=ON. Point it at a reachable plaintext "
        "broker, or configure with AZ_IOT_BUILD_CONFORMANCE_TESTS=OFF so it is not "
        "registered.\n");
    return 1;
  }

  g_host = host;
  g_port = port ? (uint16_t)atoi(port) : (uint16_t)1883;
  fprintf(stderr, "reconnect-integration: broker %s:%u\n", g_host, (unsigned)g_port);

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(reconnect_after_real_drop),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
