// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Phase 3.1 - TelemetryClient unit tests, driven through the public API and
 * the in-memory mock_mqtt_iface. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_telemetry_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "support/mock_mqtt_iface.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct send_record
{
  bool fired;
  az_iot_result status;
} send_record;

static void on_send(az_iot_result status, void* user_ctx)
{
  send_record* r = (send_record*)user_ctx;
  r->fired = true;
  r->status = status;
}

typedef struct fixture
{
  az_iot_connection_client conn;
  az_iot_telemetry_client tc;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock; /* convenience alias */
} fixture;

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  assert_int_equal(az_iot_telemetry_client_init(&fx->tc, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_telemetry_client_destroy(&fx->tc);
    az_iot_connection_client_destroy(&fx->conn);
    free(fx);
  }
  return 0;
}

/* Drive the connection through CONNECTING -> CONNECTED on the mock. */
static void open_to_connected(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  /* Drop the CONNECT call from the history so PUBLISH lands at index 0. */
  az_iot_mock_mqtt_client_clear_calls(fx->mock);

  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  /* process_loop is called from inside do_work() to deliver the injected
   * event; that adds a PROCESS_LOOP call to the mock history. Clear again so
   * the test can index PUBLISH at 0. */
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void init_rejects_nulls(void** state)
{
  (void)state;
  az_iot_telemetry_client tc;
  az_iot_connection_client* dummy = (az_iot_connection_client*)(uintptr_t)1;
  assert_int_equal(az_iot_telemetry_client_init(NULL, dummy), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_telemetry_client_init(&tc, NULL), AZ_IOT_ERR_INVALID_ARG);
  /* deinit(NULL) is a no-op. */
  az_iot_telemetry_client_destroy(NULL);
}

static void send_before_connect_returns_not_connected(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  /* Not opened yet - publish must refuse. */
  static const uint8_t payload[] = "hello";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  assert_int_equal(
      az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_ERR_NOT_CONNECTED);
}

static void send_publishes_qos1(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  static const uint8_t payload[] = "hello";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;

  send_record r = { 0 };
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &r), AZ_IOT_OK);

  /* Always QoS 1 -> publish() called once with the expected D2C topic. */
  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(c0->topic, "devices/ut-device/messages/events/");
  assert_int_equal(c0->payload_len, 5);
  assert_memory_equal(c0->payload, "hello", 5);
  assert_int_equal(c0->qos, AZ_IOT_MQTT_QOS_1);

  /* QoS 1 -> ack callback NOT yet fired (deferred until PUBACK). */
  assert_false(r.fired);
}

static void send_qos1_defers_cb_until_puback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  static const uint8_t payload[] = "{\"t\":1}";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;

  send_record r = { 0 };
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &r), AZ_IOT_OK);

  /* publish() was called and a packet_id was assigned. */
  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_int_equal(c0->qos, AZ_IOT_MQTT_QOS_1);
  uint16_t pid = c0->packet_id;
  assert_true(pid != 0);

  /* QoS 1 -> ack callback NOT yet fired. */
  assert_false(r.fired);

  /* Inject the matching PUBLISH_ACK. */
  az_iot_mqtt_event evt = { 0 };
  evt.kind = AZ_IOT_MQTT_EVT_PUBLISH_ACK;
  evt.packet_id = pid;
  evt.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &evt));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(r.fired);
  assert_int_equal(r.status, AZ_IOT_OK);
}

static void send_propagates_content_type_and_properties(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_telemetry_property props[] = { { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
                                        { AZ_IOT_MSG_PROP_CONTENT_ENCODING, "utf-8" },
                                        { "k1", "v1" },
                                        { "k2", "v2" } };
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 1;
  msg.properties = props;
  msg.properties_count = 4;

  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  /* IoT Hub Classic topic with system + application properties appended.
   * Both halves of each pair are percent-encoded, so the system keys arrive
   * as %24.ct / %24.ce -- the same bytes azure-sdk-for-c emits -- and the
   * '/' in the content type as %2F. */
  assert_string_equal(
      c0->topic,
      "devices/ut-device/messages/events/%24.ct=application%2Fjson&%24.ce=utf-8&k1=v1&k2=v2");
}

static void send_url_encodes_property_values(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Every character that carries meaning in a property bag has to survive as
   * content rather than structure, and the RFC 3986 unreserved set has to
   * pass through untouched. Keys go through the same encoder as values. */
  az_iot_telemetry_property props[]
      = { { "note", "a&b=c d%e" }, { "keep", "-_.~AZaz09" }, { "$.mid", "id-1" } };
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 1;
  msg.properties = props;
  msg.properties_count = 3;

  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(
      c0->topic,
      "devices/ut-device/messages/events/"
      "note=a%26b%3Dc%20d%25e&keep=-_.~AZaz09&%24.mid=id-1");
}

static void system_property_keys_match_the_azure_sdk_wire_form(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* azure-sdk-for-c spells these constants pre-encoded
   * (AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE is "%24.ct"). Our constants are
   * the readable form and the encoder produces the same bytes, so a device
   * built on either SDK publishes an identical topic. */
  az_iot_telemetry_property props[]
      = { { AZ_IOT_MSG_PROP_MESSAGE_ID, "m1" }, { AZ_IOT_MSG_PROP_CORRELATION_ID, "c1" } };
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 1;
  msg.properties = props;
  msg.properties_count = 2;

  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(c0->topic, "devices/ut-device/messages/events/%24.mid=m1&%24.cid=c1");
}

static void send_reports_not_enough_space_when_the_bag_overflows(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Each 'x' encodes to itself, so this exceeds the topic buffer on length
   * alone. The build has to report the overflow rather than truncate. */
  char big[1024];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';

  az_iot_telemetry_property props[] = { { "k", big } };
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 1;
  msg.properties = props;
  msg.properties_count = 1;

  assert_int_equal(
      az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  /* Nothing was published. */
  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 0);
}

static void send_rejects_invalid_args(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_telemetry_message msg = { 0 };
  msg.payload_len = 4; /* but payload is NULL */
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_telemetry_client_send(NULL, &msg, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, NULL, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(init_rejects_nulls),
    cmocka_unit_test_setup_teardown(send_before_connect_returns_not_connected, setup, teardown),
    cmocka_unit_test_setup_teardown(send_publishes_qos1, setup, teardown),
    cmocka_unit_test_setup_teardown(send_qos1_defers_cb_until_puback, setup, teardown),
    cmocka_unit_test_setup_teardown(send_propagates_content_type_and_properties, setup, teardown),
    cmocka_unit_test_setup_teardown(send_url_encodes_property_values, setup, teardown),
    cmocka_unit_test_setup_teardown(
        system_property_keys_match_the_azure_sdk_wire_form, setup, teardown),
    cmocka_unit_test_setup_teardown(
        send_reports_not_enough_space_when_the_bag_overflows, setup, teardown),
    cmocka_unit_test_setup_teardown(send_rejects_invalid_args, setup, teardown),
  };
  return cmocka_run_group_tests_name("telemetry_client", tests, NULL, NULL);
}
