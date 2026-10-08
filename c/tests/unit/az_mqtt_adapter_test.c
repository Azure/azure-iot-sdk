// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter, without a network: factories and clients, ownership of a registered factory,
 * the arguments connect(), publish() and subscribe() refuse, and a CONNECT that cannot be encoded.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

static void check_client(az_iot_mqtt_factory* f, az_iot_mqtt_version version)
{
  assert_non_null(f);
  assert_int_equal(f->version, version);
  assert_non_null(f->destroy);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  assert_int_equal(c->iface->version, version);
  assert_int_equal(c->iface->disconnect(c), AZ_IOT_ERR_NOT_CONNECTED);
  az_iot_mqtt_message msg = { 0 };
  msg.topic = "t";
  assert_int_equal(c->iface->publish(c, &msg, NULL), AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
  c->iface->destroy(c);
}

static void factories_create_clients_of_their_version(void** state)
{
  (void)state;
  az_iot_mqtt_factory* v3 = az_iot_az_mqtt_factory_create_v3_1_1();
  check_client(v3, AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_az_mqtt_factory_destroy(v3);
  az_iot_mqtt_factory* v5 = az_iot_az_mqtt_factory_create_v5();
  check_client(v5, AZ_IOT_MQTT_VERSION_5);
  az_iot_az_mqtt_factory_destroy(v5);
  az_iot_az_mqtt_factory_destroy(NULL);
}

static void a_registered_factory_is_freed_by_deinit(void** state)
{
  (void)state;
  az_iot_connection_client conn;
  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  options.host = "hub.example";
  options.client_id = "az-mqtt-adapter-test";
  assert_int_equal(az_iot_connection_client_init(&conn, &options), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&conn, az_iot_az_mqtt_factory_create_v5()),
      AZ_IOT_OK);
  az_iot_connection_client_deinit(&conn); // Under a leak checker: nothing left.
}

static az_iot_mqtt_connect_options valid_connect(void)
{
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "broker.example";
  o.client_id = "c";
  return o;
}

static void connect_refuses_malformed_pointer_count_pairs(void** state)
{
  (void)state;
  az_iot_mqtt_factory* fs[]
      = { az_iot_az_mqtt_factory_create_v3_1_1(), az_iot_az_mqtt_factory_create_v5() };
  for (size_t i = 0; i < 2; i++)
  {
    az_iot_mqtt_client* c = fs[i]->create(fs[i]->factory_ctx);
    assert_non_null(c);
    az_iot_mqtt_connect_options o = valid_connect();
    o.lwt.topic = "will";
    o.lwt.payload_len = 4; // No payload.
    assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
    o = valid_connect();
    o.user_properties_count = 1; // No properties.
    assert_int_equal(
        c->iface->connect(c, &o),
        fs[i]->version == AZ_IOT_MQTT_VERSION_5 ? AZ_IOT_ERR_INVALID_ARG : AZ_IOT_OK);
    if (fs[i]->version == AZ_IOT_MQTT_VERSION_3_1_1)
    {
      (void)c->iface->disconnect(c); // MQTT 3.1.1 ignores user properties: it started.
    }
    c->iface->destroy(c);
    az_iot_az_mqtt_factory_destroy(fs[i]);
  }
}

static void invalid_qos_and_v3_password_without_user_are_refused(void** state)
{
  (void)state;
  az_iot_mqtt_factory* fs[]
      = { az_iot_az_mqtt_factory_create_v3_1_1(), az_iot_az_mqtt_factory_create_v5() };
  for (size_t i = 0; i < 2; i++)
  {
    az_iot_mqtt_client* c = fs[i]->create(fs[i]->factory_ctx);
    assert_non_null(c);
    az_iot_mqtt_message msg = { 0 };
    msg.topic = "t";
    msg.qos = (az_iot_mqtt_qos)3;
    assert_int_equal(c->iface->publish(c, &msg, NULL), AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(c->iface->subscribe(c, "t", (az_iot_mqtt_qos)3, NULL), AZ_IOT_ERR_INVALID_ARG);
    az_iot_mqtt_connect_options o = valid_connect();
    o.lwt.topic = "will";
    o.lwt.qos = (az_iot_mqtt_qos)3;
    assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
    o = valid_connect();
    o.password = "p";
    bool const v5 = fs[i]->version == AZ_IOT_MQTT_VERSION_5;
    assert_int_equal(c->iface->connect(c, &o), v5 ? AZ_IOT_OK : AZ_IOT_ERR_INVALID_ARG);
    if (v5)
    {
      (void)c->iface->disconnect(c);
    }
    c->iface->destroy(c);
    az_iot_az_mqtt_factory_destroy(fs[i]);
  }
}

typedef struct
{
  int connected_events;
  az_iot_result status;
} connect_result;

static void record_connect(const az_iot_mqtt_event* evt, void* ctx)
{
  connect_result* r = (connect_result*)ctx;
  if (evt->kind == AZ_IOT_MQTT_EVT_CONNECTED)
  {
    r->connected_events++;
    r->status = evt->status;
  }
}

/** @brief connect() accepts @p o; the first process_loop() reports @p expected; then a new
 * connect() is accepted. */
static void connect_fails_before_any_io(az_iot_mqtt_connect_options o, az_iot_result expected)
{
  az_iot_mqtt_factory* fs[]
      = { az_iot_az_mqtt_factory_create_v3_1_1(), az_iot_az_mqtt_factory_create_v5() };
  for (size_t i = 0; i < 2; i++)
  {
    az_iot_mqtt_client* c = fs[i]->create(fs[i]->factory_ctx);
    assert_non_null(c);
    connect_result r = { 0 };
    c->iface->set_inbound_cb(c, record_connect, &r);
    assert_int_equal(c->iface->connect(c, &o), AZ_IOT_OK);
    assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
    assert_int_equal(r.connected_events, 1);
    assert_int_equal(r.status, expected);
    // The attempt is over: a new one is accepted.
    az_iot_mqtt_connect_options const next = valid_connect();
    assert_int_equal(c->iface->connect(c, &next), AZ_IOT_OK);
    (void)c->iface->disconnect(c);
    c->iface->set_inbound_cb(c, NULL, NULL);
    c->iface->destroy(c);
    az_iot_az_mqtt_factory_destroy(fs[i]);
  }
}

static void a_connect_larger_than_the_send_buffer_fails_the_attempt(void** state)
{
  (void)state;
  // Five fields of 65,000 bytes (each under the MQTT limit): more than the send buffer.
  static char text[65001];
  memset(text, 'a', sizeof(text) - 1);
  assert_true(5u * (sizeof(text) - 1) > AZ_IOT_AZ_MQTT_BUFFER_SIZE);
  az_iot_mqtt_connect_options o = valid_connect();
  o.client_id = text;
  o.username = text;
  o.password = text;
  o.lwt.topic = text;
  o.lwt.payload = (const uint8_t*)text;
  o.lwt.payload_len = sizeof(text) - 1;
  connect_fails_before_any_io(o, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void a_connect_the_encoder_refuses_fails_the_attempt(void** state)
{
  (void)state;
  static uint8_t will[65536]; // MQTT binary data: at most 65,535 bytes.
  az_iot_mqtt_connect_options o = valid_connect();
  o.lwt.topic = "will";
  o.lwt.payload = will;
  o.lwt.payload_len = sizeof(will);
  connect_fails_before_any_io(o, AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(factories_create_clients_of_their_version),
    cmocka_unit_test(a_registered_factory_is_freed_by_deinit),
    cmocka_unit_test(connect_refuses_malformed_pointer_count_pairs),
    cmocka_unit_test(invalid_qos_and_v3_password_without_user_are_refused),
    cmocka_unit_test(a_connect_larger_than_the_send_buffer_fails_the_attempt),
    cmocka_unit_test(a_connect_the_encoder_refuses_fails_the_attempt),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
