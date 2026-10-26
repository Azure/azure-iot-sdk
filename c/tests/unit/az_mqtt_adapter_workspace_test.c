// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter in application memory: sizes, init in a workspace, connect strings copied there,
 * and the workspace wiped by destroy(). */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include <az_mqtt/az_mqtt_transport.h>

#define TEST_WORKSPACE_MAX (64 * 1024)

static uint8_t g_workspace[TEST_WORKSPACE_MAX + 1];

static az_iot_mqtt_limits small_limits(void)
{
  az_iot_mqtt_limits limits = {
    .send_buffer_size = 2048,
    .receive_buffer_size = 4096,
    .max_in_flight_messages = 4,
    .message_store_size = 0,
    .connect_strings_size = 512,
  };
  return limits;
}

static bool holds(void const* data, size_t size, char const* text)
{
  uint8_t const* bytes = (uint8_t const*)data;
  size_t const n = strlen(text);
  for (size_t i = 0; i + n <= size; i++)
  {
    if (memcmp(bytes + i, text, n) == 0)
    {
      return true;
    }
  }
  return false;
}

static void invalid_limits_have_no_size(void** state)
{
  (void)state;
  az_iot_mqtt_limits limits = small_limits();
  assert_true(az_iot_az_mqtt_get_workspace_size(&limits) > 0);
  assert_int_equal(az_iot_az_mqtt_get_workspace_size(NULL), 0);

  limits.send_buffer_size = 0;
  assert_int_equal(az_iot_az_mqtt_get_workspace_size(&limits), 0);
  limits = small_limits();
  limits.receive_buffer_size = 0;
  assert_int_equal(az_iot_az_mqtt_get_workspace_size(&limits), 0);
  limits = small_limits();
  limits.max_in_flight_messages = 0;
  assert_int_equal(az_iot_az_mqtt_get_workspace_size(&limits), 0);
  limits.max_in_flight_messages = 32768;
  assert_int_equal(az_iot_az_mqtt_get_workspace_size(&limits), 0);
  limits = small_limits();
  limits.message_store_size = limits.send_buffer_size; /* Smaller than one packet + overhead. */
  assert_int_equal(az_iot_az_mqtt_get_workspace_size(&limits), 0);
}

static void the_macro_covers_the_function(void** state)
{
  (void)state;
  size_t const tls_state = (size_t)az_mqtt_transport_sizeof();
  az_iot_mqtt_limits limits = small_limits();
  assert_true(
      AZ_IOT_AZ_MQTT_WORKSPACE_SIZE(2048, 4096, 4, 0, 512, tls_state)
      >= az_iot_az_mqtt_get_workspace_size(&limits));

  limits.send_buffer_size = 8 * 1024 + 1;
  limits.receive_buffer_size = 64 * 1024 + 256;
  limits.max_in_flight_messages = 64;
  limits.message_store_size = 8 * 1024 + 1 + 18;
  limits.connect_strings_size = 4 * 1024;
  assert_true(
      AZ_IOT_AZ_MQTT_WORKSPACE_SIZE(
          8 * 1024 + 1, 64 * 1024 + 256, 64, 8 * 1024 + 19, 4 * 1024, tls_state)
      >= az_iot_az_mqtt_get_workspace_size(&limits));
}

static void init_needs_the_whole_workspace_at_any_alignment(void** state)
{
  (void)state;
  az_iot_mqtt_limits const limits = small_limits();
  size_t const size = az_iot_az_mqtt_get_workspace_size(&limits);
  assert_true(size <= TEST_WORKSPACE_MAX);
  az_iot_mqtt_adapter const* adapter = az_iot_az_mqtt_get_adapter();
  az_iot_mqtt_version const versions[] = { AZ_IOT_MQTT_VERSION_3_1_1, AZ_IOT_MQTT_VERSION_5 };
  for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
  {
    az_iot_mqtt_client* client = NULL;
    if (versions[i] == AZ_IOT_MQTT_VERSION_5) /* The largest client: exactly the size. */
    {
      assert_int_equal(
          adapter->init(versions[i], &limits, g_workspace + 1, size - 1, &client),
          AZ_IOT_ERR_NOT_ENOUGH_SPACE);
      assert_null(client);
    }
    /* Start one byte in: the adapter aligns inside the workspace. */
    assert_int_equal(
        adapter->init(versions[i], &limits, g_workspace + 1, size, &client), AZ_IOT_OK);
    assert_non_null(client);
    assert_non_null(client->iface);
    assert_true((uint8_t*)client >= g_workspace + 1 && (uint8_t*)client < g_workspace + 1 + size);
    client->iface->destroy(client);
  }
}

static void init_rejects_bad_arguments(void** state)
{
  (void)state;
  az_iot_mqtt_limits const limits = small_limits();
  az_iot_mqtt_limits bad = limits;
  bad.max_in_flight_messages = 0;
  az_iot_mqtt_adapter const* adapter = az_iot_az_mqtt_get_adapter();
  az_iot_mqtt_client* client = NULL;
  assert_int_equal(
      adapter->init(AZ_IOT_MQTT_VERSION_5, &limits, g_workspace, sizeof(g_workspace), NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      adapter->init(AZ_IOT_MQTT_VERSION_5, NULL, g_workspace, sizeof(g_workspace), &client),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      adapter->init(AZ_IOT_MQTT_VERSION_5, &bad, g_workspace, sizeof(g_workspace), &client),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      adapter->init(AZ_IOT_MQTT_VERSION_5, &limits, NULL, sizeof(g_workspace), &client),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      adapter->init((az_iot_mqtt_version)99, &limits, g_workspace, sizeof(g_workspace), &client),
      AZ_IOT_ERR_NOT_SUPPORTED);
}

static az_iot_result connect_with(az_iot_mqtt_client* client, char const* password, bool clean)
{
  az_iot_mqtt_connect_options options = { 0 };
  options.host = "h";
  options.client_id = "c";
  options.username = "u";
  options.password = password;
  options.clean_start = clean;
  options.session_expiry_seconds = clean ? 0 : 60;
  return client->iface->connect(client, &options);
}

static void one_workspace_serves_each_version_in_turn_and_is_wiped(void** state)
{
  (void)state;
  az_iot_mqtt_limits const limits = small_limits();
  size_t const size = az_iot_az_mqtt_get_workspace_size(&limits);
  az_iot_mqtt_adapter const* adapter = az_iot_az_mqtt_get_adapter();
  az_iot_mqtt_version const versions[] = { AZ_IOT_MQTT_VERSION_3_1_1, AZ_IOT_MQTT_VERSION_5 };
  for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
  {
    az_iot_mqtt_client* client = NULL;
    assert_int_equal(adapter->init(versions[i], &limits, g_workspace, size, &client), AZ_IOT_OK);
    assert_int_equal(connect_with(client, "WORKSPACE-SECRET", true), AZ_IOT_OK);
    assert_true(holds(g_workspace, size, "WORKSPACE-SECRET")); /* Copied into the workspace. */
    (void)client->iface->disconnect(client);
    client->iface->destroy(client);

    /* Nothing of the client is left: the application gets a clean workspace back. */
    for (size_t b = 0; b < size; b++)
    {
      assert_int_equal(g_workspace[b], 0);
    }
  }
}

static void connect_strings_that_do_not_fit_are_refused(void** state)
{
  (void)state;
  az_iot_mqtt_limits limits = small_limits();
  limits.connect_strings_size = 9; /* "h", "c", "u", "pw" and their NULs. */
  size_t const size = az_iot_az_mqtt_get_workspace_size(&limits);
  az_iot_mqtt_client* client = NULL;
  assert_int_equal(
      az_iot_az_mqtt_get_adapter()->init(
          AZ_IOT_MQTT_VERSION_5, &limits, g_workspace, size, &client),
      AZ_IOT_OK);
  assert_int_equal(connect_with(client, "a-longer-password", true), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(connect_with(client, "pw", true), AZ_IOT_OK);
  (void)client->iface->disconnect(client);
  client->iface->destroy(client);
}

static void a_session_that_outlives_the_connection_needs_a_message_store(void** state)
{
  (void)state;
  az_iot_mqtt_limits limits = small_limits();
  size_t size = az_iot_az_mqtt_get_workspace_size(&limits);
  az_iot_mqtt_adapter const* adapter = az_iot_az_mqtt_get_adapter();
  az_iot_mqtt_client* client = NULL;
  assert_int_equal(
      adapter->init(AZ_IOT_MQTT_VERSION_5, &limits, g_workspace, size, &client), AZ_IOT_OK);
  assert_int_equal(connect_with(client, "pw", false), AZ_IOT_ERR_NOT_SUPPORTED);
  client->iface->destroy(client);

  limits.message_store_size = limits.send_buffer_size + 18;
  size = az_iot_az_mqtt_get_workspace_size(&limits);
  assert_int_equal(
      adapter->init(AZ_IOT_MQTT_VERSION_5, &limits, g_workspace, size, &client), AZ_IOT_OK);
  assert_int_equal(connect_with(client, "pw", false), AZ_IOT_OK);
  (void)client->iface->disconnect(client);
  client->iface->destroy(client);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(invalid_limits_have_no_size),
    cmocka_unit_test(the_macro_covers_the_function),
    cmocka_unit_test(init_needs_the_whole_workspace_at_any_alignment),
    cmocka_unit_test(init_rejects_bad_arguments),
    cmocka_unit_test(one_workspace_serves_each_version_in_turn_and_is_wiped),
    cmocka_unit_test(connect_strings_that_do_not_fit_are_refused),
    cmocka_unit_test(a_session_that_outlives_the_connection_needs_a_message_store),
  };
  return cmocka_run_group_tests_name("az_mqtt_adapter_workspace", tests, NULL, NULL);
}
