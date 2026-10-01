// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv3/az_iot_telemetry_client.h"

#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef struct fixture
{
  az_iot_connection_client connection;
  az_iot_mqttv3_telemetry_client telemetry;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock;
  bool factory_registered;
} fixture;

typedef struct send_record
{
  bool fired;
  az_iot_result status;
} send_record;

static void on_send(az_iot_result status, void* user_ctx)
{
  send_record* record = (send_record*)user_ctx;
  record->fired = true;
  record->status = status;
}

static int setup_profile(
    void** state,
    az_iot_connection_profile profile,
    az_iot_mqtt_version version)
{
  fixture* test = (fixture*)calloc(1, sizeof(*test));
  assert_non_null(test);

  az_iot_connection_client_options options = { 0 };
  options.host = "broker.example";
  options.port = 8883;
  options.client_id = "ut-device";
  options.connection_profile = profile;
  assert_int_equal(az_iot_test_connection_client_init(&test->connection, &options), AZ_IOT_OK);

  test->factory = az_iot_mock_mqtt_factory_create(version);
  assert_non_null(test->factory);
  *state = test;
  return 0;
}

static int setup(void** state)
{
  return setup_profile(state, AZ_IOT_CONNECTION_PROFILE_MQTT_V3, AZ_IOT_MQTT_VERSION_3_1_1);
}

static int setup_v5(void** state)
{
  return setup_profile(state, AZ_IOT_CONNECTION_PROFILE_MQTT_V5, AZ_IOT_MQTT_VERSION_5);
}

static int teardown(void** state)
{
  fixture* test = (fixture*)*state;
  if (test)
  {
    az_iot_mqttv3_telemetry_client_deinit(&test->telemetry);
    az_iot_connection_client_deinit(&test->connection);
    if (!test->factory_registered)
    {
      az_iot_mock_mqtt_factory_destroy(test->factory);
    }
    free(test);
  }
  return 0;
}

static const az_iot_mock_call* find_publish(fixture* test, const char* topic)
{
  size_t count = az_iot_mock_mqtt_client_call_count(test->mock);
  for (size_t i = count; i > 0; --i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(test->mock, i - 1);
    if (call->kind == AZ_IOT_MOCK_CALL_PUBLISH && strcmp(call->topic, topic) == 0)
    {
      return call;
    }
  }
  return NULL;
}

static void open_mqtt_v3(fixture* test)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&test->connection, test->factory), AZ_IOT_OK);
  test->factory_registered = true;
  assert_int_equal(az_iot_connection_client_open(&test->connection), AZ_IOT_OK);
  test->mock = az_iot_mock_mqtt_factory_last_client(test->factory);
  assert_non_null(test->mock);
  az_iot_mock_mqtt_client_clear_calls(test->mock);

  assert_true(az_iot_mock_mqtt_client_inject_connected(test->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_init(&test->telemetry, &test->connection), AZ_IOT_OK);
  az_iot_mock_mqtt_client_clear_calls(test->mock);
}

static void open_v5(fixture* test)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&test->connection, test->factory), AZ_IOT_OK);
  test->factory_registered = true;
  assert_int_equal(az_iot_connection_client_open(&test->connection), AZ_IOT_OK);
  test->mock = az_iot_mock_mqtt_factory_last_client(test->factory);
  assert_non_null(test->mock);

  az_iot_mqtt_event connected = { 0 };
  connected.kind = AZ_IOT_MQTT_EVT_CONNECTED;
  connected.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(test->mock, &connected));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);

  uint16_t packet_ids[16];
  size_t packet_id_count = 0;
  size_t call_count = az_iot_mock_mqtt_client_call_count(test->mock);
  for (size_t i = 0; i < call_count && packet_id_count < ARRAY_SIZE(packet_ids); ++i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(test->mock, i);
    if (call->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE)
    {
      packet_ids[packet_id_count++] = call->packet_id;
    }
  }
  assert_true(packet_id_count > 0);
  for (size_t i = 0; i < packet_id_count; ++i)
  {
    az_iot_mqtt_event suback = { 0 };
    suback.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
    suback.status = AZ_IOT_OK;
    suback.packet_id = packet_ids[i];
    assert_true(az_iot_mock_mqtt_client_inject_event(test->mock, &suback));
    assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  }

  const az_iot_mock_call* birth = find_publish(test, "ih/ut-device/srv/presence");
  assert_non_null(birth);
  assert_int_equal(birth->correlation_data_len, 16);

  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property type = { "type", "birth-ack:1" };
  az_iot_mqtt_message message = { 0 };
  message.topic = "ih/ut-device/dev/presence";
  message.correlation_data = nonce;
  message.correlation_data_len = sizeof(nonce);
  message.user_properties = &type;
  message.user_properties_count = 1;
  az_iot_mqtt_event ack = { 0 };
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &message;
  assert_true(az_iot_mock_mqtt_client_inject_event(test->mock, &ack));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
}

static void init_contract(void** state)
{
  fixture* test = (fixture*)*state;
  az_iot_connection_client* dummy = (az_iot_connection_client*)(uintptr_t)1;
  assert_int_equal(az_iot_mqttv3_telemetry_client_init(NULL, dummy), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_init(&test->telemetry, NULL), AZ_IOT_ERR_INVALID_ARG);
  az_iot_mqttv3_telemetry_client_deinit(NULL);

  memset(&test->telemetry, 0xEE, sizeof(test->telemetry));
  /* Init records the generation this client needs rather than reading a live
   * connection, so it succeeds before open(). */
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_init(&test->telemetry, &test->connection), AZ_IOT_OK);
  assert_ptr_equal(test->telemetry._internal.conn, &test->connection);
  az_iot_mqttv3_telemetry_client_deinit(&test->telemetry);
}

static void init_rejects_v5_profile(void** state)
{
  fixture* test = (fixture*)*state;
  open_v5(test);

  memset(&test->telemetry, 0xEE, sizeof(test->telemetry));
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_init(&test->telemetry, &test->connection),
      AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);
  assert_null(test->telemetry._internal.conn);
}

static void lifecycle_is_deterministic(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  az_iot_mqttv3_telemetry_client local;
  memset(&local, 0xEE, sizeof(local));
  assert_int_equal(az_iot_mqttv3_telemetry_client_init(&local, &test->connection), AZ_IOT_OK);
  assert_ptr_equal(local._internal.conn, &test->connection);
  az_iot_mqttv3_telemetry_client_deinit(&local);
  az_iot_mqttv3_telemetry_client_deinit(&local);

  az_iot_telemetry_message message = { 0 };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&local, &message, NULL, NULL), AZ_IOT_ERR_NOT_SUPPORTED);
}

static void send_rejects_invalid_arguments(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  az_iot_telemetry_message message = { .payload_len = 1 };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  message.payload_len = 0;
  message.properties_count = 1;
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(NULL, &message, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, NULL, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

static void send_uses_the_mqtt_v3_wire_shape_and_waits_for_puback(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  static const uint8_t payload[] = "hello";
  az_iot_telemetry_message message = { .payload = payload, .payload_len = sizeof(payload) - 1 };
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, on_send, &record), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(test->mock), 1);
  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_int_equal(publish->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(publish->topic, "devices/ut-device/messages/events/");
  assert_memory_equal(publish->payload, payload, sizeof(payload) - 1);
  assert_int_equal(publish->qos, AZ_IOT_MQTT_QOS_1);
  assert_false(publish->retain);
  assert_false(record.fired);

  assert_true(az_iot_mock_mqtt_client_inject_puback(test->mock, publish->packet_id, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  assert_true(record.fired);
  assert_int_equal(record.status, AZ_IOT_OK);
}

static void properties_are_percent_encoded_in_order(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  const az_iot_telemetry_property properties[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json;charset=utf-8" },
    { AZ_IOT_MSG_PROP_CONTENT_ENCODING, "utf-8" },
    { AZ_IOT_MSG_PROP_MESSAGE_ID, "m1" },
    { AZ_IOT_MSG_PROP_CORRELATION_ID, "c1" },
    { AZ_IOT_MSG_PROP_USER_ID, "u1" },
    { AZ_IOT_MSG_PROP_CREATION_TIME, "2026-01-01T00:00:00Z" },
    { AZ_IOT_MSG_PROP_COMPONENT_NAME, "thermostat" },
    { "note", "a&b=c d%e" },
    { "keep", "-_.~AZaz09" },
  };
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message message = {
    .payload = payload,
    .payload_len = sizeof(payload) - 1,
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(
      publish->topic,
      "devices/ut-device/messages/events/"
      "%24.ct=application%2Fjson%3Bcharset%3Dutf-8&%24.ce=utf-8&%24.mid=m1&%24.cid=c1&"
      "%24.uid=u1&%24.ctime=2026-01-01T00%3A00%3A00Z&%24.sub=thermostat&"
      "note=a%26b%3Dc%20d%25e&keep=-_.~AZaz09");
}

static void property_edges_do_not_corrupt_the_bag(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  const az_iot_telemetry_property properties[] = {
    { NULL, "orphan" },
    { "", "orphan" },
    { "flag", NULL },
    { "empty", "" },
  };
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);
  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(publish->topic, "devices/ut-device/messages/events/flag&empty=");
}

/* `#` and `+` are MQTT wildcards, and this bag is part of the topic. A value
 * carrying one unencoded would change what the topic matches, so the encoder's
 * unreserved set must stay the RFC 3986 one -- widening it for readability is
 * the plausible mistake this guards. */
static void mqtt_wildcards_in_a_value_cannot_reach_the_topic(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  const az_iot_telemetry_property properties[] = {
    { "w", "a#b+c/d?e" },
  };
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(publish->topic, "devices/ut-device/messages/events/w=a%23b%2Bc%2Fd%3Fe");
  assert_null(strchr(publish->topic + strlen("devices/ut-device/messages/events/"), '#'));
  assert_null(strchr(publish->topic + strlen("devices/ut-device/messages/events/"), '+'));
}

/* MQTTv3 gives system properties no special treatment on the way out, so a
 * name the SDK has never heard of encodes exactly like an application one. The
 * mqttv5 client does discriminate, which is why this is worth stating on both. */
static void an_unknown_system_property_is_encoded_like_any_other(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  const az_iot_telemetry_property properties[] = {
    { "$.unknown", "keep-me" },
    { "$notdot", "keep-me-too" },
  };
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(
      publish->topic,
      "devices/ut-device/messages/events/%24.unknown=keep-me&%24notdot=keep-me-too");
}

/* The overflow test above proves a huge value is refused. This proves the edge
 * is sharp: the largest topic that fits is still published, and one byte more
 * publishes nothing. Searching for the boundary rather than hardcoding it keeps
 * the test honest if AZ_IOT_MQTTV3_TELEMETRY_TOPIC_MAX changes. */
static void the_topic_length_boundary_is_sharp(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  /* Must be able to overshoot the client's topic buffer, which is larger than
   * the mock's record of it. */
  char value[1024];
  size_t first_failing = 0;
  for (size_t n = 1; n < sizeof(value); ++n)
  {
    memset(value, 'a', n);
    value[n] = '\0';
    const az_iot_telemetry_property properties[] = { { "k", value } };
    az_iot_telemetry_message message = {
      .properties = properties,
      .properties_count = ARRAY_SIZE(properties),
    };
    az_iot_mock_mqtt_client_clear_calls(test->mock);
    if (az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, NULL, NULL) != AZ_IOT_OK)
    {
      first_failing = n;
      break;
    }
  }
  assert_true(first_failing > 1);

  /* One under the edge: published, and the topic ends with the value intact. */
  memset(value, 'a', first_failing - 1);
  value[first_failing - 1] = '\0';
  const az_iot_telemetry_property fits[] = { { "k", value } };
  az_iot_telemetry_message fitting = {
    .properties = fits,
    .properties_count = ARRAY_SIZE(fits),
  };
  az_iot_mock_mqtt_client_clear_calls(test->mock);
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &fitting, NULL, NULL), AZ_IOT_OK);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(test->mock), 1);

  /* At the edge: refused, and nothing reaches the adapter -- a truncated topic
   * would publish to the wrong place. */
  memset(value, 'a', first_failing);
  value[first_failing] = '\0';
  const az_iot_telemetry_property spills[] = { { "k", value } };
  az_iot_telemetry_message spilling = {
    .properties = spills,
    .properties_count = ARRAY_SIZE(spills),
  };
  az_iot_mock_mqtt_client_clear_calls(test->mock);
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &spilling, NULL, NULL),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(test->mock), 0);
}

static void topic_overflow_is_reported_without_publishing(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  char value[1024];
  memset(value, 'x', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';
  const az_iot_telemetry_property property = { "k", value };
  az_iot_telemetry_message message = { .properties = &property, .properties_count = 1 };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, NULL, NULL),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(test->mock), 0);
}

static void empty_payload_is_valid_and_publish_failures_are_returned(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  az_iot_telemetry_message empty = { 0 };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &empty, NULL, NULL), AZ_IOT_OK);
  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_int_equal(publish->payload_len, 0);

  az_iot_mock_mqtt_client_clear_calls(test->mock);
  az_iot_mock_mqtt_client_set_next_result(
      test->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_NOT_CONNECTED);
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &empty, on_send, &record),
      AZ_IOT_ERR_NOT_CONNECTED);
  assert_false(record.fired);
}

static void a_disconnect_completes_the_pending_send(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  az_iot_telemetry_message message = { 0 };
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, on_send, &record), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(test->mock));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  assert_true(record.fired);
  assert_int_equal(record.status, AZ_IOT_ERR_NOT_CONNECTED);
}

static void a_full_puback_table_sends_nothing(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  az_iot_telemetry_message message = { 0 };
  send_record records[AZ_IOT_MAX_PENDING_PUBACKS];
  memset(records, 0, sizeof(records));
  for (size_t i = 0; i < ARRAY_SIZE(records); ++i)
  {
    assert_int_equal(
        az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, on_send, &records[i]),
        AZ_IOT_OK);
  }

  size_t before = az_iot_mock_mqtt_client_call_count(test->mock);
  send_record overflow = { 0 };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&test->telemetry, &message, on_send, &overflow),
      AZ_IOT_ERR_BUSY);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(test->mock), before);
  assert_false(overflow.fired);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_contract, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_v5_profile, setup_v5, teardown),
    cmocka_unit_test_setup_teardown(lifecycle_is_deterministic, setup, teardown),
    cmocka_unit_test_setup_teardown(send_rejects_invalid_arguments, setup, teardown),
    cmocka_unit_test_setup_teardown(
        send_uses_the_mqtt_v3_wire_shape_and_waits_for_puback, setup, teardown),
    cmocka_unit_test_setup_teardown(properties_are_percent_encoded_in_order, setup, teardown),
    cmocka_unit_test_setup_teardown(property_edges_do_not_corrupt_the_bag, setup, teardown),
    cmocka_unit_test_setup_teardown(
        mqtt_wildcards_in_a_value_cannot_reach_the_topic, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_unknown_system_property_is_encoded_like_any_other, setup, teardown),
    cmocka_unit_test_setup_teardown(the_topic_length_boundary_is_sharp, setup, teardown),
    cmocka_unit_test_setup_teardown(topic_overflow_is_reported_without_publishing, setup, teardown),
    cmocka_unit_test_setup_teardown(
        empty_payload_is_valid_and_publish_failures_are_returned, setup, teardown),
    cmocka_unit_test_setup_teardown(a_disconnect_completes_the_pending_send, setup, teardown),
    cmocka_unit_test_setup_teardown(a_full_puback_table_sends_nothing, setup, teardown),
  };
  return cmocka_run_group_tests_name("mqttv3_telemetry_client", tests, NULL, NULL);
}
