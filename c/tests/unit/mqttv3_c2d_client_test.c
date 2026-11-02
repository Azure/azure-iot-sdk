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

#include "internal/connection_client_internal.h"

#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"
#include "support/subscription_ack.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

/* Large enough for every value a test sends; a test passes a smaller
 * value_size to exercise the not-enough-space path. */
#define REC_VALUE_MAX 512
#define REC_PROPERTIES_MAX 16

typedef struct getter_record
{
  az_iot_result result;
  char value[REC_VALUE_MAX];
  size_t len;
} getter_record;

typedef struct message_record
{
  int count;
  char payload[128];
  size_t payload_len;
  bool had_payload_pointer;
  /* Buffer size handed to every reader; 0 means REC_VALUE_MAX. */
  size_t value_size;
  getter_record message_id, correlation_id, expiry_time, content_type, content_encoding, user_id;
  /* Optional az_iot_c2d_properties_find() probe. */
  const char* find_key;
  getter_record found;
  /* az_iot_c2d_properties_next() results, up to AZ_IOT_ERR_NOT_FOUND. */
  size_t properties_count;
  struct
  {
    az_iot_result result;
    char key[64];
    size_t key_len;
    char value[REC_VALUE_MAX];
    size_t value_len;
    bool has_value;
  } properties[REC_PROPERTIES_MAX];
  /* The first property that did not fit, read again from the cursor saved
   * before it, with full-size buffers. */
  struct
  {
    bool attempted;
    az_iot_result result;
    char key[REC_VALUE_MAX];
    char value[REC_VALUE_MAX];
  } retry;
} message_record;

#define RECORD_GETTER(r, msg, size, name) \
  ((r)->name.result = az_iot_c2d_message_get_##name((msg), (r)->name.value, (size), &(r)->name.len))

/* Read everything inside the callback: the header promises the message is
 * valid only for its duration, so a test that read it afterwards would be
 * asserting on a dangling pointer. */
static void on_c2d(const az_iot_c2d_message* msg, void* user_ctx)
{
  message_record* r = (message_record*)user_ctx;
  size_t value_size = r->value_size ? r->value_size : REC_VALUE_MAX;
  r->count++;
  r->payload_len = msg->payload_len;
  r->had_payload_pointer = (msg->payload != NULL);
  if (msg->payload != NULL && msg->payload_len > 0 && msg->payload_len < sizeof(r->payload))
  {
    memcpy(r->payload, msg->payload, msg->payload_len);
    r->payload[msg->payload_len] = '\0';
  }

  RECORD_GETTER(r, msg, value_size, message_id);
  RECORD_GETTER(r, msg, value_size, correlation_id);
  RECORD_GETTER(r, msg, value_size, expiry_time);
  RECORD_GETTER(r, msg, value_size, content_type);
  RECORD_GETTER(r, msg, value_size, content_encoding);
  RECORD_GETTER(r, msg, value_size, user_id);
  if (r->find_key != NULL)
  {
    r->found.result = az_iot_c2d_properties_find(
        &msg->properties, r->find_key, r->found.value, value_size, &r->found.len);
  }

  size_t cursor = 0;
  r->properties_count = 0;
  for (size_t i = 0; i < REC_PROPERTIES_MAX; ++i)
  {
    size_t saved = cursor;
    az_iot_result result = az_iot_c2d_properties_next(
        &msg->properties,
        &cursor,
        r->properties[i].key,
        sizeof(r->properties[i].key),
        &r->properties[i].key_len,
        r->properties[i].value,
        value_size,
        &r->properties[i].value_len,
        &r->properties[i].has_value);
    if (result == AZ_IOT_ERR_NOT_FOUND)
    {
      break;
    }
    if (result == AZ_IOT_ERR_NOT_ENOUGH_SPACE && !r->retry.attempted)
    {
      r->retry.attempted = true;
      r->retry.result = az_iot_c2d_properties_next(
          &msg->properties,
          &saved,
          r->retry.key,
          sizeof(r->retry.key),
          NULL,
          r->retry.value,
          sizeof(r->retry.value),
          NULL,
          NULL);
    }
    r->properties[i].result = result;
    r->properties_count++;
  }
}

typedef struct fixture
{
  az_iot_connection_client conn;
  az_iot_mqttv3_c2d_client c2d;
  az_iot_mock_mqtt_client* mock;
} fixture;

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.iothub_hostname = "broker.example";
  opts.port = 8883;
  opts.device_id = "ut-device";
  opts.mqtt_adapter = az_iot_mock_mqtt_get_adapter_for_version(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(az_iot_test_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

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
    az_iot_test_connection_client_deinit(&fx->conn);
    free(fx);
  }
  return 0;
}

static void open_to_connected(fixture* fx)
{
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_adapter_last_client();
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
  opts.iothub_hostname = "broker.example";
  opts.device_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  az_iot_connection_client conn;
  assert_int_equal(az_iot_test_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client c2d;
  assert_int_equal(
      az_iot_mqttv3_c2d_client_init(&c2d, &conn), AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);

  az_iot_test_connection_client_deinit(&conn);
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
  opts.iothub_hostname = "broker.example";
  opts.device_id = "fallback-device";
  opts.mqtt_adapter = az_iot_mock_mqtt_get_adapter_for_version(AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_connection_client conn;
  assert_int_equal(az_iot_test_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client c2d;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&c2d, &conn), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&conn), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_adapter_last_client();
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&conn, 0), AZ_IOT_OK);

  assert_non_null(find_subscribe(m, "devices/fallback-device/messages/devicebound/#"));

  az_iot_mqttv3_c2d_client_deinit(&c2d);
  az_iot_test_connection_client_deinit(&conn);
}

static void without_any_device_id_the_connect_attempt_fails(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.iothub_hostname = "broker.example";
  opts.mqtt_adapter = az_iot_mock_mqtt_get_adapter_for_version(AZ_IOT_MQTT_VERSION_3_1_1);
  /* Neither client_id nor a DPS registration id: there is no topic to build.
   * Init only records the requirement, so this now surfaces when the
   * connection tries to bind rather than at init. */
  az_iot_connection_client conn;
  assert_int_equal(az_iot_test_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client c2d;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&c2d, &conn), AZ_IOT_OK);

  assert_int_not_equal(az_iot_connection_client_open(&conn), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client_deinit(&c2d);
  az_iot_test_connection_client_deinit(&conn);
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
  opts.iothub_hostname = "broker.example";
  opts.device_id = huge;
  opts.mqtt_adapter = az_iot_mock_mqtt_get_adapter_for_version(AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_connection_client conn;
  assert_int_equal(az_iot_test_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client c2d;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&c2d, &conn), AZ_IOT_OK);

  assert_int_not_equal(az_iot_connection_client_open(&conn), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client_deinit(&c2d);
  az_iot_test_connection_client_deinit(&conn);
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

static void the_content_type_is_read_with_a_typed_accessor(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  inject(fx, C2D_TOPIC "%24.ct=application%2Fjson", "{}");

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.content_type.result, AZ_IOT_OK);
  assert_string_equal(rec.content_type.value, "application/json");
  assert_int_equal(rec.content_type.len, 16);
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
  assert_int_equal(rec.properties[0].result, AZ_IOT_OK);
  assert_string_equal(rec.properties[0].key, "$.mid");
  assert_string_equal(rec.properties[0].value, "id-1");
  assert_int_equal(rec.properties[1].result, AZ_IOT_OK);
  assert_string_equal(rec.properties[1].key, "note");
  assert_string_equal(rec.properties[1].value, "a&b=c d%e");
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

  assert_string_equal(rec.properties[0].key, "?prop1");
  assert_false(rec.properties[0].has_value);
  assert_string_equal(rec.properties[0].value, "");

  assert_string_equal(rec.properties[1].key, "prop2");
  assert_true(rec.properties[1].has_value);
  assert_string_equal(rec.properties[1].value, "");

  assert_string_equal(rec.properties[2].key, "prop3");
  assert_true(rec.properties[2].has_value);
  assert_string_equal(rec.properties[2].value, "a string");
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
  assert_int_equal(rec.content_type.result, AZ_IOT_ERR_NOT_FOUND);
  assert_int_equal(rec.message_id.result, AZ_IOT_ERR_NOT_FOUND);
}

static void a_malformed_escape_fails_only_that_property(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  rec.find_key = "bad";
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* "%zz" is not a valid escape. Guessing at it would hand the application
   * bytes the service never sent; the error is reported for that property
   * alone, and the payload and the other properties are still delivered. */
  inject(fx, C2D_TOPIC "good=1&bad=%zz&%24.mid=m-1", "still delivered");

  assert_int_equal(rec.count, 1);
  assert_string_equal(rec.payload, "still delivered");
  assert_int_equal(rec.properties_count, 3);
  assert_int_equal(rec.properties[0].result, AZ_IOT_OK);
  assert_int_equal(rec.properties[1].result, AZ_IOT_ERR_PROTOCOL);
  assert_string_equal(rec.properties[1].key, "");
  assert_string_equal(rec.properties[1].value, "");
  assert_int_equal(rec.properties[2].result, AZ_IOT_OK);
  assert_int_equal(rec.found.result, AZ_IOT_ERR_PROTOCOL);
  assert_string_equal(rec.message_id.value, "m-1");
}

static void every_property_is_reachable(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  rec.find_key = "k10";
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* No SDK-side cap on the number of properties. */
  char topic[512];
  int n = snprintf(topic, sizeof(topic), "%s", C2D_TOPIC);
  for (int i = 0; i < 11; ++i)
  {
    n += snprintf(topic + n, sizeof(topic) - (size_t)n, "%sk%d=v%d", i ? "&" : "", i, i);
  }
  inject(fx, topic, "body");

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.properties_count, 11);
  assert_string_equal(rec.properties[10].key, "k10");
  assert_string_equal(rec.properties[10].value, "v10");
  assert_int_equal(rec.found.result, AZ_IOT_OK);
  assert_string_equal(rec.found.value, "v10");
}

static void a_long_value_needs_no_sdk_buffer(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* Longer than any fixed buffer the SDK used to decode into. */
  const size_t mid_len = 400;
  char topic[REC_VALUE_MAX + 128];
  int n = snprintf(topic, sizeof(topic), "%s%%24.mid=", C2D_TOPIC);
  memset(topic + n, 'm', mid_len);
  topic[n + (int)mid_len] = '\0';
  inject(fx, topic, "body");

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.message_id.result, AZ_IOT_OK);
  assert_int_equal(rec.message_id.len, mid_len);
  assert_int_equal(strlen(rec.message_id.value), mid_len);
}

/* ------------------------------------------------------------------------- */
/* typed system properties                                                   */
/* ------------------------------------------------------------------------- */

static void system_properties_are_read_with_typed_accessors(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* The shape IoT Hub sends, "$.to" and "iothub-ack" included. */
  inject(
      fx,
      C2D_TOPIC "%24.mid=m-1&%24.to=%2Fdevices%2Fut-device%2Fmessages%2FdeviceBound"
                "&%24.cid=c%2F1&%24.exp=2026-10-13T07%3A01%3A14.930Z&%24.ce=utf-8"
                "&%24.uid=user-1&%24.ct=application%2Fjson&iothub-ack=full",
      "body");

  assert_int_equal(rec.count, 1);
  assert_string_equal(rec.message_id.value, "m-1");
  assert_string_equal(rec.correlation_id.value, "c/1");
  assert_string_equal(rec.expiry_time.value, "2026-10-13T07:01:14.930Z");
  assert_string_equal(rec.content_encoding.value, "utf-8");
  assert_string_equal(rec.user_id.value, "user-1");
  assert_string_equal(rec.content_type.value, "application/json");
  assert_int_equal(rec.message_id.result, AZ_IOT_OK);
  assert_int_equal(rec.expiry_time.result, AZ_IOT_OK);
  assert_int_equal(rec.properties_count, 8);
  assert_string_equal(rec.properties[3].key, AZ_IOT_MSG_PROP_EXPIRY_TIME);
}

static void absent_and_valueless_system_properties(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* A bare key and a trailing '=' are both present, with an empty value.
   * "$.mid" must not match a longer key that starts with it, nor one that
   * decodes to it followed by a NUL. */
  inject(fx, C2D_TOPIC "%24.mid&%24.cid=&%24.midx=1&mid=2&%24.ce%00x=bad&%24.ce=utf-8", "body");

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.message_id.result, AZ_IOT_OK);
  assert_string_equal(rec.message_id.value, "");
  assert_int_equal(rec.message_id.len, 0);
  assert_int_equal(rec.correlation_id.result, AZ_IOT_OK);
  assert_string_equal(rec.correlation_id.value, "");
  assert_int_equal(rec.expiry_time.result, AZ_IOT_ERR_NOT_FOUND);
  assert_int_equal(rec.user_id.result, AZ_IOT_ERR_NOT_FOUND);
  assert_int_equal(rec.content_type.result, AZ_IOT_ERR_NOT_FOUND);
  assert_int_equal(rec.content_encoding.result, AZ_IOT_OK);
  assert_string_equal(rec.content_encoding.value, "utf-8");
  /* The NUL key cannot be decoded into a C string either. */
  assert_int_equal(rec.properties[4].result, AZ_IOT_ERR_PROTOCOL);
}

static void the_first_occurrence_of_a_property_wins(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* A first occurrence with no value is still the first. */
  inject(fx, C2D_TOPIC "%24.mid=first&%24.mid=second&%24.cid&%24.cid=later", "body");

  assert_int_equal(rec.count, 1);
  assert_string_equal(rec.message_id.value, "first");
  assert_int_equal(rec.correlation_id.result, AZ_IOT_OK);
  assert_string_equal(rec.correlation_id.value, "");
}

static void a_value_too_large_reports_the_size_it_needs(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  rec.value_size = 4;
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* "abcd" needs 5 bytes with its terminator; "abc" fits exactly. */
  inject(fx, C2D_TOPIC "%24.mid=abcd&%24.cid=abc", "body");

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.message_id.result, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(rec.message_id.len, 4);
  assert_string_equal(rec.message_id.value, "");
  assert_int_equal(rec.correlation_id.result, AZ_IOT_OK);
  assert_string_equal(rec.correlation_id.value, "abc");
  assert_int_equal(rec.properties_count, 2);
  assert_int_equal(rec.properties[0].result, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(rec.properties[0].key_len, 5);
  assert_int_equal(rec.properties[0].value_len, 4);
  assert_int_equal(rec.properties[1].result, AZ_IOT_OK);
  assert_int_equal(rec.properties[1].value_len, 3);
  /* Retried from the saved cursor with bigger buffers. */
  assert_true(rec.retry.attempted);
  assert_int_equal(rec.retry.result, AZ_IOT_OK);
  assert_string_equal(rec.retry.key, "$.mid");
  assert_string_equal(rec.retry.value, "abcd");
}

static void a_key_too_large_fails_only_that_property(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  char topic[256];
  int n = snprintf(topic, sizeof(topic), "%s", C2D_TOPIC);
  memset(topic + n, 'k', 70); /* the record's key buffer holds 63 */
  n += 70;
  snprintf(topic + n, sizeof(topic) - (size_t)n, "=v&next=1");
  inject(fx, topic, "body");

  assert_int_equal(rec.properties_count, 2);
  assert_int_equal(rec.properties[0].result, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(rec.properties[0].key_len, 70);
  assert_int_equal(rec.properties[0].value_len, 1);
  assert_int_equal(rec.properties[1].result, AZ_IOT_OK);
  assert_string_equal(rec.properties[1].key, "next");
  assert_int_equal(rec.retry.result, AZ_IOT_OK);
  assert_int_equal(strlen(rec.retry.key), 70);
  assert_string_equal(rec.retry.value, "v");
}

static void a_value_with_an_encoded_nul_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* Truncating at the NUL would hand back a different value. */
  inject(fx, C2D_TOPIC "%24.ct=a%00b&%24.cid=c-3", "body");

  assert_int_equal(rec.content_type.result, AZ_IOT_ERR_PROTOCOL);
  assert_string_equal(rec.content_type.value, "");
  assert_string_equal(rec.correlation_id.value, "c-3");
}

static void property_readers_reject_invalid_arguments(void** state)
{
  (void)state;
  char key[8];
  char value[8];
  size_t cursor = 0;
  az_iot_c2d_message msg = { 0 };
  const az_iot_c2d_properties* props = &msg.properties;

  assert_int_equal(
      az_iot_c2d_properties_next(
          NULL, &cursor, key, sizeof(key), NULL, value, sizeof(value), NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_c2d_properties_next(
          props, NULL, key, sizeof(key), NULL, value, sizeof(value), NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_c2d_properties_next(
          props, &cursor, NULL, sizeof(key), NULL, value, sizeof(value), NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_c2d_properties_next(props, &cursor, key, 0, NULL, value, sizeof(value), NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_c2d_properties_next(
          props, &cursor, key, sizeof(key), NULL, NULL, sizeof(value), NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_c2d_properties_next(props, &cursor, key, sizeof(key), NULL, value, 0, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_c2d_properties_find(props, NULL, value, sizeof(value), NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_c2d_properties_find(props, "", value, sizeof(value), NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_c2d_message_get_message_id(NULL, value, sizeof(value), NULL), AZ_IOT_ERR_INVALID_ARG);

  /* A message with no property bag is valid and simply has none. */
  assert_int_equal(
      az_iot_c2d_properties_next(
          props, &cursor, key, sizeof(key), NULL, value, sizeof(value), NULL, NULL),
      AZ_IOT_ERR_NOT_FOUND);
  assert_int_equal(
      az_iot_c2d_message_get_message_id(&msg, value, sizeof(value), NULL), AZ_IOT_ERR_NOT_FOUND);
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
  opts.iothub_hostname = "broker.example";
  opts.device_id = "other-device";
  az_iot_connection_client other_conn;
  assert_int_equal(az_iot_test_connection_client_init(&other_conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client other;
  assert_int_equal(az_iot_mqttv3_c2d_client_init(&other, &other_conn), AZ_IOT_OK);

  az_iot_mqttv3_c2d_client_deinit(&other);
  az_iot_test_connection_client_deinit(&other_conn);

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

  /* Reopen with nothing set up again. */
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_adapter_last_client();
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

/* A hub session ended for a provisioning session (a software update check) is
 * an ordinary session end: the subscription is reissued on the session that
 * reconnects once the provisioning session is released. */
static void the_subscription_is_reissued_after_a_provisioning_session(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->conn.opts.dps.id_scope = "0ne00000000";
  fx->conn.opts.dps.registration_id = "ut-device";
  open_to_connected(fx);
  assert_non_null(find_subscribe(fx->mock, C2D_FILTER));

  /* The hub session ends first; the provisioning session opens once it is gone. */
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->conn), AZ_IOT_ERR_BUSY);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_null(fx->conn.active_client);
  assert_non_null(fx->conn.dps_mqtt);
  assert_int_equal(
      az_iot_connection_client_get_state(&fx->conn, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_RETRY_PENDING);

  /* Released: the hub session reconnects, without open(), and subscribes again. */
  az_iot_connection_client__dps_session_release(&fx->conn);
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_null(fx->conn.dps_mqtt);
  fx->mock = az_iot_mock_mqtt_adapter_last_client();
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
  assert_non_null(find_subscribe(fx->mock, C2D_FILTER));
  assert_int_equal(
      az_iot_connection_client_get_state(&fx->conn, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);
  inject(fx, C2D_TOPIC, "after the provisioning session");
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
        the_content_type_is_read_with_a_typed_accessor, setup, teardown),
    cmocka_unit_test_setup_teardown(property_keys_and_values_arrive_as_plain_text, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_three_property_bag_value_forms_are_distinguished, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_message_with_no_property_bag_has_no_properties, setup, teardown),
    cmocka_unit_test_setup_teardown(a_malformed_escape_fails_only_that_property, setup, teardown),
    cmocka_unit_test_setup_teardown(every_property_is_reachable, setup, teardown),
    cmocka_unit_test_setup_teardown(a_long_value_needs_no_sdk_buffer, setup, teardown),
    cmocka_unit_test_setup_teardown(
        system_properties_are_read_with_typed_accessors, setup, teardown),
    cmocka_unit_test_setup_teardown(absent_and_valueless_system_properties, setup, teardown),
    cmocka_unit_test_setup_teardown(the_first_occurrence_of_a_property_wins, setup, teardown),
    cmocka_unit_test_setup_teardown(a_value_too_large_reports_the_size_it_needs, setup, teardown),
    cmocka_unit_test_setup_teardown(a_key_too_large_fails_only_that_property, setup, teardown),
    cmocka_unit_test_setup_teardown(a_value_with_an_encoded_nul_is_rejected, setup, teardown),
    cmocka_unit_test(property_readers_reject_invalid_arguments),
    cmocka_unit_test_setup_teardown(
        a_second_client_for_the_same_identity_fails_the_connect_attempt, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_client_for_a_different_identity_registers_alongside, setup, teardown),
    cmocka_unit_test_setup_teardown(a_replacement_client_takes_over_delivery, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_subscription_is_reissued_after_a_reconnect, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_subscription_is_reissued_after_a_provisioning_session, setup, teardown),
    cmocka_unit_test_setup_teardown(a_message_after_deinit_reaches_nobody, setup, teardown),
    cmocka_unit_test(deinit_tolerates_null),
    cmocka_unit_test_setup_teardown(deinit_is_idempotent, setup, teardown),
  };
  return cmocka_run_group_tests_name("mqttv3_c2d_client", tests, NULL, NULL);
}
