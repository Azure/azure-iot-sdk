// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end: connection lifecycle and DPS on an mqttv5 hub.
 *
 * The device provisions through DPS (e2e_device.h), which must assign it to an mqttv5 hub. The
 * assignment is saved and reused as a "saved hub with DPS fallback" (docs/connecting.md). Telemetry
 * is read from the hub's events endpoint (IOTHUB_* variables, see az_iot_e2e_service.h). Every
 * state transition is printed, so a CI log shows what the hub and DPS did.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot.h"

#include "az_iot_e2e_service.h"
#include "e2e_device.h"
#include "e2e_log.h"

#define E2E_CONNECT_TIMEOUT_S 90
/* Failed attempts at the saved hub, a DPS registration, then the assigned hub. */
#define E2E_FALLBACK_TIMEOUT_S 180
#define E2E_CLOSE_TIMEOUT_S 30
#define E2E_EVICT_TIMEOUT_S 60
#define E2E_ROUTE_TIMEOUT_S 90
#define E2E_ACK_TIMEOUT_S 30
#define E2E_REQUEST_TIMEOUT_S 60
#define E2E_SERVICE_CONNECT_TIMEOUT_S 45
#define E2E_RETRY_INTERVAL_MS 1000
/* Long enough that a closed second connection is gone before the first one comes back. */
#define E2E_EVICTED_RETRY_INTERVAL_MS 3000
#define E2E_REPROVISION_THRESHOLD 2
#define E2E_OPEN_CLOSE_CYCLES 3
#define E2E_PUMP_MS 20
/* tag, '-', device id (<= 128), '-', 11 hex digits, NUL. */
#define E2E_MARKER_MAX 160

/** @brief What one device's connection did, from its state observer. */
typedef struct
{
  const char* name;
  int dps_events;
  int dps_connected;
  int hub_connected;
  int hub_drops;
  int hub_failures;
  bool hub_up;
  bool reprovision_announced;
  char last_failed_endpoint[256];
  /** @brief Host whose failures are counted in watched_failures; empty counts none. */
  char watched_endpoint[256];
  int watched_failures;
} state_log;

typedef struct
{
  int done;
  az_iot_result status;
} op_record;

#define E2E_MAX_PUBLISHES 8

typedef struct
{
  e2e_device a;
  state_log a_log;
  /* Second connection with the same identity; see the eviction test. */
  e2e_device b;
  state_log b_log;
  bool b_active;
  az_iot_e2e_service* service;
  az_iot_mqttv5_telemetry_client telemetry;
  bool telemetry_ok;
  az_iot_mqttv5_twin_client twin;
  bool twin_ok;
  az_iot_mqttv5_direct_method_client methods;
  bool methods_ok;
  /* Callback contexts live as long as the connection: a late completion may still arrive. */
  op_record publishes[E2E_MAX_PUBLISHES];
  size_t publishes_used;
  op_record get;
} fixture;

static fixture g_fx;
/* The assignment from the first DPS connect; reused by later tests. */
static e2e_device_hub g_saved;

static const char* state_name(az_iot_connection_state s)
{
  switch (s)
  {
    case AZ_IOT_CONN_STATE_IDLE:
      return "IDLE";
    case AZ_IOT_CONN_STATE_CONNECTING:
      return "CONNECTING";
    case AZ_IOT_CONN_STATE_CONNECTED:
      return "CONNECTED";
    case AZ_IOT_CONN_STATE_RETRY_PENDING:
      return "RETRY_PENDING";
    case AZ_IOT_CONN_STATE_DISCONNECTING:
      return "DISCONNECTING";
    case AZ_IOT_CONN_STATE_FAULTED:
      return "FAULTED";
    case AZ_IOT_CONN_STATE_SETTING_UP:
      return "SETTING_UP";
    default:
      return "?";
  }
}

/** @brief Prints every transition and counts the ones the tests assert on. */
static void on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  state_log* log = (state_log*)user_ctx;
  const bool hub = event->scope == AZ_IOT_CONN_SCOPE_HUB;
  printf(
      "[e2e] %s %s %s reason=%s retriable=%d auth=%d/%u renewal=%d",
      log->name,
      hub ? "HUB" : "DPS",
      state_name(event->state),
      az_iot_result_to_string(event->reason),
      (int)event->is_retriable,
      (int)event->auth_source,
      (unsigned)event->x509_index,
      (int)event->is_credential_renewal);
  if (event->error != NULL)
  {
    printf(
        " error=%d:%ld '%.*s'",
        (int)event->error->source,
        (long)event->error->code,
        (int)az_span_size(event->error->message),
        (const char*)az_span_ptr(event->error->message));
  }
  if (event->recovery != NULL)
  {
    printf(
        " recovery=class %d endpoint %s attempt %u delay %ums reprovision %d",
        (int)event->recovery->classification,
        event->recovery->endpoint ? event->recovery->endpoint : "(none)",
        (unsigned)event->recovery->attempt,
        (unsigned)event->recovery->next_attempt_delay_ms,
        (int)event->recovery->next_attempt_reprovisions);
  }
  if (event->profile != NULL)
  {
    printf(
        " profile=%d '%s'%s",
        (int)event->profile->connection_profile,
        event->profile->connection_profile_raw,
        event->profile->connection_profile_raw_truncated ? " (truncated)" : "");
  }
  printf("\n");
  fflush(stdout);

  if (!hub)
  {
    log->dps_events++;
    log->dps_connected += event->state == AZ_IOT_CONN_STATE_CONNECTED;
    return;
  }
  const bool connected = event->state == AZ_IOT_CONN_STATE_CONNECTED;
  if (connected)
  {
    log->hub_connected++;
  }
  if (log->hub_up && !connected)
  {
    log->hub_drops++;
  }
  log->hub_up = connected;
  if ((event->state == AZ_IOT_CONN_STATE_RETRY_PENDING || event->state == AZ_IOT_CONN_STATE_FAULTED)
      && !event->is_credential_renewal)
  {
    log->hub_failures++;
    if (event->recovery != NULL)
    {
      log->reprovision_announced |= event->recovery->next_attempt_reprovisions;
      if (log->watched_endpoint[0] != '\0' && event->recovery->endpoint != NULL
          && strcmp(event->recovery->endpoint, log->watched_endpoint) == 0)
      {
        log->watched_failures++;
      }
      snprintf(
          log->last_failed_endpoint,
          sizeof(log->last_failed_endpoint),
          "%s",
          event->recovery->endpoint ? event->recovery->endpoint : "");
    }
  }
}

static void print_log(const state_log* log)
{
  printf(
      "[e2e] %s summary: dps events %d (connected %d), hub connected %d, drops %d, failures %d, "
      "reprovision announced %d, last failed endpoint '%s', failures at watched '%s' %d\n",
      log->name,
      log->dps_events,
      log->dps_connected,
      log->hub_connected,
      log->hub_drops,
      log->hub_failures,
      (int)log->reprovision_announced,
      log->last_failed_endpoint,
      log->watched_endpoint,
      log->watched_failures);
  fflush(stdout);
}

static void on_publish(az_iot_result status, void* user_ctx)
{
  op_record* r = (op_record*)user_ctx;
  r->status = status;
  r->done = 1;
}

static void on_get(az_iot_result status, const az_iot_mqttv5_twin_state* twin, void* user_ctx)
{
  (void)twin;
  op_record* r = (op_record*)user_ctx;
  r->status = status;
  r->done = 1;
}

static void on_method(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  printf("[e2e] method '%s' (%zu bytes)\n", method_name, payload_len);
  (void)az_iot_mqttv5_direct_method_respond(
      (az_iot_mqttv5_direct_method_client*)user_ctx, request, 200, payload, payload_len);
}

static void pump(fixture* fx)
{
  e2e_device_do_work(&fx->a, E2E_PUMP_MS);
  if (fx->b_active)
  {
    e2e_device_do_work(&fx->b, E2E_PUMP_MS);
  }
  if (fx->twin_ok)
  {
    (void)az_iot_mqttv5_twin_client_do_work(&fx->twin);
  }
}

/** @brief Pumps until @p dev's hub reaches @p want; gives up early on FAULTED. */
static bool wait_hub_state(
    fixture* fx,
    e2e_device* dev,
    az_iot_connection_state want,
    int timeout_s)
{
  for (time_t start = time(NULL); dev->conn_state != want && (time(NULL) - start) < timeout_s;)
  {
    if (dev->conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
    pump(fx);
  }
  return dev->conn_state == want;
}

/* Unique per run: the device id is per run, and the events endpoint is shared by concurrent runs.
 */
static void make_marker(char* out, size_t cap, const char* tag, const char* device_id)
{
  static unsigned counter = 0;
  snprintf(
      out, cap, "%s-%s-%08lx%03x", tag, device_id, (unsigned long)time(NULL), (counter++) & 0xfffu);
}

/** @brief before_open: observe device A from the start, DPS session included. */
static int observe_a(e2e_device* dev, void* ctx)
{
  fixture* fx = (fixture*)ctx;
  return az_iot_connection_client_add_state_observer(&dev->conn, on_state, &fx->a_log) == AZ_IOT_OK
      ? 0
      : 1;
}

/** @brief before_open: observe device B. */
static int observe_b(e2e_device* dev, void* ctx)
{
  fixture* fx = (fixture*)ctx;
  return az_iot_connection_client_add_state_observer(&dev->conn, on_state, &fx->b_log) == AZ_IOT_OK
      ? 0
      : 1;
}

/** @brief before_open: observe device A and attach the twin client. */
static int observe_a_with_twin(e2e_device* dev, void* ctx)
{
  fixture* fx = (fixture*)ctx;
  if (observe_a(dev, ctx) != 0
      || az_iot_mqttv5_twin_client_init(&fx->twin, &dev->conn) != AZ_IOT_OK)
  {
    return 1;
  }
  fx->twin_ok = true;
  return 0;
}

/** @brief before_open: observe device A and attach telemetry, direct method and twin clients. */
static int observe_a_with_clients(e2e_device* dev, void* ctx)
{
  fixture* fx = (fixture*)ctx;
  if (observe_a_with_twin(dev, ctx) != 0
      || az_iot_mqttv5_telemetry_client_init(&fx->telemetry, &dev->conn) != AZ_IOT_OK)
  {
    return 1;
  }
  fx->telemetry_ok = true;
  if (az_iot_mqttv5_direct_method_client_init(&fx->methods, &dev->conn) != AZ_IOT_OK)
  {
    return 1;
  }
  fx->methods_ok = true;
  return az_iot_mqttv5_direct_method_client_register_method(
             &fx->methods, "e2eEcho", 0, on_method, &fx->methods)
          == AZ_IOT_OK
      ? 0
      : 1;
}

static void release_clients(fixture* fx)
{
  if (fx->methods_ok)
  {
    az_iot_mqttv5_direct_method_client_deinit(&fx->methods);
    fx->methods_ok = false;
  }
  if (fx->twin_ok)
  {
    az_iot_mqttv5_twin_client_deinit(&fx->twin);
    fx->twin_ok = false;
  }
  if (fx->telemetry_ok)
  {
    az_iot_mqttv5_telemetry_client_deinit(&fx->telemetry);
    fx->telemetry_ok = false;
  }
}

/** @brief Releases device A (and its clients) so it can connect again with other options. */
static void reset_a(fixture* fx)
{
  release_clients(fx);
  e2e_device_disconnect(&fx->a);
  memset(&fx->a, 0, sizeof(fx->a));
  memset(&fx->a_log, 0, sizeof(fx->a_log));
  fx->a_log.name = "A";
}

/** @brief Connects device A; fails the test with the transition summary when it does not. */
static void connect_a(fixture* fx, e2e_device_connect_options* opts)
{
  opts->before_open_ctx = fx;
  if (opts->before_open == NULL)
  {
    opts->before_open = observe_a;
  }
  const time_t start = time(NULL);
  const int rc = e2e_device_connect_with(&fx->a, opts);
  printf("[e2e] A connect: rc %d after %lds\n", rc, (long)(time(NULL) - start));
  print_log(&fx->a_log);
  if (rc != 0)
  {
    fail_msg("device A did not reach CONNECTED (hub state %s)", state_name(fx->a.conn_state));
  }
}

static void assert_mqttv5_profile(const e2e_device* dev)
{
  az_iot_hub_profile profile;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&dev->conn, &profile), AZ_IOT_OK);
  printf(
      "[e2e] hub %s device %s profile %d '%s'%s\n",
      az_iot_connection_client_get_iothub_address(&dev->conn),
      az_iot_connection_client_get_device_id(&dev->conn),
      (int)profile.connection_profile,
      profile.connection_profile_raw,
      profile.connection_profile_raw_truncated ? " (truncated)" : "");
  assert_int_equal(profile.connection_profile, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  assert_string_equal(profile.connection_profile_raw, "mqttV5");
  assert_false(profile.connection_profile_raw_truncated);
}

/** @brief Saves the hub of a DPS connect in g_saved, connecting once if no test has yet. */
static void ensure_saved_hub(fixture* fx)
{
  if (g_saved.hostname[0] != '\0')
  {
    return;
  }
  e2e_device_connect_options opts = { 0 };
  connect_a(fx, &opts);
  assert_int_equal(e2e_device_get_hub(&fx->a, &g_saved), 0);
  printf("[e2e] saved hub %s device %s\n", g_saved.hostname, g_saved.device_id);
  reset_a(fx);
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
    fail_msg("telemetry watch failed: %s", err ? err : "unknown");
  }
}

/** @brief Sends marked telemetry from device A and asserts it reaches the events endpoint. */
static void assert_telemetry_routed(fixture* fx)
{
  if (!fx->telemetry_ok)
  {
    assert_int_equal(az_iot_mqttv5_telemetry_client_init(&fx->telemetry, &fx->a.conn), AZ_IOT_OK);
    fx->telemetry_ok = true;
  }
  assert_true(fx->publishes_used < E2E_MAX_PUBLISHES);
  op_record* record = &fx->publishes[fx->publishes_used++];

  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "cn", fx->a.device_id);
  char payload[E2E_MARKER_MAX + 16];
  snprintf(payload, sizeof(payload), "{\"marker\":\"%s\"}", marker);

  watch_begin(fx);
  az_iot_telemetry_message message = { 0 };
  message.payload = (const uint8_t*)payload;
  message.payload_len = strlen(payload);
  const az_iot_result sent
      = az_iot_mqttv5_telemetry_client_send(&fx->telemetry, &message, on_publish, record);
  if (sent != AZ_IOT_OK)
  {
    az_iot_e2e_service_telemetry_watch_end(fx->service);
    fail_msg("telemetry send: %s", az_iot_result_to_string(sent));
  }

  bool seen = false;
  for (time_t start = time(NULL); !seen && (time(NULL) - start) < E2E_ROUTE_TIMEOUT_S;)
  {
    pump(fx);
    assert_true(az_iot_e2e_service_do_work(fx->service, E2E_PUMP_MS));
    seen = az_iot_e2e_service_telemetry_seen(fx->service, marker);
  }
  for (time_t start = time(NULL); !record->done && (time(NULL) - start) < E2E_ACK_TIMEOUT_S;)
  {
    pump(fx);
  }
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  printf(
      "[e2e] telemetry %s: completion %s, routed %s\n",
      marker,
      record->done ? az_iot_result_to_string(record->status) : "(none)",
      seen ? "yes" : "no");
  assert_true(record->done);
  assert_int_equal(record->status, AZ_IOT_OK);
  assert_true(seen);
}

/** @brief A twin GET from device A completes with AZ_IOT_OK. */
static void assert_twin_get(fixture* fx)
{
  memset(&fx->get, 0, sizeof(fx->get));
  const az_iot_result rc = az_iot_mqttv5_twin_client_get(&fx->twin, on_get, &fx->get);
  if (rc != AZ_IOT_OK)
  {
    fail_msg("twin GET: %s", az_iot_result_to_string(rc));
  }
  for (time_t start = time(NULL); !fx->get.done && (time(NULL) - start) < E2E_REQUEST_TIMEOUT_S;)
  {
    pump(fx);
  }
  printf("[e2e] twin GET: %s\n", fx->get.done ? az_iot_result_to_string(fx->get.status) : "(none)");
  if (!fx->get.done || fx->get.status != AZ_IOT_OK)
  {
    fail_msg(
        "twin GET: %s",
        fx->get.done ? az_iot_result_to_string(fx->get.status) : "no response in time");
  }
}

static int teardown(void** state);

static int setup(void** state)
{
  memset(&g_fx, 0, sizeof(g_fx));
  g_fx.a_log.name = "A";
  g_fx.b_log.name = "B";
  *state = &g_fx;
  e2e_install_log_sink();

  const char* svc_err = NULL;
  g_fx.service = az_iot_e2e_service_create(&svc_err);
  if (g_fx.service == NULL)
  {
    fprintf(stderr, "[e2e] service create failed: %s\n", svc_err ? svc_err : "unknown");
    (void)teardown(state);
    return -1;
  }
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx == NULL)
  {
    fx = &g_fx;
  }
  release_clients(fx);
  e2e_device_disconnect(&fx->b);
  fx->b_active = false;
  e2e_device_disconnect(&fx->a);
  if (fx->service != NULL)
  {
    az_iot_e2e_service_destroy(fx->service);
    fx->service = NULL;
  }
  return 0;
}

/* DPS reports the hub's profile in the assignment; the client exposes it verbatim. */
static void dps_assigns_mqttv5_profile(void** state)
{
  fixture* fx = (fixture*)*state;
  e2e_device_connect_options opts = { 0 };
  connect_a(fx, &opts);
  assert_true(fx->a_log.dps_connected >= 1);
  assert_mqttv5_profile(&fx->a);
  assert_int_equal(e2e_device_get_hub(&fx->a, &g_saved), 0);
  printf("[e2e] saved hub %s device %s\n", g_saved.hostname, g_saved.device_id);
}

/* A saved mqttv5 hub with DPS configured is connected directly; the DPS session never starts. */
static void saved_mqttv5_hub_connects_without_dps(void** state)
{
  fixture* fx = (fixture*)*state;
  ensure_saved_hub(fx);
  e2e_device_connect_options opts = { 0 };
  opts.saved_hub = &g_saved;
  connect_a(fx, &opts);

  assert_int_equal(fx->a_log.dps_events, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(&fx->a.conn, AZ_IOT_CONN_SCOPE_DPS),
      AZ_IOT_CONN_STATE_IDLE);
  assert_string_equal(az_iot_connection_client_get_iothub_address(&fx->a.conn), g_saved.hostname);
  assert_mqttv5_profile(&fx->a);
  assert_telemetry_routed(fx);
  assert_int_equal(fx->a_log.dps_events, 0);
}

/* A saved hub that cannot be reached is given up after the threshold, and DPS assigns the real
 * one. */
static void unreachable_saved_hub_falls_back_to_dps(void** state)
{
  fixture* fx = (fixture*)*state;
  ensure_saved_hub(fx);
  e2e_device_hub unreachable = g_saved;
  /* .invalid never resolves (RFC 2606). */
  snprintf(
      unreachable.hostname,
      sizeof(unreachable.hostname),
      "e2e-unreachable-%08lx.azure-devices.invalid",
      (unsigned long)time(NULL));
  const az_iot_retry_policy retry
      = az_iot_connection_client_get_fixed_interval_retry_policy(E2E_RETRY_INTERVAL_MS, 0);
  e2e_device_connect_options opts = { 0 };
  opts.saved_hub = &unreachable;
  opts.reconnection_policy = &retry;
  opts.identity_recovery_policy = &retry;
  opts.max_hub_connect_attempts_before_reprovision = E2E_REPROVISION_THRESHOLD;
  opts.connect_timeout_s = E2E_FALLBACK_TIMEOUT_S;
  snprintf(
      fx->a_log.watched_endpoint, sizeof(fx->a_log.watched_endpoint), "%s", unreachable.hostname);
  connect_a(fx, &opts);

  /* Latched: a transient failure at the assigned hub may follow and must not hide the fallback. */
  assert_true(fx->a_log.watched_failures >= E2E_REPROVISION_THRESHOLD);
  assert_true(fx->a_log.reprovision_announced);
  assert_true(fx->a_log.dps_connected >= 1);
  assert_string_equal(az_iot_connection_client_get_iothub_address(&fx->a.conn), g_saved.hostname);
  assert_mqttv5_profile(&fx->a);
}

/* A saved hub declared mqttv3 that is really mqttv5: whatever the hub does with the MQTT 3.1.1
 * CONNECT, the device must end on mqttv5 after DPS. */
static void saved_hub_declared_mqttv3_is_corrected_by_dps(void** state)
{
  fixture* fx = (fixture*)*state;
  ensure_saved_hub(fx);
  e2e_device_hub declared_v3 = g_saved;
  declared_v3.profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
  const az_iot_retry_policy retry
      = az_iot_connection_client_get_fixed_interval_retry_policy(E2E_RETRY_INTERVAL_MS, 0);
  const az_iot_identity_recovery_mode reprovision = AZ_IOT_IDENTITY_RECOVERY_REPROVISION;
  e2e_device_connect_options opts = { 0 };
  opts.saved_hub = &declared_v3;
  opts.reconnection_policy = &retry;
  opts.identity_recovery_mode = &reprovision;
  opts.identity_recovery_policy = &retry;
  opts.max_hub_connect_attempts_before_reprovision = E2E_REPROVISION_THRESHOLD;
  opts.connect_timeout_s = E2E_FALLBACK_TIMEOUT_S;
  const time_t start = time(NULL);
  connect_a(fx, &opts);

  /* The first CONNECTED may be the MQTT 3.1.1 session; keep pumping until DPS restores mqttv5. */
  az_iot_hub_profile profile;
  bool on_v5 = false;
  for (;;)
  {
    on_v5 = fx->a.conn_state == AZ_IOT_CONN_STATE_CONNECTED
        && az_iot_connection_client_get_hub_profile(&fx->a.conn, &profile) == AZ_IOT_OK
        && profile.connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
    if (on_v5 || fx->a.conn_state == AZ_IOT_CONN_STATE_FAULTED
        || (time(NULL) - start) >= E2E_FALLBACK_TIMEOUT_S)
    {
      break;
    }
    pump(fx);
  }
  print_log(&fx->a_log);
  if (!on_v5)
  {
    memset(&profile, 0, sizeof(profile));
    const az_iot_result got = az_iot_connection_client_get_hub_profile(&fx->a.conn, &profile);
    fail_msg(
        "no mqttv5 session within %ds: hub state %s, profile %s %d '%s' (DPS connected %d times, "
        "hub drops %d)",
        E2E_FALLBACK_TIMEOUT_S,
        state_name(fx->a.conn_state),
        az_iot_result_to_string(got),
        (int)profile.connection_profile,
        got == AZ_IOT_OK ? profile.connection_profile_raw : "",
        fx->a_log.dps_connected,
        fx->a_log.hub_drops);
  }
  assert_true(fx->a_log.dps_connected >= 1);
  assert_string_equal(az_iot_connection_client_get_iothub_address(&fx->a.conn), g_saved.hostname);
  assert_mqttv5_profile(&fx->a);
}

/* close() + open() on one client repeats the whole connect, birth included, each time. */
static void close_and_reopen_repeats_birth(void** state)
{
  fixture* fx = (fixture*)*state;
  e2e_device_connect_options opts = { 0 };
  opts.before_open = observe_a_with_twin;
  connect_a(fx, &opts);
  assert_true(fx->a_log.hub_connected >= 1);
  for (int cycle = 1; cycle <= E2E_OPEN_CLOSE_CYCLES; ++cycle)
  {
    if (cycle > 1)
    {
      const int connects = fx->a_log.hub_connected;
      (void)az_iot_connection_client_close(&fx->a.conn);
      if (!wait_hub_state(fx, &fx->a, AZ_IOT_CONN_STATE_IDLE, E2E_CLOSE_TIMEOUT_S))
      {
        fail_msg("cycle %d: close did not reach IDLE (%s)", cycle, state_name(fx->a.conn_state));
      }
      const az_iot_result opened = az_iot_connection_client_open(&fx->a.conn);
      if (opened != AZ_IOT_OK)
      {
        fail_msg("cycle %d: open: %s", cycle, az_iot_result_to_string(opened));
      }
      if (!wait_hub_state(fx, &fx->a, AZ_IOT_CONN_STATE_CONNECTED, E2E_CONNECT_TIMEOUT_S))
      {
        print_log(&fx->a_log);
        fail_msg("cycle %d: not CONNECTED (%s)", cycle, state_name(fx->a.conn_state));
      }
      /* At least one new CONNECTED: a transient drop and retry may add more. */
      assert_true(fx->a_log.hub_connected > connects);
    }
    printf("[e2e] cycle %d connected (%d in total)\n", cycle, fx->a_log.hub_connected);
    assert_mqttv5_profile(&fx->a);
    assert_twin_get(fx);
  }
  print_log(&fx->a_log);
}

/* A second connection with the same identity evicts the first; once it closes, the first
 * reconnects on its own and its feature clients work again. */
static void evicted_session_reconnects_and_restores_clients(void** state)
{
  fixture* fx = (fixture*)*state;
  ensure_saved_hub(fx);
  const az_iot_retry_policy retry
      = az_iot_connection_client_get_fixed_interval_retry_policy(E2E_EVICTED_RETRY_INTERVAL_MS, 0);
  e2e_device_connect_options a_opts = { 0 };
  a_opts.saved_hub = &g_saved;
  a_opts.reconnection_policy = &retry;
  a_opts.identity_recovery_policy = &retry;
  a_opts.before_open = observe_a_with_clients;
  connect_a(fx, &a_opts);
  assert_twin_get(fx);

  /* B connects to the saved hub directly and never retries, so it cannot take the session back.
   * A is not pumped meanwhile, so it cannot reconnect before B is up. */
  const az_iot_retry_policy no_retry = az_iot_connection_client_get_disabled_retry_policy();
  e2e_device_connect_options b_opts = { 0 };
  b_opts.saved_hub = &g_saved;
  b_opts.reconnection_policy = &no_retry;
  b_opts.before_open = observe_b;
  b_opts.before_open_ctx = fx;
  fx->b_active = true;
  const int b_rc = e2e_device_connect_with(&fx->b, &b_opts);
  print_log(&fx->b_log);
  if (b_rc != 0)
  {
    fail_msg("device B did not reach CONNECTED (hub state %s)", state_name(fx->b.conn_state));
  }

  const int drops = fx->a_log.hub_drops;
  for (time_t start = time(NULL);
       fx->a_log.hub_drops == drops && (time(NULL) - start) < E2E_EVICT_TIMEOUT_S;)
  {
    pump(fx);
  }
  const bool evicted = fx->a_log.hub_drops != drops;
  /* Close B at once, before A's retry could evict it in turn. */
  fx->b_active = false;
  e2e_device_disconnect(&fx->b);
  print_log(&fx->a_log);
  print_log(&fx->b_log);
  if (!evicted)
  {
    fail_msg("A was not disconnected within %ds of B connecting", E2E_EVICT_TIMEOUT_S);
  }

  const int connects = fx->a_log.hub_connected;
  for (time_t start = time(NULL);
       fx->a_log.hub_connected == connects && (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S;)
  {
    pump(fx);
  }
  print_log(&fx->a_log);
  if (fx->a.conn_state != AZ_IOT_CONN_STATE_CONNECTED)
  {
    fail_msg(
        "A did not reconnect within %ds (%s)", E2E_CONNECT_TIMEOUT_S, state_name(fx->a.conn_state));
  }
  assert_mqttv5_profile(&fx->a);
  assert_telemetry_routed(fx);
  assert_twin_get(fx);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(dps_assigns_mqttv5_profile, setup, teardown),
    cmocka_unit_test_setup_teardown(saved_mqttv5_hub_connects_without_dps, setup, teardown),
    cmocka_unit_test_setup_teardown(unreachable_saved_hub_falls_back_to_dps, setup, teardown),
    cmocka_unit_test_setup_teardown(saved_hub_declared_mqttv3_is_corrected_by_dps, setup, teardown),
    cmocka_unit_test_setup_teardown(close_and_reopen_repeats_birth, setup, teardown),
    cmocka_unit_test_setup_teardown(
        evicted_session_reconnects_and_restores_clients, setup, teardown),
  };
  return cmocka_run_group_tests_name("e2e_mqttv5_connection", tests, NULL, NULL);
}
