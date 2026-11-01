// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Construction-level smoke test for the Paho adapter. Does not touch the
 * network; verifies that a client the adapter builds is of the version asked
 * and carries a fully-populated iface vtable, and that the application
 * destroys a registered factory after the connection client's deinit(). */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#include <cmocka.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_connection_client.h"

#include "support/test_provider.h"

/* Paho sizes its own buffers: the limits only have to be valid. */
static const az_iot_mqtt_limits k_limits = { 1024, 1024, 4, 0, 0 };

static void adapter_builds_a_v3_client(void** state)
{
  (void)state;
  uint8_t workspace[AZ_IOT_PAHO_WORKSPACE_SIZE];
  assert_true(az_iot_paho_get_adapter()->get_workspace_size(&k_limits) <= sizeof(workspace));
  az_iot_mqtt_client* c = NULL;
  assert_int_equal(
      az_iot_paho_get_adapter()->init(
          AZ_IOT_MQTT_VERSION_3_1_1, &k_limits, workspace, sizeof(workspace), &c),
      AZ_IOT_OK);
  assert_non_null(c);
  assert_non_null(c->iface);
  assert_int_equal(c->iface->version, AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(c->iface->connect);
  assert_non_null(c->iface->disconnect);
  assert_non_null(c->iface->subscribe);
  assert_non_null(c->iface->unsubscribe);
  assert_non_null(c->iface->publish);
  assert_non_null(c->iface->process_loop);
  assert_non_null(c->iface->set_inbound_cb);
  assert_non_null(c->iface->destroy);

  /* Operations on a not-yet-connected client are rejected gracefully. */
  assert_int_equal(c->iface->disconnect(c), AZ_IOT_ERR_NOT_CONNECTED);
  /* process_loop on an empty queue is a no-op. */
  assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);

  c->iface->destroy(c);
}

static void adapter_builds_a_v5_client(void** state)
{
  (void)state;
  uint8_t workspace[AZ_IOT_PAHO_WORKSPACE_SIZE];
  az_iot_mqtt_client* c = NULL;
  assert_int_equal(
      az_iot_paho_get_adapter()->init(
          AZ_IOT_MQTT_VERSION_5, &k_limits, workspace, sizeof(workspace), &c),
      AZ_IOT_OK);
  assert_non_null(c);
  assert_int_equal(c->iface->version, AZ_IOT_MQTT_VERSION_5);
  c->iface->destroy(c);
}

/* A client in an application workspace: same vtable; destroy() wipes the state and frees
 * nothing it did not allocate (a free of the stack array would abort). */
static void adapter_runs_a_client_in_a_workspace(void** state)
{
  (void)state;
  const az_iot_mqtt_adapter* adapter = az_iot_paho_get_adapter();
  az_iot_mqtt_limits limits = { 1024, 1024, 4, 0, 0 };
  size_t const size = adapter->get_workspace_size(&limits);
  assert_true(size > 0 && size <= AZ_IOT_PAHO_WORKSPACE_SIZE);
  uint8_t workspace[AZ_IOT_PAHO_WORKSPACE_SIZE];
  az_iot_mqtt_client* c = NULL;

  assert_int_equal(
      AZ_IOT_ERR_NOT_ENOUGH_SPACE,
      adapter->init(AZ_IOT_MQTT_VERSION_5, &limits, workspace, size - 1, &c));
  assert_null(c);
  assert_int_equal(
      AZ_IOT_ERR_NOT_SUPPORTED,
      adapter->init((az_iot_mqtt_version)99, &limits, workspace, size, &c));
  limits.max_in_flight_messages = 0;
  assert_int_equal(0, adapter->get_workspace_size(&limits));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, adapter->init(AZ_IOT_MQTT_VERSION_5, &limits, workspace, size, &c));
  limits.max_in_flight_messages = 4;
  limits.message_store_size = limits.send_buffer_size; /* Under one packet plus overhead. */
  assert_int_equal(0, adapter->get_workspace_size(&limits));
  limits.message_store_size = 0;

  assert_int_equal(AZ_IOT_OK, adapter->init(AZ_IOT_MQTT_VERSION_5, &limits, workspace, size, &c));
  size_t const offset = (size_t)((uint8_t*)c - workspace);
  assert_true(offset < 16u);
  assert_int_equal(c->iface->version, AZ_IOT_MQTT_VERSION_5);
  assert_int_equal(c->iface->disconnect(c), AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
  c->iface->destroy(c);
  for (size_t i = offset; i < offset + size - 15u; i++)
  {
    assert_int_equal(workspace[i], 0);
  }
}

static bool limits_accepted(const az_iot_mqtt_limits* limits)
{
  return az_iot_paho_get_adapter()->get_workspace_size(limits) != 0;
}

/* The bounds every adapter shares (az_iot_mqtt__limits_are_valid): the largest value of each
 * field is accepted, the next one and SIZE_MAX are not. */
static void the_shared_limits_check_bounds_each_size(void** state)
{
  (void)state;
  az_iot_mqtt_limits const base = { 1024, 1024, 4, 0, 0 };
  az_iot_mqtt_limits l = base;
  assert_true(limits_accepted(&l));

  l.send_buffer_size = INT32_MAX;
  assert_true(limits_accepted(&l));
  l.send_buffer_size = (size_t)INT32_MAX + 1u;
  assert_false(limits_accepted(&l));
  l.send_buffer_size = SIZE_MAX;
  assert_false(limits_accepted(&l));

  l = base;
  l.receive_buffer_size = (size_t)INT32_MAX - 3u;
  assert_true(limits_accepted(&l));
  l.receive_buffer_size = (size_t)INT32_MAX - 2u;
  assert_false(limits_accepted(&l));
  l.receive_buffer_size = SIZE_MAX;
  assert_false(limits_accepted(&l));

  l = base;
  l.max_in_flight_messages = 32767;
  assert_true(limits_accepted(&l));
  l.max_in_flight_messages = 32768;
  assert_false(limits_accepted(&l));
  l.max_in_flight_messages = UINT16_MAX;
  assert_false(limits_accepted(&l));

  l = base;
  l.message_store_size = INT32_MAX;
  assert_true(limits_accepted(&l));
  l.message_store_size = (size_t)INT32_MAX + 1u;
  assert_false(limits_accepted(&l));
  l.message_store_size = SIZE_MAX;
  assert_false(limits_accepted(&l));
  l.message_store_size = l.send_buffer_size + 18u; /* One packet plus the per-message overhead. */
  assert_true(limits_accepted(&l));
  l.message_store_size = l.send_buffer_size + 17u;
  assert_false(limits_accepted(&l));

  l = base;
  l.connect_strings_size = INT32_MAX;
  assert_true(limits_accepted(&l));
  l.connect_strings_size = (size_t)INT32_MAX + 1u;
  assert_false(limits_accepted(&l));
  l.connect_strings_size = SIZE_MAX;
  assert_false(limits_accepted(&l));
}

/* Registration only lends the factories to the connection client: the
 * application destroys them after deinit(). Under valgrind/ASan, a double free
 * here means deinit() released them; a leak means nothing did. */
static void registered_factories_are_destroyed_by_the_application_after_deinit(void** state)
{
  (void)state;
  az_iot_connection_client client;
  az_iot_connection_client_options opts = { 0 };
  opts.iothub_hostname = "broker.example";
  opts.port = 8883;
  opts.device_id = "ut-device";
  assert_int_equal(az_iot_test_connection_client_init(&client, &opts), AZ_IOT_OK);

  az_iot_mqtt_factory* v3 = az_iot_paho_factory_create_v3_1_1();
  az_iot_mqtt_factory* v5 = az_iot_paho_factory_create_v5();
  assert_non_null(v3);
  assert_non_null(v5);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&client, v3), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&client, v5), AZ_IOT_OK);

  az_iot_test_connection_client_deinit(&client);
  az_iot_paho_factory_destroy(v3);
  az_iot_paho_factory_destroy(v5);
}

/* A factory whose registration failed is still the application's to destroy. */
static void a_factory_that_failed_to_register_is_destroyed_by_the_application(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_paho_factory_create_v5();
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(NULL, f), AZ_IOT_ERR_INVALID_ARG);
  az_iot_paho_factory_destroy(f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(adapter_builds_a_v3_client),
    cmocka_unit_test(adapter_builds_a_v5_client),
    cmocka_unit_test(adapter_runs_a_client_in_a_workspace),
    cmocka_unit_test(the_shared_limits_check_bounds_each_size),
    cmocka_unit_test(registered_factories_are_destroyed_by_the_application_after_deinit),
    cmocka_unit_test(a_factory_that_failed_to_register_is_destroyed_by_the_application),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
