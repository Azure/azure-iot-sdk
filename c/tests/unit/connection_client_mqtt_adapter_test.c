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
static const char k_assigned_mqtt_v5_body[]
    = "{\"operationId\":\"op-1\",\"status\":\"assigned\","
      "\"registrationState\":{\"registrationId\":\"ut-device\","
      "\"assignedHub\":\"myhub.azure-devices.net\",\"connectionProfile\":\"mqttV5\","
      "\"deviceId\":\"assigned-device\"}}";

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
  size_t i = log->count;
  while (i > 0 && log->scopes[i - 1] != scope)
  {
    --i;
  }
  assert_true(i > 0);
  return i - 1;
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

/**
 * @brief Opens a DPS client and takes it to the CONNECT of the hub @p assigned_body assigns;
 * returns the DPS client.
 */
static az_iot_mock_mqtt_client* register_with_the_assignment(
    adapter_fixture* fx,
    const char* assigned_body)
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
      (const uint8_t*)assigned_body,
      strlen(assigned_body),
      AZ_IOT_MQTT_QOS_1));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(&fx->client, 0);
  }
  return dps;
}

/** @brief register_with_the_assignment() to an MQTT 3.1.1 hub. */
static az_iot_mock_mqtt_client* register_to_the_assigned_hub(adapter_fixture* fx)
{
  return register_with_the_assignment(fx, k_assigned_body);
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

/** @brief Where init_*_workspaces() place workspace and mqtt_workspace in one buffer. */
typedef struct workspace_layout
{
  size_t workspace_offset;
  size_t workspace_size;
  size_t mqtt_workspace_offset;
  size_t mqtt_workspace_size;
} workspace_layout;

/** @brief direct_options() with the mock adapter and both workspaces placed in @p buffer. */
static az_iot_connection_client_options options_with_layout(
    uint8_t* buffer,
    const workspace_layout* layout)
{
  az_iot_connection_client_options opts = direct_options();
  opts.workspace = buffer + layout->workspace_offset;
  opts.workspace_size = layout->workspace_size;
  opts.mqtt_adapter = az_iot_mock_mqtt_get_adapter();
  opts.mqtt_limits = k_limits;
  opts.mqtt_workspace = buffer + layout->mqtt_workspace_offset;
  opts.mqtt_workspace_size = layout->mqtt_workspace_size;
  return opts;
}

static void init_refuses_overlapping_workspaces(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  static uint8_t untouched[sizeof(az_iot_connection_client)];
  memset(&fx->client, 0x5A, sizeof(fx->client));
  memset(untouched, 0x5A, sizeof(untouched));
  az_iot_connection_client_options defaults = direct_options();
  const size_t w = az_iot_connection_client_get_workspace_size(&defaults.limits);
  const size_t m = fx->mqtt_workspace_size;
  const size_t largest = w > m ? w : m;
  const size_t buffer_size = 2u * (w + m);
  uint8_t* buffer = (uint8_t*)malloc(buffer_size);
  assert_non_null(buffer);
  memset(buffer, 0xC3, buffer_size);

  const workspace_layout layouts[] = {
    /* Identical. */
    { 0, largest, 0, largest },
    /* mqtt_workspace starts on workspace's last byte. */
    { 0, w, w - 1u, m },
    /* workspace starts on mqtt_workspace's last byte. */
    { m - 1u, w, 0, m },
    /* mqtt_workspace inside workspace. */
    { 0, w + m + 2u, 1, m },
    /* workspace inside mqtt_workspace. */
    { 1, w, 0, w + m + 2u },
  };
  for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i)
  {
    az_iot_connection_client_options opts = options_with_layout(buffer, &layouts[i]);
    assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);
  }

  /* Refused before the client or either workspace changed. */
  assert_memory_equal(&fx->client, untouched, sizeof(fx->client));
  for (size_t i = 0; i < buffer_size; ++i)
  {
    assert_int_equal(buffer[i], 0xC3);
  }
  assert_int_equal(az_iot_mock_mqtt_adapter_get_stats().built, 0);
  free(buffer);
}

static void init_accepts_adjacent_workspaces(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  az_iot_connection_client_options defaults = direct_options();
  const size_t w = az_iot_connection_client_get_workspace_size(&defaults.limits);
  const size_t m = fx->mqtt_workspace_size;
  uint8_t* buffer = (uint8_t*)malloc(w + m);
  assert_non_null(buffer);

  const workspace_layout layouts[] = {
    /* mqtt_workspace right after workspace. */
    { 0, w, w, m },
    /* workspace right after mqtt_workspace. */
    { m, w, 0, m },
  };
  for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i)
  {
    az_iot_connection_client_options opts = options_with_layout(buffer, &layouts[i]);
    assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_deinit(&fx->client), AZ_IOT_OK);
  }
  free(buffer);
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

static void registration_then_an_mqtt_v5_hub_session_reuse_the_workspace(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  init_with_mock_adapter(fx, dps_options());

  az_iot_mock_mqtt_client* dps = register_with_the_assignment(fx, k_assigned_mqtt_v5_body);
  assert_in_mqtt_workspace(fx, dps, sizeof(az_iot_mqtt_client));
  assert_null(fx->client.dps_mqtt);

  /* DPS asked for MQTT 3.1.1, the assigned hub for MQTT 5. */
  az_iot_mqtt_version version;
  assert_true(az_iot_mock_mqtt_adapter_get_requested_version(0, &version));
  assert_int_equal(version, AZ_IOT_MQTT_VERSION_3_1_1);
  assert_true(az_iot_mock_mqtt_adapter_get_requested_version(1, &version));
  assert_int_equal(version, AZ_IOT_MQTT_VERSION_5);
  assert_false(az_iot_mock_mqtt_adapter_get_requested_version(2, &version));

  /* max_live 1: the DPS client was destroyed before the hub client was built in its place. */
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_adapter_last_client();
  assert_ptr_equal(hub, dps);
  assert_ptr_equal(fx->client.active_client, hub);
  assert_connects_to(hub, "myhub.azure-devices.net");
  assert_adapter_stats(2, 1);

  /* MQTT 5: the presence birth-ack, not the CONNACK, connects the session. */
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_int_not_equal(
      az_iot_connection_client_get_state(&fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(hub, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(hub, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  const az_iot_mock_call* birth = az_iot_mock_mqtt_client_last_of(hub, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(birth);
  assert_int_equal(birth->correlation_data_len, 16);
  /* nonce and ack_type must outlive the do_work that delivers the event. */
  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_message;
  memset(&ack_message, 0, sizeof(ack_message));
  ack_message.topic = "ih/assigned-device/dev/presence";
  ack_message.correlation_data = nonce;
  ack_message.correlation_data_len = sizeof(nonce);
  ack_message.user_properties = az_iot_mqtt_user_properties_from_array(&ack_type, 1);
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_message;
  assert_true(az_iot_mock_mqtt_client_inject_event(hub, &ack));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(&fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);
  az_iot_hub_profile profile;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&fx->client, &profile), AZ_IOT_OK);
  assert_int_equal(profile.connection_profile, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
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

/* One MQTT session at a time: the hub session after registration waits for a
 * held provisioning session, whose client keeps the workspace, then is built in
 * its place. */
static void a_hub_session_waits_for_a_held_provisioning_session(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  az_iot_connection_client_options opts = dps_options();
  with_retries(&opts);
  init_with_mock_adapter(fx, opts);
  fx->client.dps_session_wanted = true;

  az_iot_mock_mqtt_client* dps = register_to_the_assigned_hub(fx);
  size_t dps_calls = az_iot_mock_mqtt_client_call_count(dps);

  /* The held DPS client is live and untouched; the hub session waits. */
  assert_ptr_equal(fx->client.dps_mqtt, dps);
  assert_ptr_equal(az_iot_mock_mqtt_adapter_last_client(), dps);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(dps, AZ_IOT_MOCK_CALL_DESTROY), 0);
  assert_true(az_iot_connection_client__dps_session_ready(&fx->client));
  assert_null(fx->client.active_client);
  assert_adapter_stats(1, 1);
  size_t waiting = last_event_on(&fx->log, AZ_IOT_CONN_SCOPE_HUB);
  assert_int_equal(fx->log.states[waiting], AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(fx->log.reasons[waiting], AZ_IOT_OK);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(dps), dps_calls);

  /* Released, the DPS session closes and the hub client is built in its place. */
  az_iot_connection_client__dps_session_release(&fx->client);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_null(fx->client.dps_mqtt);
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_adapter_last_client();
  assert_ptr_equal(fx->client.active_client, hub);
  assert_ptr_equal(hub, dps);
  assert_connects_to(hub, "myhub.azure-devices.net");
  assert_adapter_stats(2, 1);
}

/* One MQTT session at a time: a provisioning session asked for beside the hub
 * session ends it first, and its client is built in the workspace the hub client
 * leaves. */
static void a_provisioning_session_ends_the_hub_session_and_takes_its_workspace(void** state)
{
  adapter_fixture* fx = (adapter_fixture*)*state;
  init_with_mock_adapter(fx, dps_options());
  (void)register_to_the_assigned_hub(fx);
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_adapter_last_client();
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  fx->client.dps_session_wanted = true;
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_null(fx->client.dps_mqtt);
  assert_ptr_equal(fx->client.active_client, hub);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(hub, AZ_IOT_MOCK_CALL_DISCONNECT), 1);
  assert_adapter_stats(2, 1);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(hub));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_null(fx->client.active_client);
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_adapter_last_client();
  assert_ptr_equal(fx->client.dps_mqtt, dps);
  assert_ptr_equal(dps, hub);
  assert_adapter_stats(3, 1);
  az_iot_connection_client__dps_session_release(&fx->client);
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

/* The mock adapter reports its newest client still alive, of any version or of
 * one, after a newer client is destroyed. */
static void the_mock_adapter_reports_its_newest_live_client(void** state)
{
  (void)state;
  const az_iot_mqtt_adapter* adapter = az_iot_mock_mqtt_get_adapter();
  size_t const size = adapter->get_workspace_size(&az_iot_mock_mqtt_limits);
  uint8_t* older_workspace = (uint8_t*)malloc(size);
  uint8_t* newer_workspace = (uint8_t*)malloc(size);
  assert_non_null(older_workspace);
  assert_non_null(newer_workspace);
  az_iot_mqtt_client* older = NULL;
  az_iot_mqtt_client* newer = NULL;
  assert_int_equal(
      adapter->init(
          AZ_IOT_MQTT_VERSION_3_1_1, &az_iot_mock_mqtt_limits, older_workspace, size, &older),
      AZ_IOT_OK);
  assert_int_equal(
      adapter->init(
          AZ_IOT_MQTT_VERSION_3_1_1, &az_iot_mock_mqtt_limits, newer_workspace, size, &newer),
      AZ_IOT_OK);
  assert_ptr_equal(
      az_iot_mock_mqtt_adapter_last_client_of(AZ_IOT_MQTT_VERSION_3_1_1),
      az_iot_mock_mqtt_client_from(newer));

  newer->iface->destroy(newer);
  assert_ptr_equal(
      az_iot_mock_mqtt_adapter_last_client_of(AZ_IOT_MQTT_VERSION_3_1_1),
      az_iot_mock_mqtt_client_from(older));
  assert_ptr_equal(az_iot_mock_mqtt_adapter_last_client(), az_iot_mock_mqtt_client_from(older));
  assert_null(az_iot_mock_mqtt_adapter_last_client_of(AZ_IOT_MQTT_VERSION_5));

  older->iface->destroy(older);
  assert_null(az_iot_mock_mqtt_adapter_last_client_of(AZ_IOT_MQTT_VERSION_3_1_1));
  assert_null(az_iot_mock_mqtt_adapter_last_client());
  free(newer_workspace);
  free(older_workspace);
}

/* The test helper's deinit gives its loaned workspaces back and forgets them, so
 * a second deinit (a teardown after an explicit one) does not wipe them. */
static void the_test_helper_forgets_its_returned_workspaces(void** state)
{
  (void)state;
  az_iot_connection_client client;
  az_iot_connection_client_options opts = { 0 };
  opts.iothub_hostname = "broker.example";
  opts.device_id = "ut-device";
  opts.mqtt_adapter = az_iot_mock_mqtt_get_adapter();
  assert_int_equal(az_iot_test_connection_client_init(&client, &opts), AZ_IOT_OK);
  assert_non_null(client.opts.workspace);
  assert_non_null(client.opts.mqtt_workspace);

  assert_int_equal(az_iot_test_connection_client_deinit(&client), AZ_IOT_OK);
  assert_null(client.opts.workspace);
  assert_int_equal(client.opts.workspace_size, 0);
  assert_null(client.opts.mqtt_workspace);
  assert_int_equal(client.opts.mqtt_workspace_size, 0);
  (void)az_iot_test_connection_client_deinit(&client);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(the_test_helper_forgets_its_returned_workspaces),
    cmocka_unit_test_setup_teardown(
        the_mock_adapter_reports_its_newest_live_client, setup, teardown),
    cmocka_unit_test_setup_teardown(init_checks_the_mqtt_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(init_refuses_overlapping_workspaces, setup, teardown),
    cmocka_unit_test_setup_teardown(init_accepts_adjacent_workspaces, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_direct_hub_connect_builds_its_client_in_the_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(
        registration_then_the_hub_session_reuse_the_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(
        registration_then_an_mqtt_v5_hub_session_reuse_the_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(a_reconnect_after_a_drop_reuses_the_workspace, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_hub_session_waits_for_a_held_provisioning_session, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_provisioning_session_ends_the_hub_session_and_takes_its_workspace, setup, teardown),
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
