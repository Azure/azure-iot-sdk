// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The connection client building its MQTT clients in an application workspace
 * (opts.mqtt_adapter): init() checks, one client at a time across the DPS and hub sessions,
 * adapter errors, and the wipe at deinit(). The factory path is covered by the other suites. */

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
#ifdef AZ_IOT_TEST_WITH_AZ_MQTT_ADAPTER
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include <az_mqtt/az_mqtt_transport.h>
#endif

#include "internal/connection_client_internal.h"

#include "support/connection_test_harness.h"
#include "support/test_provider.h"

#define RETRY_DELAY_MS 20u
/* Bytes past the MQTT workspace that must stay untouched. */
#define GUARD_SIZE 32u

static const char k_assigned_topic[] = "$dps/registrations/res/200/?$rid=1";
static const char k_assigned_body[]
    = "{\"operationId\":\"op-1\",\"status\":\"assigned\","
      "\"registrationState\":{\"registrationId\":\"ut-device\","
      "\"assignedHub\":\"myhub.azure-devices.net\",\"deviceId\":\"assigned-device\"}}";

static const az_iot_mqtt_limits k_limits = { 1024, 1024, 4, 0, 1024 };

static uint8_t g_connection_workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];

typedef struct adapter_fixture
{
  az_iot_connection_client client;
  az_iot_test_state_log log;
  uint8_t* mqtt_workspace; /* mqtt_workspace_size + GUARD_SIZE bytes */
  size_t mqtt_workspace_size;
  bool initialized;
} adapter_fixture;

static int setup(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);
  az_iot_mock_mqtt_adapter_reset();
  fx->mqtt_workspace_size = az_iot_mock_mqtt_get_adapter()->get_workspace_size(&k_limits);
  assert_true(fx->mqtt_workspace_size > 0);
  fx->mqtt_workspace = (uint8_t*)malloc(fx->mqtt_workspace_size + GUARD_SIZE);
  assert_non_null(fx->mqtt_workspace);
  memset(fx->mqtt_workspace, 0xA5, fx->mqtt_workspace_size + GUARD_SIZE);
  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  if (fx->initialized)
  {
    (void)az_iot_test_connection_client_deinit(&fx->client);
  }
  free(fx->mqtt_workspace);
  free(fx);
  return 0;
}

static az_iot_connection_client_options direct_options(void)
{
  az_iot_connection_client_options opts = az_iot_test_mqtt_v3_options();
  opts.workspace = g_connection_workspace;
  opts.workspace_size = sizeof(g_connection_workspace);
  return opts;
}

static az_iot_connection_client_options dps_options(void)
{
  az_iot_connection_client_options opts = { 0 };
  opts.device_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  opts.workspace = g_connection_workspace;
  opts.workspace_size = sizeof(g_connection_workspace);
  return opts;
}

static void with_retries(az_iot_connection_client_options* opts)
{
  opts->reconnection_policy.initial_delay_ms = RETRY_DELAY_MS;
  opts->reconnection_policy.max_delay_ms = RETRY_DELAY_MS;
  opts->reconnection_policy.max_attempts = 3;
  opts->reconnection_policy.jitter_pct = 0;
}

/** @brief Inits @p fx's client with @p opts and the mock adapter in @p fx's MQTT workspace. */
static void init_with_mock_adapter(adapter_fixture* fx, az_iot_connection_client_options opts)
{
  opts.mqtt_adapter = az_iot_mock_mqtt_get_adapter();
  opts.mqtt_limits = k_limits;
  opts.mqtt_workspace = fx->mqtt_workspace;
  opts.mqtt_workspace_size = fx->mqtt_workspace_size;
  assert_int_equal(az_iot_test_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);
}

static void assert_in_mqtt_workspace(const adapter_fixture* fx, const void* object, size_t size)
{
  const uint8_t* p = (const uint8_t*)object;
  assert_true(p >= fx->mqtt_workspace && p + size <= fx->mqtt_workspace + fx->mqtt_workspace_size);
}

static void assert_adapter_stats(size_t built, size_t live)
{
  az_iot_mock_mqtt_adapter_stats stats = az_iot_mock_mqtt_adapter_get_stats();
  assert_int_equal(stats.built, built);
  assert_int_equal(stats.live, live);
  assert_int_equal(stats.destroyed, built - live);
  assert_int_equal(stats.max_live, 1);
}

/** @brief Index of the newest event on @p scope; fails if there is none. */
static size_t last_event_on(const az_iot_test_state_log* log, az_iot_connection_scope scope)
{
  for (size_t i = log->count; i > 0; --i)
  {
    if (log->scopes[i - 1] == scope)
    {
      return i - 1;
    }
  }
  fail();
  return 0;
}

/** @brief Asserts the event at @p index failed locally with @p result and @p message. */
static void assert_local_error(
    const az_iot_test_state_log* log,
    size_t index,
    az_iot_result result,
    const char* message)
{
  assert_int_equal(log->reasons[index], result);
  assert_true(log->error_present[index]);
  assert_int_equal(log->error_sources[index], AZ_IOT_CONN_ERR_SRC_LOCAL);
  assert_int_equal(log->error_codes[index], result);
  assert_string_equal(log->error_message[index], message);
}

/** @brief Opens a DPS client and takes it to the assigned hub's CONNECT; returns the DPS client. */
static az_iot_mock_mqtt_client* register_to_the_assigned_hub(adapter_fixture* fx)
{
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_adapter_last_client();
  assert_non_null(dps);
  assert_ptr_equal(fx->client.dps_mqtt, dps);
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(dps, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(dps, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_true(az_iot_mock_mqtt_client_inject_message(
      dps,
      k_assigned_topic,
      (const uint8_t*)k_assigned_body,
      strlen(k_assigned_body),
      AZ_IOT_MQTT_QOS_1));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(&fx->client, 0);
  }
  return dps;
}

static void assert_connects_to(az_iot_mock_mqtt_client* m, const char* host)
{
  const az_iot_mock_call* connect = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(connect);
  assert_string_equal(connect->connect.host, host);
}

static void init_checks_the_mqtt_workspace(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  static uint8_t untouched[sizeof(az_iot_connection_client)];
  memset(&fx->client, 0x5A, sizeof(fx->client));
  memset(untouched, 0x5A, sizeof(untouched));

  az_iot_connection_client_options opts = direct_options();
  opts.mqtt_adapter = az_iot_mock_mqtt_get_adapter();
  opts.mqtt_limits = k_limits;
  opts.mqtt_workspace_size = fx->mqtt_workspace_size;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);

  opts.mqtt_workspace = fx->mqtt_workspace;
  opts.mqtt_workspace_size = fx->mqtt_workspace_size - 1u;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  opts.mqtt_workspace_size = fx->mqtt_workspace_size;
  opts.mqtt_limits.max_in_flight_messages = 0;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);

  const az_iot_mqtt_adapter no_init = { az_iot_mock_mqtt_get_adapter()->get_workspace_size, NULL };
  opts.mqtt_limits = k_limits;
  opts.mqtt_adapter = &no_init;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);

  assert_memory_equal(&fx->client, untouched, sizeof(fx->client));

  opts.mqtt_adapter = az_iot_mock_mqtt_get_adapter();
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_deinit(&fx->client), AZ_IOT_OK);
}

static void a_direct_hub_connect_builds_its_client_in_the_workspace(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  init_with_mock_adapter(fx, direct_options());

  /* No factory registered: the adapter builds the client. */
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_adapter_last_client();
  assert_non_null(hub);
  assert_ptr_equal(fx->client.active_client, hub);
  assert_in_mqtt_workspace(fx, hub, sizeof(az_iot_mqtt_client));
  assert_connects_to(hub, "broker.example");

  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(&fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);
  assert_adapter_stats(1, 1);
}

static void registration_then_the_hub_session_reuse_the_workspace(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  init_with_mock_adapter(fx, dps_options());

  az_iot_mock_mqtt_client* dps = register_to_the_assigned_hub(fx);
  assert_in_mqtt_workspace(fx, dps, sizeof(az_iot_mqtt_client));
  assert_null(fx->client.dps_mqtt);

  /* The DPS client was destroyed before the hub client was built in its place. */
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_adapter_last_client();
  assert_ptr_equal(hub, dps);
  assert_ptr_equal(fx->client.active_client, hub);
  assert_connects_to(hub, "myhub.azure-devices.net");
  assert_adapter_stats(2, 1);
}

static void a_reconnect_after_a_drop_reuses_the_workspace(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  az_iot_connection_client_options opts = direct_options();
  with_retries(&opts);
  init_with_mock_adapter(fx, opts);

  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* first = az_iot_mock_mqtt_adapter_last_client();
  assert_true(az_iot_mock_mqtt_client_inject_connected(first, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(first));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(&fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_adapter_stats(1, 0);

  az_iot_test_wait_until_ms(fx->client.reconnect_due_ms);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  az_iot_mock_mqtt_client* second = az_iot_mock_mqtt_adapter_last_client();
  assert_ptr_equal(second, first);
  assert_ptr_equal(fx->client.active_client, second);
  assert_connects_to(second, "broker.example");
  assert_adapter_stats(2, 1);
}

static void a_hub_session_beside_a_held_provisioning_session_is_refused_busy(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  az_iot_connection_client_options opts = dps_options();
  with_retries(&opts);
  init_with_mock_adapter(fx, opts);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* dps = register_to_the_assigned_hub(fx);
  size_t dps_calls = az_iot_mock_mqtt_client_call_count(dps);

  /* The held DPS client is live and untouched; the hub start is refused and retried. */
  assert_ptr_equal(fx->client.dps_mqtt, dps);
  assert_ptr_equal(az_iot_mock_mqtt_adapter_last_client(), dps);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(dps, AZ_IOT_MOCK_CALL_DESTROY), 0);
  assert_true(az_iot_connection_client__dps_session_ready(&fx->client));
  assert_null(fx->client.active_client);
  assert_adapter_stats(1, 1);
  size_t refused = last_event_on(&fx->log, AZ_IOT_CONN_SCOPE_HUB);
  assert_int_equal(fx->log.states[refused], AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_local_error(
      &fx->log, refused, AZ_IOT_ERR_BUSY, "mqtt_workspace holds the other session's MQTT client");
  assert_int_equal(az_iot_mock_mqtt_client_call_count(dps), dps_calls);

  /* Released, the DPS session closes and the retry builds the hub client in its place. */
  az_iot_connection_client__dps_user_release(&fx->client);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_null(fx->client.dps_mqtt);
  az_iot_test_wait_until_ms(fx->client.reconnect_due_ms);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_adapter_last_client();
  assert_ptr_equal(fx->client.active_client, hub);
  assert_ptr_equal(hub, dps);
  assert_connects_to(hub, "myhub.azure-devices.net");
  assert_adapter_stats(2, 1);
}

static void a_provisioning_session_beside_the_hub_session_is_refused_busy(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  init_with_mock_adapter(fx, dps_options());
  (void)register_to_the_assigned_hub(fx);
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_adapter_last_client();
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  size_t hub_calls = az_iot_mock_mqtt_client_call_count(hub);

  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);

  assert_null(fx->client.dps_mqtt);
  assert_ptr_equal(fx->client.active_client, hub);
  assert_ptr_equal(az_iot_mock_mqtt_adapter_last_client(), hub);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(hub), hub_calls);
  assert_int_equal(
      az_iot_connection_client_get_state(&fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);
  assert_adapter_stats(2, 1);
  size_t refused = last_event_on(&fx->log, AZ_IOT_CONN_SCOPE_DPS);
  assert_int_equal(fx->log.states[refused], AZ_IOT_CONN_STATE_IDLE);
  assert_local_error(
      &fx->log, refused, AZ_IOT_ERR_BUSY, "mqtt_workspace holds the other session's MQTT client");
  az_iot_connection_client__dps_user_release(&fx->client);
}

static void a_failed_hub_connect_releases_the_workspace_for_the_retry(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  az_iot_connection_client_options opts = direct_options();
  with_retries(&opts);
  init_with_mock_adapter(fx, opts);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* slot = az_iot_mock_mqtt_adapter_last_client();
  assert_true(az_iot_mock_mqtt_client_inject_connected(slot, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(slot));
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  /* The reconnect's client fails connect(): destroyed once, the workspace released. */
  az_iot_mock_mqtt_adapter_fail_next_connect(AZ_IOT_ERR_TLS);
  az_iot_test_wait_until_ms(fx->client.reconnect_due_ms);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(slot, AZ_IOT_MOCK_CALL_CONNECT), 1);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(slot, AZ_IOT_MOCK_CALL_DESTROY), 1);
  assert_null(fx->client.active_client);
  assert_null(fx->client.mqtt_workspace_client);
  assert_adapter_stats(2, 0);
  size_t failed = last_event_on(&fx->log, AZ_IOT_CONN_SCOPE_HUB);
  assert_int_equal(fx->log.states[failed], AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_local_error(&fx->log, failed, AZ_IOT_ERR_TLS, "MQTT adapter connect() failed");

  /* The next retry builds its client in the same workspace and connects. */
  az_iot_test_wait_until_ms(fx->client.reconnect_due_ms);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_ptr_equal(az_iot_mock_mqtt_adapter_last_client(), slot);
  assert_ptr_equal(fx->client.active_client, slot);
  assert_adapter_stats(3, 1);
  assert_true(az_iot_mock_mqtt_client_inject_connected(slot, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(&fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);
}

static void a_failed_dps_connect_releases_the_workspace_for_the_retry(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  init_with_mock_adapter(fx, dps_options());

  /* The DPS client fails connect(): destroyed once, the workspace released. */
  az_iot_mock_mqtt_adapter_fail_next_connect(AZ_IOT_ERR_TLS);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_TLS);
  assert_null(fx->client.dps_mqtt);
  assert_null(fx->client.mqtt_workspace_client);
  assert_adapter_stats(1, 0);
  size_t failed = last_event_on(&fx->log, AZ_IOT_CONN_SCOPE_DPS);
  assert_int_equal(fx->log.states[failed], AZ_IOT_CONN_STATE_IDLE);
  assert_local_error(&fx->log, failed, AZ_IOT_ERR_TLS, "MQTT adapter connect() failed");

  /* The retry registers in the same workspace, and the hub session follows it there. */
  az_iot_mock_mqtt_client* dps = register_to_the_assigned_hub(fx);
  assert_in_mqtt_workspace(fx, dps, sizeof(az_iot_mqtt_client));
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_adapter_last_client();
  assert_ptr_equal(hub, dps);
  assert_ptr_equal(fx->client.active_client, hub);
  assert_connects_to(hub, "myhub.azure-devices.net");
  assert_adapter_stats(3, 1);
}

static void deinit_wipes_the_mqtt_workspace(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  init_with_mock_adapter(fx, direct_options());
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  assert_true(
      az_iot_mock_mqtt_client_inject_connected(az_iot_mock_mqtt_adapter_last_client(), AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  assert_int_equal(az_iot_test_connection_client_deinit(&fx->client), AZ_IOT_OK);
  fx->initialized = false;
  /* The mock's destroy() wipes nothing: deinit() did. */
  assert_adapter_stats(1, 0);
  for (size_t i = 0; i < fx->mqtt_workspace_size; ++i)
  {
    assert_int_equal(fx->mqtt_workspace[i], 0);
  }
  for (size_t i = 0; i < GUARD_SIZE; ++i)
  {
    assert_int_equal(fx->mqtt_workspace[fx->mqtt_workspace_size + i], 0xA5);
  }
}

static az_iot_result g_scripted_init_result;

static size_t scripted_get_workspace_size(const az_iot_mqtt_limits* limits)
{
  return limits != NULL ? 64u : 0u;
}

static az_iot_result scripted_init(
    az_iot_mqtt_version mqtt_version,
    const az_iot_mqtt_limits* limits,
    uint8_t* workspace,
    size_t workspace_size,
    az_iot_mqtt_client** out_client)
{
  (void)mqtt_version;
  (void)limits;
  (void)workspace;
  (void)workspace_size;
  *out_client = NULL;
  return g_scripted_init_result;
}

static void adapter_init_errors_fail_the_start(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  static const az_iot_mqtt_adapter scripted = { scripted_get_workspace_size, scripted_init };
  static const struct
  {
    az_iot_result init_result;
    az_iot_result open_result;
    const char* message;
  } cases[] = {
    { AZ_IOT_ERR_NOT_SUPPORTED,
      AZ_IOT_ERR_NOT_SUPPORTED,
      "the MQTT adapter does not build MQTT 3.1.1 clients" },
    { AZ_IOT_ERR_NOT_ENOUGH_SPACE,
      AZ_IOT_ERR_NOT_ENOUGH_SPACE,
      "mqtt_workspace is too small for the MQTT client" },
    { AZ_IOT_ERR_INVALID_ARG, AZ_IOT_ERR_INTERNAL, "MQTT adapter init() failed" },
    /* AZ_IOT_OK without a client. */
    { AZ_IOT_OK, AZ_IOT_ERR_INTERNAL, "MQTT adapter init() failed" },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    az_iot_connection_client_options opts = direct_options();
    opts.mqtt_adapter = &scripted;
    opts.mqtt_limits = k_limits;
    opts.mqtt_workspace = fx->mqtt_workspace;
    opts.mqtt_workspace_size = fx->mqtt_workspace_size;
    memset(&fx->log, 0, sizeof(fx->log));
    assert_int_equal(az_iot_test_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
    assert_int_equal(
        az_iot_connection_client_add_state_observer(&fx->client, az_iot_test_on_state, &fx->log),
        AZ_IOT_OK);
    g_scripted_init_result = cases[i].init_result;

    assert_int_equal(az_iot_connection_client_open(&fx->client), cases[i].open_result);
    assert_null(fx->client.active_client);
    assert_null(fx->client.mqtt_workspace_client);
    size_t failed = last_event_on(&fx->log, AZ_IOT_CONN_SCOPE_HUB);
    assert_int_equal(fx->log.states[failed], AZ_IOT_CONN_STATE_IDLE);
    assert_local_error(&fx->log, failed, cases[i].open_result, cases[i].message);
    assert_int_equal(az_iot_test_connection_client_deinit(&fx->client), AZ_IOT_OK);
  }
}

#ifdef AZ_IOT_TEST_WITH_AZ_MQTT_ADAPTER
static void init_sizes_the_mqtt_workspace_for_the_az_mqtt_adapter(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  /* The limits of the example in docs/client-configuration.md and docs/connecting.md. */
  const az_iot_mqtt_limits limits = { 4096, 4096, 8, 8192, 4096 };
  const az_iot_mqtt_adapter* adapter = az_iot_az_mqtt_get_adapter();
  const size_t needed = az_iot_az_mqtt_get_workspace_size(&limits);
  assert_true(needed > 0);
  assert_int_equal(adapter->get_workspace_size(&limits), needed);
  /* The macro's TLS bound depends on the backend (Schannel: about 197 KB). */
  size_t const tls_state = (size_t)az_mqtt_transport_sizeof();
  assert_true(AZ_IOT_AZ_MQTT_WORKSPACE_SIZE(4096, 4096, 8, 8192, 4096, tls_state) >= needed);
  uint8_t* workspace = (uint8_t*)malloc(needed);
  assert_non_null(workspace);

  az_iot_connection_client_options opts = direct_options();
  opts.mqtt_adapter = adapter;
  opts.mqtt_limits = limits;
  opts.mqtt_workspace = workspace;
  opts.mqtt_workspace_size = needed - 1u;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  opts.mqtt_workspace_size = needed;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_deinit(&fx->client), AZ_IOT_OK);
  free(workspace);
}
#endif

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_checks_the_mqtt_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_direct_hub_connect_builds_its_client_in_the_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(
        registration_then_the_hub_session_reuse_the_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(a_reconnect_after_a_drop_reuses_the_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_hub_session_beside_a_held_provisioning_session_is_refused_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_provisioning_session_beside_the_hub_session_is_refused_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_hub_connect_releases_the_workspace_for_the_retry, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_dps_connect_releases_the_workspace_for_the_retry, setup, teardown),
    cmocka_unit_test_setup_teardown(deinit_wipes_the_mqtt_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(adapter_init_errors_fail_the_start, setup, teardown),
#ifdef AZ_IOT_TEST_WITH_AZ_MQTT_ADAPTER
    cmocka_unit_test_setup_teardown(
        init_sizes_the_mqtt_workspace_for_the_az_mqtt_adapter, setup, teardown),
#endif
  };
  return cmocka_run_group_tests_name("connection_client_mqtt_adapter", tests, NULL, NULL);
}
