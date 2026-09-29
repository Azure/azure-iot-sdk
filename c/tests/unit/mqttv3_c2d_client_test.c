// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv3 (MQTTv3 hub, MQTT v3.1.1) C2D client unit tests, driven through the
 * public API and the in-memory mock_mqtt_iface.
 *
 * MQTTv3 delivers cloud-to-device messages on
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

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/mqttv3/az_iot_c2d_client.h"

#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"
#include "support/subscription_ack.h"

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
  size_t properties_count;
  char keys[AZ_IOT_C2D_MAX_PROPERTIES][64];
  char values[AZ_IOT_C2D_MAX_PROPERTIES][64];
  bool value_was_null[AZ_IOT_C2D_MAX_PROPERTIES];
} message_record;

static void on_c2d(const az_iot_c2d_message* msg, void* user_ctx)
{
  message_record* r = (message_record*)user_ctx;
  r->count++;
  r->payload_len = msg->payload_len;
  r->had_payload_pointer = (msg->payload != NULL);
  r->content_type_was_null = (msg->content_type == NULL);
  if (msg->content_type != NULL)
  {
    snprintf(r->content_type, sizeof(r->content_type), "%s", msg->content_type);
  }
  if (msg->payload != NULL && msg->payload_len > 0 && msg->payload_len < sizeof(r->payload))
  {
    memcpy(r->payload, msg->payload, msg->payload_len);
    r->payload[msg->payload_len] = '\0';
  }

  /* Copy the properties out: the header promises they are valid only for the
   * duration of this call, and a test that read them afterwards would be
   * asserting on a dangling pointer. */
  r->properties_count = msg->properties_count;
  for (size_t i = 0; i < msg->properties_count && i < AZ_IOT_C2D_MAX_PROPERTIES; ++i)
  {
    snprintf(r->keys[i], sizeof(r->keys[i]), "%s", msg->properties[i].key);
    r->value_was_null[i] = (msg->properties[i].value == NULL);
    snprintf(
        r->values[i],
        sizeof(r->values[i]),
        "%s",
        msg->properties[i].value ? msg->properties[i].value : "");
  }
}

typedef struct fixture
{
  az_iot_connection_client conn;
  az_iot_mqttv3_c2d_client c2d;
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
  assert_int_equal(az_iot_test_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  assert_int_equal(az_iot_mqttv3_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_mqttv3_c2d_client_deinit(&fx->c2d);
    /* A registered factory is adopted by the client and freed from deinit();
     * an unregistered one is still ours. */
    bool adopted = (fx->conn.factory_count > 0);
    az_iot_connection_client_deinit(&fx->conn);
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
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
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
/* init and profile pinning                                                  */
/* ------------------------------------------------------------------------- */

static void init_rejects_a_null_client(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(NULL, &fx->conn), AZ_IOT_ERR_INVALID_ARG);
}

static void init_rejects_a_null_connection(void** state)
{
  (void)state;
  az_iot_mqttv3_c2d_client c;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&c, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void init_against_an_mqtt_v5_connection_is_rejected(void** state)
{
  (void)state;

  /* A direct connection declares its generation up front, so the pin can be
   * answered at init rather than deferred to connect. */
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  az_iot_connection_client conn;
  assert_int_equal(az_iot_test_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client c2d;
  assert_int_equal(
      az_iot_mqttv3_c2d_client_init(&c2d, &conn), AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);

  az_iot_connection_client_deinit(&conn);
}

/* ------------------------------------------------------------------------- */
/* subscriptions                                                             */
/* ------------------------------------------------------------------------- */

static void the_devicebound_filter_is_subscribed_on_connect(void** state)
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

static void the_topic_is_built_from_the_client_id(void** state)
{
  (void)state;
  /* No DPS registration id configured, so the topic is built from client_id. */
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "fallback-device";
  az_iot_connection_client conn;
  assert_int_equal(az_iot_test_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client c2d;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&c2d, &conn), AZ_IOT_OK);

  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&conn, f), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&conn), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(f);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&conn, 0), AZ_IOT_OK);

  assert_non_null(find_subscribe(m, "devices/fallback-device/messages/devicebound/#"));

  az_iot_mqttv3_c2d_client_deinit(&c2d);
  az_iot_connection_client_deinit(&conn);
}

static void without_any_device_id_the_connect_attempt_fails(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  /* Neither client_id nor a DPS registration id: there is no topic to build.
   * Init only records the requirement, so this now surfaces when the
   * connection tries to bind rather than at init. */
  az_iot_connection_client conn;
  assert_int_equal(az_iot_test_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client c2d;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&c2d, &conn), AZ_IOT_OK);

  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&conn, f), AZ_IOT_OK);
  assert_int_not_equal(az_iot_connection_client_open(&conn), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client_deinit(&c2d);
  az_iot_connection_client_deinit(&conn);
}

static void a_device_id_that_overflows_the_topic_fails_the_connect_attempt(void** state)
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
  assert_int_equal(az_iot_test_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client c2d;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&c2d, &conn), AZ_IOT_OK);

  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&conn, f), AZ_IOT_OK);
  assert_int_not_equal(az_iot_connection_client_open(&conn), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client_deinit(&c2d);
  az_iot_connection_client_deinit(&conn);
}

/* ------------------------------------------------------------------------- */
/* handler registration                                                      */
/* ------------------------------------------------------------------------- */

static void set_handler_rejects_a_null_client(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_mqttv3_c2d_client_set_handler(NULL, on_c2d, NULL), AZ_IOT_ERR_INVALID_ARG);
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
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &first), AZ_IOT_OK);
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &second), AZ_IOT_OK);

  inject(fx, C2D_TOPIC, "hello");

  assert_int_equal(first.count, 0);
  assert_int_equal(second.count, 1);
}

static void clearing_the_handler_stops_delivery(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "one");
  assert_int_equal(rec.count, 1);

  /* A NULL callback stops delivery without tearing the subscription down, so
   * the application can pause processing and resume later. */
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, NULL, NULL), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "two");
  assert_int_equal(rec.count, 1);

  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);
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
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

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
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

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
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

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
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  inject(fx, "devices/other-device/messages/devicebound/", "not mine");

  assert_int_equal(rec.count, 0);
}

/* ------------------------------------------------------------------------- */
/* properties                                                                */
/* ------------------------------------------------------------------------- */

static void the_content_type_is_decoded_from_the_property_bag(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  inject(fx, C2D_TOPIC "%24.ct=application%2Fjson", "{}");

  assert_int_equal(rec.count, 1);
  assert_false(rec.content_type_was_null);
  assert_string_equal(rec.content_type, "application/json");
}

static void property_keys_and_values_arrive_as_plain_text(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* The same spelling the sender passed to az_iot_telemetry_property: a system
   * key as "$.mid" rather than "%24.mid", and reserved characters restored. */
  inject(fx, C2D_TOPIC "%24.mid=id-1&note=a%26b%3Dc%20d%25e", "body");

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.properties_count, 2);
  assert_string_equal(rec.keys[0], "$.mid");
  assert_string_equal(rec.values[0], "id-1");
  assert_string_equal(rec.keys[1], "note");
  assert_string_equal(rec.values[1], "a&b=c d%e");
}

static void the_three_property_bag_value_forms_are_distinguished(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* IoT Hub encodes a null value as a bare key, an empty value as a trailing
   * '=', and anything else as "key=value". Collapsing the first two would lose
   * the difference between "unset" and "set to nothing". */
  inject(fx, C2D_TOPIC "?prop1&prop2=&prop3=a%20string", "body");

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.properties_count, 3);

  assert_string_equal(rec.keys[0], "?prop1");
  assert_true(rec.value_was_null[0]);

  assert_string_equal(rec.keys[1], "prop2");
  assert_false(rec.value_was_null[1]);
  assert_string_equal(rec.values[1], "");

  assert_string_equal(rec.keys[2], "prop3");
  assert_string_equal(rec.values[2], "a string");
}

static void a_message_with_no_property_bag_has_no_properties(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  inject(fx, C2D_TOPIC, "plain");

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.properties_count, 0);
  assert_true(rec.content_type_was_null);
}

static void a_malformed_escape_drops_the_properties_but_keeps_the_message(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* "%zz" is not a valid escape. Guessing at it would hand the application
   * bytes the service never sent, but losing the payload over an unreadable
   * property would be the worse trade. */
  inject(fx, C2D_TOPIC "good=1&bad=%zz", "still delivered");

  assert_int_equal(rec.count, 1);
  assert_string_equal(rec.payload, "still delivered");
  assert_int_equal(rec.properties_count, 0);
}

static void properties_past_the_bound_are_dropped_and_the_message_survives(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  char topic[512];
  int n = snprintf(topic, sizeof(topic), "%s", C2D_TOPIC);
  for (int i = 0; i < AZ_IOT_C2D_MAX_PROPERTIES + 3; ++i)
  {
    n += snprintf(topic + n, sizeof(topic) - (size_t)n, "%sk%d=v%d", i ? "&" : "", i, i);
  }
  inject(fx, topic, "body");

  assert_int_equal(rec.count, 1);
  assert_string_equal(rec.payload, "body");
  assert_int_equal(rec.properties_count, AZ_IOT_C2D_MAX_PROPERTIES);
}

static void a_property_can_be_looked_up_by_name(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);
  inject(fx, C2D_TOPIC "%24.ct=text%2Fplain&colour=blue", "body");
  assert_int_equal(rec.count, 1);

  /* Rebuild an equivalent message to exercise the accessor directly: the
   * SDK-owned one is gone once the handler returns. */
  az_iot_c2d_property props[] = { { "$.ct", "text/plain" }, { "colour", "blue" } };
  az_iot_c2d_message msg = { 0 };
  msg.properties = props;
  msg.properties_count = 2;

  assert_string_equal(az_iot_c2d_message_property(&msg, "colour"), "blue");
  assert_string_equal(
      az_iot_c2d_message_property(&msg, AZ_IOT_MSG_PROP_CONTENT_TYPE), "text/plain");
  assert_null(az_iot_c2d_message_property(&msg, "absent"));
  assert_null(az_iot_c2d_message_property(NULL, "colour"));
  assert_null(az_iot_c2d_message_property(&msg, NULL));

  /* A hand-built message with a count but no array is a caller mistake, and
   * this is public API: answer NULL rather than dereferencing it. */
  az_iot_c2d_message bad = { 0 };
  bad.properties_count = 3;
  assert_null(az_iot_c2d_message_property(&bad, "colour"));
}

/* ------------------------------------------------------------------------- */
/* multiple clients and reconnects                                           */
/* ------------------------------------------------------------------------- */

static void a_second_client_for_the_same_identity_fails_the_connect_attempt(void** state)
{
  fixture* fx = (fixture*)*state;

  /* There is one device-bound stream per identity, so a second C2D client
   * could never receive anything. Dispatch is longest-prefix-wins with no
   * tie-break, so the loser would simply go quiet for the life of the
   * connection with nothing to show for it. Both clients now register their
   * topics when the connection binds, so the clash is refused there -- and it
   * takes the connect attempt with it rather than coming up half-working. */
  az_iot_mqttv3_c2d_client second;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&second, &fx->conn), AZ_IOT_OK);

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_not_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client_deinit(&second);
}

static void a_client_for_a_different_identity_registers_alongside(void** state)
{
  fixture* fx = (fixture*)*state;

  /* A different identity produces a different device-bound topic, so its C2D
   * client is not the duplicate the previous test refuses.
   *
   * This deliberately does NOT claim to exercise connection multiplexing:
   * these are two separate az_iot_connection_client instances with separate
   * dispatch tables. That distinct prefixes coexist in ONE table -- the part
   * multiplexing actually rests on -- is asserted by
   * dispatch_allows_distinct_identities_to_coexist, where the table is
   * directly in view. */
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "other-device";
  az_iot_connection_client other_conn;
  assert_int_equal(az_iot_test_connection_client_init(&other_conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client other;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&other, &other_conn), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client_deinit(&other);
  az_iot_connection_client_deinit(&other_conn);

  /* And on this connection, the slot frees up once its holder goes away. */
  az_iot_mqttv3_c2d_client_deinit(&fx->c2d);
  az_iot_mqttv3_c2d_client replacement;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&replacement, &fx->conn), AZ_IOT_OK);
  az_iot_mqttv3_c2d_client_deinit(&replacement);

  assert_int_equal(az_iot_mqttv3_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);
}

static void a_replacement_client_takes_over_delivery(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record first = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &first), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "to the first");
  assert_int_equal(first.count, 1);

  /* Tearing a client down and standing another one up in its place is the
   * supported way to re-point delivery, now that two cannot coexist. */
  az_iot_mqttv3_c2d_client_deinit(&fx->c2d);
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);

  message_record second = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &second), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "to the second");

  assert_int_equal(first.count, 1);
  assert_int_equal(second.count, 1);
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
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);

  assert_non_null(find_subscribe(fx->mock, C2D_FILTER));

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "after reconnect");
  assert_int_equal(rec.count, 1);
}

/* ------------------------------------------------------------------------- */
/* destroy                                                                   */
/* ------------------------------------------------------------------------- */

static void a_message_after_deinit_reaches_nobody(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client_deinit(&fx->c2d);
  inject(fx, C2D_TOPIC, "too late");
  assert_int_equal(rec.count, 0);

  /* Re-init so the shared teardown has a live client to destroy. */
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);
}

static void deinit_tolerates_null(void** state)
{
  (void)state;
  az_iot_mqttv3_c2d_client_deinit(NULL);
}

static void deinit_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_mqttv3_c2d_client_deinit(&fx->c2d);
  az_iot_mqttv3_c2d_client_deinit(&fx->c2d);

  assert_int_equal(az_iot_mqttv3_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test(init_rejects_a_null_connection),
    cmocka_unit_test(init_against_an_mqtt_v5_connection_is_rejected),
    cmocka_unit_test_setup_teardown(
        the_devicebound_filter_is_subscribed_on_connect, setup, teardown),
    cmocka_unit_test_setup_teardown(the_subscription_uses_qos_1, setup, teardown),
    cmocka_unit_test(the_topic_is_built_from_the_client_id),
    cmocka_unit_test(without_any_device_id_the_connect_attempt_fails),
    cmocka_unit_test(a_device_id_that_overflows_the_topic_fails_the_connect_attempt),
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
    cmocka_unit_test_setup_teardown(
        the_content_type_is_decoded_from_the_property_bag, setup, teardown),
    cmocka_unit_test_setup_teardown(property_keys_and_values_arrive_as_plain_text, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_three_property_bag_value_forms_are_distinguished, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_message_with_no_property_bag_has_no_properties, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_malformed_escape_drops_the_properties_but_keeps_the_message, setup, teardown),
    cmocka_unit_test_setup_teardown(
        properties_past_the_bound_are_dropped_and_the_message_survives, setup, teardown),
    cmocka_unit_test_setup_teardown(a_property_can_be_looked_up_by_name, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_second_client_for_the_same_identity_fails_the_connect_attempt, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_client_for_a_different_identity_registers_alongside, setup, teardown),
    cmocka_unit_test_setup_teardown(a_replacement_client_takes_over_delivery, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_subscription_is_reissued_after_a_reconnect, setup, teardown),
    cmocka_unit_test_setup_teardown(a_message_after_deinit_reaches_nobody, setup, teardown),
    cmocka_unit_test(deinit_tolerates_null),
    cmocka_unit_test_setup_teardown(deinit_is_idempotent, setup, teardown),
  };
  return cmocka_run_group_tests_name("mqttv3_c2d_client", tests, NULL, NULL);
}
