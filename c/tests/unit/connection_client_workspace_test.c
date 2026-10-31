// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Connection client entry counts at run time: sizes, init in the application's workspace
 * (required), the workspace wiped by deinit(), and the documented sizing examples. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/mqttv3/az_iot_c2d_client.h"
#include "azure/iot/mqttv3/az_iot_direct_method_client.h"
#include "azure/iot/mqttv3/az_iot_telemetry_client.h"
#include "azure/iot/mqttv3/az_iot_twin_client.h"
#include "azure/iot/mqttv5/az_iot_direct_method_client.h"
#include "azure/iot/mqttv5/az_iot_telemetry_client.h"
#include "azure/iot/mqttv5/az_iot_twin_client.h"
#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"

#define TEST_WORKSPACE_MAX 16384u

static uint8_t g_workspace[TEST_WORKSPACE_MAX + 1];

static void on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  (void)event;
  (void)user_ctx;
}

static az_iot_connection_client_options direct_options(void)
{
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.iothub_hostname = "hub.example";
  opts.device_id = "device";
  return opts;
}

static void sizes_follow_the_limits(void** state)
{
  (void)state;
  az_iot_connection_client_limits limits = { 0 };
  assert_int_equal(az_iot_connection_client_get_workspace_size(NULL), 0);
  assert_int_equal(
      az_iot_connection_client_get_workspace_size(&limits),
      AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE);
  assert_int_equal(
      AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE,
      AZ_IOT_CONNECTION_CLIENT_WORKSPACE_SIZE(
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PENDING_PUBLISHES,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PUBLISH_RESERVATIONS,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PERSISTENT_SUBSCRIPTIONS,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_APPLICATION_STATE_OBSERVERS,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_CLIENTS,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_INBOUND_HANDLERS));

  limits.max_pending_publishes = 64;
  limits.max_application_state_observers = 1;
  assert_int_equal(
      az_iot_connection_client_get_workspace_size(&limits),
      AZ_IOT_CONNECTION_CLIENT_WORKSPACE_SIZE(
          64,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PUBLISH_RESERVATIONS,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PERSISTENT_SUBSCRIPTIONS,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS,
          1,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_CLIENTS,
          AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_INBOUND_HANDLERS));

  /* Above a maximum: no size. */
  limits = (az_iot_connection_client_limits){ 0 };
  limits.max_publish_reservations = 255;
  assert_int_equal(az_iot_connection_client_get_workspace_size(&limits), 0);
  limits = (az_iot_connection_client_limits){ 0 };
  limits.max_persistent_subscriptions = 256;
  assert_int_equal(az_iot_connection_client_get_workspace_size(&limits), 0);
  limits = (az_iot_connection_client_limits){ 0 };
  limits.max_pending_publishes = 65536;
  assert_int_equal(az_iot_connection_client_get_workspace_size(&limits), 0);
}

static void zero_limits_resolve_to_the_defaults_in_a_static_workspace(void** state)
{
  (void)state;
  static az_iot_connection_client client;
  static uint8_t workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];
  az_iot_connection_client_options opts = direct_options();
  opts.workspace = workspace;
  opts.workspace_size = sizeof(workspace);
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_OK);
  const az_iot_connection_client_limits* l = &client.opts.limits;
  assert_int_equal(
      l->max_pending_publishes, AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PENDING_PUBLISHES);
  assert_int_equal(
      l->max_publish_reservations, AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PUBLISH_RESERVATIONS);
  assert_int_equal(
      l->max_persistent_subscriptions,
      AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PERSISTENT_SUBSCRIPTIONS);
  assert_int_equal(
      l->max_feature_observers, AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS);
  assert_int_equal(
      l->max_application_state_observers,
      AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_APPLICATION_STATE_OBSERVERS);
  assert_int_equal(l->max_feature_clients, AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_CLIENTS);
  assert_int_equal(l->max_inbound_handlers, AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_INBOUND_HANDLERS);
  assert_true(
      (uint8_t*)client.pending_pubacks >= workspace
      && (uint8_t*)client.pending_pubacks < workspace + sizeof(workspace));
  az_iot_connection_client_deinit(&client);
}

static void entries_live_in_the_application_workspace_and_are_wiped(void** state)
{
  (void)state;
  static az_iot_connection_client client;
  az_iot_connection_client_options opts = direct_options();
  opts.limits.max_application_state_observers = 1;
  const size_t size = az_iot_connection_client_get_workspace_size(&opts.limits);
  /* 32 bytes more than needed: deinit() wipes all of it. */
  const size_t supplied = size + 32;
  assert_true(size > 0 && supplied < TEST_WORKSPACE_MAX);
  memset(g_workspace, 0xA5, sizeof(g_workspace));
  /* One byte in: the client aligns inside the workspace. */
  opts.workspace = g_workspace + 1;
  opts.workspace_size = supplied;
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_OK);
  assert_true(
      (uint8_t*)client.app_state_observers >= g_workspace + 1
      && (uint8_t*)client.app_state_observers < g_workspace + 1 + size);

  /* The application pool holds exactly one observer. */
  int first = 0;
  int second = 0;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&client, on_state, &first), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&client, on_state, &second),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  az_iot_connection_client_deinit(&client);
  for (size_t i = 1; i <= supplied; i++)
  {
    assert_int_equal(g_workspace[i], 0);
  }
  /* Nothing past it is written. */
  assert_int_equal(g_workspace[supplied + 1], 0xA5);
}

static void a_workspace_holds_more_than_the_defaults(void** state)
{
  (void)state;
  enum
  {
    OBSERVERS = AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_APPLICATION_STATE_OBSERVERS * 3
  };
  static az_iot_connection_client client;
  az_iot_connection_client_options opts = direct_options();
  opts.limits.max_application_state_observers = OBSERVERS;
  const size_t size = az_iot_connection_client_get_workspace_size(&opts.limits);
  assert_true(size > AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE && size < TEST_WORKSPACE_MAX);
  memset(g_workspace, 0xA5, sizeof(g_workspace));
  opts.workspace = g_workspace;
  opts.workspace_size = size;
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_OK);
  assert_true(
      (uint8_t*)client.app_state_observers >= g_workspace
      && (uint8_t*)(client.app_state_observers + OBSERVERS) <= g_workspace + size);

  int contexts[OBSERVERS + 1];
  for (size_t i = 0; i < OBSERVERS; i++)
  {
    assert_int_equal(
        az_iot_connection_client_add_state_observer(&client, on_state, &contexts[i]), AZ_IOT_OK);
  }
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&client, on_state, &contexts[OBSERVERS]),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  az_iot_connection_client_deinit(&client);
  for (size_t i = 0; i < size; i++)
  {
    assert_int_equal(g_workspace[i], 0);
  }
  assert_int_equal(g_workspace[size], 0xA5);
}

static void init_refuses_limits_it_cannot_hold(void** state)
{
  (void)state;
  static az_iot_connection_client client;
  static uint8_t untouched[sizeof(az_iot_connection_client)];
  memset(&client, 0x5A, sizeof(client));
  memset(untouched, 0x5A, sizeof(untouched));

  /* Above a maximum. */
  az_iot_connection_client_options opts = direct_options();
  opts.limits.max_publish_reservations = 255;
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_ERR_INVALID_ARG);

  /* No workspace, whatever the limits. */
  opts = direct_options();
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_ERR_INVALID_ARG);
  opts.limits = (az_iot_connection_client_limits){ 1, 1, 1, 1, 1, 1, 1 };
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_ERR_INVALID_ARG);

  /* A workspace one byte short. */
  opts.workspace = g_workspace;
  opts.workspace_size = az_iot_connection_client_get_workspace_size(&opts.limits) - 1;
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  assert_memory_equal(&client, untouched, sizeof(client));
}

/** @brief Feature clients of the sizing examples in docs/client-configuration.md. */
typedef struct example_device
{
  az_iot_connection_client connection;
  az_iot_mqttv3_telemetry_client telemetry_v3;
  az_iot_mqttv3_c2d_client c2d;
  az_iot_mqttv3_direct_method_client direct_method_v3;
  az_iot_mqttv3_twin_client twin_v3;
  az_iot_mqttv5_telemetry_client telemetry_v5;
  az_iot_mqttv5_direct_method_client direct_method_v5;
  az_iot_mqttv5_twin_client twin_v5;
} example_device;

static example_device g_device;

/**
 * @brief Runs a documented sizing example: attaches its feature clients (MQTT 3.1.1: telemetry,
 * C2D, direct methods, twin; MQTT 5: telemetry, direct methods, twin), opens and takes the
 * CONNACK, which binds every feature client's topics.
 * @return true if all of it succeeds with @p limits in a workspace of exactly their size.
 */
static bool example_device_connects(bool mqtt_v5, const az_iot_connection_client_limits* limits)
{
  az_iot_connection_client_options opts = direct_options();
  opts.connection_profile
      = mqtt_v5 ? AZ_IOT_CONNECTION_PROFILE_MQTT_V5 : AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
  opts.limits = *limits;
  opts.workspace = g_workspace;
  opts.workspace_size = az_iot_connection_client_get_workspace_size(limits);
  assert_true(opts.workspace_size <= TEST_WORKSPACE_MAX);
  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(
      mqtt_v5 ? AZ_IOT_MQTT_VERSION_5 : AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(factory);
  memset(&g_device, 0, sizeof(g_device));
  assert_int_equal(az_iot_test_connection_client_init(&g_device.connection, &opts), AZ_IOT_OK);
  az_iot_connection_client* c = &g_device.connection;

  size_t attached = 0;
  az_iot_result result = mqtt_v5 ? az_iot_mqttv5_telemetry_client_init(&g_device.telemetry_v5, c)
                                 : az_iot_mqttv3_telemetry_client_init(&g_device.telemetry_v3, c);
  attached += result == AZ_IOT_OK ? 1u : 0u;
  if (result == AZ_IOT_OK)
  {
    result = mqtt_v5 ? az_iot_mqttv5_direct_method_client_init(&g_device.direct_method_v5, c)
                     : az_iot_mqttv3_direct_method_client_init(&g_device.direct_method_v3, c);
    attached += result == AZ_IOT_OK ? 1u : 0u;
  }
  if (result == AZ_IOT_OK)
  {
    result = mqtt_v5 ? az_iot_mqttv5_twin_client_init(&g_device.twin_v5, c)
                     : az_iot_mqttv3_twin_client_init(&g_device.twin_v3, c);
    attached += result == AZ_IOT_OK ? 1u : 0u;
  }
  if (result == AZ_IOT_OK && !mqtt_v5)
  {
    result = az_iot_mqttv3_c2d_client_init(&g_device.c2d, c);
    attached += result == AZ_IOT_OK ? 1u : 0u;
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client_register_mqtt_factory(c, factory);
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client_open(c);
  }
  if (result == AZ_IOT_OK)
  {
    az_iot_mock_mqtt_client* mock = az_iot_mock_mqtt_factory_last_client(factory);
    assert_non_null(mock);
    assert_true(az_iot_mock_mqtt_client_inject_connected(mock, AZ_IOT_OK));
    result = az_iot_connection_client_do_work(c, 0);
  }
  const az_iot_connection_state hub = c->state[AZ_IOT_CONN_SCOPE_HUB];
  const bool connects = result == AZ_IOT_OK
      && (hub == AZ_IOT_CONN_STATE_CONNECTING || hub == AZ_IOT_CONN_STATE_CONNECTED);

  if (mqtt_v5)
  {
    if (attached > 2)
    {
      az_iot_mqttv5_twin_client_deinit(&g_device.twin_v5);
    }
    if (attached > 1)
    {
      az_iot_mqttv5_direct_method_client_deinit(&g_device.direct_method_v5);
    }
    if (attached > 0)
    {
      az_iot_mqttv5_telemetry_client_deinit(&g_device.telemetry_v5);
    }
  }
  else
  {
    if (attached > 3)
    {
      az_iot_mqttv3_c2d_client_deinit(&g_device.c2d);
    }
    if (attached > 2)
    {
      az_iot_mqttv3_twin_client_deinit(&g_device.twin_v3);
    }
    if (attached > 1)
    {
      az_iot_mqttv3_direct_method_client_deinit(&g_device.direct_method_v3);
    }
    if (attached > 0)
    {
      az_iot_mqttv3_telemetry_client_deinit(&g_device.telemetry_v3);
    }
  }
  az_iot_connection_client_deinit(c);
  az_iot_mock_mqtt_factory_destroy(factory);
  return connects;
}

/**
 * @brief Asserts that @p limits run the example and that one less of any limit above 1 does not,
 * skipping max_pending_publishes and max_application_state_observers (the application's own).
 */
static void assert_example_limits_are_exact(
    bool mqtt_v5,
    const az_iot_connection_client_limits* limits)
{
  assert_true(example_device_connects(mqtt_v5, limits));
  az_iot_connection_client_limits fewer;
#define ASSERT_ONE_LESS_FAILS(field)                        \
  if (limits->field > 1u)                                   \
  {                                                         \
    fewer = *limits;                                        \
    fewer.field--;                                          \
    assert_false(example_device_connects(mqtt_v5, &fewer)); \
  }
  ASSERT_ONE_LESS_FAILS(max_publish_reservations);
  ASSERT_ONE_LESS_FAILS(max_persistent_subscriptions);
  ASSERT_ONE_LESS_FAILS(max_feature_observers);
  ASSERT_ONE_LESS_FAILS(max_feature_clients);
  ASSERT_ONE_LESS_FAILS(max_inbound_handlers);
#undef ASSERT_ONE_LESS_FAILS
}

static void the_mqttv3_sizing_example_holds_its_feature_clients_exactly(void** state)
{
  (void)state;
  /* docs/client-configuration.md, "Sizing the limits": telemetry, C2D, direct methods, twin. */
  const az_iot_connection_client_limits limits = { 4, 1, 4, 2, 1, 1, 4 };
  assert_example_limits_are_exact(false, &limits);
}

static void the_mqttv5_sizing_example_holds_its_feature_clients_exactly(void** state)
{
  (void)state;
  /* docs/client-configuration.md, "Sizing the limits": telemetry, direct methods, twin. The
   * direct method client reserves AZ_IOT_MQTTV5_DM_RESERVED_PUBACKS slots; 4 are left to share. */
  const az_iot_connection_client_limits limits
      = { AZ_IOT_MQTTV5_DM_RESERVED_PUBACKS + 4u, 1, 1, 4, 1, 2, 2 };
  assert_example_limits_are_exact(true, &limits);

  az_iot_connection_client_limits reserved_only = limits;
  reserved_only.max_pending_publishes = AZ_IOT_MQTTV5_DM_RESERVED_PUBACKS;
  assert_true(example_device_connects(true, &reserved_only));
  reserved_only.max_pending_publishes--;
  assert_false(example_device_connects(true, &reserved_only));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(sizes_follow_the_limits),
    cmocka_unit_test(zero_limits_resolve_to_the_defaults_in_a_static_workspace),
    cmocka_unit_test(entries_live_in_the_application_workspace_and_are_wiped),
    cmocka_unit_test(a_workspace_holds_more_than_the_defaults),
    cmocka_unit_test(init_refuses_limits_it_cannot_hold),
    cmocka_unit_test(the_mqttv3_sizing_example_holds_its_feature_clients_exactly),
    cmocka_unit_test(the_mqttv5_sizing_example_holds_its_feature_clients_exactly),
  };
  return cmocka_run_group_tests_name("connection_client_workspace", tests, NULL, NULL);
}
