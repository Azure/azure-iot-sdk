// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end: MQTT over WebSockets and MQTT through an HTTP CONNECT proxy on an mqttv5 hub.
 *
 * Each test connects its own device (the transport is fixed at connect), provisions through DPS
 * over that transport, checks the hub profile is mqttv5, and round-trips a marked telemetry message
 * to the events endpoint (IOTHUB_* variables, see az_iot_e2e_service.h).
 *
 * Besides the e2e_device.h variables, the proxy tests need AZ_IOT_PROXY_HOST, AZ_IOT_PROXY_PORT
 * and AZ_IOT_E2E_PROXY_LOG, the proxy's access log: the tests assert it recorded a CONNECT for the
 * DPS and the hub host, so a device that connected around the proxy fails. A tinyproxy or squid
 * log works: both record the CONNECT request line. Missing proxy configuration fails the tests.
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

#define E2E_TELEMETRY_TIMEOUT_S 90
/* The first watch per process is a cold AMQP connect (see e2e_transport_test.c). */
#define E2E_SERVICE_CONNECT_TIMEOUT_S 120
#define E2E_ACK_TIMEOUT_S 30
/* How long an unreachable proxy must keep the device off the hub after the connect gave up. */
#define E2E_NO_FALLBACK_WINDOW_S 30
#define E2E_PUMP_MS 20
/* Upper bound on the proxy log read per test. */
#define E2E_PROXY_LOG_MAX (512 * 1024)
#define E2E_DEFAULT_DPS_HOST "global.azure-devices-provisioning.net"

typedef struct
{
  int done;
  az_iot_result status;
} send_record;

/** @brief Hub CONNECTED events seen while the proxy is unreachable. */
typedef struct
{
  int connected;
} no_fallback_record;

typedef struct
{
  az_iot_e2e_service* service;
  /* Configuration from the environment at start (heap, NULL when unset); each test sets its own
   * egress. */
  char* proxy_host;
  char* proxy_port;
  char* proxy_log;
  char* dps_host;
  /* Device or registration id, so a marker cannot match another run's telemetry. */
  char* device_id;
  /* Per-test state, here rather than on the stack so test_teardown can release it after a
   * failed assertion. */
  e2e_device dev;
  az_iot_mqttv5_telemetry_client telemetry;
  bool telemetry_ok;
  bool watching;
  send_record sent;
  no_fallback_record no_fallback;
} fixture;

static fixture g_fx;

/** @brief Portable setenv/unsetenv: e2e_device reads its egress configuration from the
 * environment. */
static void set_env(const char* name, const char* value)
{
#ifdef _WIN32
  (void)_putenv_s(name, value != NULL ? value : "");
#else
  if (value != NULL)
  {
    (void)setenv(name, value, 1);
  }
  else
  {
    (void)unsetenv(name);
  }
#endif
}

static const char* or_empty(const char* s) { return s != NULL ? s : ""; }

/** @brief Selects the transport and proxy for the next connect; NULL @p proxy_host means direct.
 * The WebSocket path is always the default. */
static void set_egress(const char* transport, const char* proxy_host, const char* proxy_port)
{
  set_env("AZ_IOT_MQTT_TRANSPORT", transport);
  set_env("AZ_IOT_MQTT_WEBSOCKET_PATH", NULL);
  set_env("AZ_IOT_PROXY_HOST", proxy_host);
  set_env("AZ_IOT_PROXY_PORT", proxy_port);
}

/** @brief Opens @p path for binary reading; NULL on failure. */
static FILE* open_read(const char* path)
{
#ifdef _MSC_VER
  FILE* f = NULL;
  return fopen_s(&f, path, "rb") == 0 ? f : NULL;
#else
  return fopen(path, "rb");
#endif
}

static void on_send(az_iot_result status, void* user_ctx)
{
  send_record* r = (send_record*)user_ctx;
  r->status = status;
  r->done = 1;
}

/* Time, a random nonce and a counter come first so truncation never cuts them; the device id
 * separates parallel runs against the same hub. */
static void make_marker(const fixture* fx, char* out, size_t cap, const char* tag)
{
  static unsigned counter = 0;
  snprintf(
      out,
      cap,
      "%s-%08lx%04x%03x-%.48s",
      tag,
      (unsigned long)time(NULL) & 0xfffffffful,
      (unsigned)rand() & 0xffffu,
      (counter++) & 0xfffu,
      fx->device_id != NULL ? fx->device_id : "noid");
}

static int group_setup(void** state)
{
  memset(&g_fx, 0, sizeof(g_fx));
  srand((unsigned)time(NULL) ^ (unsigned)clock());
  e2e_install_log_sink();
  g_fx.proxy_host = az_iot_test_env_dup("AZ_IOT_PROXY_HOST");
  g_fx.proxy_port = az_iot_test_env_dup("AZ_IOT_PROXY_PORT");
  g_fx.proxy_log = az_iot_test_env_dup("AZ_IOT_E2E_PROXY_LOG");
  g_fx.dps_host = az_iot_test_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT");
  char* hub_hostname = az_iot_test_env_dup("AZ_IOT_HUB_HOSTNAME");
  g_fx.device_id = az_iot_test_env_dup("AZ_IOT_DEVICE_ID");
  if (hub_hostname == NULL || g_fx.device_id == NULL)
  {
    free(g_fx.device_id);
    g_fx.device_id = az_iot_test_env_dup("AZ_IOT_DPS_REGISTRATION_ID");
  }
  free(hub_hostname);
  printf(
      "[e2e] proxy host='%s' port='%s' log='%s', device '%s'\n",
      or_empty(g_fx.proxy_host),
      or_empty(g_fx.proxy_port),
      or_empty(g_fx.proxy_log),
      or_empty(g_fx.device_id));

  const char* svc_err = NULL;
  g_fx.service = az_iot_e2e_service_create(&svc_err);
  if (g_fx.service == NULL)
  {
    fprintf(stderr, "[e2e] service create failed: %s\n", svc_err ? svc_err : "unknown");
    return -1;
  }
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
  if (fx->service != NULL)
  {
    az_iot_e2e_service_destroy(fx->service);
    fx->service = NULL;
  }
  free(fx->proxy_host);
  free(fx->proxy_port);
  free(fx->proxy_log);
  free(fx->dps_host);
  free(fx->device_id);
  fx->proxy_host = NULL;
  fx->proxy_port = NULL;
  fx->proxy_log = NULL;
  fx->dps_host = NULL;
  fx->device_id = NULL;
  return 0;
}

/** @brief Releases the telemetry client, the events watch and the device; safe to repeat. */
static void release(fixture* fx)
{
  if (fx->telemetry_ok)
  {
    az_iot_mqttv5_telemetry_client_deinit(&fx->telemetry);
    fx->telemetry_ok = false;
  }
  if (fx->watching)
  {
    az_iot_e2e_service_telemetry_watch_end(fx->service);
    fx->watching = false;
  }
  e2e_device_disconnect(&fx->dev);
}

/** @brief Per-test teardown: a failed test cannot leave a watch or a connection to the next. */
static int test_teardown(void** state)
{
  release((fixture*)*state);
  return 0;
}

/** @brief Fails the test unless the proxy, its port and its log are configured. */
static void require_proxy(const fixture* fx)
{
  if (fx->proxy_host == NULL || fx->proxy_port == NULL || fx->proxy_log == NULL)
  {
    fail_msg("AZ_IOT_PROXY_HOST, AZ_IOT_PROXY_PORT and AZ_IOT_E2E_PROXY_LOG are required (proxy "
             "tests do not skip)");
  }
}

/** @brief Current size of the proxy log, so a test reads only what it caused. */
static long proxy_log_size(const fixture* fx)
{
  FILE* f = open_read(fx->proxy_log);
  if (f == NULL)
  {
    fail_msg("cannot open proxy log '%s'", fx->proxy_log);
  }
  long size = -1;
  if (fseek(f, 0, SEEK_END) == 0)
  {
    size = ftell(f);
  }
  (void)fclose(f);
  assert_true(size >= 0);
  return size;
}

/** @brief Proxy log text written after @p offset (heap, NUL-terminated); from the start if the log
 * shrank. */
static char* proxy_log_since(const fixture* fx, long offset)
{
  FILE* f = open_read(fx->proxy_log);
  if (f == NULL)
  {
    fail_msg("cannot open proxy log '%s'", fx->proxy_log);
  }
  long size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
  if (size < offset)
  {
    offset = 0;
  }
  size_t len = size > offset ? (size_t)(size - offset) : 0;
  if (len > E2E_PROXY_LOG_MAX)
  {
    offset = size - E2E_PROXY_LOG_MAX;
    len = E2E_PROXY_LOG_MAX;
  }
  char* text = (char*)malloc(len + 1);
  if (text == NULL)
  {
    (void)fclose(f);
    fail_msg("out of memory reading the proxy log");
  }
  size_t got = 0;
  if (fseek(f, offset, SEEK_SET) == 0)
  {
    got = fread(text, 1, len, f);
  }
  (void)fclose(f);
  text[got] = '\0';
  return text;
}

/** @brief Asserts the proxy log, past @p offset, has the CONNECT request for @p host : @p port and,
 * when @p dps_host is set, the one for DPS before it. */
static void assert_tunnelled(
    const fixture* fx,
    long offset,
    const char* dps_host,
    const char* host,
    unsigned port)
{
  char* log = proxy_log_since(fx, offset);
  char hub_line[320];
  char dps_line[320];
  snprintf(hub_line, sizeof(hub_line), "CONNECT %s:%u ", host, port);
  snprintf(dps_line, sizeof(dps_line), "CONNECT %s:%u ", dps_host ? dps_host : "", port);
  const char* hub_at = strstr(log, hub_line);
  const char* dps_at = dps_host != NULL ? strstr(log, dps_line) : log;
  const bool ok = hub_at != NULL && dps_at != NULL && dps_at <= hub_at;
  printf(
      "[e2e] proxy log: '%s' %s, '%s' %s\n",
      dps_host ? dps_line : "(no DPS)",
      dps_at ? "found" : "missing",
      hub_line,
      hub_at ? "found" : "missing");
  if (!ok)
  {
    fprintf(stderr, "[e2e] proxy log since the connect:\n%s\n[e2e] end of proxy log\n", log);
  }
  free(log);
  assert_true(ok);
}

/** @brief Starts the events watch, pumping the device while the service connects. */
static void watch_begin(fixture* fx)
{
  bool watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  for (time_t start = time(NULL);
       !watching && (time(NULL) - start) < E2E_SERVICE_CONNECT_TIMEOUT_S;)
  {
    for (int i = 0; i < 50; ++i)
    {
      e2e_device_do_work(&fx->dev, E2E_PUMP_MS);
    }
    watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  }
  if (!watching)
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fprintf(stderr, "[e2e] telemetry watch failed: %s\n", err ? err : "unknown");
  }
  /* Set even on failure: a half-started watcher is still released by watch_end. */
  fx->watching = true;
  assert_true(watching);
}

/** @brief Connects with the egress in the environment, asserts an mqttv5 hub, round-trips one
 * marked telemetry message and, when @p proxied, asserts the proxy carried DPS and the hub. */
static void telemetry_round_trip(fixture* fx, const char* tag, bool proxied, unsigned port)
{
  const long log_offset = proxied ? proxy_log_size(fx) : 0;
  e2e_device* dev = &fx->dev;
  memset(dev, 0, sizeof(*dev));
  if (e2e_device_connect(dev) != 0)
  {
    if (proxied)
    {
      char* log = proxy_log_since(fx, log_offset);
      fprintf(stderr, "[e2e] proxy log since the connect:\n%s\n[e2e] end of proxy log\n", log);
      free(log);
    }
    fail_msg("%s: device did not connect (state=%d)", tag, (int)dev->conn_state);
  }

  az_iot_hub_profile profile;
  const az_iot_result profile_rc = az_iot_connection_client_get_hub_profile(&dev->conn, &profile);
  char hub[256];
  const char* address = az_iot_connection_client_get_iothub_address(&dev->conn);
  snprintf(hub, sizeof(hub), "%s", address != NULL ? address : "");
  /* Same condition e2e_device.c uses to skip DPS. */
  const bool via_dps = dev->hub_hostname == NULL || dev->hub_device_id == NULL;
  printf(
      "[e2e] %s: connected to '%s' via %s, profile %s ('%s')\n",
      tag,
      hub,
      via_dps ? "DPS" : "direct hub",
      az_iot_result_to_string(profile_rc),
      profile_rc == AZ_IOT_OK ? profile.connection_profile_raw : "");
  if (profile_rc != AZ_IOT_OK || profile.connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    fail_msg("%s: the device is not on an mqttv5 hub", tag);
  }

  char marker[128];
  make_marker(fx, marker, sizeof(marker), tag);
  char payload[256];
  snprintf(payload, sizeof(payload), "{\"e2e\":\"%s\",\"marker\":\"%s\"}", tag, marker);

  watch_begin(fx);
  assert_int_equal(az_iot_mqttv5_telemetry_client_init(&fx->telemetry, &dev->conn), AZ_IOT_OK);
  fx->telemetry_ok = true;
  az_iot_telemetry_property props[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
  };
  az_iot_telemetry_message message = { 0 };
  message.payload = (const uint8_t*)payload;
  message.payload_len = strlen(payload);
  message.properties = props;
  message.properties_count = sizeof(props) / sizeof(props[0]);
  send_record* sent = &fx->sent;
  memset(sent, 0, sizeof(*sent));
  const az_iot_result send_rc
      = az_iot_mqttv5_telemetry_client_send(&fx->telemetry, &message, on_send, sent);

  bool seen = false;
  for (time_t start = time(NULL);
       send_rc == AZ_IOT_OK && (time(NULL) - start) < E2E_TELEMETRY_TIMEOUT_S;)
  {
    e2e_device_do_work(dev, E2E_PUMP_MS);
    assert_true(az_iot_e2e_service_do_work(fx->service, E2E_PUMP_MS));
    if (az_iot_e2e_service_telemetry_seen(fx->service, marker))
    {
      seen = true;
      break;
    }
  }
  for (time_t start = time(NULL);
       send_rc == AZ_IOT_OK && !sent->done && (time(NULL) - start) < E2E_ACK_TIMEOUT_S;)
  {
    e2e_device_do_work(dev, E2E_PUMP_MS);
  }
  printf(
      "[e2e] %s: send %s, completion %s, marker '%s' %s\n",
      tag,
      az_iot_result_to_string(send_rc),
      sent->done ? az_iot_result_to_string(sent->status) : "(none)",
      marker,
      seen ? "seen" : "not seen");

  release(fx);

  assert_int_equal(send_rc, AZ_IOT_OK);
  assert_true(sent->done);
  assert_int_equal(sent->status, AZ_IOT_OK);
  assert_true(seen);
  if (proxied)
  {
    const char* dps_host = fx->dps_host != NULL ? fx->dps_host : E2E_DEFAULT_DPS_HOST;
    assert_tunnelled(fx, log_offset, via_dps ? dps_host : NULL, hub, port);
  }
}

/* DPS and the hub over WebSockets on 443 with the default path, no proxy: a network that passes
 * HTTPS only. */
static void telemetry_over_websockets(void** state)
{
  fixture* fx = (fixture*)*state;
  set_egress("websocket", NULL, NULL);
  telemetry_round_trip(fx, "v5-ws", false, 443u);
}

/* MQTT over TCP 8883, tunnelled through the proxy for DPS and the hub. */
static void telemetry_over_tcp_through_a_proxy(void** state)
{
  fixture* fx = (fixture*)*state;
  require_proxy(fx);
  set_egress("tcp", fx->proxy_host, fx->proxy_port);
  telemetry_round_trip(fx, "v5-proxy-tcp", true, 8883u);
}

/* WebSockets on 443, tunnelled through the proxy for DPS and the hub. */
static void websockets_through_a_proxy(void** state)
{
  fixture* fx = (fixture*)*state;
  require_proxy(fx);
  set_egress("websocket", fx->proxy_host, fx->proxy_port);
  telemetry_round_trip(fx, "v5-proxy-ws", true, 443u);
}

static void on_no_fallback_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB && event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    ((no_fallback_record*)user_ctx)->connected++;
  }
}

/* Attached before open() so a CONNECTED during the connect attempt is counted. */
static int attach_no_fallback(e2e_device* dev, void* ctx)
{
  fixture* fx = (fixture*)ctx;
  if (az_iot_connection_client_add_state_observer(
          &dev->conn, on_no_fallback_state, &fx->no_fallback)
      != AZ_IOT_OK)
  {
    return 1;
  }
  if (az_iot_mqttv5_telemetry_client_init(&fx->telemetry, &dev->conn) != AZ_IOT_OK)
  {
    return 1;
  }
  fx->telemetry_ok = true;
  return 0;
}

/* A proxy on a closed loopback port: the device must stay off the hub rather than connect directly,
 * and no telemetry may leave it. Port 1 refuses, so attempts fail fast. */
static void unreachable_proxy_does_not_fall_back(void** state)
{
  fixture* fx = (fixture*)*state;
  set_egress("tcp", "127.0.0.1", "1");

  e2e_device* dev = &fx->dev;
  memset(dev, 0, sizeof(*dev));
  no_fallback_record* rec = &fx->no_fallback;
  memset(rec, 0, sizeof(*rec));
  e2e_device_connect_options opts = { 0 };
  opts.before_open = attach_no_fallback;
  opts.before_open_ctx = fx;
  const int rc = e2e_device_connect_with(dev, &opts);
  printf("[e2e] unreachable proxy: connect rc=%d state=%d\n", rc, (int)dev->conn_state);

  char marker[128];
  make_marker(fx, marker, sizeof(marker), "v5-no-fallback");
  char payload[256];
  snprintf(payload, sizeof(payload), "{\"e2e\":\"no-fallback\",\"marker\":\"%s\"}", marker);
  az_iot_telemetry_message message = { 0 };
  message.payload = (const uint8_t*)payload;
  message.payload_len = strlen(payload);
  send_record* sent = &fx->sent;
  memset(sent, 0, sizeof(*sent));

  const bool telemetry_ok = fx->telemetry_ok;
  int accepted = 0;
  bool seen = false;
  if (telemetry_ok)
  {
    watch_begin(fx);
    time_t last_send = 0;
    for (time_t start = time(NULL); (time(NULL) - start) < E2E_NO_FALLBACK_WINDOW_S;)
    {
      if (time(NULL) != last_send && accepted == 0)
      {
        last_send = time(NULL);
        if (az_iot_mqttv5_telemetry_client_send(&fx->telemetry, &message, on_send, sent)
            == AZ_IOT_OK)
        {
          accepted++;
        }
      }
      e2e_device_do_work(dev, E2E_PUMP_MS);
      assert_true(az_iot_e2e_service_do_work(fx->service, E2E_PUMP_MS));
      seen = seen || az_iot_e2e_service_telemetry_seen(fx->service, marker);
    }
  }
  printf(
      "[e2e] unreachable proxy: hub CONNECTED %d time(s), send accepted %d, marker %s, state=%d\n",
      rec->connected,
      accepted,
      seen ? "seen" : "not seen",
      (int)dev->conn_state);
  release(fx);

  assert_true(telemetry_ok);
  assert_int_not_equal(rc, 0);
  assert_int_equal(rec->connected, 0);
  assert_int_equal(accepted, 0);
  assert_false(seen);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(telemetry_over_websockets, test_teardown),
    cmocka_unit_test_teardown(telemetry_over_tcp_through_a_proxy, test_teardown),
    cmocka_unit_test_teardown(websockets_through_a_proxy, test_teardown),
    cmocka_unit_test_teardown(unreachable_proxy_does_not_fall_back, test_teardown),
  };
  return cmocka_run_group_tests_name("e2e_mqttv5_transport", tests, group_setup, group_teardown);
}
