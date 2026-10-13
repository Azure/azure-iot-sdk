// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Construction-level smoke test for the Paho adapter. Does not touch the
 * network; verifies that the factory advertises the right version and that a
 * created client carries a fully-populated iface vtable, and that the
 * application destroys a registered factory after the connection client's
 * deinit(). */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_connection_client.h"

#include "support/test_provider.h"

static void v3_factory_creates_v3_client(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_paho_factory_create_v3_1_1();
  assert_non_null(f);
  assert_int_equal(f->version, AZ_IOT_MQTT_VERSION_3_1_1);

  az_iot_mqtt_client* c = f->create(f->factory_ctx);
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
  az_iot_paho_factory_destroy(f);
}

static void v5_factory_creates_v5_client(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_paho_factory_create_v5();
  assert_non_null(f);
  assert_int_equal(f->version, AZ_IOT_MQTT_VERSION_5);

  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  assert_int_equal(c->iface->version, AZ_IOT_MQTT_VERSION_5);
  c->iface->destroy(c);
  az_iot_paho_factory_destroy(f);
}

/* Registration only lends the factories to the connection client: the
 * application destroys them after deinit(). Under valgrind/ASan, a double free
 * here means deinit() released them; a leak means nothing did. */
static void registered_factories_are_destroyed_by_the_application_after_deinit(void** state)
{
  (void)state;
  az_iot_connection_client client;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  assert_int_equal(az_iot_test_connection_client_init(&client, &opts), AZ_IOT_OK);

  az_iot_mqtt_factory* v3 = az_iot_paho_factory_create_v3_1_1();
  az_iot_mqtt_factory* v5 = az_iot_paho_factory_create_v5();
  assert_non_null(v3);
  assert_non_null(v5);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&client, v3), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&client, v5), AZ_IOT_OK);

  az_iot_connection_client_deinit(&client);
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
    cmocka_unit_test(v3_factory_creates_v3_client),
    cmocka_unit_test(v5_factory_creates_v5_client),
    cmocka_unit_test(registered_factories_are_destroyed_by_the_application_after_deinit),
    cmocka_unit_test(a_factory_that_failed_to_register_is_destroyed_by_the_application),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
