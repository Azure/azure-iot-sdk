// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/*
 * Paho adapter teardown under load, against a real broker.
 *
 * Each round connects, subscribes to its own topic, queues a QoS 1 flood to
 * that topic and tears down at once, while Paho's threads are still sending,
 * receiving and acking. Half the rounds call disconnect() then destroy(); the
 * other half call destroy() alone. The adapter relies on Paho running every
 * callback and MQTTAsync_destroy() under one global mutex, so no callback
 * can touch the adapter after destroy() returns. This test pins that: a
 * use-after-free or race shows under ASan, valgrind, helgrind and DRD.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "support/test_env.h"

#define ROUNDS_PER_VERSION 6
#define FLOOD 32
#define PUMP_SLICE_MS 10u

static char* g_host;
static uint16_t g_port = 1883;
static unsigned g_scale = 1;
static unsigned long g_run_id;

typedef struct
{
  int connected;
  int suback;
  int failures;
} round_log;

/* Runs on the test thread only: process_loop() dispatches the adapter's queue. */
static void on_event(const az_iot_mqtt_event* evt, void* ctx)
{
  round_log* log = (round_log*)ctx;
  switch (evt->kind)
  {
    case AZ_IOT_MQTT_EVT_CONNECTED:
      log->connected += (evt->status == AZ_IOT_OK);
      log->failures += (evt->status != AZ_IOT_OK);
      break;
    case AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK:
      log->suback += (evt->status == AZ_IOT_OK);
      log->failures += (evt->status != AZ_IOT_OK);
      break;
    case AZ_IOT_MQTT_EVT_DISCONNECTED:
    case AZ_IOT_MQTT_EVT_MESSAGE:
    case AZ_IOT_MQTT_EVT_PUBLISH_ACK:
    case AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK:
    case AZ_IOT_MQTT_EVT_ERROR:
      break;
  }
}

/* Pumps until *counter is non-zero or a failure is seen, within 5 s times the
 * timeout scale. Returns true when *counter was reached. */
static bool pump_until(az_iot_mqtt_client* c, const round_log* log, const int* counter)
{
  unsigned const slices = (5000u / PUMP_SLICE_MS) * g_scale;
  for (unsigned i = 0; i < slices && *counter == 0 && log->failures == 0; ++i)
  {
    assert_int_equal(c->iface->process_loop(c, PUMP_SLICE_MS), AZ_IOT_OK);
  }
  return *counter != 0;
}

static void run_round(az_iot_mqtt_version version, int round)
{
  char client_id[64];
  char topic[96];
  (void)snprintf(client_id, sizeof(client_id), "az-teardown-%lu-%d", g_run_id, round);
  (void)snprintf(topic, sizeof(topic), "az/teardown/%lu/%d", g_run_id, round);

  /* Paho sizes its own buffers: the limits only have to be valid. */
  static const az_iot_mqtt_limits k_limits = { 4096, 4096, 8, 0, 1024 };
  size_t const workspace_size = az_iot_paho_get_workspace_size(&k_limits);
  /* On the heap and freed after destroy(), so a Paho thread still touching the client is a
   * use-after-free the sanitizers report. */
  uint8_t* workspace = (uint8_t*)malloc(workspace_size);
  assert_non_null(workspace);
  az_iot_mqtt_client* c = NULL;
  assert_int_equal(
      az_iot_paho_get_adapter()->init(version, &k_limits, workspace, workspace_size, &c),
      AZ_IOT_OK);
  round_log log = { 0 };
  c->iface->set_inbound_cb(c, on_event, &log);

  az_iot_mqtt_connect_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = g_host;
  opts.port = g_port;
  opts.client_id = client_id;
  opts.keep_alive_seconds = 30;
  opts.connect_timeout_seconds = 5u * g_scale;
  opts.clean_start = true;
  assert_int_equal(c->iface->connect(c, &opts), AZ_IOT_OK);
  assert_true(pump_until(c, &log, &log.connected));

  uint16_t pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &pid), AZ_IOT_OK);
  assert_true(pump_until(c, &log, &log.suback));

  static const uint8_t payload[64] = { 0 };
  for (int i = 0; i < FLOOD; ++i)
  {
    az_iot_mqtt_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.topic = topic;
    msg.payload = payload;
    msg.payload_len = sizeof(payload);
    msg.qos = AZ_IOT_MQTT_QOS_1;
    assert_int_equal(c->iface->publish(c, &msg, &pid), AZ_IOT_OK);
  }

  /* No pumping from here: Paho's threads are mid-flood when teardown starts. */
  if ((round % 2) == 0)
  {
    assert_int_equal(c->iface->disconnect(c), AZ_IOT_OK);
  }
  c->iface->destroy(c);
  free(workspace);
}

static void teardown_under_load(az_iot_mqtt_version version, int first_round)
{
  for (int r = 0; r < ROUNDS_PER_VERSION; ++r)
  {
    run_round(version, first_round + r);
  }
}

static void test_teardown_under_load_v3(void** state)
{
  (void)state;
  teardown_under_load(AZ_IOT_MQTT_VERSION_3_1_1, 0);
}

static void test_teardown_under_load_v5(void** state)
{
  (void)state;
  teardown_under_load(AZ_IOT_MQTT_VERSION_5, ROUNDS_PER_VERSION);
}

int main(void)
{
  g_host = az_iot_test_env_dup("AZ_IOT_MQTT_BROKER_HOST");
  if (g_host == NULL)
  {
    fprintf(
        stderr,
        "paho-teardown: AZ_IOT_MQTT_BROKER_HOST is unset or empty, but this test was built "
        "with AZ_IOT_BUILD_CONFORMANCE_TESTS=ON. Point it at a reachable plaintext broker, or "
        "configure with AZ_IOT_BUILD_CONFORMANCE_TESTS=OFF so it is not registered.\n");
    return 1;
  }
  char* port = az_iot_test_env_dup("AZ_IOT_MQTT_BROKER_PORT");
  if (port != NULL)
  {
    g_port = (uint16_t)atoi(port);
    free(port);
  }
  g_scale = az_iot_test_timeout_scale();
  g_run_id = (unsigned long)time(NULL);

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_teardown_under_load_v3),
    cmocka_unit_test(test_teardown_under_load_v5),
  };
  int rc = cmocka_run_group_tests_name("paho_teardown_race", tests, NULL, NULL);
  free(g_host);
  return rc;
}
