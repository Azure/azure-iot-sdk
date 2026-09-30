// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_subscribe.c
 * @brief Integration tests for subscribe flows in AZ_MQTT_T(client).
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

static bool s_publish_received;
static az_mqtt_qos s_publish_qos;
static char s_publish_topic[128];
static char s_publish_payload[128];

static bool s_suback_received;
static uint16_t s_suback_packet_id;
static int s_suback_reason;

static bool s_unsuback_received;
static uint16_t s_unsuback_packet_id;
static int s_unsuback_reason;

static void reset_callback_state(void)
{
  s_publish_received = false;
  s_publish_qos = AZ_MQTT_QOS_AT_MOST_ONCE;
  s_publish_topic[0] = '\0';
  s_publish_payload[0] = '\0';

  s_suback_received = false;
  s_suback_packet_id = 0;
  s_suback_reason = -1;

  s_unsuback_received = false;
  s_unsuback_packet_id = 0;
  s_unsuback_reason = -1;
}

static bool cond_suback_received(void)
{
  return s_suback_received;
}

static bool cond_unsuback_received(void)
{
  return s_unsuback_received;
}

static bool cond_publish_received(void)
{
  return s_publish_received;
}

static void on_publish(AZ_MQTT_T(client)* client, AZ_MQTT_T(publish_data) const* publish)
{
  (void)client;
  s_publish_received = true;
  s_publish_qos = publish->qos;
  az_mqtt_e2e_copy_span_to_cstr(publish->topic, s_publish_topic, sizeof(s_publish_topic));
  az_mqtt_e2e_copy_span_to_cstr(publish->payload, s_publish_payload, sizeof(s_publish_payload));
}

static void on_suback(AZ_MQTT_T(client)* client, AZ_MQTT_T(suback_data) const* suback)
{
  (void)client;
  s_suback_received = true;
  s_suback_packet_id = suback->packet_id;
#if AZ_MQTT_TEST_VERSION == 5
  s_suback_reason = (suback->reason_code_count > 0) ? (int)suback->reason_codes[0] : -1;
#else
  s_suback_reason
      = az_span_size(suback->return_codes) > 0 ? (int)az_span_ptr(suback->return_codes)[0] : -1;
#endif
}

#if AZ_MQTT_TEST_VERSION == 5
static void on_unsuback(AZ_MQTT_T(client)* client, AZ_MQTT_T(suback_data) const* unsuback)
{
  (void)client;
  s_unsuback_received = true;
  s_unsuback_packet_id = unsuback->packet_id;
  s_unsuback_reason = (unsuback->reason_code_count > 0) ? (int)unsuback->reason_codes[0] : -1;
}
#else
static void on_unsuback(AZ_MQTT_T(client)* client, AZ_MQTT_T(ack_data) const* unsuback)
{
  (void)client;
  s_unsuback_received = true;
  s_unsuback_packet_id = unsuback->packet_id;
}
#endif

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
  params.on_publish = on_publish;
  params.on_suback = on_suback;
  params.on_unsuback = on_unsuback;

  return az_mqtt_e2e_init_client(&s_fixture, client, &params);
}

static void test_subscribe_while_disconnected(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-sub-disc"));
  assert_int_equal(rc, AZ_OK);

  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("az/e2e/subscribe/disconnected");
  sub.qos = AZ_MQTT_QOS_AT_MOST_ONCE;

  rc = AZ_MQTT_T(client_subscribe)(&client, &sub, 1, NULL);
  assert_int_equal(rc, AZ_MQTT_ERROR_NOT_CONNECTED);
}

static void test_unsubscribe_while_disconnected(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-unsub-disc"));
  assert_int_equal(rc, AZ_OK);

  az_span topics[1];
  topics[0] = AZ_SPAN_FROM_STR("az/e2e/unsubscribe/disconnected");

  rc = AZ_MQTT_T(client_unsubscribe)(&client, topics, 1, NULL);
  assert_int_equal(rc, AZ_MQTT_ERROR_NOT_CONNECTED);
}

static void test_subscribe_and_unsubscribe_callbacks(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-sub-unsub-cb"));
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("az/e2e/sub/callback");
  sub.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;

  uint16_t sub_packet_id = 0;
  rc = AZ_MQTT_T(client_subscribe)(&client, &sub, 1, &sub_packet_id);
  assert_int_equal(rc, AZ_OK);
  assert_true(sub_packet_id > 0);

  rc = az_mqtt_e2e_wait_until(&client, WAIT_ITERATIONS, PROCESS_LOOP_TIMEOUT_MS, cond_suback_received);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(s_suback_packet_id, sub_packet_id);
  assert_true(s_suback_reason == 1 || s_suback_reason == 0); // Granted QoS 1 or 0.

  az_span topic_filters[1];
  topic_filters[0] = sub.topic_filter;

  uint16_t unsub_packet_id = 0;
  rc = AZ_MQTT_T(client_unsubscribe)(&client, topic_filters, 1, &unsub_packet_id);
  assert_int_equal(rc, AZ_OK);
  assert_true(unsub_packet_id > 0);

  rc = az_mqtt_e2e_wait_until(&client, WAIT_ITERATIONS, PROCESS_LOOP_TIMEOUT_MS, cond_unsuback_received);
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(s_unsuback_packet_id, unsub_packet_id);
#if AZ_MQTT_TEST_VERSION == 5
  assert_true(
      s_unsuback_reason == AZ_MQTT5_REASON_SUCCESS || s_unsuback_reason == AZ_MQTT5_REASON_NO_SUBSCRIPTION_EXISTED);
#endif

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

static void test_subscribe_then_publish_receive(void** state)
{
  (void)state;
  reset_callback_state();

  AZ_MQTT_T(client) client;
  az_result rc = init_client(&client, AZ_SPAN_FROM_STR("test-sub-pub-recv"));
  assert_int_equal(rc, AZ_OK);

  rc = AZ_MQTT_T(client_connect)(&client, 5000);
  assert_int_equal(rc, AZ_OK);

  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("az/e2e/sub/publish-receive");
  sub.qos = AZ_MQTT_QOS_AT_MOST_ONCE;

  uint16_t sub_packet_id = 0;
  rc = AZ_MQTT_T(client_subscribe)(&client, &sub, 1, &sub_packet_id);
  assert_int_equal(rc, AZ_OK);
  assert_true(sub_packet_id > 0);

  rc = az_mqtt_e2e_wait_until(&client, WAIT_ITERATIONS, PROCESS_LOOP_TIMEOUT_MS, cond_suback_received);
  assert_int_equal(rc, AZ_OK);

  AZ_MQTT_T(publish_options) pub = AZ_MQTT_T(publish_options_default)();
  pub.topic = sub.topic_filter;
  pub.payload = AZ_SPAN_FROM_STR("hello-from-subscribe-test");
  pub.qos = AZ_MQTT_QOS_AT_MOST_ONCE;

  rc = AZ_MQTT_T(client_publish)(&client, &pub, NULL);
  assert_int_equal(rc, AZ_OK);

  rc = az_mqtt_e2e_wait_until(&client, WAIT_ITERATIONS, PROCESS_LOOP_TIMEOUT_MS, cond_publish_received);
  assert_int_equal(rc, AZ_OK);
  assert_true(az_span_is_content_equal(sub.topic_filter, az_span_create_from_str(s_publish_topic)));
  assert_string_equal(s_publish_payload, "hello-from-subscribe-test");
  assert_int_equal((int)s_publish_qos, (int)AZ_MQTT_QOS_AT_MOST_ONCE);

  rc = AZ_MQTT_TEST_DISCONNECT(&client);
  assert_int_equal(rc, AZ_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_subscribe_while_disconnected),
    cmocka_unit_test(test_unsubscribe_while_disconnected),
    cmocka_unit_test(test_subscribe_and_unsubscribe_callbacks),
    cmocka_unit_test(test_subscribe_then_publish_receive),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
