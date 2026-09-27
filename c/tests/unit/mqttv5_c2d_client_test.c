// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv5 (Hub-Next / AEG, MQTT v5) C2D client unit tests, driven through the
 * public API and the in-memory mock_mqtt_iface.
 *
 * The AEG hub delivers cloud-to-device messages on the exact topic
 * "ih/{device-id}/dev/c2d", and properties ride MQTT v5 User Properties already
 * decoded by the adapter rather than percent-encoded into a topic property bag
 * the way Classic requires.
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
#include "azure/iot/mqttv5/az_iot_c2d_client.h"

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
  az_iot_mqttv5_c2d_client c2d;
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
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(fx->factory);

  assert_int_equal(az_iot_mqttv5_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_mqttv5_c2d_client_destroy(&fx->c2d);
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

static const az_iot_mock_call* find_publish_topic(az_iot_mock_mqtt_client* m, const char* topic)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = n; i > 0; --i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i - 1);
    if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH && strcmp(c->topic, topic) == 0)
    {
      return c;
    }
  }
  return NULL;
}

/* Drive a Hub-Next session to CONNECTED. Classic announces CONNECTED on
 * CONNACK; Next first SUBACKs the presence wildcard and completes birth. */
static void open_to_connected(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);

  az_iot_mqtt_event connack;
  memset(&connack, 0, sizeof(connack));
  connack.kind = AZ_IOT_MQTT_EVT_CONNECTED;
  connack.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &connack));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  uint16_t ids[16];
  size_t id_count = 0;
  size_t n = az_iot_mock_mqtt_client_call_count(fx->mock);
  for (size_t i = 0; i < n && id_count < (sizeof(ids) / sizeof(ids[0])); ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(fx->mock, i);
    if (c->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE)
    {
      ids[id_count++] = c->packet_id;
    }
  }
  assert_true(id_count > 0);
  for (size_t i = 0; i < id_count; ++i)
  {
    az_iot_mqtt_event suback;
    memset(&suback, 0, sizeof(suback));
    suback.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
    suback.status = AZ_IOT_OK;
    suback.packet_id = ids[i];
    assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &suback));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  }

  const az_iot_mock_call* birth = find_publish_topic(fx->mock, "ih/ut-device/srv/presence");
  assert_non_null(birth);
  assert_int_equal(birth->correlation_data_len, 16);

  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/ut-device/dev/presence";
  ack_msg.correlation_data = nonce;
  ack_msg.correlation_data_len = sizeof(nonce);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &ack));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

/* Deliver an inbound Next C2D message with v5 properties and content type. */
static void inject(
    fixture* fx,
    const char* body,
    const char* content_type,
    const az_iot_mqtt_user_property* props,
    size_t props_count)
{
  az_iot_mqtt_message msg;
  memset(&msg, 0, sizeof(msg));
  msg.topic = "ih/ut-device/dev/c2d";
  msg.payload = (const uint8_t*)body;
  msg.payload_len = body ? strlen(body) : 0;
  msg.content_type = content_type;
  msg.user_properties = props;
  msg.user_properties_count = props_count;
  msg.qos = AZ_IOT_MQTT_QOS_1;
  az_iot_mqtt_event evt;
  memset(&evt, 0, sizeof(evt));
  evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  evt.message = &msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &evt));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* profile pinning                                                           */
/* ------------------------------------------------------------------------- */

static void init_rejects_a_null_client(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_mqttv5_c2d_client_init(NULL, &fx->conn), AZ_IOT_ERR_INVALID_ARG);
}

static void init_rejects_a_null_connection(void** state)
{
  (void)state;
  az_iot_mqttv5_c2d_client c2d;
  assert_int_equal(az_iot_mqttv5_c2d_client_init(&c2d, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void init_against_a_classic_connection_is_rejected(void** state)
{
  (void)state;

  /* A direct connection declares its generation up front, so the pin can be
   * answered immediately rather than deferred to connect. */
  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_CLASSIC;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv5_c2d_client c2d;
  assert_int_equal(
      az_iot_mqttv5_c2d_client_init(&c2d, &conn), AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);

  az_iot_connection_client_destroy(&conn);
}

/* ------------------------------------------------------------------------- */
/* subscriptions                                                             */
/* ------------------------------------------------------------------------- */

static void init_does_not_subscribe_a_redundant_c2d_filter(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* ih/ut-device/dev/# from the presence handshake already covers dev/c2d, so
   * registering it again would spend a registry slot and re-issue a redundant
   * filter on every reconnect. Delivery is unaffected: the dispatch prefix
   * routes the message, not this filter -- see
   * a_message_is_delivered_to_the_handler. */
  assert_non_null(find_subscribe(fx->mock, "ih/ut-device/dev/#"));
  assert_null(find_subscribe(fx->mock, "ih/ut-device/dev/c2d"));
}

/* ------------------------------------------------------------------------- */
/* delivery                                                                  */
/* ------------------------------------------------------------------------- */

static void a_message_is_delivered_to_the_handler(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv5_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  inject(fx, "hello-next", NULL, NULL, 0);

  assert_int_equal(rec.count, 1);
  assert_string_equal(rec.payload, "hello-next");
}

static void user_properties_arrive_as_plain_text(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv5_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* Already decoded by the adapter, so the handler sees exactly what the
   * sender wrote -- the same shape the Classic path produces after undoing
   * its percent-encoding, which is what makes a round trip lossless. */
  const az_iot_mqtt_user_property props[] = {
    { "$.mid", "m-1" },
    { "site", "plant 3" },
  };
  inject(fx, "body", NULL, props, 2);

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.properties_count, 2);
  assert_string_equal(rec.keys[0], "$.mid");
  assert_string_equal(rec.values[0], "m-1");
  assert_string_equal(rec.keys[1], "site");
  assert_string_equal(rec.values[1], "plant 3");
}

static void the_content_type_comes_from_its_own_field(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv5_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* MQTT v5 has a Content Type field, so it does not ride the property bag as
   * `%24.ct` the way it must on Classic. */
  inject(fx, "{}", "application/json", NULL, 0);

  assert_int_equal(rec.count, 1);
  assert_string_equal(rec.content_type, "application/json");
}

static void a_property_with_no_key_is_skipped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv5_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* A keyless pair cannot be looked up, so it is dropped rather than handed to
   * the application as a property with a NULL name. */
  const az_iot_mqtt_user_property props[] = {
    { NULL, "orphan" },
    { "kept", "yes" },
  };
  inject(fx, "body", NULL, props, 2);

  assert_int_equal(rec.count, 1);
  assert_int_equal(rec.properties_count, 1);
  assert_string_equal(rec.keys[0], "kept");
}

static void properties_past_the_bound_are_dropped_and_the_message_survives(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv5_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  /* Same contract as the Classic path: losing the payload over an excess of
   * properties would be the worse trade. */
  char keys[AZ_IOT_C2D_MAX_PROPERTIES + 3][8];
  az_iot_mqtt_user_property props[AZ_IOT_C2D_MAX_PROPERTIES + 3];
  for (size_t i = 0; i < (size_t)AZ_IOT_C2D_MAX_PROPERTIES + 3; ++i)
  {
    snprintf(keys[i], sizeof(keys[i]), "k%u", (unsigned)i);
    props[i].key = keys[i];
    props[i].value = "v";
  }
  inject(fx, "body", NULL, props, AZ_IOT_C2D_MAX_PROPERTIES + 3);

  assert_int_equal(rec.count, 1);
  assert_string_equal(rec.payload, "body");
  assert_int_equal(rec.properties_count, AZ_IOT_C2D_MAX_PROPERTIES);
}

static void a_message_before_any_handler_is_set_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* No handler yet: the message is discarded rather than buffered, and the
   * client must not dereference a NULL callback. */
  inject(fx, "body", NULL, NULL, 0);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv5_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);
  assert_int_equal(rec.count, 0);
}

/* ------------------------------------------------------------------------- */
/* lifetime                                                                  */
/* ------------------------------------------------------------------------- */

static void a_message_after_destroy_reaches_nobody(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  message_record rec = { 0 };
  assert_int_equal(az_iot_mqttv5_c2d_client_set_handler(&fx->c2d, on_c2d, &rec), AZ_IOT_OK);

  az_iot_mqttv5_c2d_client_destroy(&fx->c2d);
  inject(fx, "body", NULL, NULL, 0);
  assert_int_equal(rec.count, 0);

  /* Re-initializing against a live connection binds its topics immediately,
   * rather than waiting for a connect that has already happened. */
  assert_int_equal(az_iot_mqttv5_c2d_client_init(&fx->c2d, &fx->conn), AZ_IOT_OK);
}

static void destroy_tolerates_null(void** state)
{
  (void)state;
  az_iot_mqttv5_c2d_client_destroy(NULL);
}

static void destroy_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqttv5_c2d_client_destroy(&fx->c2d);
  az_iot_mqttv5_c2d_client_destroy(&fx->c2d);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test(init_rejects_a_null_connection),
    cmocka_unit_test(init_against_a_classic_connection_is_rejected),
    cmocka_unit_test_setup_teardown(
        init_does_not_subscribe_a_redundant_c2d_filter, setup, teardown),
    cmocka_unit_test_setup_teardown(a_message_is_delivered_to_the_handler, setup, teardown),
    cmocka_unit_test_setup_teardown(user_properties_arrive_as_plain_text, setup, teardown),
    cmocka_unit_test_setup_teardown(the_content_type_comes_from_its_own_field, setup, teardown),
    cmocka_unit_test_setup_teardown(a_property_with_no_key_is_skipped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        properties_past_the_bound_are_dropped_and_the_message_survives, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_message_before_any_handler_is_set_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_message_after_destroy_reaches_nobody, setup, teardown),
    cmocka_unit_test(destroy_tolerates_null),
    cmocka_unit_test_setup_teardown(destroy_is_idempotent, setup, teardown),
  };
  return cmocka_run_group_tests_name("mqttv5_c2d_client", tests, NULL, NULL);
}
