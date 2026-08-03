// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Phase 3.2 - DirectMethodClient unit tests, driven through the public API
 * and the in-memory mock_mqtt_iface. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_direct_method_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "support/mock_mqtt_iface.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct invocation_record
{
  bool fired;
  char method_name[64];
  char payload[64];
  size_t payload_len;
  az_iot_direct_method_request* request; /* owned by user; we'll respond to it */
} invocation_record;

static void on_method(
    az_iot_direct_method_request* request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  invocation_record* r = (invocation_record*)user_ctx;
  r->fired = true;
  snprintf(r->method_name, sizeof(r->method_name), "%s", method_name);
  if (payload && payload_len > 0 && payload_len < sizeof(r->payload))
  {
    memcpy(r->payload, payload, payload_len);
    r->payload[payload_len] = '\0';
    r->payload_len = payload_len;
  }
  r->request = request;
}

typedef struct fixture
{
  az_iot_connection_client conn;
  az_iot_direct_method_client dm;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock;
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

  assert_int_equal(az_iot_direct_method_client_init(&fx->dm, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_direct_method_client_destroy(&fx->dm);
    az_iot_connection_client_destroy(&fx->conn);
    free(fx);
  }
  return 0;
}

static void open_to_connected(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void create_subscribes_methods_topic_on_connect(void** state)
{
  fixture* fx = (fixture*)*state;
  /* The DirectMethodClient was already created in setup(); now connect and
   * verify a SUBSCRIBE for the methods filter is issued automatically. */
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  /* Walk the call history looking for a SUBSCRIBE on the methods filter. */
  bool found = false;
  size_t n = az_iot_mock_mqtt_client_call_count(fx->mock);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(fx->mock, i);
    if (c->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE && strcmp(c->topic, "$iothub/methods/POST/#") == 0)
    {
      found = true;
      break;
    }
  }
  assert_true(found);
}

static void inbound_invocation_dispatched_to_handler(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Inject a method invocation. */
  static const uint8_t payload[] = "{\"x\":1}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock,
      "$iothub/methods/POST/reboot/?$rid=42",
      payload,
      sizeof(payload) - 1,
      AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_string_equal(rec.method_name, "reboot");
  assert_int_equal(rec.payload_len, sizeof(payload) - 1);
  assert_string_equal(rec.payload, "{\"x\":1}");
  assert_non_null(rec.request);

  /* Respond. The publish must land on the response topic with the same rid. */
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  static const uint8_t resp[] = "{\"ok\":true}";
  assert_int_equal(
      az_iot_direct_method_respond(rec.request, 200, resp, sizeof(resp) - 1), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(c->topic, "$iothub/methods/res/200/?$rid=42");
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_0);
  assert_int_equal(c->payload_len, sizeof(resp) - 1);
  assert_memory_equal(c->payload, resp, sizeof(resp) - 1);
}

static void malformed_topic_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Missing rid query string. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/foo/", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

static void respond_rejects_null_request(void** state)
{
  (void)state;
  assert_int_equal(az_iot_direct_method_respond(NULL, 200, NULL, 0), AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(create_subscribes_methods_topic_on_connect, setup, teardown),
    cmocka_unit_test_setup_teardown(inbound_invocation_dispatched_to_handler, setup, teardown),
    cmocka_unit_test_setup_teardown(malformed_topic_dropped, setup, teardown),
    cmocka_unit_test(respond_rejects_null_request),
  };
  return cmocka_run_group_tests_name("direct_method_client", tests, NULL, NULL);
}
