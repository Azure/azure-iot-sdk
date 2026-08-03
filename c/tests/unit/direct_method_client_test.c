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
#include "azure/iot/az_iot_log.h"
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

/* ---- in-flight request pool ---------------------------------------------- */

/* Deliver one invocation named "m<n>" with rid <n>. */
static void inject_invocation(fixture* fx, int n)
{
  char topic[64];
  snprintf(topic, sizeof(topic), "$iothub/methods/POST/m%d/?$rid=%d", n, n);
  assert_true(az_iot_mock_mqtt_client_inject_message(fx->mock, topic, NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

static void the_pool_holds_the_documented_number_of_concurrent_requests(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Requests may legitimately outlive the handler, so holding this many at
   * once has to work. */
  for (int i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    rec.fired = false;
    inject_invocation(fx, i);
    assert_true(rec.fired);
  }
}

static void an_invocation_past_the_pool_capacity_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  for (int i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    inject_invocation(fx, i);
  }

  /* Nothing has been answered, so every slot is still taken and the extra
   * invocation cannot be delivered. That it is not dropped in silence is
   * asserted separately, by a_dropped_invocation_says_why. */
  rec.fired = false;
  inject_invocation(fx, AZ_IOT_DM_MAX_INFLIGHT);
  assert_false(rec.fired);
}

static void responding_frees_the_slot_for_the_next_invocation(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  for (int i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    inject_invocation(fx, i);
  }
  assert_non_null(rec.request);
  assert_int_equal(az_iot_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);

  rec.fired = false;
  inject_invocation(fx, 99);
  assert_true(rec.fired);
}

static void responding_twice_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  inject_invocation(fx, 7);
  assert_true(rec.fired);
  assert_non_null(rec.request);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);

  /* The slot is back in the pool and may already belong to another
   * invocation, so a second answer would carry that invocation's rid and
   * reply to the wrong call. */
  assert_int_equal(az_iot_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
}

static void respond_carries_a_non_success_status(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  inject_invocation(fx, 5);
  assert_non_null(rec.request);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_direct_method_respond(rec.request, 501, NULL, 0), AZ_IOT_OK);

  const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(c->topic, "$iothub/methods/res/501/?$rid=5");
  assert_int_equal(c->payload_len, 0);
}

static void respond_rejects_a_null_payload_with_a_length(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  inject_invocation(fx, 3);
  assert_non_null(rec.request);

  assert_int_equal(az_iot_direct_method_respond(rec.request, 200, NULL, 4), AZ_IOT_ERR_INVALID_ARG);
  /* Rejecting the arguments must not consume the request: the application can
   * still answer it properly. */
  assert_int_equal(az_iot_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);
}

/* ---- diagnostics ---------------------------------------------------------- */

typedef struct log_capture
{
  int warn_count;
  char last_warning[AZ_IOT_LOG_MESSAGE_MAX];
} log_capture;

static void capture_warnings(
    void* user_ctx,
    az_iot_log_level level,
    const char* file,
    int line,
    const char* msg)
{
  log_capture* c = (log_capture*)user_ctx;
  (void)file;
  (void)line;
  if (level != AZ_IOT_LOG_LEVEL_WARN || msg == NULL)
  {
    return;
  }
  c->warn_count++;
  snprintf(c->last_warning, sizeof(c->last_warning), "%s", msg);
}

static void install_warning_capture(log_capture* c)
{
  memset(c, 0, sizeof(*c));
  az_iot_log_sink sink;
  sink.sink = capture_warnings;
  sink.user_ctx = c;
  sink.min_level = AZ_IOT_LOG_LEVEL_WARN;
  az_iot_log_set_global_sink(&sink);
}

static void a_dropped_invocation_says_why(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  for (int i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    inject_invocation(fx, i);
  }

  /* Dropping in silence looked exactly like the service having stopped
   * delivering, with nothing to point at the application's own missing
   * respond() call. Assert the text, not just that something was logged. */
  log_capture cap;
  install_warning_capture(&cap);
  inject_invocation(fx, AZ_IOT_DM_MAX_INFLIGHT);
  az_iot_log_set_global_sink(NULL);

  assert_int_equal(cap.warn_count, 1);
  assert_non_null(strstr(cap.last_warning, "in-flight"));
  assert_non_null(strstr(cap.last_warning, "respond"));
}

static void an_unparsable_topic_says_why(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  log_capture cap;
  install_warning_capture(&cap);
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/foo/", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  az_iot_log_set_global_sink(NULL);

  assert_false(rec.fired);
  assert_int_equal(cap.warn_count, 1);
  assert_non_null(strstr(cap.last_warning, "$iothub/methods/POST/foo/"));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(create_subscribes_methods_topic_on_connect, setup, teardown),
    cmocka_unit_test_setup_teardown(inbound_invocation_dispatched_to_handler, setup, teardown),
    cmocka_unit_test_setup_teardown(malformed_topic_dropped, setup, teardown),
    cmocka_unit_test(respond_rejects_null_request),
    cmocka_unit_test_setup_teardown(
        the_pool_holds_the_documented_number_of_concurrent_requests, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_invocation_past_the_pool_capacity_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        responding_frees_the_slot_for_the_next_invocation, setup, teardown),
    cmocka_unit_test_setup_teardown(responding_twice_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(respond_carries_a_non_success_status, setup, teardown),
    cmocka_unit_test_setup_teardown(respond_rejects_a_null_payload_with_a_length, setup, teardown),
    cmocka_unit_test_setup_teardown(a_dropped_invocation_says_why, setup, teardown),
    cmocka_unit_test_setup_teardown(an_unparsable_topic_says_why, setup, teardown),
  };
  return cmocka_run_group_tests_name("direct_method_client", tests, NULL, NULL);
}
