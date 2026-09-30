// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_both_versions.c
 * @brief az_mqttv3 and az_mqttv5 linked into one program: an MQTT 3.1.1
 * client publishes, an MQTT 5.0 client receives.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>

#include <cmocka.h>

#include <az_mqtt3/az_mqtt3_client.h>
#include <az_mqtt5/az_mqtt5_client.h>

#include <stdint.h>
#include <string.h>

#define TRANSPORT_BUF_SIZE (256 * 1024)

/** @brief Transport storage, aligned for the transport's members. */
typedef union
{
  uint8_t bytes[TRANSPORT_BUF_SIZE];
  void* align_pointer;
  int64_t align_int64;
  double align_double;
} transport_storage;

static transport_storage s_transport3;
static transport_storage s_transport5;
static uint8_t s_send3[1024];
static uint8_t s_recv3[1024];
static uint8_t s_send5[1024];
static uint8_t s_recv5[1024];
static az_mqtt5_reason_code s_suback_codes[4];

static int s_subacks;
static int s_received;
static char s_payload[32];

static void on_suback(az_mqtt5_client* client, az_mqtt5_suback_data const* suback)
{
  (void)client;
  (void)suback;
  s_subacks++;
}

static void on_publish(az_mqtt5_client* client, az_mqtt5_publish_data const* publish)
{
  (void)client;
  s_received++;
  int32_t n = az_span_size(publish->payload);
  n = n < (int32_t)sizeof(s_payload) - 1 ? n : (int32_t)sizeof(s_payload) - 1;
  memcpy(s_payload, az_span_ptr(publish->payload), (size_t)n);
  s_payload[n] = '\0';
}

static void test_mqtt3_publishes_mqtt5_receives(void** state)
{
  (void)state;
  assert_true(az_mqtt_transport_sizeof() <= TRANSPORT_BUF_SIZE);
  az_mqtt_transport* t3 = (az_mqtt_transport*)s_transport3.bytes;
  az_mqtt_transport* t5 = (az_mqtt_transport*)s_transport5.bytes;
  assert_int_equal(az_mqtt_transport_init(t3), AZ_OK);
  assert_int_equal(az_mqtt_transport_init(t5), AZ_OK);

  az_mqtt5_client_options o5;
  memset(&o5, 0, sizeof(o5));
  o5.transport = t5;
  o5.send_buffer = AZ_SPAN_FROM_BUFFER(s_send5);
  o5.receive_buffer = AZ_SPAN_FROM_BUFFER(s_recv5);
  o5.hostname = AZ_SPAN_FROM_STR("localhost");
  o5.port = 1883;
  o5.connect_options = az_mqtt5_connect_options_default();
  o5.connect_options.client_id = AZ_SPAN_FROM_STR("test-both-v5");
  o5.on_suback = on_suback;
  o5.on_publish = on_publish;
  o5.buffers.suback_reason_codes
      = az_span_create((uint8_t*)s_suback_codes, (int32_t)sizeof(s_suback_codes));
  az_mqtt5_client c5;
  assert_int_equal(az_mqtt5_client_init(&c5, &o5), AZ_OK);

  az_mqtt3_client_options o3;
  memset(&o3, 0, sizeof(o3));
  o3.transport = t3;
  o3.send_buffer = AZ_SPAN_FROM_BUFFER(s_send3);
  o3.receive_buffer = AZ_SPAN_FROM_BUFFER(s_recv3);
  o3.hostname = AZ_SPAN_FROM_STR("localhost");
  o3.port = 1883;
  o3.connect_options = az_mqtt3_connect_options_default();
  o3.connect_options.client_id = AZ_SPAN_FROM_STR("test-both-v3");
  az_mqtt3_client c3;
  assert_int_equal(az_mqtt3_client_init(&c3, &o3), AZ_OK);

  assert_int_equal(az_mqtt5_client_connect(&c5, 5000), AZ_OK);
  assert_int_equal(az_mqtt3_client_connect(&c3, 5000), AZ_OK);

  az_mqtt5_subscription sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("az/e2e/both");
  sub.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  assert_int_equal(az_mqtt5_client_subscribe(&c5, &sub, 1, NULL), AZ_OK);
  for (int i = 0; i < 50 && s_subacks == 0; i++)
  {
    assert_int_equal(az_mqtt5_client_process_loop(&c5, 100), AZ_OK);
  }
  assert_int_equal(s_subacks, 1);

  az_mqtt3_publish_options pub = az_mqtt3_publish_options_default();
  pub.topic = AZ_SPAN_FROM_STR("az/e2e/both");
  pub.payload = AZ_SPAN_FROM_STR("from-mqttv3");
  pub.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  assert_int_equal(az_mqtt3_client_publish(&c3, &pub, NULL), AZ_OK);

  for (int i = 0; i < 50 && s_received == 0; i++)
  {
    assert_int_equal(az_mqtt3_client_process_loop(&c3, 10), AZ_OK);
    assert_int_equal(az_mqtt5_client_process_loop(&c5, 100), AZ_OK);
  }
  assert_int_equal(s_received, 1);
  assert_string_equal(s_payload, "from-mqttv3");

  assert_int_equal(az_mqtt3_client_disconnect(&c3), AZ_OK);
  assert_int_equal(az_mqtt5_client_disconnect(&c5, AZ_MQTT5_REASON_NORMAL_DISCONNECTION), AZ_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_mqtt3_publishes_mqtt5_receives),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
