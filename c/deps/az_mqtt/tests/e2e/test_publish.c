// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_publish.c
 * @brief Integration tests for publish flows in AZ_MQTT_T(client).
 *
 * Requires a running broker on localhost:1883 (plain TCP).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// cmocka must be included after the standard headers above
#include <cmocka.h>

#include <azure/core/az_span.h>

#include "test_common.h"

#include <string.h>
#define PROCESS_LOOP_TIMEOUT_MS 100
#define WAIT_ITERATIONS 60

static az_mqtt_e2e_fixture s_fixture;

static bool s_puback_received;
static uint16_t s_puback_packet_id;
static int s_puback_reason;

static bool s_pubcomp_received;
static uint16_t s_pubcomp_packet_id;
static int s_pubcomp_reason;

static void reset_callback_state(void)
{
  s_puback_received = false;
  s_puback_packet_id = 0;
  s_puback_reason = -1;

  s_pubcomp_received = false;
  s_pubcomp_packet_id = 0;
  s_pubcomp_reason = -1;
}

static bool cond_puback_received(void)
{
  return s_puback_received;
}

static bool cond_pubcomp_received(void)
{
  return s_pubcomp_received;
}

static void on_puback(AZ_MQTT_T(client)* client, AZ_MQTT_T(ack_data) const* ack)
{
  (void)client;
  s_puback_received = true;
  s_puback_packet_id = ack->packet_id;
  s_puback_reason = AZ_MQTT_TEST_ACK_REASON(ack);
}

static void on_pubcomp(AZ_MQTT_T(client)* client, AZ_MQTT_T(ack_data) const* ack)
{
  (void)client;
  s_pubcomp_received = true;
  s_pubcomp_packet_id = ack->packet_id;
  s_pubcomp_reason = AZ_MQTT_TEST_ACK_REASON(ack);
}

static az_result init_client(AZ_MQTT_T(client)* client, az_span client_id)
{
  az_mqtt_e2e_fixture_reset(&s_fixture);

  az_mqtt_e2e_client_params params;
  memset(&params, 0, sizeof(params));
  params.client_id = client_id;
  params.hostname = AZ_SPAN_FROM_STR("localhost");
  params.port = 1883;
  params.keep_alive_seconds = 30;
  params.clean_start = true;
  params.tls_options = NULL;
  params.on_puback = on_puback;
  params.on_pubcomp = on_pubcomp;

  return az_mqtt_e2e_init_client(&s_fixture, client, &params);
}

static void test_publish_while_disconnected(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-publish-disc"));
  assert_int_equal(rc, AZ_OK);

  AZ_MQTT_T(publish_options) pub = AZ_MQTT_T(publish_options_default)();
  pub.topic = AZ_SPAN_FROM_STR("az/e2e/publish/disconnected");
  pub.payload = AZ_SPAN_FROM_STR("hello");

  rc = AZ_MQTT_T(client_publish)(&client, &pub, NULL);
  assert_int_equal(rc, AZ_MQTT_ERROR_NOT_CONNECTED);
}

static void test_publish_qos1_puback(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-publish-qos1"));
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  AZ_MQTT_T(publish_options) pub = AZ_MQTT_T(publish_options_default)();
  pub.topic = AZ_SPAN_FROM_STR("az/e2e/publish/qos1");
  pub.payload = AZ_SPAN_FROM_STR("qos1-msg");
  pub.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;

  uint16_t packet_id = 0;
  rc = AZ_MQTT_T(client_publish)(&client, &pub, &packet_id);
  assert_int_equal(rc, AZ_OK);
  assert_true(packet_id > 0);

  rc = az_mqtt_e2e_wait_until(&client, WAIT_ITERATIONS, PROCESS_LOOP_TIMEOUT_MS, cond_puback_received);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(s_puback_packet_id, packet_id);
  // 0: success; 0x10: no matching subscribers (MQTT 5 only).
  assert_true(s_puback_reason == 0 || s_puback_reason == 0x10);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

static void test_publish_qos2_pubcomp(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-publish-qos2"));
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  AZ_MQTT_T(publish_options) pub = AZ_MQTT_T(publish_options_default)();
  pub.topic = AZ_SPAN_FROM_STR("az/e2e/publish/qos2");
  pub.payload = AZ_SPAN_FROM_STR("qos2-msg");
  pub.qos = AZ_MQTT_QOS_EXACTLY_ONCE;

  uint16_t packet_id = 0;
  rc = AZ_MQTT_T(client_publish)(&client, &pub, &packet_id);
  assert_int_equal(rc, AZ_OK);
  assert_true(packet_id > 0);

  rc = az_mqtt_e2e_wait_until(&client, WAIT_ITERATIONS, PROCESS_LOOP_TIMEOUT_MS, cond_pubcomp_received);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(s_pubcomp_packet_id, packet_id);
  assert_int_equal(s_pubcomp_reason, 0);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_publish_while_disconnected),
    cmocka_unit_test(test_publish_qos1_puback),
    cmocka_unit_test(test_publish_qos2_pubcomp),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
