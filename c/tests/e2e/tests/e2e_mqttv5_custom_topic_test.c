// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end: az_iot_mqttv5_custom_topic_client on an mqttv5 hub.
 *
 * The hub must have the custom topic templates e2e/{deviceId}/# and e2e/shared/#.
 * The device provisions through DPS (or connects directly), as e2e_device.h describes;
 * DPS must assign it to that mqttv5 hub. Routed messages are read from the hub's
 * events endpoint (IOTHUB_* variables, see az_iot_e2e_service.h).
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot.h"

#include "az_iot_e2e_service.h"
#include "e2e_device.h"
#include "e2e_log.h"

#define E2E_ROUTE_TIMEOUT_S 90
#define E2E_NOT_ROUTED_WAIT_S 30
#define E2E_ACK_TIMEOUT_S 30
#define E2E_CONNECT_TIMEOUT_S 60
#define E2E_SERVICE_CONNECT_TIMEOUT_S 45
#define E2E_PUMP_MS 20

typedef struct
{
  int done;
  az_iot_result status;
} publish_record;

#define E2E_MAX_PUBLISHES 16

typedef struct
{
  e2e_device dev;
  az_iot_e2e_service* service;
  az_iot_mqttv5_custom_topic_client custom;
  bool custom_ok;
  /* Callback contexts live as long as the connection: a late PUBACK may still complete one. */
  publish_record records[E2E_MAX_PUBLISHES];
  size_t records_used;
} fixture;

static fixture g_fx;

static void on_publish(az_iot_result status, void* user_ctx)
{
  publish_record* record = (publish_record*)user_ctx;
  record->status = status;
  record->done = 1;
}

static publish_record* new_record(fixture* fx)
{
  assert_true(fx->records_used < E2E_MAX_PUBLISHES);
  return &fx->records[fx->records_used++];
}

/* Set while the hub does not route custom topic messages to the events endpoint: the routed
 * cases then skip instead of failing when nothing arrives. */
static bool routing_check_skipped(void)
{
  const char* skip_routing = getenv("AZ_IOT_E2E_SKIP_CUSTOM_TOPIC_ROUTING");
  return skip_routing != NULL && skip_routing[0] != '\0' && strcmp(skip_routing, "0") != 0;
}

static void assert_routed(bool seen)
{
  if (!seen && routing_check_skipped())
  {
    printf("[e2e] not routed; skipped (AZ_IOT_E2E_SKIP_CUSTOM_TOPIC_ROUTING)\n");
    skip();
  }
  assert_true(seen);
}

/* A refused publish: PUBACK 0x87, or AZ_IOT_ERR_MQTT from an adapter that does not report the code.
 */
static void assert_refused(const publish_record* record)
{
  assert_true(record->done);
  if (record->status != AZ_IOT_ERR_PUBLISH_REFUSED && record->status != AZ_IOT_ERR_MQTT)
  {
    fail_msg("expected a refusal, got %s", az_iot_result_to_string(record->status));
  }
}

static void pump(fixture* fx) { e2e_device_do_work(&fx->dev, E2E_PUMP_MS); }

static bool wait_connected(fixture* fx)
{
  for (time_t start = time(NULL); (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S;)
  {
    if (fx->dev.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      return true;
    }
    pump(fx);
  }
  return fx->dev.conn_state == AZ_IOT_CONN_STATE_CONNECTED;
}

static void make_marker(char* out, size_t cap, const char* tag)
{
  static unsigned counter = 0;
  snprintf(
      out,
      cap,
      "%s-%08lx%04x%03x",
      tag,
      (unsigned long)time(NULL),
      (unsigned)rand() & 0xffffu,
      (counter++) & 0xfffu);
}

static int group_setup(void** state)
{
  memset(&g_fx, 0, sizeof(g_fx));
  srand((unsigned)time(NULL));
  e2e_install_log_sink();

  const char* svc_err = NULL;
  g_fx.service = az_iot_e2e_service_create(&svc_err);
  if (g_fx.service == NULL)
  {
    fprintf(stderr, "[e2e] service create failed: %s\n", svc_err ? svc_err : "unknown");
    return -1;
  }
  if (e2e_device_connect(&g_fx.dev) != 0)
  {
    return -1;
  }

  az_iot_hub_profile profile;
  if (az_iot_connection_client_get_hub_profile(&g_fx.dev.conn, &profile) != AZ_IOT_OK
      || profile.connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    fprintf(stderr, "[e2e] the device is not on an mqttv5 hub\n");
    return -1;
  }
  if (az_iot_mqttv5_custom_topic_client_init(&g_fx.custom, &g_fx.dev.conn) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] custom topic client init failed\n");
    return -1;
  }
  g_fx.custom_ok = true;

  *state = &g_fx;
  return 0;
}

static int group_teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx == NULL)
  {
    fx = &g_fx;
  }
  if (fx->custom_ok)
  {
    az_iot_mqttv5_custom_topic_client_deinit(&fx->custom);
  }
  e2e_device_disconnect(&fx->dev);
  if (fx->service != NULL)
  {
    az_iot_e2e_service_destroy(fx->service);
  }
  return 0;
}

static void watch_begin(fixture* fx)
{
  bool watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  for (time_t start = time(NULL);
       !watching && (time(NULL) - start) < E2E_SERVICE_CONNECT_TIMEOUT_S;)
  {
    for (int i = 0; i < 50; ++i)
    {
      pump(fx);
    }
    watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  }
  if (!watching)
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fprintf(stderr, "[e2e] telemetry watch failed: %s\n", err ? err : "unknown");
  }
  assert_true(watching);
}

/* Pumps device and service until @p marker is routed or @p timeout_s passes, then lets the
 * publish complete. */
static bool wait_routed(fixture* fx, const char* marker, int timeout_s, publish_record* record)
{
  bool seen = false;
  for (time_t start = time(NULL); (time(NULL) - start) < timeout_s;)
  {
    pump(fx);
    assert_true(az_iot_e2e_service_do_work(fx->service, E2E_PUMP_MS));
    if (az_iot_e2e_service_telemetry_seen(fx->service, marker))
    {
      seen = true;
      break;
    }
  }
  for (time_t start = time(NULL); !record->done && (time(NULL) - start) < E2E_ACK_TIMEOUT_S;)
  {
    pump(fx);
  }
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  return seen;
}

/* Publishes a marked message to @p topic_template; returns whether it was routed. */
static bool publish_and_watch(
    fixture* fx,
    const char* topic_template,
    az_iot_mqtt_qos qos,
    int timeout_s,
    publish_record** out_record)
{
  assert_true(wait_connected(fx));
  publish_record* record = new_record(fx);
  *out_record = record;
  char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1];
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &fx->custom, topic_template, topic, sizeof(topic), NULL),
      AZ_IOT_OK);

  char marker[64];
  make_marker(marker, sizeof(marker), "ct");
  char payload[128];
  snprintf(payload, sizeof(payload), "{\"marker\":\"%s\"}", marker);

  watch_begin(fx);
  const az_iot_mqtt_user_property properties[] = { { "e2e", "custom-topic" } };
  az_iot_mqttv5_custom_topic_message message = { 0 };
  message.topic = topic;
  message.payload = (const uint8_t*)payload;
  message.payload_len = strlen(payload);
  message.qos = qos;
  message.content_type = "application/json";
  message.user_properties = properties;
  message.user_properties_count = sizeof(properties) / sizeof(properties[0]);
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_publish(&fx->custom, &message, on_publish, record),
      AZ_IOT_OK);

  const bool seen = wait_routed(fx, marker, timeout_s, record);
  printf(
      "[e2e] '%s' qos %d: completion %s, routed %s\n",
      topic,
      (int)qos,
      record->done ? az_iot_result_to_string(record->status) : "(none)",
      seen ? "yes" : "no");
  return seen;
}

/* Baseline: telemetry over the same session reaches the events endpoint, so a custom topic
 * failure below is about custom topics, not the environment. */
static void baseline_telemetry_is_routed(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char marker[64];
  make_marker(marker, sizeof(marker), "tm");
  char payload[128];
  snprintf(payload, sizeof(payload), "{\"marker\":\"%s\"}", marker);

  watch_begin(fx);
  az_iot_mqttv5_telemetry_client telemetry;
  assert_int_equal(az_iot_mqttv5_telemetry_client_init(&telemetry, &fx->dev.conn), AZ_IOT_OK);
  az_iot_telemetry_message message = { 0 };
  message.payload = (const uint8_t*)payload;
  message.payload_len = strlen(payload);
  publish_record* record = new_record(fx);
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&telemetry, &message, on_publish, record), AZ_IOT_OK);
  const bool seen = wait_routed(fx, marker, E2E_ROUTE_TIMEOUT_S, record);
  az_iot_mqttv5_telemetry_client_deinit(&telemetry);

  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
  assert_true(seen);
}

static void qos1_to_a_device_template_is_routed(void** state)
{
  publish_record* record;
  const bool seen = publish_and_watch(
      (fixture*)*state,
      "e2e/{deviceId}/telemetry",
      AZ_IOT_MQTT_QOS_1,
      E2E_ROUTE_TIMEOUT_S,
      &record);
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
  assert_routed(seen);
}

static void qos0_to_a_shared_template_is_routed(void** state)
{
  publish_record* record;
  const bool seen = publish_and_watch(
      (fixture*)*state, "e2e/shared/alerts", AZ_IOT_MQTT_QOS_0, E2E_ROUTE_TIMEOUT_S, &record);
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
  assert_routed(seen);
}

static void a_topic_no_template_allows_is_refused(void** state)
{
  publish_record* record;
  const bool seen = publish_and_watch(
      (fixture*)*state,
      "e2e-unlisted/telemetry",
      AZ_IOT_MQTT_QOS_1,
      E2E_NOT_ROUTED_WAIT_S,
      &record);
  assert_refused(record);
  assert_false(seen);
}

static void another_device_id_is_refused(void** state)
{
  publish_record* record;
  const bool seen = publish_and_watch(
      (fixture*)*state,
      "e2e/not-{deviceId}/telemetry",
      AZ_IOT_MQTT_QOS_1,
      E2E_NOT_ROUTED_WAIT_S,
      &record);
  assert_refused(record);
  assert_false(seen);
}

/* A refusal leaves the session usable. */
static void publishing_continues_after_a_refusal(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->dev.conn_state, AZ_IOT_CONN_STATE_CONNECTED);
  publish_record* record;
  (void)publish_and_watch(fx, "e2e/{deviceId}/after", AZ_IOT_MQTT_QOS_1, 1, &record);
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(baseline_telemetry_is_routed),
    cmocka_unit_test(qos1_to_a_device_template_is_routed),
    cmocka_unit_test(qos0_to_a_shared_template_is_routed),
    cmocka_unit_test(a_topic_no_template_allows_is_refused),
    cmocka_unit_test(another_device_id_is_refused),
    cmocka_unit_test(publishing_continues_after_a_refusal),
  };
  return cmocka_run_group_tests_name("e2e_mqttv5_custom_topic", tests, group_setup, group_teardown);
}
