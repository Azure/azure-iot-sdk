// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end: az_iot_mqttv5_telemetry_client on an mqttv5 hub.
 *
 * The device provisions through DPS (e2e_device.h), which must assign it to an mqttv5 hub.
 * Messages are read back from the hub's events endpoint (IOTHUB_* variables, see
 * az_iot_e2e_service.h) with their AMQP properties, application properties and annotations, so
 * each test checks what the service sees, not only that the hub acknowledged.
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

#define E2E_ROUTE_TIMEOUT_S 60
#define E2E_ACK_TIMEOUT_S 60
#define E2E_CONNECT_TIMEOUT_S 60
#define E2E_SERVICE_CONNECT_TIMEOUT_S 45
/* How long a refused message must stay away from the events endpoint. */
#define E2E_NOT_ROUTED_WINDOW_S 30
#define E2E_PUMP_MS 20
/* tag, '-', device id (<= 128), '-', 11 hex digits, NUL. */
#define E2E_MARKER_MAX 160
#define E2E_VALUE_MAX 512
#define E2E_MAX_PUBLISHES 32
#define E2E_IN_FLIGHT 8
#define E2E_APP_PROPERTIES 8
/* Observed on the mqttv5 hub: 257 KiB is routed; 1 MiB - 8 KiB drops the session. The documented
 * broker limit is 512 KB; the accepted body stays under it with 8 KiB for properties. Neither
 * Paho nor the SDK caps a publish. */
#define E2E_PAYLOAD_AT_LIMIT (512 * 1000 - 8 * 1024)
#define E2E_PAYLOAD_PAST_LIMIT (1024 * 1024 + 4 * 1024)
/* Not the SDK default (application/json), so a dropped $.ct cannot pass. */
#define E2E_CONTENT_TYPE "application/vnd.e2e+json"

typedef struct
{
  int done;
  az_iot_result status;
} publish_record;

typedef struct
{
  e2e_device dev;
  /** Hub-assigned device id; DPS may assign one other than the registration id. */
  char device_id[129];
  az_iot_e2e_service* service;
  az_iot_mqttv5_telemetry_client telemetry;
  bool telemetry_ok;
  /* Callback contexts live as long as the connection: a late PUBACK may still complete one. */
  publish_record records[E2E_MAX_PUBLISHES];
  size_t records_used;
  /* Hub transitions out of CONNECTED. */
  int hub_drops;
  bool hub_connected;
} fixture;

static fixture g_fx;

static void on_publish(az_iot_result status, void* user_ctx)
{
  publish_record* record = (publish_record*)user_ctx;
  record->status = status;
  record->done = 1;
}

static void on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }
  const bool connected = event->state == AZ_IOT_CONN_STATE_CONNECTED;
  if (fx->hub_connected && !connected)
  {
    fx->hub_drops++;
    printf(
        "[e2e] hub left CONNECTED: state %d, reason %s\n",
        (int)event->state,
        az_iot_result_to_string(event->reason));
  }
  fx->hub_connected = connected;
}

static publish_record* new_record(fixture* fx)
{
  assert_true(fx->records_used < E2E_MAX_PUBLISHES);
  return &fx->records[fx->records_used++];
}

static const char* record_status(const publish_record* record)
{
  return record->done ? az_iot_result_to_string(record->status) : "(none)";
}

static void pump(fixture* fx) { e2e_device_do_work(&fx->dev, E2E_PUMP_MS); }

/** @brief Pumps the device and the service once. */
static void pump_both(fixture* fx)
{
  pump(fx);
  if (!az_iot_e2e_service_do_work(fx->service, E2E_PUMP_MS))
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fail_msg("service pump failed: %s", err ? err : "unknown");
  }
}

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
  const char* assigned = az_iot_connection_client_get_device_id(&g_fx.dev.conn);
  const size_t assigned_len = assigned != NULL ? strlen(assigned) : 0;
  if (assigned == NULL || assigned_len >= sizeof(g_fx.device_id))
  {
    fprintf(stderr, "[e2e] no usable hub device id\n");
    return -1;
  }
  memcpy(g_fx.device_id, assigned, assigned_len + 1);

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
  if (az_iot_mqttv5_telemetry_client_init(&g_fx.telemetry, &g_fx.dev.conn) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] telemetry client init failed\n");
    return -1;
  }
  g_fx.telemetry_ok = true;

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
  if (fx->telemetry_ok)
  {
    az_iot_mqttv5_telemetry_client_deinit(&fx->telemetry);
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

/* Restarts the watcher, so a test that failed before ending its own leaves nothing behind. */
static void watch_begin(fixture* fx)
{
  az_iot_e2e_service_telemetry_watch_end(fx->service);
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

/** @brief Sends one message, retrying while every PUBACK slot is in use. */
static publish_record* send_message(
    fixture* fx,
    const uint8_t* payload,
    size_t payload_len,
    const az_iot_telemetry_property* properties,
    size_t properties_count)
{
  publish_record* record = new_record(fx);
  az_iot_telemetry_message message = { 0 };
  message.payload = payload;
  message.payload_len = payload_len;
  message.properties = properties;
  message.properties_count = properties_count;
  az_iot_result rc = AZ_IOT_ERR_BUSY;
  for (time_t start = time(NULL); (time(NULL) - start) < E2E_ACK_TIMEOUT_S;)
  {
    rc = az_iot_mqttv5_telemetry_client_send(&fx->telemetry, &message, on_publish, record);
    if (rc != AZ_IOT_ERR_BUSY)
    {
      break;
    }
    pump_both(fx);
  }
  if (rc != AZ_IOT_OK)
  {
    fail_msg("send of %zu bytes returned %s", payload_len, az_iot_result_to_string(rc));
  }
  return record;
}

/** @brief Sends {"marker":"<marker>"} with @p properties. */
static publish_record* send_marked(
    fixture* fx,
    const char* marker,
    const az_iot_telemetry_property* properties,
    size_t properties_count)
{
  /* static: the callback may complete after the test returns, the payload must not dangle. */
  static char payloads[E2E_MAX_PUBLISHES][E2E_MARKER_MAX + 16];
  assert_true(fx->records_used < E2E_MAX_PUBLISHES);
  char* payload = payloads[fx->records_used];
  snprintf(payload, sizeof(payloads[0]), "{\"marker\":\"%s\"}", marker);
  return send_message(fx, (const uint8_t*)payload, strlen(payload), properties, properties_count);
}

static bool wait_seen(fixture* fx, const char* marker, int timeout_s)
{
  for (time_t start = time(NULL); (time(NULL) - start) < timeout_s;)
  {
    if (az_iot_e2e_service_telemetry_seen(fx->service, marker))
    {
      return true;
    }
    pump_both(fx);
  }
  return az_iot_e2e_service_telemetry_seen(fx->service, marker);
}

static void wait_done(fixture* fx, const publish_record* record, int timeout_s)
{
  for (time_t start = time(NULL); !record->done && (time(NULL) - start) < timeout_s;)
  {
    pump_both(fx);
  }
}

/** @brief Waits for @p marker and its acknowledgement; asserts both. */
static void assert_delivered(fixture* fx, const char* marker, const publish_record* record)
{
  const bool seen = wait_seen(fx, marker, E2E_ROUTE_TIMEOUT_S);
  wait_done(fx, record, E2E_ACK_TIMEOUT_S);
  printf(
      "[e2e] '%s': completion %s, routed %s\n", marker, record_status(record), seen ? "yes" : "no");
  if (!seen)
  {
    az_iot_e2e_service_telemetry_dump(fx->service, marker);
  }
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
  assert_true(seen);
}

/** @brief Whether property @p name of the message carrying @p marker is @p expected; logs
 * where @p expected went when it is not. */
static bool property_is(fixture* fx, const char* marker, const char* name, const char* expected)
{
  char actual[E2E_VALUE_MAX];
  if (!az_iot_e2e_service_telemetry_property(fx->service, marker, name, actual, sizeof(actual)))
  {
    char found_under[E2E_VALUE_MAX];
    if (az_iot_e2e_service_telemetry_find_value(
            fx->service, marker, expected, found_under, sizeof(found_under)))
    {
      printf("[e2e] '%s' is absent; its value '%s' is under '%s'\n", name, expected, found_under);
    }
    else
    {
      printf("[e2e] '%s' is absent; expected '%s'\n", name, expected);
    }
    return false;
  }
  if (strcmp(actual, expected) != 0)
  {
    printf("[e2e] '%s' is '%s', expected '%s'\n", name, actual, expected);
    az_iot_e2e_service_telemetry_dump(fx->service, marker);
    return false;
  }
  printf("[e2e] '%s' = '%s'\n", name, actual);
  return true;
}

/** @brief Whether @p text is decimal epoch milliseconds (optionally "timestamp:"-prefixed)
 * within the second @p epoch_s. */
static bool epoch_ms_matches(const char* text, time_t epoch_s)
{
  static const char prefix[] = "timestamp:";
  if (strncmp(text, prefix, sizeof(prefix) - 1) == 0)
  {
    text += sizeof(prefix) - 1;
  }
  char* end = NULL;
  const long long ms = strtoll(text, &end, 10);
  return end != text && *end == '\0' && ms >= 0 && ms / 1000 == (long long)epoch_s;
}

/** @brief For a property whose service-side name is unconfirmed: passes when @p expected is the
 * value of any property, or when one of @p names holds a value matching its first
 * @p match_len characters (0: exact match only) or, when @p expected_epoch_s >= 0, epoch
 * milliseconds within that second. Logs the name it was found under. */
static bool property_found(
    fixture* fx,
    const char* marker,
    const char* label,
    const char* const* names,
    size_t names_count,
    const char* expected,
    size_t match_len,
    time_t expected_epoch_s)
{
  char name[E2E_VALUE_MAX];
  if (az_iot_e2e_service_telemetry_find_value(fx->service, marker, expected, name, sizeof(name)))
  {
    printf("[e2e] %s '%s' arrived as '%s'\n", label, expected, name);
    return true;
  }
  for (size_t i = 0; i < names_count; ++i)
  {
    char actual[E2E_VALUE_MAX];
    if (az_iot_e2e_service_telemetry_property(
            fx->service, marker, names[i], actual, sizeof(actual)))
    {
      printf("[e2e] %s '%s' arrived as '%s' = '%s'\n", label, expected, names[i], actual);
      if ((match_len > 0 && strncmp(actual, expected, match_len) == 0)
          || (expected_epoch_s >= 0 && epoch_ms_matches(actual, expected_epoch_s)))
      {
        return true;
      }
    }
  }
  printf("[e2e] %s '%s' not found in any property\n", label, expected);
  return false;
}

/* The marked body reaches the events endpoint and is acknowledged. */
static void telemetry_is_routed(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "tr", fx->device_id);

  watch_begin(fx);
  publish_record* record = send_marked(fx, marker, NULL, 0);
  assert_delivered(fx, marker, record);
  az_iot_e2e_service_telemetry_dump(fx->service, marker);
  az_iot_e2e_service_telemetry_watch_end(fx->service);
}

/* $.ct and $.cid travel as native Content Type and Correlation Data, and the service reads them
 * as the AMQP content-type and correlation-id (not as application properties). The content type
 * is not the SDK default, so it can only come from $.ct. */
static void content_type_and_correlation_id_are_native(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "cc", fx->device_id);

  watch_begin(fx);
  const az_iot_telemetry_property properties[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, E2E_CONTENT_TYPE },
    { AZ_IOT_MSG_PROP_CORRELATION_ID, marker },
  };
  publish_record* record
      = send_marked(fx, marker, properties, sizeof(properties) / sizeof(properties[0]));
  assert_delivered(fx, marker, record);

  const bool content_type_ok = property_is(fx, marker, "amqp:content-type", E2E_CONTENT_TYPE);
  const bool correlation_id_ok = property_is(fx, marker, "amqp:correlation-id", marker);
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  assert_true(content_type_ok);
  assert_true(correlation_id_ok);
}

/* The other system properties travel as user properties named as on mqttv3; the service shows
 * where each lands. $.uid goes in a message of its own, so a hub refusing it does not hide the
 * rest. */
static void system_properties_arrive(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char marker[E2E_MARKER_MAX];
  char uid_marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "sp", fx->device_id);
  make_marker(uid_marker, sizeof(uid_marker), "su", fx->device_id);

  char message_id[E2E_MARKER_MAX + 8];
  snprintf(message_id, sizeof(message_id), "mid-%s", marker);
  char creation_time[32];
  const time_t now = time(NULL);
  struct tm utc;
#ifdef _MSC_VER
  assert_int_equal(gmtime_s(&utc, &now), 0);
#else
  const struct tm* utc_p = gmtime(&now);
  assert_non_null(utc_p);
  utc = *utc_p;
#endif
  assert_true(strftime(creation_time, sizeof(creation_time), "%Y-%m-%dT%H:%M:%S.000Z", &utc) > 0);
  const char* component = "e2eComponent";

  watch_begin(fx);
  const az_iot_telemetry_property properties[] = {
    { AZ_IOT_MSG_PROP_MESSAGE_ID, message_id },
    { AZ_IOT_MSG_PROP_CONTENT_ENCODING, "utf-8" },
    { AZ_IOT_MSG_PROP_CREATION_TIME, creation_time },
    { AZ_IOT_MSG_PROP_COMPONENT_NAME, component },
  };
  /* The hub requires the user id to be the authenticated device id (mqttv3). */
  const az_iot_telemetry_property uid_properties[] = {
    { AZ_IOT_MSG_PROP_USER_ID, fx->device_id },
  };
  publish_record* record
      = send_marked(fx, marker, properties, sizeof(properties) / sizeof(properties[0]));
  /* $.uid is the one property the hub validates: it must equal the authenticated device id or the
   * whole message is refused. It travels alone so such a refusal fails only its own check, not the
   * pass-through properties above. The markers only correlate each message on the shared events
   * endpoint; neither the SDK nor the hub reads them. */
  publish_record* uid_record = send_marked(fx, uid_marker, uid_properties, 1);

  const bool seen = wait_seen(fx, marker, E2E_ROUTE_TIMEOUT_S);
  const bool uid_seen = wait_seen(fx, uid_marker, E2E_ROUTE_TIMEOUT_S);
  wait_done(fx, record, E2E_ACK_TIMEOUT_S);
  wait_done(fx, uid_record, E2E_ACK_TIMEOUT_S);
  printf(
      "[e2e] system properties: completion %s, routed %s; $.uid: completion %s, routed %s\n",
      record_status(record),
      seen ? "yes" : "no",
      record_status(uid_record),
      uid_seen ? "yes" : "no");
  az_iot_e2e_service_telemetry_dump(fx->service, marker);
  az_iot_e2e_service_telemetry_dump(fx->service, uid_marker);

  int failures = 0;
  if (!seen || !record->done || record->status != AZ_IOT_OK)
  {
    failures++;
  }
  else
  {
    static const char* const ctime_names[]
        = { "app:iothub-creation-time-utc", "amqp:creation-time" };
    static const char* const sub_names[] = { "app:dt-subject", "amqp:subject" };
    failures += property_is(fx, marker, "amqp:message-id", message_id) ? 0 : 1;
    failures += property_is(fx, marker, "amqp:content-encoding", "utf-8") ? 0 : 1;
    /* Compared to the second, as ISO-8601 or epoch milliseconds: the hub may normalize the
     * fraction. */
    failures
        += property_found(fx, marker, "$.ctime", ctime_names, 2, creation_time, 19, now) ? 0 : 1;
    failures += property_found(fx, marker, "$.sub", sub_names, 2, component, 0, -1) ? 0 : 1;
  }
  if (!uid_seen || !uid_record->done || uid_record->status != AZ_IOT_OK)
  {
    printf("[e2e] $.uid='%s' was not delivered\n", fx->device_id);
    failures++;
  }
  else
  {
    failures += property_is(fx, uid_marker, "amqp:user-id", fx->device_id) ? 0 : 1;
  }
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  assert_int_equal(failures, 0);
}

/* Custom user properties arrive as application properties with their exact values. */
static void application_properties_arrive(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "ap", fx->device_id);

  static const char* const keys[E2E_APP_PROPERTIES]
      = { "e2e-a", "e2e-b", "e2e-c", "e2e-d", "e2e-e", "e2e-f", "e2e-g", "e2e-h" };
  static const char* const app_names[E2E_APP_PROPERTIES] = {
    "app:e2e-a", "app:e2e-b", "app:e2e-c", "app:e2e-d",
    "app:e2e-e", "app:e2e-f", "app:e2e-g", "app:e2e-h",
  };
  char values[E2E_APP_PROPERTIES][32];
  az_iot_telemetry_property properties[E2E_APP_PROPERTIES];
  for (size_t i = 0; i < E2E_APP_PROPERTIES; ++i)
  {
    snprintf(values[i], sizeof(values[i]), "value-%u-%lx", (unsigned)i, (unsigned long)time(NULL));
    properties[i].key = keys[i];
    properties[i].value = values[i];
  }

  watch_begin(fx);
  publish_record* record = send_marked(fx, marker, properties, E2E_APP_PROPERTIES);
  assert_delivered(fx, marker, record);

  int failures = 0;
  for (size_t i = 0; i < E2E_APP_PROPERTIES; ++i)
  {
    failures += property_is(fx, marker, app_names[i], values[i]) ? 0 : 1;
  }
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  assert_int_equal(failures, 0);
}

/* Messages sent back to back, without waiting for acknowledgements, all succeed and arrive in
 * send order (one device id maps to one partition). */
static void messages_in_flight_keep_order(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char markers[E2E_IN_FLIGHT][E2E_MARKER_MAX];
  publish_record* records[E2E_IN_FLIGHT];

  watch_begin(fx);
  for (size_t i = 0; i < E2E_IN_FLIGHT; ++i)
  {
    make_marker(markers[i], sizeof(markers[i]), "or", fx->device_id);
    records[i] = send_marked(fx, markers[i], NULL, 0);
  }
  for (size_t i = 0; i < E2E_IN_FLIGHT; ++i)
  {
    (void)wait_seen(fx, markers[i], E2E_ROUTE_TIMEOUT_S);
  }
  for (size_t i = 0; i < E2E_IN_FLIGHT; ++i)
  {
    wait_done(fx, records[i], E2E_ACK_TIMEOUT_S);
  }

  int indices[E2E_IN_FLIGHT];
  bool all_ok = true;
  bool ordered = true;
  for (size_t i = 0; i < E2E_IN_FLIGHT; ++i)
  {
    indices[i] = az_iot_e2e_service_telemetry_index(fx->service, markers[i]);
    printf(
        "[e2e] #%u '%s': completion %s, arrival %d\n",
        (unsigned)i,
        markers[i],
        record_status(records[i]),
        indices[i]);
    all_ok = all_ok && records[i]->done && records[i]->status == AZ_IOT_OK && indices[i] >= 0;
    ordered = ordered && (i == 0 || indices[i] > indices[i - 1]);
  }
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  assert_true(all_ok);
  assert_true(ordered);
}

/** @brief Heap payload of @p size bytes: a JSON object with @p marker first, padded to size. */
static uint8_t* make_large_payload(const char* marker, size_t size)
{
  char head[E2E_MARKER_MAX + 32];
  const int head_len = snprintf(head, sizeof(head), "{\"marker\":\"%s\",\"pad\":\"", marker);
  assert_true(head_len > 0 && (size_t)head_len + 2 < size);
  uint8_t* payload = (uint8_t*)malloc(size);
  assert_non_null(payload);
  memcpy(payload, head, (size_t)head_len);
  memset(payload + head_len, 'x', size - (size_t)head_len - 2);
  payload[size - 2] = '"';
  payload[size - 1] = '}';
  return payload;
}

/* A body just under the hub limit is acknowledged and routed. */
static void payload_at_limit_is_accepted(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "al", fx->device_id);
  uint8_t* payload = make_large_payload(marker, E2E_PAYLOAD_AT_LIMIT);

  watch_begin(fx);
  publish_record* record = send_message(fx, payload, E2E_PAYLOAD_AT_LIMIT, NULL, 0);
  const bool seen = wait_seen(fx, marker, E2E_ROUTE_TIMEOUT_S);
  wait_done(fx, record, E2E_ACK_TIMEOUT_S);
  /* Paho copies the payload on send, and the publish has completed or timed out by now. */
  free(payload);
  size_t routed_size = 0;
  const bool sized = az_iot_e2e_service_telemetry_body_size(fx->service, marker, &routed_size);
  printf(
      "[e2e] %d-byte payload: completion %s, routed %s (%u bytes), hub drops %d\n",
      E2E_PAYLOAD_AT_LIMIT,
      record_status(record),
      seen ? "yes" : "no",
      (unsigned)routed_size,
      fx->hub_drops);
  az_iot_e2e_service_telemetry_dump(fx->service, marker);
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
  assert_true(seen);
  assert_true(sized);
  assert_int_equal(routed_size, E2E_PAYLOAD_AT_LIMIT);
}

/** @brief Reopens the hub connection if it settled instead of reconnecting by itself. */
static void ensure_connected(fixture* fx)
{
  if (fx->dev.conn_state == AZ_IOT_CONN_STATE_FAULTED)
  {
    printf("[e2e] connection FAULTED; closing and reopening\n");
    az_iot_connection_client_close(&fx->dev.conn);
    for (int i = 0; i < 100 && fx->dev.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
    {
      pump(fx);
    }
  }
  if (fx->dev.conn_state == AZ_IOT_CONN_STATE_IDLE)
  {
    printf("[e2e] connection IDLE; reopening\n");
    assert_int_equal(az_iot_connection_client_open(&fx->dev.conn), AZ_IOT_OK);
  }
  assert_true(wait_connected(fx));
}

/* A body past the hub limit is refused (PUBACK refusal or a dropped session), never routed, and
 * telemetry is delivered again afterwards. */
static void payload_past_limit_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_true(wait_connected(fx));
  const int drops = fx->hub_drops;
  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "pl", fx->device_id);
  uint8_t* payload = make_large_payload(marker, E2E_PAYLOAD_PAST_LIMIT);

  watch_begin(fx);
  /* Latched on arrival, so an oversized delivery evicted from the capture ring still counts. */
  assert_true(az_iot_e2e_service_telemetry_watch_needle(fx->service, marker));
  publish_record* record = send_message(fx, payload, E2E_PAYLOAD_PAST_LIMIT, NULL, 0);
  wait_done(fx, record, E2E_ACK_TIMEOUT_S);
  bool seen = wait_seen(fx, marker, E2E_NOT_ROUTED_WINDOW_S);
  bool latched = az_iot_e2e_service_telemetry_needle_seen(fx->service, marker);
  free(payload);
  printf(
      "[e2e] %d-byte payload: completion %s, routed %s (latched %s), hub drops %d, state %d\n",
      E2E_PAYLOAD_PAST_LIMIT,
      record_status(record),
      seen ? "yes" : "no",
      latched ? "yes" : "no",
      fx->hub_drops - drops,
      (int)fx->dev.conn_state);
  if (seen)
  {
    az_iot_e2e_service_telemetry_dump(fx->service, marker);
  }
  assert_true(record->done);
  if (record->status == AZ_IOT_ERR_PUBLISH_REFUSED || record->status == AZ_IOT_ERR_MQTT)
  {
    printf(
        "[e2e] observed: refused with %s, %s\n",
        az_iot_result_to_string(record->status),
        fx->hub_drops == drops ? "session kept" : "session dropped");
  }
  else if (record->status == AZ_IOT_ERR_NOT_CONNECTED)
  {
    printf("[e2e] observed: the hub dropped the session\n");
  }
  else
  {
    fail_msg("expected a refusal or a disconnect, got %s", record_status(record));
  }
  assert_false(seen);
  assert_false(latched);

  ensure_connected(fx);
  assert_false(az_iot_e2e_service_telemetry_needle_seen(fx->service, marker));
  char after[E2E_MARKER_MAX];
  make_marker(after, sizeof(after), "pa", fx->device_id);
  publish_record* after_record = send_marked(fx, after, NULL, 0);
  assert_delivered(fx, after, after_record);
  /* assert_delivered() pumped the watcher again: a late oversized delivery must still fail. */
  seen = az_iot_e2e_service_telemetry_seen(fx->service, marker);
  latched = az_iot_e2e_service_telemetry_needle_seen(fx->service, marker);
  if (seen || latched)
  {
    printf("[e2e] oversized payload routed late (in capture ring: %s)\n", seen ? "yes" : "no");
    az_iot_e2e_service_telemetry_dump(fx->service, marker);
  }
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  assert_false(seen);
  assert_false(latched);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(telemetry_is_routed),
    cmocka_unit_test(content_type_and_correlation_id_are_native),
    cmocka_unit_test(system_properties_arrive),
    cmocka_unit_test(application_properties_arrive),
    cmocka_unit_test(messages_in_flight_keep_order),
    cmocka_unit_test(payload_at_limit_is_accepted),
    cmocka_unit_test(payload_past_limit_is_refused),
  };
  return cmocka_run_group_tests_name("e2e_mqttv5_telemetry", tests, group_setup, group_teardown);
}
