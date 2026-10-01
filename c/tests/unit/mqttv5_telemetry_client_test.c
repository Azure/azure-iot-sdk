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
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv5/az_iot_telemetry_client.h"

#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef struct log_capture
{
  int count;
  az_iot_log_level last_level;
  char last[AZ_IOT_LOG_MESSAGE_MAX];
} log_capture;

static void capture_log(
    void* user_ctx,
    az_iot_log_level level,
    const char* file,
    int line,
    const char* msg)
{
  log_capture* c = (log_capture*)user_ctx;
  (void)file;
  (void)line;
  if (msg == NULL)
  {
    return;
  }
  c->count++;
  c->last_level = level;
  snprintf(c->last, sizeof(c->last), "%s", msg);
}

static void install_capture(log_capture* c, az_iot_log_level min_level)
{
  memset(c, 0, sizeof(*c));
  az_iot_log_sink sink;
  sink.sink = capture_log;
  sink.user_ctx = c;
  sink.min_level = min_level;
  az_iot_log_set_global_sink(&sink);
}

typedef struct fixture
{
  az_iot_connection_client connection;
  az_iot_mqttv5_telemetry_client telemetry;
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
    az_iot_mqttv5_telemetry_client_deinit(&test->telemetry);
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
  assert_true(az_iot_mock_mqtt_client_inject_connected(test->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
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

static void connect_and_init(fixture* test)
{
  open_v5(test);
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_init(&test->telemetry, &test->connection), AZ_IOT_OK);
  az_iot_mock_mqtt_client_clear_calls(test->mock);
}

static void init_contract(void** state)
{
  fixture* test = (fixture*)*state;
  az_iot_connection_client* dummy = (az_iot_connection_client*)(uintptr_t)1;
  assert_int_equal(az_iot_mqttv5_telemetry_client_init(NULL, dummy), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_init(&test->telemetry, NULL), AZ_IOT_ERR_INVALID_ARG);
  az_iot_mqttv5_telemetry_client_deinit(NULL);

  memset(&test->telemetry, 0xEE, sizeof(test->telemetry));
  /* Init records the generation this client needs rather than reading a live
   * connection, so it succeeds before open(). */
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_init(&test->telemetry, &test->connection), AZ_IOT_OK);
  assert_ptr_equal(test->telemetry._internal.conn, &test->connection);
  az_iot_mqttv5_telemetry_client_deinit(&test->telemetry);
}

static void init_rejects_mqtt_v3_profile(void** state)
{
  fixture* test = (fixture*)*state;
  open_mqtt_v3(test);

  memset(&test->telemetry, 0xEE, sizeof(test->telemetry));
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_init(&test->telemetry, &test->connection),
      AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);
  assert_null(test->telemetry._internal.conn);
}

static void lifecycle_is_deterministic(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  az_iot_mqttv5_telemetry_client local;
  memset(&local, 0xEE, sizeof(local));
  assert_int_equal(az_iot_mqttv5_telemetry_client_init(&local, &test->connection), AZ_IOT_OK);
  assert_ptr_equal(local._internal.conn, &test->connection);
  az_iot_mqttv5_telemetry_client_deinit(&local);
  az_iot_mqttv5_telemetry_client_deinit(&local);

  az_iot_telemetry_message message = { 0 };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&local, &message, NULL, NULL), AZ_IOT_ERR_NOT_SUPPORTED);
}

static void send_rejects_invalid_arguments(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  az_iot_telemetry_message message = { .payload_len = 1 };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  message.payload_len = 0;
  message.properties_count = 1;
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(NULL, &message, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, NULL, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

static void send_uses_the_v5_wire_shape_and_waits_for_puback(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  static const uint8_t payload[] = "{\"t\":21}";
  az_iot_telemetry_message message = { .payload = payload, .payload_len = sizeof(payload) - 1 };
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, on_send, &record), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(test->mock), 1);
  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_int_equal(publish->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(publish->topic, "ih/ut-device/srv/telemetry");
  assert_memory_equal(publish->payload, payload, sizeof(payload) - 1);
  assert_int_equal(publish->qos, AZ_IOT_MQTT_QOS_1);
  assert_false(publish->retain);
  assert_false(record.fired);

  assert_true(az_iot_mock_mqtt_client_inject_puback(test->mock, publish->packet_id, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  assert_true(record.fired);
  assert_int_equal(record.status, AZ_IOT_OK);
}

static void metadata_uses_native_v5_fields(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  const az_iot_telemetry_property properties[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "text/plain" },
    { AZ_IOT_MSG_PROP_MESSAGE_ID, "m-1" },
    { "site", "plant-3" },
    { "unit", "celsius" },
    { NULL, "orphan" },
    { "", "orphan" },
    { "flag", NULL },
  };
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(publish->user_type, "telemetry:1");
  assert_string_equal(publish->content_type, "text/plain");
  assert_string_equal(az_iot_mock_call_user_property(publish, "content-type"), "text/plain");
  assert_string_equal(az_iot_mock_call_user_property(publish, "site"), "plant-3");
  assert_string_equal(az_iot_mock_call_user_property(publish, "unit"), "celsius");
  assert_string_equal(az_iot_mock_call_user_property(publish, "flag"), "");
  /* Content type is the one system property with a native v5 field, so it does
   * not also travel under its own name. */
  assert_null(az_iot_mock_call_user_property(publish, AZ_IOT_MSG_PROP_CONTENT_TYPE));
  /* Every other one has no v5 equivalent and must survive the trip: dropping it
   * would lose the caller's data with nothing said. */
  assert_string_equal(az_iot_mock_call_user_property(publish, AZ_IOT_MSG_PROP_MESSAGE_ID), "m-1");
  assert_int_equal(publish->user_properties_count, 6);
}

/* The header promises these reach the service on both generations. mqttv3 encodes
 * them into the topic; nothing but this test stops mqttv5 quietly discarding
 * them, which is how they were lost between the split and now. */
static void every_system_property_reaches_the_wire(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  const az_iot_telemetry_property properties[] = {
    { AZ_IOT_MSG_PROP_CONTENT_ENCODING, "utf-8" },
    { AZ_IOT_MSG_PROP_MESSAGE_ID, "m-7" },
    { AZ_IOT_MSG_PROP_CORRELATION_ID, "c-7" },
    { AZ_IOT_MSG_PROP_USER_ID, "u-7" },
    { AZ_IOT_MSG_PROP_CREATION_TIME, "2026-09-13T00:00:00Z" },
    { AZ_IOT_MSG_PROP_COMPONENT_NAME, "thermostat" },
  };
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(
      az_iot_mock_call_user_property(publish, AZ_IOT_MSG_PROP_CONTENT_ENCODING), "utf-8");
  assert_string_equal(az_iot_mock_call_user_property(publish, AZ_IOT_MSG_PROP_MESSAGE_ID), "m-7");
  assert_string_equal(
      az_iot_mock_call_user_property(publish, AZ_IOT_MSG_PROP_CORRELATION_ID), "c-7");
  assert_string_equal(az_iot_mock_call_user_property(publish, AZ_IOT_MSG_PROP_USER_ID), "u-7");
  assert_string_equal(
      az_iot_mock_call_user_property(publish, AZ_IOT_MSG_PROP_CREATION_TIME),
      "2026-09-13T00:00:00Z");
  assert_string_equal(
      az_iot_mock_call_user_property(publish, AZ_IOT_MSG_PROP_COMPONENT_NAME), "thermostat");
  /* The six above plus the type and content-type the client adds. */
  assert_int_equal(publish->user_properties_count, 8);
}

/* Overflow still drops, because the array is fixed -- but it must say so, and
 * name what went missing. A silent drop here is the same defect this file just
 * fixed, one cause further along. */
static void properties_past_the_cap_are_dropped_with_a_warning(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  /* Two slots are already spent on type and content-type, so the last two of
   * these cannot fit whatever the cap is set to. */
  enum
  {
    k_carried = AZ_IOT_MQTTV5_TELEMETRY_MAX_USER_PROPERTIES - 2,
    k_offered = k_carried + 2,
  };
  char keys[k_offered][16];
  az_iot_telemetry_property properties[k_offered];
  for (int i = 0; i < k_offered; ++i)
  {
    snprintf(keys[i], sizeof(keys[i]), "k%d", i);
    properties[i].key = keys[i];
    properties[i].value = "v";
  }
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };

  log_capture log;
  install_capture(&log, AZ_IOT_LOG_LEVEL_WARN);
  az_iot_result result
      = az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL);
  az_iot_log_set_global_sink(NULL);

  assert_int_equal(result, AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_int_equal(publish->user_properties_count, AZ_IOT_MQTTV5_TELEMETRY_MAX_USER_PROPERTIES);

  char last_carried[16];
  char first_dropped[16];
  snprintf(last_carried, sizeof(last_carried), "k%d", k_carried - 1);
  snprintf(first_dropped, sizeof(first_dropped), "k%d", k_carried);
  assert_string_equal(az_iot_mock_call_user_property(publish, last_carried), "v");
  assert_null(az_iot_mock_call_user_property(publish, first_dropped));

  /* Naming the first casualty is what makes the warning actionable, and warning
   * once rather than per property is what keeps it readable. */
  assert_int_equal(log.count, 1);
  assert_int_equal(log.last_level, AZ_IOT_LOG_LEVEL_WARN);
  assert_non_null(strstr(log.last, first_dropped));
}

/* The fix for D-11 narrowed a prefix test to an exact one. Anything that widens
 * it back -- "$." again, or a startswith on "$" -- silently eats a property the
 * caller set, so pin a name the client knows nothing about. */
static void an_unknown_system_property_still_travels(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  const az_iot_telemetry_property properties[] = {
    { "$.unknown", "keep-me" },
    { "$notdot", "keep-me-too" },
  };
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(az_iot_mock_call_user_property(publish, "$.unknown"), "keep-me");
  assert_string_equal(az_iot_mock_call_user_property(publish, "$notdot"), "keep-me-too");
}

/* "$.CT" is not "$.ct". Matching it loosely would take a caller's ordinary
 * property and quietly redirect it into the Content Type field. */
static void content_type_is_matched_case_sensitively(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  const az_iot_telemetry_property properties[] = {
    { "$.CT", "text/plain" },
  };
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(publish->content_type, "application/json");
  assert_string_equal(az_iot_mock_call_user_property(publish, "$.CT"), "text/plain");
}

/* Two content types is the caller contradicting themselves. Taking the first is
 * a choice, not an accident, so it is worth pinning -- and neither may travel
 * under its own name, or the service would see a third answer. */
static void a_repeated_content_type_resolves_to_the_first(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  const az_iot_telemetry_property properties[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "text/plain" },
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/xml" },
  };
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(publish->content_type, "text/plain");
  assert_string_equal(az_iot_mock_call_user_property(publish, "content-type"), "text/plain");
  assert_null(az_iot_mock_call_user_property(publish, AZ_IOT_MSG_PROP_CONTENT_TYPE));
  assert_int_equal(publish->user_properties_count, 2);
}

/* The other side of the cap: a message that exactly fills it loses nothing and
 * must not warn. An off-by-one in the bound shows up here and nowhere else. */
static void filling_the_cap_exactly_carries_everything_and_says_nothing(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  /* Two of the slots are the type and content-type the client adds. */
  enum
  {
    k_carried = AZ_IOT_MQTTV5_TELEMETRY_MAX_USER_PROPERTIES - 2
  };
  char keys[k_carried][16];
  az_iot_telemetry_property properties[k_carried];
  for (int i = 0; i < k_carried; ++i)
  {
    snprintf(keys[i], sizeof(keys[i]), "k%d", i);
    properties[i].key = keys[i];
    properties[i].value = "v";
  }
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };

  log_capture log;
  install_capture(&log, AZ_IOT_LOG_LEVEL_WARN);
  az_iot_result result
      = az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL);
  az_iot_log_set_global_sink(NULL);

  assert_int_equal(result, AZ_IOT_OK);
  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_int_equal(publish->user_properties_count, AZ_IOT_MQTTV5_TELEMETRY_MAX_USER_PROPERTIES);

  char last_carried[16];
  snprintf(last_carried, sizeof(last_carried), "k%d", k_carried - 1);
  assert_string_equal(az_iot_mock_call_user_property(publish, "k0"), "v");
  assert_string_equal(az_iot_mock_call_user_property(publish, last_carried), "v");
  assert_int_equal(log.count, 0);
}

/* The v5 path carries values as-is. MQTTv3 has to percent-encode the same
 * bytes because they go in the topic; borrowing that encoder here would corrupt
 * every value containing a reserved character. */
static void values_are_not_encoded_on_the_v5_path(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  const az_iot_telemetry_property properties[] = {
    { "reserved", "a&b=c d%e/f" },
    { "wildcards", "a#b+c" },
  };
  az_iot_telemetry_message message = {
    .properties = properties,
    .properties_count = ARRAY_SIZE(properties),
  };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);

  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(az_iot_mock_call_user_property(publish, "reserved"), "a&b=c d%e/f");
  assert_string_equal(az_iot_mock_call_user_property(publish, "wildcards"), "a#b+c");
  /* The topic carries none of it, which is the whole reason no encoding is
   * needed. */
  assert_string_equal(publish->topic, "ih/ut-device/srv/telemetry");
}

static void content_type_defaults_to_json(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  az_iot_telemetry_message message = { 0 };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, NULL, NULL), AZ_IOT_OK);
  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_string_equal(publish->content_type, "application/json");
  assert_string_equal(az_iot_mock_call_user_property(publish, "content-type"), "application/json");
}

static void empty_payload_is_valid_and_publish_failures_are_returned(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  az_iot_telemetry_message empty = { 0 };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &empty, NULL, NULL), AZ_IOT_OK);
  const az_iot_mock_call* publish = az_iot_mock_mqtt_client_call_at(test->mock, 0);
  assert_int_equal(publish->payload_len, 0);

  az_iot_mock_mqtt_client_clear_calls(test->mock);
  az_iot_mock_mqtt_client_set_next_result(
      test->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_NOT_CONNECTED);
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &empty, on_send, &record),
      AZ_IOT_ERR_NOT_CONNECTED);
  assert_false(record.fired);
}

static void a_disconnect_completes_the_pending_send(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  az_iot_telemetry_message message = { 0 };
  send_record record = { 0 };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, on_send, &record), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(test->mock));
  assert_int_equal(az_iot_connection_client_do_work(&test->connection, 0), AZ_IOT_OK);
  assert_true(record.fired);
  assert_int_equal(record.status, AZ_IOT_ERR_NOT_CONNECTED);
}

static void a_full_puback_table_sends_nothing(void** state)
{
  fixture* test = (fixture*)*state;
  connect_and_init(test);

  az_iot_telemetry_message message = { 0 };
  send_record records[AZ_IOT_MAX_PENDING_PUBACKS];
  memset(records, 0, sizeof(records));
  for (size_t i = 0; i < ARRAY_SIZE(records); ++i)
  {
    assert_int_equal(
        az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, on_send, &records[i]),
        AZ_IOT_OK);
  }

  size_t before = az_iot_mock_mqtt_client_call_count(test->mock);
  send_record overflow = { 0 };
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&test->telemetry, &message, on_send, &overflow),
      AZ_IOT_ERR_BUSY);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(test->mock), before);
  assert_false(overflow.fired);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_contract, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_mqtt_v3_profile, setup_mqtt_v3, teardown),
    cmocka_unit_test_setup_teardown(lifecycle_is_deterministic, setup, teardown),
    cmocka_unit_test_setup_teardown(send_rejects_invalid_arguments, setup, teardown),
    cmocka_unit_test_setup_teardown(
        send_uses_the_v5_wire_shape_and_waits_for_puback, setup, teardown),
    cmocka_unit_test_setup_teardown(metadata_uses_native_v5_fields, setup, teardown),
    cmocka_unit_test_setup_teardown(every_system_property_reaches_the_wire, setup, teardown),
    cmocka_unit_test_setup_teardown(
        properties_past_the_cap_are_dropped_with_a_warning, setup, teardown),
    cmocka_unit_test_setup_teardown(an_unknown_system_property_still_travels, setup, teardown),
    cmocka_unit_test_setup_teardown(content_type_is_matched_case_sensitively, setup, teardown),
    cmocka_unit_test_setup_teardown(a_repeated_content_type_resolves_to_the_first, setup, teardown),
    cmocka_unit_test_setup_teardown(
        filling_the_cap_exactly_carries_everything_and_says_nothing, setup, teardown),
    cmocka_unit_test_setup_teardown(values_are_not_encoded_on_the_v5_path, setup, teardown),
    cmocka_unit_test_setup_teardown(content_type_defaults_to_json, setup, teardown),
    cmocka_unit_test_setup_teardown(
        empty_payload_is_valid_and_publish_failures_are_returned, setup, teardown),
    cmocka_unit_test_setup_teardown(a_disconnect_completes_the_pending_send, setup, teardown),
    cmocka_unit_test_setup_teardown(a_full_puback_table_sends_nothing, setup, teardown),
  };
  return cmocka_run_group_tests_name("mqttv5_telemetry_client", tests, NULL, NULL);
}
