// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end: az_iot_mqttv5_custom_topic_client on an mqttv5 hub.
 *
 * The hub must have the custom topic templates e2e/{deviceId}/# and e2e/shared/#.
 * The device provisions through DPS (e2e_device.h), which must assign it to that mqttv5 hub. Routed
 * messages are read from the hub's events endpoint (IOTHUB_* variables, see az_iot_e2e_service.h).
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
#include "support/test_env.h"

#define E2E_ROUTE_TIMEOUT_S 90
#define E2E_ACK_TIMEOUT_S 30
#define E2E_CONNECT_TIMEOUT_S 60
#define E2E_SERVICE_CONNECT_TIMEOUT_S 45
#define E2E_PUMP_MS 20
/* tag, '-', device id (<= 128), '-', 11 hex digits, NUL. */
#define E2E_MARKER_MAX 160

typedef struct
{
  int done;
  az_iot_result status;
  int32_t reason_code;
  char reason[128]; /* The hub's Reason String, copied; "" when none. */
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
  /* Hub transitions out of CONNECTED; a refusal must not cause one. */
  int hub_drops;
  bool hub_connected;
  /* Provisioning sessions that reached CONNECTED after group setup. */
  int dps_sessions;
} fixture;

static fixture g_fx;

static void on_publish(const az_iot_publish_result* result, void* user_ctx)
{
  publish_record* record = (publish_record*)user_ctx;
  record->status = result->status;
  record->reason_code = result->reason_code;
  size_t n = result->reason_string_len < sizeof(record->reason) - 1 ? result->reason_string_len
                                                                    : sizeof(record->reason) - 1;
  if (n > 0)
  {
    memcpy(record->reason, result->reason_string, n);
  }
  record->reason[n] = '\0';
  record->done = 1;
}

static void on_telemetry_sent(az_iot_result status, void* user_ctx)
{
  publish_record* record = (publish_record*)user_ctx;
  record->status = status;
  record->done = 1;
}

static void on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope == AZ_IOT_CONN_SCOPE_DPS && event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    fx->dps_sessions++;
  }
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }
  const bool connected = event->state == AZ_IOT_CONN_STATE_CONNECTED;
  if (fx->hub_connected && !connected)
  {
    fx->hub_drops++;
  }
  fx->hub_connected = connected;
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
  char* skip_routing = az_iot_test_env_dup("AZ_IOT_E2E_SKIP_CUSTOM_TOPIC_ROUTING");
  const bool skipped = skip_routing != NULL && strcmp(skip_routing, "0") != 0;
  free(skip_routing);
  return skipped;
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

/* A refused publish: PUBACK 0x87 (not authorized), which every bundled adapter reports. */
static void assert_refused(const publish_record* record)
{
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_ERR_PUBLISH_REFUSED);
  assert_int_equal(record->reason_code, 0x87);
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

/* Unique per run: the device id is per run (a device holds one connection), and the
 * events endpoint is shared by concurrent runs. */
static void make_marker(char* out, size_t cap, const char* tag, const char* device_id)
{
  static unsigned counter = 0;
  snprintf(
      out, cap, "%s-%s-%08lx%03x", tag, device_id, (unsigned long)time(NULL), (counter++) & 0xfffu);
}

/** @brief Number of `/`-separated levels in @p topic. */
static size_t count_levels(const char* topic)
{
  size_t levels = 1;
  for (; *topic != '\0'; ++topic)
  {
    levels += *topic == '/' ? 1u : 0u;
  }
  return levels;
}

static int group_setup(void** state)
{
  memset(&g_fx, 0, sizeof(g_fx));
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
  g_fx.hub_connected = true;
  if (az_iot_connection_client_add_state_observer(&g_fx.dev.conn, on_state, &g_fx) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] state observer registration failed\n");
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
  if (fx->dev.conn_ok)
  {
    (void)az_iot_connection_client_remove_state_observer(&fx->dev.conn, on_state, fx);
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

/** @brief Publishes a marked message to the concrete @p topic; returns whether it was routed. */
static bool publish_topic_and_watch(
    fixture* fx,
    const char* topic,
    az_iot_mqtt_qos qos,
    const az_iot_mqtt_user_property* properties,
    size_t properties_count,
    int timeout_s,
    publish_record** out_record)
{
  assert_true(wait_connected(fx));
  publish_record* record = new_record(fx);
  *out_record = record;

  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "ct", fx->dev.device_id);
  char payload[E2E_MARKER_MAX + 16];
  snprintf(payload, sizeof(payload), "{\"marker\":\"%s\"}", marker);

  watch_begin(fx);
  az_iot_mqttv5_custom_topic_message message = { 0 };
  message.topic = topic;
  message.payload = (const uint8_t*)payload;
  message.payload_len = strlen(payload);
  message.qos = qos;
  message.content_type = "application/json";
  message.user_properties = properties;
  message.user_properties_count = properties_count;
  const az_iot_result rc
      = az_iot_mqttv5_custom_topic_client_publish(&fx->custom, &message, on_publish, record);
  if (rc != AZ_IOT_OK)
  {
    az_iot_e2e_service_telemetry_watch_end(fx->service);
    fail_msg("publish to '%s' returned %s", topic, az_iot_result_to_string(rc));
  }

  const bool seen = wait_routed(fx, marker, timeout_s, record);
  printf(
      "[e2e] '%s' (%u bytes, %u levels) qos %d, %u properties: completion %s (reason code 0x%02X, "
      "\"%s\"), routed %s\n",
      topic,
      (unsigned)strlen(topic),
      (unsigned)count_levels(topic),
      (int)qos,
      (unsigned)properties_count,
      record->done ? az_iot_result_to_string(record->status) : "(none)",
      (unsigned)record->reason_code,
      record->reason,
      seen ? "yes" : "no");
  return seen;
}

/** @brief Formats @p topic_template, then publishes it like publish_topic_and_watch(). */
static bool publish_and_watch(
    fixture* fx,
    const char* topic_template,
    az_iot_mqtt_qos qos,
    int timeout_s,
    publish_record** out_record)
{
  assert_true(wait_connected(fx));
  char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1];
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &fx->custom, topic_template, topic, sizeof(topic), NULL),
      AZ_IOT_OK);
  const az_iot_mqtt_user_property properties[] = { { "e2e", "custom-topic" } };
  return publish_topic_and_watch(
      fx,
      topic,
      qos,
      properties,
      sizeof(properties) / sizeof(properties[0]),
      timeout_s,
      out_record);
}

/** @brief Builds `e2e/<device id>/len/` padded with `x` to exactly @p length bytes. */
static void make_topic_of_length(fixture* fx, char* out, size_t out_size, size_t length)
{
  size_t used = 0;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &fx->custom, "e2e/{deviceId}/len/", out, out_size, &used),
      AZ_IOT_OK);
  assert_true(used < length && length < out_size);
  memset(out + used, 'x', length - used);
  out[length] = '\0';
}

/** @brief Builds `e2e/<device id>/l2/l3/...` with exactly @p levels levels. */
static void make_topic_of_levels(fixture* fx, char* out, size_t out_size, size_t levels)
{
  size_t used = 0;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &fx->custom, "e2e/{deviceId}", out, out_size, &used),
      AZ_IOT_OK);
  for (size_t level = count_levels(out); level < levels; ++level)
  {
    const int written = snprintf(out + used, out_size - used, "/l%u", (unsigned)level);
    assert_true(written > 0 && (size_t)written < out_size - used);
    used += (size_t)written;
  }
  assert_int_equal(count_levels(out), levels);
}

/* Baseline: telemetry over the same session reaches the events endpoint, so a custom topic
 * failure below is about custom topics, not the environment. */
static void baseline_telemetry_is_routed(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "tm", fx->dev.device_id);
  char payload[E2E_MARKER_MAX + 16];
  snprintf(payload, sizeof(payload), "{\"marker\":\"%s\"}", marker);

  watch_begin(fx);
  az_iot_mqttv5_telemetry_client telemetry;
  assert_int_equal(az_iot_mqttv5_telemetry_client_init(&telemetry, &fx->dev.conn), AZ_IOT_OK);
  az_iot_telemetry_message message = { 0 };
  message.payload = (const uint8_t*)payload;
  message.payload_len = strlen(payload);
  publish_record* record = new_record(fx);
  assert_int_equal(
      az_iot_mqttv5_telemetry_client_send(&telemetry, &message, on_telemetry_sent, record),
      AZ_IOT_OK);
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
  const int drops = ((fixture*)*state)->hub_drops;
  publish_record* record;
  const bool seen = publish_and_watch(
      (fixture*)*state, "e2e-unlisted/telemetry", AZ_IOT_MQTT_QOS_1, E2E_ROUTE_TIMEOUT_S, &record);
  assert_refused(record);
  assert_int_equal(((fixture*)*state)->hub_drops, drops);
  assert_false(seen);
}

static void another_device_id_is_refused(void** state)
{
  const int drops = ((fixture*)*state)->hub_drops;
  publish_record* record;
  const bool seen = publish_and_watch(
      (fixture*)*state,
      "e2e/not-{deviceId}/telemetry",
      AZ_IOT_MQTT_QOS_1,
      E2E_ROUTE_TIMEOUT_S,
      &record);
  assert_refused(record);
  assert_int_equal(((fixture*)*state)->hub_drops, drops);
  assert_false(seen);
}

/* A refusal leaves the session usable. */
static void publishing_continues_after_a_refusal(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->hub_drops, 0);
  assert_int_equal(fx->dev.conn_state, AZ_IOT_CONN_STATE_CONNECTED);
  publish_record* record;
  (void)publish_and_watch(fx, "e2e/{deviceId}/after", AZ_IOT_MQTT_QOS_1, 1, &record);
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
}

/** @brief QoS 1 publish of @p topic with one property; asserts the PUBACK and the routed check. */
static void assert_accepted_and_routed(fixture* fx, const char* topic)
{
  const az_iot_mqtt_user_property properties[] = { { "e2e", "custom-topic" } };
  publish_record* record;
  const bool seen = publish_topic_and_watch(
      fx,
      topic,
      AZ_IOT_MQTT_QOS_1,
      properties,
      sizeof(properties) / sizeof(properties[0]),
      E2E_ROUTE_TIMEOUT_S,
      &record);
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
  assert_routed(seen);
}

/* `{deviceId}` expands to the connected device id, and the hub accepts the result. */
static void formatted_device_id_template_is_routed(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1];
  size_t topic_len = 0;
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &fx->custom, "e2e/{deviceId}/fmt/a-b_c.0", topic, sizeof(topic), &topic_len),
      AZ_IOT_OK);
  /* The hub-assigned id, which need not equal the registration id. */
  const char* device_id = az_iot_connection_client_get_device_id(&fx->dev.conn);
  assert_non_null(device_id);
  char expected[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1];
  snprintf(expected, sizeof(expected), "e2e/%s/fmt/a-b_c.0", device_id);
  assert_string_equal(topic, expected);
  assert_int_equal(topic_len, strlen(expected));
  assert_accepted_and_routed(fx, topic);
}

static void topic_at_max_length_is_accepted(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 2];
  make_topic_of_length(fx, topic, sizeof(topic), AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN);
  assert_accepted_and_routed(fx, topic);
}

static void topic_at_max_levels_is_accepted(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 2];
  make_topic_of_levels(fx, topic, sizeof(topic), AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEVELS);
  assert_accepted_and_routed(fx, topic);
}

/** @brief Asserts the client refuses @p topic with AZ_IOT_ERR_INVALID_ARG and sends nothing. */
static void assert_rejected_locally(fixture* fx, const char* topic)
{
  publish_record* record = new_record(fx);
  const char payload[] = "{}";
  az_iot_mqttv5_custom_topic_message message = { 0 };
  message.topic = topic;
  message.payload = (const uint8_t*)payload;
  message.payload_len = sizeof(payload) - 1;
  message.qos = AZ_IOT_MQTT_QOS_1;
  const az_iot_result rc
      = az_iot_mqttv5_custom_topic_client_publish(&fx->custom, &message, on_publish, record);
  for (int i = 0; i < 50; ++i)
  {
    pump(fx);
  }
  printf(
      "[e2e] '%s' (%u bytes, %u levels): publish %s, completion %s\n",
      topic,
      (unsigned)strlen(topic),
      (unsigned)count_levels(topic),
      az_iot_result_to_string(rc),
      record->done ? az_iot_result_to_string(record->status) : "(none)");
  assert_int_equal(rc, AZ_IOT_ERR_INVALID_ARG);
  assert_false(record->done);
}

/* One byte or one level past the limit is refused by the client; the session stays usable. */
static void topic_past_limits_is_rejected_locally(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  const int drops = fx->hub_drops;
  char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 2];

  make_topic_of_length(fx, topic, sizeof(topic), AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1);
  assert_rejected_locally(fx, topic);
  make_topic_of_levels(fx, topic, sizeof(topic), AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEVELS + 1);
  assert_rejected_locally(fx, topic);

  assert_int_equal(fx->hub_drops, drops);
  assert_int_equal(fx->dev.conn_state, AZ_IOT_CONN_STATE_CONNECTED);
  publish_record* record;
  (void)publish_and_watch(fx, "e2e/{deviceId}/after-limits", AZ_IOT_MQTT_QOS_1, 1, &record);
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
}

/* The facade captures bodies only, so the routed check covers the body, not the properties. */
static void user_properties_are_routed(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1];
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &fx->custom, "e2e/{deviceId}/props", topic, sizeof(topic), NULL),
      AZ_IOT_OK);
  const az_iot_mqtt_user_property properties[] = {
    { "e2e", "custom-topic" },       { "color", "blue" }, { "with space", "a value with spaces" },
    { "reserved", "a/b+c#d?e=f&g" }, { "empty", "" },     { "e2e", "repeated" },
  };
  publish_record* record;
  const bool seen = publish_topic_and_watch(
      fx,
      topic,
      AZ_IOT_MQTT_QOS_1,
      properties,
      sizeof(properties) / sizeof(properties[0]),
      E2E_ROUTE_TIMEOUT_S,
      &record);
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
  assert_routed(seen);
}

/* After a re-provision the topic is built again from the template and is accepted. The
 * enrollment fixes the device id, so DPS assigns the same one; the unit tests cover a change.
 * Last: it closes the connection, which the drop counts of earlier cases must not see. */
static void a_topic_rebuilt_after_a_reprovision_is_accepted(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  const int dps_sessions = fx->dps_sessions;

  assert_int_equal(az_iot_connection_client_request_reprovision(&fx->dev.conn), AZ_IOT_OK);
  (void)az_iot_connection_client_close(&fx->dev.conn);
  for (time_t start = time(NULL); fx->dev.conn_state != AZ_IOT_CONN_STATE_IDLE
       && (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S;)
  {
    pump(fx);
  }
  assert_int_equal(fx->dev.conn_state, AZ_IOT_CONN_STATE_IDLE);

  char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1];
  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &fx->custom, "e2e/{deviceId}/reprovisioned", topic, sizeof(topic), NULL),
      AZ_IOT_ERR_NOT_CONNECTED);

  assert_int_equal(az_iot_connection_client_open(&fx->dev.conn), AZ_IOT_OK);
  assert_true(wait_connected(fx));
  assert_true(fx->dps_sessions > dps_sessions);

  assert_int_equal(
      az_iot_mqttv5_custom_topic_client_format_topic(
          &fx->custom, "e2e/{deviceId}/reprovisioned", topic, sizeof(topic), NULL),
      AZ_IOT_OK);
  const char* device_id = az_iot_connection_client_get_device_id(&fx->dev.conn);
  assert_non_null(device_id);
  char expected[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1];
  snprintf(expected, sizeof(expected), "e2e/%s/reprovisioned", device_id);
  assert_string_equal(topic, expected);

  /* Only the PUBACK is asserted; routing is covered by the cases above. */
  const az_iot_mqtt_user_property properties[] = { { "e2e", "custom-topic" } };
  publish_record* record;
  (void)publish_topic_and_watch(
      fx,
      topic,
      AZ_IOT_MQTT_QOS_1,
      properties,
      sizeof(properties) / sizeof(properties[0]),
      1,
      &record);
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
    cmocka_unit_test(formatted_device_id_template_is_routed),
    cmocka_unit_test(topic_at_max_length_is_accepted),
    cmocka_unit_test(topic_at_max_levels_is_accepted),
    cmocka_unit_test(topic_past_limits_is_rejected_locally),
    cmocka_unit_test(user_properties_are_routed),
    cmocka_unit_test(a_topic_rebuilt_after_a_reprovision_is_accepted),
  };
  return cmocka_run_group_tests_name("e2e_mqttv5_custom_topic", tests, group_setup, group_teardown);
}
