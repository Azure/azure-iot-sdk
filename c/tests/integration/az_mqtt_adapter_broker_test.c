// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter against a broker (AZ_IOT_MQTT_BROKER_HOST / _PORT): process_loop() returns when
 * an event callback raises more events. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_mqtt_iface.h"

typedef struct
{
  az_iot_mqtt_client* client;
  int connected;
  int acks;
} recorder;

/* Each publish acknowledgement publishes again (QoS 0: acknowledged by the next process_loop()). */
static void republish_on_ack(const az_iot_mqtt_event* evt, void* ctx)
{
  recorder* r = (recorder*)ctx;
  if (evt->kind == AZ_IOT_MQTT_EVT_CONNECTED && evt->status == AZ_IOT_OK)
  {
    r->connected++;
  }
  else if (evt->kind == AZ_IOT_MQTT_EVT_PUBLISH_ACK)
  {
    r->acks++;
    az_iot_mqtt_message msg = { 0 };
    msg.topic = "az-iot/az-mqtt-adapter-test";
    msg.payload = (const uint8_t*)"x";
    msg.payload_len = 1;
    assert_int_equal(r->client->iface->publish(r->client, &msg, NULL), AZ_IOT_OK);
  }
}

static void events_raised_by_a_callback_wait_for_the_next_process_loop(az_iot_mqtt_factory* f)
{
  const char* host = getenv("AZ_IOT_MQTT_BROKER_HOST");
  const char* port = getenv("AZ_IOT_MQTT_BROKER_PORT");
  recorder r = { 0 };
  r.client = f->create(f->factory_ctx);
  assert_non_null(r.client);
  r.client->iface->set_inbound_cb(r.client, republish_on_ack, &r);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = host != NULL ? host : "localhost";
  o.port = (uint16_t)(port != NULL ? atoi(port) : 1883);
  o.client_id
      = f->version == AZ_IOT_MQTT_VERSION_5 ? "az-iot-az-mqtt-drain-5" : "az-iot-az-mqtt-drain-3";
  o.clean_start = true;
  o.connect_timeout_seconds = 10;
  assert_int_equal(r.client->iface->connect(r.client, &o), AZ_IOT_OK);
  for (int i = 0; i < 200 && r.connected == 0; i++)
  {
    assert_int_equal(r.client->iface->process_loop(r.client, 50), AZ_IOT_OK);
  }
  assert_int_equal(r.connected, 1);

  az_iot_mqtt_message msg = { 0 };
  msg.topic = "az-iot/az-mqtt-adapter-test";
  assert_int_equal(r.client->iface->publish(r.client, &msg, NULL), AZ_IOT_OK);
  // Each callback publishes again. Every call must still return, with acknowledgements delivered
  // (at most two per call: the one held on entry, and one after the receive step).
  for (int i = 0; i < 10; i++)
  {
    int const before = r.acks;
    assert_int_equal(r.client->iface->process_loop(r.client, 0), AZ_IOT_OK);
    assert_in_range(r.acks - before, 1, 2);
  }
  r.client->iface->set_inbound_cb(r.client, NULL, NULL);
  (void)r.client->iface->disconnect(r.client);
  r.client->iface->destroy(r.client);
  az_iot_az_mqtt_factory_destroy(f);
}

static void v3(void** state)
{
  (void)state;
  events_raised_by_a_callback_wait_for_the_next_process_loop(
      az_iot_az_mqtt_factory_create_v3_1_1());
}

static void v5(void** state)
{
  (void)state;
  events_raised_by_a_callback_wait_for_the_next_process_loop(az_iot_az_mqtt_factory_create_v5());
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(v3),
    cmocka_unit_test(v5),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
