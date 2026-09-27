// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end transport scenarios: MQTT over WebSockets, and MQTT tunnelled
 * through an HTTP CONNECT proxy, against a real Azure IoT Hub.
 *
 * These are the two ways a device on a filtered network reaches the service,
 * and neither can be proven by a unit test: the unit tests assert what the
 * adapter asks Paho for, while what matters here is that the service answers.
 * Each scenario therefore does a full round trip -- connect, publish telemetry,
 * and observe that same message arriving on the hub's Event Hub-compatible
 * endpoint -- so a proxy or WebSocket path that connects but carries nothing
 * fails the test.
 *
 * Unlike the other suites the device is connected per scenario rather than once
 * for the group, because the transport under test is fixed at connect time.
 *
 * Configuration: the standard e2e device and service variables (see
 * e2e_device.h and az_iot_e2e_service.h), plus
 *   AZ_IOT_PROXY_HOST, AZ_IOT_PROXY_PORT   HTTP proxy to tunnel through
 *   AZ_IOT_PROXY_USERNAME, AZ_IOT_PROXY_PASSWORD  (optional)
 * The proxy scenarios are skipped when no proxy is configured; the WebSocket
 * scenario always runs, through the proxy when one is configured and directly
 * when one is not.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot.h"

#include "az_iot_e2e_service.h"
#include "e2e_device.h"
#include "e2e_log.h"

#define E2E_TELEMETRY_TIMEOUT_S 90
/* Cold service-AMQP connect retry budget. Larger than the 45 s the scenario
 * suite uses: this suite connects a device per scenario rather than once per
 * group, so the FIRST watch_begin here is the process's cold TLS + AMQP + CBS
 * handshake against the Event Hub-compatible endpoint, and that has been
 * observed to need more than 45 s and then succeed. A budget that expires
 * during a handshake that was going to work reports a transport failure the
 * transport did not have. */
#define E2E_CONNECT_TIMEOUT_S 120
#define E2E_PUMP_MS 20

typedef struct
{
  az_iot_e2e_service* service;
} e2e_fixture;

static e2e_fixture g_fixture;

/* Portable setenv/unsetenv: the device fixture reads its configuration from the
 * environment, and each scenario has to choose a different transport. */
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

static const char* env_or_null(const char* name)
{
  const char* v = getenv(name);
  return (v != NULL && v[0] != '\0') ? v : NULL;
}

static bool proxy_is_configured(void) { return env_or_null("AZ_IOT_PROXY_HOST") != NULL; }

static int group_setup(void** state)
{
  memset(&g_fixture, 0, sizeof(g_fixture));
  srand((unsigned)time(NULL));
  e2e_install_log_sink();

  const char* svc_err = NULL;
  g_fixture.service = az_iot_e2e_service_create(&svc_err);
  if (g_fixture.service == NULL)
  {
    fprintf(
        stderr,
        "[e2e] service client create failed: %s\n",
        (svc_err != NULL) ? svc_err : "unknown");
    return -1;
  }
  *state = &g_fixture;
  return 0;
}

static int group_teardown(void** state)
{
  e2e_fixture* fx = (e2e_fixture*)*state;
  if (fx != NULL && fx->service != NULL)
  {
    az_iot_e2e_service_destroy(fx->service);
    fx->service = NULL;
  }
  return 0;
}

static void make_marker(char* out, size_t cap, const char* tag)
{
  static unsigned counter = 0;
  unsigned long long t = (unsigned long long)time(NULL);
  unsigned r = (unsigned)rand();
  snprintf(out, cap, "%s-%08llx%04x%03x", tag, t & 0xffffffffull, r & 0xffff, (counter++) & 0xfff);
}

typedef struct
{
  int done;
  az_iot_result status;
} send_ctx;

static void on_send_done(az_iot_result status, void* user_ctx)
{
  send_ctx* c = (send_ctx*)user_ctx;
  c->status = status;
  c->done = 1;
}

/* Connect a device with the egress configuration currently in the environment,
 * publish one uniquely marked telemetry message, and wait for the cloud to
 * report that exact message. Anything short of a working tunnel fails here. */
static void telemetry_round_trip(e2e_fixture* fx, const char* tag)
{
  e2e_device dev;
  memset(&dev, 0, sizeof(dev));
  assert_int_equal(e2e_device_connect(&dev), 0);

  char marker[64];
  make_marker(marker, sizeof(marker), tag);
  char payload[128];
  snprintf(payload, sizeof(payload), "{\"e2e\":\"%s\",\"marker\":\"%s\"}", tag, marker);

  bool watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  for (time_t connect_start = time(NULL);
       !watching && (time(NULL) - connect_start) < E2E_CONNECT_TIMEOUT_S;)
  {
    for (int i = 0; i < 50; i++)
    {
      e2e_device_do_work(&dev, E2E_PUMP_MS);
    }
    watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  }
  if (!watching)
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fprintf(stderr, "[e2e] telemetry watch begin failed: %s\n", (err != NULL) ? err : "unknown");
  }
  assert_true(watching);

  az_iot_mqttv3_telemetry_client telemetry_client;
  assert_int_equal(az_iot_mqttv3_telemetry_client_init(&telemetry_client, &dev.conn), AZ_IOT_OK);

  az_iot_telemetry_property props[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
  };
  az_iot_telemetry_message msg = { 0 };
  msg.payload = (const uint8_t*)payload;
  msg.payload_len = strlen(payload);
  msg.properties = props;
  msg.properties_count = sizeof(props) / sizeof(props[0]);

  send_ctx sc = { 0 };
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&telemetry_client, &msg, on_send_done, &sc), AZ_IOT_OK);

  bool seen = false;
  time_t start = time(NULL);
  while ((time(NULL) - start) < E2E_TELEMETRY_TIMEOUT_S)
  {
    e2e_device_do_work(&dev, E2E_PUMP_MS);
    assert_true(az_iot_e2e_service_do_work(fx->service, E2E_PUMP_MS));
    if (az_iot_e2e_service_telemetry_seen(fx->service, marker))
    {
      seen = true;
      break;
    }
  }
  for (time_t ack = time(NULL); !sc.done && (time(NULL) - ack) < 5;)
  {
    e2e_device_do_work(&dev, E2E_PUMP_MS);
  }

  az_iot_mqttv3_telemetry_client_deinit(&telemetry_client);
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  e2e_device_disconnect(&dev);

  assert_true(sc.done);
  assert_int_equal(sc.status, AZ_IOT_OK);
  if (!seen)
  {
    fprintf(stderr, "[e2e] marker '%s' not observed within %ds\n", marker, E2E_TELEMETRY_TIMEOUT_S);
  }
  assert_true(seen);
}

/* Plain MQTT, tunnelled: the case where the device may open outbound TCP only
 * through the proxy, even for 8883. */
static void telemetry_over_tcp_through_a_proxy(void** state)
{
  e2e_fixture* fx = (e2e_fixture*)*state;
  if (!proxy_is_configured())
  {
    fprintf(stderr, "[e2e] AZ_IOT_PROXY_HOST not set; skipping the TCP proxy scenario\n");
    skip();
  }
  set_env("AZ_IOT_MQTT_TRANSPORT", "tcp");
  telemetry_round_trip(fx, "proxy-tcp");
}

/* MQTT over WebSockets on 443, which is what a device gets on a network that
 * passes HTTPS and nothing else. Uses the proxy too when one is configured,
 * because such a network usually requires both. */
static void telemetry_over_websockets(void** state)
{
  e2e_fixture* fx = (e2e_fixture*)*state;
  set_env("AZ_IOT_MQTT_TRANSPORT", "websocket");
  telemetry_round_trip(fx, "websocket");
  set_env("AZ_IOT_MQTT_TRANSPORT", "tcp");
}

/* The failure that must not happen quietly: when the configured proxy cannot be
 * reached, the device has to stay disconnected rather than fall back to a
 * direct connection. A silent fallback would look like success here while
 * violating the egress policy the proxy exists to enforce.
 *
 * Port 1 on the loopback interface is refused rather than filtered, so the
 * attempt fails fast and the assertion is about the fallback, not a timeout. */
static void an_unreachable_proxy_does_not_fall_back_to_a_direct_connection(void** state)
{
  (void)state;
  const char* saved_host = env_or_null("AZ_IOT_PROXY_HOST");
  const char* saved_port = env_or_null("AZ_IOT_PROXY_PORT");
  char host_copy[256] = { 0 };
  char port_copy[16] = { 0 };
  if (saved_host != NULL)
  {
    snprintf(host_copy, sizeof(host_copy), "%s", saved_host);
  }
  if (saved_port != NULL)
  {
    snprintf(port_copy, sizeof(port_copy), "%s", saved_port);
  }

  set_env("AZ_IOT_MQTT_TRANSPORT", "tcp");
  set_env("AZ_IOT_PROXY_HOST", "127.0.0.1");
  set_env("AZ_IOT_PROXY_PORT", "1");
  set_env("AZ_IOT_PROXY_USERNAME", NULL);
  set_env("AZ_IOT_PROXY_PASSWORD", NULL);

  e2e_device dev;
  memset(&dev, 0, sizeof(dev));
  int rc = e2e_device_connect(&dev);
  e2e_device_disconnect(&dev);

  set_env("AZ_IOT_PROXY_HOST", host_copy[0] != '\0' ? host_copy : NULL);
  set_env("AZ_IOT_PROXY_PORT", port_copy[0] != '\0' ? port_copy : NULL);

  assert_int_not_equal(rc, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(telemetry_over_tcp_through_a_proxy),
    cmocka_unit_test(telemetry_over_websockets),
    cmocka_unit_test(an_unreachable_proxy_does_not_fall_back_to_a_direct_connection),
  };
  return cmocka_run_group_tests_name("e2e_transport", tests, group_setup, group_teardown);
}
