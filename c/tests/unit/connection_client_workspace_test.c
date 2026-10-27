// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Connection client entry counts at run time: sizes, init in an application workspace or the
 * client's default one, and the workspace wiped by deinit(). */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"

#include "support/connection_default_limits.h"

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
          AZ_IOT_TEST_DEFAULT_PENDING_PUBLISHES,
          AZ_IOT_TEST_DEFAULT_PUBLISH_RESERVATIONS,
          AZ_IOT_TEST_DEFAULT_PERSISTENT_SUBSCRIPTIONS,
          AZ_IOT_TEST_DEFAULT_SESSION_HANDLERS,
          AZ_IOT_TEST_DEFAULT_FEATURE_STATE_OBSERVERS,
          AZ_IOT_TEST_DEFAULT_APPLICATION_STATE_OBSERVERS,
          AZ_IOT_TEST_DEFAULT_FEATURE_CLIENTS,
          AZ_IOT_TEST_DEFAULT_INBOUND_HANDLERS));

  limits.max_pending_publishes = 64;
  limits.max_application_state_observers = 1;
  assert_int_equal(
      az_iot_connection_client_get_workspace_size(&limits),
      AZ_IOT_CONNECTION_CLIENT_WORKSPACE_SIZE(
          64,
          AZ_IOT_TEST_DEFAULT_PUBLISH_RESERVATIONS,
          AZ_IOT_TEST_DEFAULT_PERSISTENT_SUBSCRIPTIONS,
          AZ_IOT_TEST_DEFAULT_SESSION_HANDLERS,
          AZ_IOT_TEST_DEFAULT_FEATURE_STATE_OBSERVERS,
          1,
          AZ_IOT_TEST_DEFAULT_FEATURE_CLIENTS,
          AZ_IOT_TEST_DEFAULT_INBOUND_HANDLERS));

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

static void zero_limits_resolve_to_the_defaults(void** state)
{
  (void)state;
  static az_iot_connection_client client;
  az_iot_connection_client_options opts = direct_options();
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_OK);
  const az_iot_connection_client_limits* l = &client.opts.limits;
  assert_int_equal(l->max_pending_publishes, AZ_IOT_TEST_DEFAULT_PENDING_PUBLISHES);
  assert_int_equal(l->max_publish_reservations, AZ_IOT_TEST_DEFAULT_PUBLISH_RESERVATIONS);
  assert_int_equal(l->max_persistent_subscriptions, AZ_IOT_TEST_DEFAULT_PERSISTENT_SUBSCRIPTIONS);
  assert_int_equal(l->max_session_handlers, AZ_IOT_TEST_DEFAULT_SESSION_HANDLERS);
  assert_int_equal(l->max_feature_state_observers, AZ_IOT_TEST_DEFAULT_FEATURE_STATE_OBSERVERS);
  assert_int_equal(
      l->max_application_state_observers, AZ_IOT_TEST_DEFAULT_APPLICATION_STATE_OBSERVERS);
  assert_int_equal(l->max_feature_clients, AZ_IOT_TEST_DEFAULT_FEATURE_CLIENTS);
  assert_int_equal(l->max_inbound_handlers, AZ_IOT_TEST_DEFAULT_INBOUND_HANDLERS);
  /* No workspace given: the entries are in the client's own one. */
  assert_true(
      (uint8_t*)client.pending_pubacks >= client.default_workspace
      && (uint8_t*)client.pending_pubacks
          < client.default_workspace + sizeof(client.default_workspace));
  az_iot_connection_client_deinit(&client);
}

static void entries_live_in_the_application_workspace_and_are_wiped(void** state)
{
  (void)state;
  static az_iot_connection_client client;
  az_iot_connection_client_options opts = direct_options();
  opts.limits.max_application_state_observers = 1;
  const size_t size = az_iot_connection_client_get_workspace_size(&opts.limits);
  assert_true(size > 0 && size < TEST_WORKSPACE_MAX);
  memset(g_workspace, 0xA5, sizeof(g_workspace));
  /* One byte in: the client aligns inside the workspace. */
  opts.workspace = g_workspace + 1;
  opts.workspace_size = size;
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
  for (size_t i = 1; i <= size; i++)
  {
    assert_int_equal(g_workspace[i], 0);
  }
  /* Nothing past it is written. */
  assert_int_equal(g_workspace[size + 1], 0xA5);
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

  /* More than the defaults, with no workspace. */
  opts = direct_options();
  opts.limits.max_pending_publishes = 64;
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_ERR_INVALID_ARG);

  /* A workspace one byte short. */
  opts.workspace = g_workspace;
  opts.workspace_size = az_iot_connection_client_get_workspace_size(&opts.limits) - 1;
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  assert_memory_equal(&client, untouched, sizeof(client));

  /* Fewer than the defaults fit the client's own workspace. */
  opts = direct_options();
  opts.limits.max_pending_publishes = 1;
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_OK);
  az_iot_connection_client_deinit(&client);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(sizes_follow_the_limits),
    cmocka_unit_test(zero_limits_resolve_to_the_defaults),
    cmocka_unit_test(entries_live_in_the_application_workspace_and_are_wiped),
    cmocka_unit_test(init_refuses_limits_it_cannot_hold),
  };
  return cmocka_run_group_tests_name("connection_client_workspace", tests, NULL, NULL);
}
