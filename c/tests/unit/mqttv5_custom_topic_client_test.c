// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv5/az_iot_custom_topic_client.h"

#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DEVICE_ID "ut-device"

typedef struct fixture
{
  az_iot_connection_client connection;
  az_iot_mqttv5_custom_topic_client custom;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock;
} fixture;

typedef struct send_record
{
  int calls;
  az_iot_result status;
} send_record;

static void on_send(az_iot_result status, void* user_ctx)
{
  send_record* record = (send_record*)user_ctx;
  record->calls++;
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
  options.iothub_hostname = "broker.example";
  options.port = 8883;
  options.device_id = DEVICE_ID;
  options.connection_profile = profile;
  assert_int_equal(az_iot_test_connection_client_init(&test->connection, &options), AZ_IOT_OK);

  test->factory = az_iot_mock_mqtt_factory_create(version);
  assert_non_null(test->factory);
  *state = test;
  return 0;
}

static int setup(void** state)
{
  return setup_profile(state, AZ_IOT_CONNECTION_PROFILE_MQTT_V5, AZ_IOT_MQTT_VERSION_5);
}

static int setup_mqtt_v3(void** state)
{
  return setup_profile(state, AZ_IOT_CONNECTION_PROFILE_MQTT_V3, AZ_IOT_MQTT_VERSION_3_1_1);
}

static int teardown(void** state)
{
  fixture* test = (fixture*)*state;
  if (test)
  {
    az_iot_mqttv5_custom_topic_client_deinit(&test->custom);
    az_iot_connection_client_deinit(&test->connection);
    az_iot_mock_mqtt_factory_destroy(test->factory);
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

static void start_open(fixture* test)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&test->connection, test->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&test->connection), AZ_IOT_OK);
  test->mock = az_iot_mock_mqtt_factory_last_client(test->factory);
  assert_non_null(test->mock);
}

static void open_mqtt_v3(fixture* test)
{
  start_open(test);
  assert_true(az_iot_mock_mqtt_client_inject_connected(test->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
}

/* Drives an mqttv5 session through CONNACK, SUBACKs and the birth handshake to CONNECTED. */
static void open_v5(fixture* test)
{
  start_open(test);

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
  for (size_t i = 0; i < packet_id_count; ++i)
  {
    az_iot_mqtt_event suback = { 0 };
    suback.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
    suback.status = AZ_IOT_OK;
    suback.packet_id = packet_ids[i];
    assert_true(az_iot_mock_mqtt_client_inject_event(test->mock, &suback));
    assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  }

  const az_iot_mock_call* birth = find_publish(test, "ih/" DEVICE_ID "/srv/presence");
  assert_non_null(birth);
  assert_int_equal(birth->correlation_data_len, 16);

  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property type = { "type", "birth-ack:1" };
  az_iot_mqtt_message message = { 0 };
  message.topic = "ih/" DEVICE_ID "/dev/presence";
  message.correlation_data = nonce;
  message.correlation_data_len = sizeof(nonce);
  message.user_properties = az_iot_mqtt_user_properties_from_array(&type, 1);
  az_iot_mqtt_event ack = { 0 };
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &message;
  assert_true(az_iot_mock_mqtt_client_inject_event(test->mock, &ack));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_get_state(&test->connection, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);
}

static void connect_and_init(fixture* test)
{
  open_v5(test);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_init(&test->custom, &test->connection), AZ_IOT_OK);
  az_iot_mock_mqtt_client_clear_calls(test->mock);
}

static az_iot_result publish_topic(fixture* test, const char* topic)
{
  az_iot_mqttv5_custom_topic_message message = { .topic = topic };
  send_record record = { 0 };
  return az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record);
}

static void init_contract(void** state)
{
  fixture* test = (fixture*)*state;
  az_iot_connection_client* dummy = (az_iot_connection_client*)(uintptr_t)1;
  assert_int_equal(az_iot_mqttv5_custom_topic_client_init(NULL, dummy), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_init(&test->custom, NULL), AZ_IOT_ERR_INVALID_ARG);
  az_iot_mqttv5_custom_topic_client_deinit(NULL);

  /* Records the generation it needs, so it succeeds before open(). */
  memset(&test->custom, 0xEE, sizeof(test->custom));
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_init(&test->custom, &test->connection), AZ_IOT_OK);
  assert_ptr_equal(test->custom._internal.conn, &test->connection);
  az_iot_mqttv5_custom_topic_client_deinit(&test->custom);
  az_iot_mqttv5_custom_topic_client_deinit(&test->custom);
  assert_null(test->custom._internal.conn);
}

static void init_rejects_mqtt_v3_profile(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  memset(&test->custom, 0xEE, sizeof(test->custom));
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_init(&test->custom, &test->connection),
      AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);
  assert_null(test->custom._internal.conn);
}

static void an_uninitialized_client_is_not_supported(void** state)
{
  (void)state;
  az_iot_mqttv5_custom_topic_client client;
  memset(&client, 0, sizeof(client));
  az_iot_mqttv5_custom_topic_message message = { .topic = "a/b" };
  send_record record = { 0 };
  char out[32];
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&client, &message, on_send, &record),
      AZ_IOT_ERR_NOT_SUPPORTED);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(&client, "a", out, sizeof(out), NULL),
      AZ_IOT_ERR_NOT_SUPPORTED);
  assert_int_equal(record.calls, 0);
}

static void publish_sends_the_message_as_given(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  static const uint8_t payload[] = "21.5";
  const az_iot_mqtt_user_property properties[] = { { "site", "plant-3" }, { "unit", "" } };
  az_iot_mqttv5_custom_topic_message message = {
    .topic = "contoso/" DEVICE_ID "/telemetry/temp",
    .payload = payload,
    .payload_len = sizeof(payload) - 1,
    .qos = AZ_IOT_MQTT_QOS_1,
    .content_type = "text/plain",
    .user_properties = properties,
    .user_properties_count = ARRAY_SIZE(properties),
    .message_expiry_seconds = 30,
  };
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
      AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(test->mock), 1);
  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_int_equal(publish->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(publish->topic, "contoso/" DEVICE_ID "/telemetry/temp");
  assert_int_equal(publish->payload_len, sizeof(payload) - 1);
  assert_memory_equal(publish->payload, payload, sizeof(payload) - 1);
  assert_int_equal(publish->qos, AZ_IOT_MQTT_QOS_1);
  assert_false(publish->retain);
  assert_string_equal(publish->content_type, "text/plain");
  assert_int_equal(publish->message_expiry_seconds, 30);
  /* No properties of the SDK's own: the message is plain MQTT. */
  assert_int_equal(publish->user_properties_count, 2);
  assert_string_equal(az_iot_mock_call_user_property(publish, "site"), "plant-3");
  assert_string_equal(az_iot_mock_call_user_property(publish, "unit"), "");
  assert_string_equal(publish->user_type, "");
  assert_int_equal(record.calls, 0);

  assert_true(az_iot_mock_mqtt_client_inject_puback(test->mock, publish->packet_id, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  assert_int_equal(record.calls, 1);
  assert_int_equal(record.status, AZ_IOT_OK);
}

static void qos_0_completes_before_publish_returns(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  az_iot_mqttv5_custom_topic_message message = { .topic = "contoso/alerts" };
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
      AZ_IOT_OK);
  assert_int_equal(record.calls, 1);
  assert_int_equal(record.status, AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_int_equal(publish->qos, AZ_IOT_MQTT_QOS_0);
  assert_string_equal(publish->content_type, "");
  assert_int_equal(publish->user_properties_count, 0);
  assert_int_equal(publish->message_expiry_seconds, 0);
}

static void publish_rejects_invalid_arguments(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  send_record record = { 0 };
  az_iot_mqttv5_custom_topic_message message = { .topic = "a/b" };
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(NULL, &message, on_send, &record),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, NULL, on_send, &record),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);

  message.payload_len = 1;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
      AZ_IOT_ERR_INVALID_ARG);
  message.payload_len = 0;

  message.user_properties_count = 1;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
      AZ_IOT_ERR_INVALID_ARG);

  const az_iot_mqtt_user_property bad_properties[][1] = {
    { { NULL, "v" } },
    { { "", "v" } },
    { { "k", NULL } },
  };
  for (size_t i = 0; i < ARRAY_SIZE(bad_properties); ++i)
  {
    message.user_properties = bad_properties[i];
    assert_int_equal(
        az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
        AZ_IOT_ERR_INVALID_ARG);
  }
  message.user_properties = NULL;
  message.user_properties_count = 0;

  message.qos = AZ_IOT_MQTT_QOS_2;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
      AZ_IOT_ERR_INVALID_ARG);
  message.qos = (az_iot_mqtt_qos)7;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
      AZ_IOT_ERR_INVALID_ARG);

  assert_int_equal(az_iot_mock_mqtt_client_count_of(test->mock, AZ_IOT_MOCK_CALL_PUBLISH), 0);
  assert_int_equal(record.calls, 0);
}

static void topics_the_hub_would_refuse_are_rejected_locally(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  static const char* const invalid[] = {
    NULL,
    "",
    "a/+/b",
    "a/#",
    "+",
    "#",
    "ih",
    "ih/" DEVICE_ID "/srv/telemetry",
    "ih/x",
    "a\xC3", /* truncated sequence */
    "a\xC0\xAF", /* overlong '/' */
    "a\xED\xA0\x80", /* surrogate */
    "a\xF4\x90\x80\x80", /* above U+10FFFF */
    "a\xFF",
    "a/\x80",
  };
  for (size_t i = 0; i < ARRAY_SIZE(invalid); ++i)
  {
    assert_int_equal(publish_topic(test, invalid[i]), AZ_IOT_ERR_INVALID_ARG);
  }
  assert_int_equal(az_iot_mock_mqtt_client_count_of(test->mock, AZ_IOT_MOCK_CALL_PUBLISH), 0);
}

static void topics_outside_the_reserved_space_are_accepted(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  static const char* const valid[] = {
    "contoso/telemetry",
    "IH/x", /* the reserved level is matched case-sensitively, as on the hub */
    "ihub/x",
    "x/ih/y",
    "/leading/separator",
    "a//b",
    "a$b",
    "$custom/telemetry", /* the hub accepts `$` templates */
    "$SYS/x",
    "caf\xC3\xA9/\xE2\x82\xAC/\xF0\x9F\x98\x80",
  };
  for (size_t i = 0; i < ARRAY_SIZE(valid); ++i)
  {
    assert_int_equal(publish_topic(test, valid[i]), AZ_IOT_OK);
  }
  assert_int_equal(
      az_iot_mock_mqtt_client_count_of(test->mock, AZ_IOT_MOCK_CALL_PUBLISH), ARRAY_SIZE(valid));
}

static void topic_length_and_levels_are_bounded(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 2];
  memset(topic, 'a', AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN);
  topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN] = '\0';
  assert_int_equal(publish_topic(test, topic), AZ_IOT_OK);
  topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN] = 'a';
  topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1] = '\0';
  assert_int_equal(publish_topic(test, topic), AZ_IOT_ERR_INVALID_ARG);

  /* The limit is in bytes, so a multi-byte character counts in full. */
  memset(topic, 'a', AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN - 1);
  memcpy(&topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN - 1], "\xC3\xA9", 3);
  assert_int_equal(publish_topic(test, topic), AZ_IOT_ERR_INVALID_ARG);

  char levels[2 * (AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEVELS + 1)];
  size_t n = 0;
  for (size_t i = 0; i < AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEVELS; ++i)
  {
    if (i > 0)
    {
      levels[n++] = '/';
    }
    levels[n++] = 'l';
  }
  levels[n] = '\0';
  assert_int_equal(publish_topic(test, levels), AZ_IOT_OK);
  levels[n++] = '/';
  levels[n] = '\0';
  assert_int_equal(publish_topic(test, levels), AZ_IOT_ERR_INVALID_ARG);

  assert_int_equal(az_iot_mock_mqtt_client_count_of(test->mock, AZ_IOT_MOCK_CALL_PUBLISH), 2);
}

/* Every PUBACK outcome reaches the callback as the adapter classified it. */
static void a_failed_puback_reaches_the_callback_unchanged(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  static const az_iot_result statuses[]
      = { AZ_IOT_ERR_PUBLISH_REFUSED, AZ_IOT_ERR_BUSY, AZ_IOT_ERR_MQTT, AZ_IOT_ERR_MQTT };
  static const int32_t codes[] = { 0x87, 0x97, 0x80, 0 }; /* 0: not reported by the adapter. */
  for (size_t i = 0; i < ARRAY_SIZE(statuses); ++i)
  {
    az_iot_mock_mqtt_client_clear_calls(test->mock);
    az_iot_mqttv5_custom_topic_message message = { .topic = "x/y", .qos = AZ_IOT_MQTT_QOS_1 };
    send_record record = { 0 };
    assert_int_equal(
        az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
        AZ_IOT_OK);
    const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
    az_iot_mqtt_event ack = { 0 };
    ack.kind = AZ_IOT_MQTT_EVT_PUBLISH_ACK;
    ack.packet_id = publish->packet_id;
    ack.status = statuses[i];
    ack.protocol_code = codes[i];
    assert_true(az_iot_mock_mqtt_client_inject_event(test->mock, &ack));
    assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
    assert_int_equal(record.calls, 1);
    assert_int_equal(record.status, statuses[i]);
  }
}

static void a_disconnect_completes_the_pending_publish(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  az_iot_mqttv5_custom_topic_message message = { .topic = "x/y", .qos = AZ_IOT_MQTT_QOS_1 };
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
      AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(test->mock));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  assert_int_equal(record.calls, 1);
  assert_int_equal(record.status, AZ_IOT_ERR_NOT_CONNECTED);
}

static void publish_before_connect_is_not_connected(void** state)
{
  fixture* test = (fixture*)*state;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_init(&test->custom, &test->connection), AZ_IOT_OK);
  az_iot_mqttv5_custom_topic_message message = { .topic = "x/y", .qos = AZ_IOT_MQTT_QOS_1 };
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &record),
      AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(record.calls, 0);
}

static void a_full_puback_table_sends_nothing(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  az_iot_mqttv5_custom_topic_message message = { .topic = "x/y", .qos = AZ_IOT_MQTT_QOS_1 };
  send_record records[AZ_IOT_MAX_PENDING_PUBACKS];
  memset(records, 0, sizeof(records));
  for (size_t i = 0; i < ARRAY_SIZE(records); ++i)
  {
    assert_int_equal(
        az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &records[i]),
        AZ_IOT_OK);
  }

  size_t before = az_iot_mock_mqtt_client_call_count(test->mock);
  send_record overflow = { 0 };
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &overflow),
      AZ_IOT_ERR_BUSY);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(test->mock), before);
  assert_int_equal(overflow.calls, 0);

  /* QoS 0 needs no slot. */
  message.qos = AZ_IOT_MQTT_QOS_0;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&test->custom, &message, on_send, &overflow),
      AZ_IOT_OK);
  assert_int_equal(overflow.calls, 1);
}

static void format_topic_expands_every_device_id(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  static const struct
  {
    const char* template_;
    const char* expected;
  } cases[] = {
    { "contoso/telemetry", "contoso/telemetry" },
    { "contoso/{deviceId}/telemetry", "contoso/" DEVICE_ID "/telemetry" },
    { "{deviceId}", DEVICE_ID },
    { "{deviceId}/{deviceId}", DEVICE_ID "/" DEVICE_ID },
    { "a{deviceId}b", "a" DEVICE_ID "b" },
    { "", "" },
  };
  for (size_t i = 0; i < ARRAY_SIZE(cases); ++i)
  {
    char out[64];
    size_t out_len = 999;
    assert_int_equal(
        az_iot_mqttv5_custom_topic_client_format_topic(
            &test->custom, cases[i].template_, out, sizeof(out), &out_len),
        AZ_IOT_OK);
    assert_string_equal(out, cases[i].expected);
    assert_int_equal(out_len, strlen(cases[i].expected));
  }

  char out[64];
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &test->custom, "{deviceId}/x", out, sizeof(out), NULL),
      AZ_IOT_OK);
  assert_string_equal(out, DEVICE_ID "/x");
}

static void format_topic_rejects_other_variables(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  static const char* const invalid[] = {
    "contoso/{DeviceId}", "contoso/{deviceid}",
    "contoso/{moduleId}", "contoso/${client.authenticationName}",
    "contoso/{}",         "contoso/{deviceId",
    "contoso/deviceId}",  "contoso/{deviceId}/{other}",
  };
  for (size_t i = 0; i < ARRAY_SIZE(invalid); ++i)
  {
    char out[64];
    memset(out, 'x', sizeof(out));
    assert_int_equal(
        az_iot_mqttv5_custom_topic_client_format_topic(
            &test->custom, invalid[i], out, sizeof(out), NULL),
        AZ_IOT_ERR_INVALID_ARG);
    assert_string_equal(out, "");
  }
}

static void format_topic_reports_a_short_buffer(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  const char* template_ = "c/{deviceId}";
  const size_t needed = strlen("c/" DEVICE_ID) + 1;
  char out[64];
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &test->custom, template_, out, needed - 1, NULL),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(&test->custom, template_, out, needed, NULL),
      AZ_IOT_OK);
  assert_string_equal(out, "c/" DEVICE_ID);

  /* A literal run longer than the buffer fails before it is appended, before or after a variable.
   */
  static const char* const too_long[]
      = { "0123456789abcdef/{deviceId}", "{deviceId}/0123456789abcdef" };
  for (size_t i = 0; i < ARRAY_SIZE(too_long); ++i)
  {
    char small[16];
    memset(small, 'x', sizeof(small));
    assert_int_equal(
        az_iot_mqttv5_custom_topic_client_format_topic(
            &test->custom, too_long[i], small, sizeof(small), NULL),
        AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    assert_string_equal(small, "");
  }
}

static void format_topic_rejects_invalid_arguments(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  char out[16];
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(NULL, "a", out, sizeof(out), NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(&test->custom, NULL, out, sizeof(out), NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(&test->custom, "a", NULL, sizeof(out), NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(&test->custom, "a", out, 0, NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

/* Until connected, a DPS assignment may still change the device id. */
static void format_topic_needs_a_connection(void** state)
{
  fixture* test = (fixture*)*state;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_init(&test->custom, &test->connection), AZ_IOT_OK);
  char out[64];
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &test->custom, "c/{deviceId}", out, sizeof(out), NULL),
      AZ_IOT_ERR_NOT_CONNECTED);

  open_v5(test);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &test->custom, "c/{deviceId}", out, sizeof(out), NULL),
      AZ_IOT_OK);
  assert_string_equal(out, "c/" DEVICE_ID);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(test->mock));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &test->custom, "c/{deviceId}", out, sizeof(out), NULL),
      AZ_IOT_ERR_NOT_CONNECTED);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_contract, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_mqtt_v3_profile, setup_mqtt_v3, teardown),
    cmocka_unit_test_setup_teardown(an_uninitialized_client_is_not_supported, setup, teardown),
    cmocka_unit_test_setup_teardown(publish_sends_the_message_as_given, setup, teardown),
    cmocka_unit_test_setup_teardown(qos_0_completes_before_publish_returns, setup, teardown),
    cmocka_unit_test_setup_teardown(publish_rejects_invalid_arguments, setup, teardown),
    cmocka_unit_test_setup_teardown(
        topics_the_hub_would_refuse_are_rejected_locally, setup, teardown),
    cmocka_unit_test_setup_teardown(
        topics_outside_the_reserved_space_are_accepted, setup, teardown),
    cmocka_unit_test_setup_teardown(topic_length_and_levels_are_bounded, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_puback_reaches_the_callback_unchanged, setup, teardown),
    cmocka_unit_test_setup_teardown(a_disconnect_completes_the_pending_publish, setup, teardown),
    cmocka_unit_test_setup_teardown(publish_before_connect_is_not_connected, setup, teardown),
    cmocka_unit_test_setup_teardown(a_full_puback_table_sends_nothing, setup, teardown),
    cmocka_unit_test_setup_teardown(format_topic_expands_every_device_id, setup, teardown),
    cmocka_unit_test_setup_teardown(format_topic_rejects_other_variables, setup, teardown),
    cmocka_unit_test_setup_teardown(format_topic_reports_a_short_buffer, setup, teardown),
    cmocka_unit_test_setup_teardown(format_topic_rejects_invalid_arguments, setup, teardown),
    cmocka_unit_test_setup_teardown(format_topic_needs_a_connection, setup, teardown),
  };
  return cmocka_run_group_tests_name("mqttv5_custom_topic_client", tests, NULL, NULL);
}
