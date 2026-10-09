// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter with static clients (az_mqtt_static_config.h): a shared factory, a fixed number
 * of clients, connect strings that must fit, and no session that outlives the connection.
 * AZ_MQTT_STATIC_TINY_TRANSPORT: create() refuses a transport area that is too small. */
#include "az_mqtt_static_config.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "az_iot_az_mqtt_config.h"
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_mqtt_iface.h"

typedef az_iot_mqtt_factory* (*factory_fn)(void);
static factory_fn const k_factories[]
    = { az_iot_az_mqtt_factory_create_v3_1_1, az_iot_az_mqtt_factory_create_v5 };
#define FACTORIES (sizeof(k_factories) / sizeof(k_factories[0]))

#ifdef AZ_MQTT_STATIC_TINY_TRANSPORT

static void create_refuses_a_transport_area_too_small(void** state)
{
  (void)state;
  for (size_t i = 0; i < FACTORIES; i++)
  {
    az_iot_mqtt_factory* f = k_factories[i]();
    assert_non_null(f);
    assert_null(f->create(f->factory_ctx));
    az_iot_az_mqtt_factory_destroy(f);
  }
}

#else

static void the_factory_is_shared_and_destroy_frees_nothing(void** state)
{
  (void)state;
  for (size_t i = 0; i < FACTORIES; i++)
  {
    az_iot_mqtt_factory* a = k_factories[i]();
    az_iot_mqtt_factory* b = k_factories[i]();
    assert_non_null(a);
    assert_ptr_equal(a, b);
    az_iot_az_mqtt_factory_destroy(a);
    az_iot_az_mqtt_factory_destroy(b);
    assert_ptr_equal(k_factories[i](), a);
  }
}

static void clients_are_limited_and_reused(void** state)
{
  (void)state;
  for (size_t i = 0; i < FACTORIES; i++)
  {
    az_iot_mqtt_factory* f = k_factories[i]();
    az_iot_mqtt_client* c[AZ_IOT_AZ_MQTT_STATIC_CLIENTS];
    for (size_t k = 0; k < AZ_IOT_AZ_MQTT_STATIC_CLIENTS; k++)
    {
      c[k] = f->create(f->factory_ctx);
      assert_non_null(c[k]);
    }
    assert_null(f->create(f->factory_ctx));
    c[0]->iface->destroy(c[0]);
    c[0] = f->create(f->factory_ctx);
    assert_non_null(c[0]);
    for (size_t k = 0; k < AZ_IOT_AZ_MQTT_STATIC_CLIENTS; k++)
    {
      c[k]->iface->destroy(c[k]);
    }
    az_iot_az_mqtt_factory_destroy(f);
  }
}

/** @brief connect() with a host of @p host_len characters and client_id "c": copies of
 * host_len + 1 and 2 bytes. */
static az_iot_result connect_with_host(az_iot_mqtt_client* c, size_t host_len)
{
  static char host[AZ_IOT_AZ_MQTT_CONNECT_STRINGS_SIZE + 1];
  memset(host, 'h', host_len);
  host[host_len] = '\0';
  az_iot_mqtt_connect_options o = { 0 };
  o.host = host;
  o.client_id = "c";
  o.clean_start = true;
  return c->iface->connect(c, &o);
}

static void connect_strings_must_fit(void** state)
{
  (void)state;
  size_t const fits = AZ_IOT_AZ_MQTT_CONNECT_STRINGS_SIZE - 3; // + NUL + "c" + NUL.
  for (size_t i = 0; i < FACTORIES; i++)
  {
    az_iot_mqtt_factory* f = k_factories[i]();
    az_iot_mqtt_client* c = f->create(f->factory_ctx);
    assert_non_null(c);
    assert_int_equal(connect_with_host(c, fits + 1), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    assert_int_equal(connect_with_host(c, fits), AZ_IOT_OK);
    (void)c->iface->disconnect(c);
    // Released by each connect: the same strings fit again.
    assert_int_equal(connect_with_host(c, fits), AZ_IOT_OK);
    (void)c->iface->disconnect(c);
    c->iface->destroy(c);
    az_iot_az_mqtt_factory_destroy(f);
  }
}

static void a_session_outliving_the_connection_needs_a_message_store(void** state)
{
  (void)state;
  for (size_t i = 0; i < FACTORIES; i++)
  {
    az_iot_mqtt_factory* f = k_factories[i]();
    az_iot_mqtt_client* c = f->create(f->factory_ctx);
    assert_non_null(c);
    az_iot_mqtt_connect_options o = { 0 };
    o.host = "broker.example";
    o.client_id = "c";
    o.clean_start = false;
    o.session_expiry_seconds = 60;
    assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_NOT_SUPPORTED);
    c->iface->destroy(c);
    az_iot_az_mqtt_factory_destroy(f);
  }
}

#endif

int main(void)
{
  const struct CMUnitTest tests[] = {
#ifdef AZ_MQTT_STATIC_TINY_TRANSPORT
    cmocka_unit_test(create_refuses_a_transport_area_too_small),
#else
    cmocka_unit_test(the_factory_is_shared_and_destroy_frees_nothing),
    cmocka_unit_test(clients_are_limited_and_reused),
    cmocka_unit_test(connect_strings_must_fit),
    cmocka_unit_test(a_session_outliving_the_connection_needs_a_message_store),
#endif
  };
  return cmocka_run_group_tests_name("az_mqtt_adapter_static", tests, NULL, NULL);
}
