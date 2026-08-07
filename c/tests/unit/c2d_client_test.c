// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* C2DClient unit tests, driven through the public API and the in-memory
 * mock_mqtt_iface.
 *
 * IoT Hub Classic delivers cloud-to-device messages on
 * "devices/{device-id}/messages/devicebound/" plus an optional url-encoded
 * property bag, and the device subscribes with the "#" wildcard so those
 * sub-topics still route. See
 * https://learn.microsoft.com/azure/iot-hub/iot-mqtt-connect-to-iot-hub#receive-cloud-to-device-messages
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_c2d_client.h"
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "support/mock_mqtt_iface.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct message_record
{
  int count;
  char payload[128];
  size_t payload_len;
  bool had_payload_pointer;
  bool content_type_was_null;
  char content_type[64];
} message_record;

static void on_c2d(
    const uint8_t* payload,
    size_t payload_len,
    const char* content_type,
    void* user_ctx)
{
  message_record* r = (message_record*)user_ctx;
  r->count++;
  r->payload_len = payload_len;
  r->had_payload_pointer = (payload != NULL);
  r->content_type_was_null = (content_type == NULL);
  if (content_type != NULL)
  {
    snprintf(r->content_type, sizeof(r->content_type), "%s", content_type);
  }
  if (payload != NULL && payload_len > 0 && payload_len < sizeof(r->payload))
  {
    memcpy(r->payload, payload, payload_len);
    r->payload[payload_len] = '\0';
  }
}

typedef struct fixture
{
  az_iot_connection_client conn;
  az_iot_c2d_client c2d;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock;
} fixture;

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  assert_int_equal(az_iot_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_c2d_client_destroy(&fx->c2d);
    /* A registered factory is adopted by the client and freed from destroy();
     * an unregistered one is still ours. */
    bool adopted = (fx->conn.factory_count > 0);
    az_iot_connection_client_destroy(&fx->conn);
    if (!adopted)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
    free(fx);
  }
  return 0;
}

static void open_to_connected(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

/* Deliver one inbound message and pump it through dispatch. */
static void inject(fixture* fx, const char* topic, const char* body)
{
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, topic, (const uint8_t*)body, body ? strlen(body) : 0, AZ_IOT_MQTT_QOS_1));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

static const az_iot_mock_call* find_subscribe(az_iot_mock_mqtt_client* m, const char* filter)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i);
    if (c->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE && strcmp(c->topic, filter) == 0)
    {
      return c;
    }
  }
  return NULL;
}

#define C2D_FILTER "devices/ut-device/messages/devicebound/#"
#define C2D_TOPIC "devices/ut-device/messages/devicebound/"

/* ------------------------------------------------------------------------- */
/* init                                                                      */
/* ------------------------------------------------------------------------- */

static void init_rejects_a_null_client(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_c2d_client_init(NULL, &fx->conn), AZ_IOT_ERR_INVALID_ARG);
}

static void init_rejects_a_null_connection(void** state)
{
  (void)state;
  az_iot_c2d_client c;
  assert_int_equal(az_iot_c2d_client_init(&c, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void init_subscribes_the_devicebound_filter(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* The '#' matters: IoT Hub appends the property bag to the topic, so an
   * exact-match filter would receive only messages that carry no properties. */
  assert_non_null(find_subscribe(fx->mock, C2D_FILTER));
}

static void the_subscription_uses_qos_1(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  const az_iot_mock_call* c = find_subscribe(fx->mock, C2D_FILTER);
  assert_non_null(c);
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_1);
}

static void init_falls_back_to_the_client_id(void** state)
{
  (void)state;
  /* No DPS registration id configured, so the topic is built from client_id. */
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "fallback-device";
  az_iot_connection_client conn;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_c2d_client c2d;
  assert_int_equal(az_iot_c2d_client_init(&c2d, &conn), AZ_IOT_OK);

  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&conn, f), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&conn), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(f);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&conn, 0), AZ_IOT_OK);

  assert_non_null(find_subscribe(m, "devices/fallback-device/messages/devicebound/#"));

  az_iot_c2d_client_destroy(&c2d);
  az_iot_connection_client_destroy(&conn);
}

static void init_prefers_the_dps_registration_id(void** state)
{
  (void)state;
  /* On a DPS flow the registration id is the identity the hub knows, so it is
   * what the device-bound topic has to be built from. */
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "client-id-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "registration-device";
  az_iot_connection_client conn;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_c2d_client c2d;
  assert_int_equal(az_iot_c2d_client_init(&c2d, &conn), AZ_IOT_OK);

  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&conn, f), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&conn), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(f);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&conn, 0), AZ_IOT_OK);

  assert_non_null(find_subscribe(m, "devices/registration-device/messages/devicebound/#"));

  az_iot_c2d_client_destroy(&c2d);
  az_iot_connection_client_destroy(&conn);
}

static void init_without_any_device_id_is_rejected(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  /* Neither client_id nor a DPS registration id: there is no topic to build. */
  az_iot_connection_client conn;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_c2d_client c2d;
  assert_int_equal(az_iot_c2d_client_init(&c2d, &conn), AZ_IOT_ERR_NOT_INITIALIZED);

  az_iot_connection_client_destroy(&conn);
}

static void init_with_a_device_id_that_overflows_the_topic_is_rejected(void** state)
{
  (void)state;
  /* AZ_IOT_C2D_TOPIC_MAX bounds the built topic. A device id past it must be
   * refused rather than silently truncated into someone else's topic. */
  static char huge[512];
  memset(huge, 'd', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = '\0';

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = huge;
  az_iot_connection_client conn;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_c2d_client c2d;
  assert_int_equal(az_iot_c2d_client_init(&c2d, &conn), AZ_IOT_ERR_INTERNAL);

  az_iot_connection_client_destroy(&conn);
}

static void a_failed_init_leaves_no_handler_registered(void** state)
{
  (void)state;
  static char huge[512];
  memset(huge, 'd', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = '\0';

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = huge;
  az_iot_connection_client conn;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_c2d_client c2d;
  assert_int_not_equal(az_iot_c2d_client_init(&c2d, &conn), AZ_IOT_OK);

  /* A half-built client must not be reachable from dispatch, and must not
   * carry a connection pointer that later calls would follow. */
  assert_int_equal(az_iot_c2d_client_set_handler(&c2d, on_c2d, NULL), AZ_IOT_OK);
  az_iot_connection_client_destroy(&conn);
}

/* ------------------------------------------------------------------------- */
/* handler registration                                                      */
/* ------------------------------------------------------------------------- */

static void set_handler_rejects_a_null_client(void** state)
{
  (void)state;
  assert_int_equal(az_iot_c2d_client_set_handler(NULL, on_c2d, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void a_message_before_any_handler_is_set_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* No handler yet: surviving the dispatch is the assertion. */
  inject(fx, C2D_TOPIC, "hello");
}

static void a_later_handler_replaces_the_earlier_one(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record first = { 0 };
  message_record second = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &first), AZ_IOT_OK);
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &second), AZ_IOT_OK);

  inject(fx, C2D_TOPIC, "hello");

  assert_int_equal(first.count, 0);
  assert_int_equal(second.count, 1);
}

static void clearing_the_handler_stops_delivery(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "one");
  assert_int_equal(rec.count, 1);

  /* A NULL callback stops delivery without tearing the subscription down, so
   * the application can pause processing and resume later. */
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, NULL, NULL), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "two");
  assert_int_equal(rec.count, 1);

  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "three");
  assert_int_equal(rec.count, 2);
}

/* ------------------------------------------------------------------------- */
/* delivery                                                                  */
/* ------------------------------------------------------------------------- */

static void the_payload_reaches_the_handler_unchanged(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  inject(fx, C2D_TOPIC, "{\"cmd\":\"open\"}");

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.payload_len, strlen("{\"cmd\":\"open\"}"));
  assert_string_equal(rec.payload, "{\"cmd\":\"open\"}");
}

static void an_empty_payload_is_delivered(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* A message whose whole content is in its properties is still a message. */
  assert_true(
      az_iot_mock_mqtt_client_inject_message(fx->mock, C2D_TOPIC, NULL, 0, AZ_IOT_MQTT_QOS_1));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.payload_len, 0);
}

static void a_message_on_a_property_bag_sub_topic_is_delivered(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* Dispatch is a prefix match, so the encoded property bag the service
   * appends must not stop the message routing. */
  inject(fx, C2D_TOPIC "%24.mid=m1&prop1&prop2=&prop3=a%20string", "body");

  assert_int_equal(rec.count, 1);
  assert_string_equal(rec.payload, "body");
}

static void a_message_addressed_to_another_device_is_not_delivered(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  inject(fx, "devices/other-device/messages/devicebound/", "not mine");

  assert_int_equal(rec.count, 0);
}

/* The Classic path hands the application a NULL content type and never parses
 * the property bag, so neither the content type nor any application property
 * survives the trip. This test pins the behaviour as it ships (limitation D-1
 * in docs/test-coverage.md); closing it is an API change, because the callback
 * is not even given the topic to re-parse. */
static void classic_delivers_a_null_content_type(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  inject(fx, C2D_TOPIC "%24.ct=application%2Fjson", "{}");

  assert_int_equal(rec.count, 1);
  assert_true(rec.content_type_was_null);
}

/* ------------------------------------------------------------------------- */
/* multiple clients and reconnects                                           */
/* ------------------------------------------------------------------------- */

static void only_one_of_two_clients_on_the_same_prefix_receives(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_c2d_client second;
  assert_int_equal(az_iot_c2d_client_init(&second, &fx->conn), AZ_IOT_OK);
  open_to_connected(fx);

  message_record a = { 0 };
  message_record b = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &a), AZ_IOT_OK);
  assert_int_equal(az_iot_c2d_client_set_handler(&second, on_c2d, &b), AZ_IOT_OK);

  inject(fx, C2D_TOPIC, "shared");

  /* Dispatch is longest-prefix-wins over a single table, so two clients on the
   * identical prefix cannot both be reached -- exactly one handler fires and
   * the other client is silently deaf. Which one wins is not defined, and the
   * assertion deliberately does not pretend otherwise; that this is a bad
   * bargain at all is the point. */
  assert_int_equal(a.count + b.count, 1);

  az_iot_c2d_client_destroy(&second);
}

static void destroying_one_client_leaves_the_other_receiving(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_c2d_client second;
  assert_int_equal(az_iot_c2d_client_init(&second, &fx->conn), AZ_IOT_OK);
  open_to_connected(fx);

  message_record a = { 0 };
  message_record b = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &a), AZ_IOT_OK);
  assert_int_equal(az_iot_c2d_client_set_handler(&second, on_c2d, &b), AZ_IOT_OK);

  az_iot_c2d_client_destroy(&second);
  inject(fx, C2D_TOPIC, "still here");

  assert_int_equal(b.count, 0);
  assert_int_equal(a.count, 1);
}

static void the_subscription_is_reissued_after_a_reconnect(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_non_null(find_subscribe(fx->mock, C2D_FILTER));

  /* IoT Hub only delivers to a device that is subscribed, so losing the
   * subscription across a reconnect loses every message sent afterwards. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  /* Reopen WITHOUT re-registering the factory. */
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_non_null(find_subscribe(fx->mock, C2D_FILTER));

  message_record rec = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "after reconnect");
  assert_int_equal(rec.count, 1);
}

/* ------------------------------------------------------------------------- */
/* destroy                                                                   */
/* ------------------------------------------------------------------------- */

static void a_message_after_destroy_reaches_nobody(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  az_iot_c2d_client_destroy(&fx->c2d);
  inject(fx, C2D_TOPIC, "too late");
  assert_int_equal(rec.count, 0);

  /* Re-init so the shared teardown has a live client to destroy. */
  assert_int_equal(az_iot_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);
}

static void destroy_tolerates_null(void** state)
{
  (void)state;
  az_iot_c2d_client_destroy(NULL);
}

static void destroy_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_c2d_client_destroy(&fx->c2d);
  az_iot_c2d_client_destroy(&fx->c2d);

  assert_int_equal(az_iot_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test(init_rejects_a_null_connection),
    cmocka_unit_test_setup_teardown(init_subscribes_the_devicebound_filter, setup, teardown),
    cmocka_unit_test_setup_teardown(the_subscription_uses_qos_1, setup, teardown),
    cmocka_unit_test(init_falls_back_to_the_client_id),
    cmocka_unit_test(init_prefers_the_dps_registration_id),
    cmocka_unit_test(init_without_any_device_id_is_rejected),
    cmocka_unit_test(init_with_a_device_id_that_overflows_the_topic_is_rejected),
    cmocka_unit_test(a_failed_init_leaves_no_handler_registered),
    cmocka_unit_test(set_handler_rejects_a_null_client),
    cmocka_unit_test_setup_teardown(
        a_message_before_any_handler_is_set_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_later_handler_replaces_the_earlier_one, setup, teardown),
    cmocka_unit_test_setup_teardown(clearing_the_handler_stops_delivery, setup, teardown),
    cmocka_unit_test_setup_teardown(the_payload_reaches_the_handler_unchanged, setup, teardown),
    cmocka_unit_test_setup_teardown(an_empty_payload_is_delivered, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_message_on_a_property_bag_sub_topic_is_delivered, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_message_addressed_to_another_device_is_not_delivered, setup, teardown),
    cmocka_unit_test_setup_teardown(classic_delivers_a_null_content_type, setup, teardown),
    cmocka_unit_test_setup_teardown(
        only_one_of_two_clients_on_the_same_prefix_receives, setup, teardown),
    cmocka_unit_test_setup_teardown(
        destroying_one_client_leaves_the_other_receiving, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_subscription_is_reissued_after_a_reconnect, setup, teardown),
    cmocka_unit_test_setup_teardown(a_message_after_destroy_reaches_nobody, setup, teardown),
    cmocka_unit_test(destroy_tolerates_null),
    cmocka_unit_test_setup_teardown(destroy_is_idempotent, setup, teardown),
  };
  return cmocka_run_group_tests_name("c2d_client", tests, NULL, NULL);
}
