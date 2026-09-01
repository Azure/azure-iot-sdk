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
#include "support/subscription_ack.h"

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
    /* A registered factory is adopted by the client and freed from destroy();
     * an unregistered one is still ours. */
    bool adopted = (fx->conn.factory_count > 0);
    az_iot_connection_client_destroy(&fx->conn);
    if (!adopted)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
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
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
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
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);

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

/* ------------------------------------------------------------------------- */
/* argument validation and lifecycle                                          */
/* ------------------------------------------------------------------------- */

static void init_rejects_a_null_client(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_direct_method_client_init(NULL, &fx->conn), AZ_IOT_ERR_INVALID_ARG);
}

static void init_rejects_a_null_connection(void** state)
{
  (void)state;
  az_iot_direct_method_client dm;
  assert_int_equal(az_iot_direct_method_client_init(&dm, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void the_subscription_uses_qos_0(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* The filter is asserted elsewhere; the QoS is the part that was never
   * checked. Method invocations are idempotent from the service's point of
   * view and it does not resend them, so QoS 1 would buy a PUBACK round trip
   * for nothing. */
  const az_iot_mock_call* sub
      = az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  if (sub == NULL)
  {
    /* The subscribe happened during connect, before calls were cleared. */
    az_iot_direct_method_client_destroy(&fx->dm);
    assert_int_equal(az_iot_direct_method_client_init(&fx->dm, &fx->conn), AZ_IOT_OK);
    sub = az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  }
  assert_non_null(sub);
  assert_int_equal(sub->qos, AZ_IOT_MQTT_QOS_0);
}

static void destroy_tolerates_null(void** state)
{
  (void)state;
  az_iot_direct_method_client_destroy(NULL);
}

static void destroy_zeroes_the_client(void** state)
{
  fixture* fx = (fixture*)*state;

  /* Uses the fixture's own client rather than a second one: a connection admits
   * exactly one direct-method client, so initialising another against the same
   * connection is refused with ALREADY_INITIALIZED. */
  az_iot_direct_method_client_destroy(&fx->dm);

  /* Zeroed rather than merely flagged: a stale connection pointer left behind
   * is what a later respond() would follow. */
  const unsigned char* raw = (const unsigned char*)&fx->dm;
  for (size_t i = 0; i < sizeof(fx->dm); ++i)
  {
    assert_int_equal(raw[i], 0);
  }

  /* Re-init so teardown has a consistent struct to destroy. */
  assert_int_equal(az_iot_direct_method_client_init(&fx->dm, &fx->conn), AZ_IOT_OK);
}

static void a_second_client_on_the_same_connection_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_direct_method_client second;

  /* The methods topic prefix can only be owned once, so the second client is
   * refused rather than silently stealing the dispatch registration from the
   * first -- which would leave the application holding a client that never
   * fires again. */
  assert_int_equal(
      az_iot_direct_method_client_init(&second, &fx->conn), AZ_IOT_ERR_ALREADY_INITIALIZED);
}

static void destroy_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_direct_method_client_destroy(&fx->dm);
  /* The second call runs against a zeroed struct, so it must not follow the
   * now-NULL connection pointer into unregister. */
  az_iot_direct_method_client_destroy(&fx->dm);

  assert_int_equal(az_iot_direct_method_client_init(&fx->dm, &fx->conn), AZ_IOT_OK);
}

static void an_invocation_after_destroy_reaches_nobody(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  az_iot_direct_method_client_destroy(&fx->dm);

  /* Destroy unregisters the inbound handler, so the dispatch table must no
   * longer own this prefix. If it did, the invocation would land in a callback
   * whose user context has been freed by the application. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/reboot/?$rid=1", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);

  /* Re-init so teardown's destroy has a consistent struct to work on. */
  assert_int_equal(az_iot_direct_method_client_init(&fx->dm, &fx->conn), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* handler registration                                                       */
/* ------------------------------------------------------------------------- */

static void set_handler_rejects_a_null_client(void** state)
{
  (void)state;
  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_direct_method_client_set_handler(NULL, on_method, &rec), AZ_IOT_ERR_INVALID_ARG);
}

static void a_later_set_handler_replaces_the_earlier_one(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record first = { 0 };
  invocation_record second = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &first), AZ_IOT_OK);
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &second), AZ_IOT_OK);

  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/reboot/?$rid=7", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  /* One handler, not a list: the second registration replaces the first
   * rather than adding to it. */
  assert_false(first.fired);
  assert_true(second.fired);
}

static void an_invocation_with_no_handler_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* No set_handler call at all. The invocation must be discarded outright --
   * in particular it must not consume a pool slot, or a device that attaches
   * its handler late would find the pool already exhausted. */
  for (unsigned i = 0; i < AZ_IOT_DM_MAX_INFLIGHT + 2; ++i)
  {
    char topic[64];
    snprintf(topic, sizeof(topic), "$iothub/methods/POST/noop/?$rid=%u", i);
    assert_true(
        az_iot_mock_mqtt_client_inject_message(fx->mock, topic, NULL, 0, AZ_IOT_MQTT_QOS_0));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  }

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/reboot/?$rid=99", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);
}

/* ------------------------------------------------------------------------- */
/* topic parsing                                                              */
/* ------------------------------------------------------------------------- */

/* Inject `topic` and assert the handler was not called. */
static void assert_topic_dropped(fixture* fx, const char* topic)
{
  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_message(fx->mock, topic, NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

static void a_topic_with_an_empty_rid_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  /* "?$rid=" with nothing after it. Responding would publish to a topic the
   * service cannot correlate, so the invocation is not worth delivering. */
  assert_topic_dropped(fx, "$iothub/methods/POST/reboot/?$rid=");
}

static void a_topic_with_an_empty_method_name_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_topic_dropped(fx, "$iothub/methods/POST//?$rid=1");
}

static void a_topic_with_the_wrong_prefix_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_topic_dropped(fx, "$iothub/methods/RES/reboot/?$rid=1");
}

static void a_method_name_past_the_bound_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  char topic[64 + AZ_IOT_DM_METHOD_NAME_MAX + 16];
  char name[AZ_IOT_DM_METHOD_NAME_MAX + 8];
  memset(name, 'm', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  snprintf(topic, sizeof(topic), "$iothub/methods/POST/%s/?$rid=1", name);

  /* The name is copied into a fixed field; dropping is the only safe answer
   * because truncating would invoke the wrong method. */
  assert_topic_dropped(fx, topic);
}

static void a_rid_past_the_bound_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  char topic[64 + AZ_IOT_DM_RID_MAX + 16];
  char rid[AZ_IOT_DM_RID_MAX + 8];
  memset(rid, '9', sizeof(rid) - 1);
  rid[sizeof(rid) - 1] = '\0';
  snprintf(topic, sizeof(topic), "$iothub/methods/POST/reboot/?$rid=%s", rid);

  /* A truncated rid would be answered on a topic the service never asked
   * about, which is worse than not answering. */
  assert_topic_dropped(fx, topic);
}

static void a_non_numeric_rid_is_accepted(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* The service defines $rid as any valid message property value, not an
   * integer. Treating it as a number would drop legitimate invocations, so it
   * is carried through opaquely and echoed back verbatim. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/reboot/?$rid=abc-123", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(c);
  assert_string_equal(c->topic, "$iothub/methods/res/200/?$rid=abc-123");
}

static void an_invocation_with_an_empty_body_is_delivered(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* The service sends either valid JSON or an empty body; an empty body is a
   * method taking no arguments, not a malformed message. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/ping/?$rid=5", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_string_equal(rec.method_name, "ping");
  assert_int_equal(rec.payload_len, 0);
}

/* ------------------------------------------------------------------------- */
/* respond                                                                    */
/* ------------------------------------------------------------------------- */

static void respond_with_an_empty_payload_publishes_an_empty_body(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/ping/?$rid=8", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_direct_method_respond(rec.request, 204, NULL, 0), AZ_IOT_OK);

  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(c);
  assert_string_equal(c->topic, "$iothub/methods/res/204/?$rid=8");
  assert_int_equal(c->payload_len, 0);
}

static void respond_after_the_handler_returned_still_publishes(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/slow/?$rid=11", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);

  /* The handler has already returned, and several do_work pumps have run. The
   * request must still be answerable: a method that takes longer than one pump
   * is the ordinary case, not an edge case, and the whole point of handing the
   * application a request handle is that it outlives the callback. */
  for (int i = 0; i < 5; ++i)
  {
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  }

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  static const uint8_t body[] = "{\"done\":true}";
  assert_int_equal(
      az_iot_direct_method_respond(rec.request, 200, body, sizeof(body) - 1), AZ_IOT_OK);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(c);
  assert_string_equal(c->topic, "$iothub/methods/res/200/?$rid=11");
}

static void respond_while_disconnected_reports_not_connected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/POST/reboot/?$rid=3", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);

  /* The session drops before the application answers. The answer cannot be
   * delivered, and saying so is better than reporting a success the service
   * will never see. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_int_equal(
      az_iot_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_ERR_NOT_CONNECTED);
}

/* ------------------------------------------------------------------------- */
/* Hub-Next (AEG, MQTT v5) flavor                                            */
/* ------------------------------------------------------------------------- */

/* Fixture variant: a direct HUB_NEXT connection. The direct-method client then
 * resolves the Next profile at init(), which uses a different topic space
 * (ih/<device>/dev/methods/<name>) and carries the request id in MQTT v5
 * Correlation Data rather than in an $rid query parameter. */
static int setup_next(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(fx->factory);

  assert_int_equal(az_iot_direct_method_client_init(&fx->dm, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

/* Most recent recorded call of `kind` whose topic equals `topic`, or NULL. */
static const az_iot_mock_call* find_call(
    az_iot_mock_mqtt_client* m,
    az_iot_mock_call_kind kind,
    const char* topic)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = n; i > 0; --i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i - 1);
    if (c->kind == kind && strcmp(c->topic, topic) == 0)
    {
      return c;
    }
  }
  return NULL;
}

/* Register the factory, open, and deliver CONNACK. On Hub-Next this leaves the
 * session CONNECTING with only the presence filter subscribed. */
static void open_next(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);

  az_iot_mqtt_event connack;
  memset(&connack, 0, sizeof(connack));
  connack.kind = AZ_IOT_MQTT_EVT_CONNECTED;
  connack.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &connack));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

/* Ack every filter the session issued, then echo the birth nonce back as a
 * birth-ack so the session reaches CONNECTED. */
static void finish_birth_next(fixture* fx)
{
  /* Collect the packet ids first: injecting appends to the same history the
   * iteration walks, so acking in-place would read a moving array. */
  uint16_t ids[16];
  size_t id_count = 0;
  size_t n = az_iot_mock_mqtt_client_call_count(fx->mock);
  for (size_t i = 0; i < n && id_count < (sizeof(ids) / sizeof(ids[0])); ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(fx->mock, i);
    if (c->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE)
    {
      ids[id_count++] = c->packet_id;
    }
  }
  assert_true(id_count > 0);

  for (size_t i = 0; i < id_count; ++i)
  {
    az_iot_mqtt_event suback;
    memset(&suback, 0, sizeof(suback));
    suback.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
    suback.status = AZ_IOT_OK;
    suback.packet_id = ids[i];
    assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &suback));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  }

  const az_iot_mock_call* birth
      = find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, "ih/ut-device/srv/presence");
  assert_non_null(birth);
  assert_int_equal(birth->correlation_data_len, 16);

  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/ut-device/dev/presence";
  ack_msg.correlation_data = nonce;
  ack_msg.correlation_data_len = sizeof(nonce);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &ack));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

/* Drive a Hub-Next session all the way to CONNECTED. Unlike the Classic path,
 * CONNACK alone does not announce CONNECTED. */
static void open_to_connected_next(fixture* fx)
{
  open_next(fx);
  finish_birth_next(fx);
  /* gen2 feature delivery uses the presence wildcard; there are no later
   * per-feature SUBACKs to wait for. */
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
}

/* Deliver one Next invocation, optionally carrying Correlation Data. */
static void inject_next_invocation(
    fixture* fx,
    const char* topic,
    const uint8_t* corr,
    size_t corr_len,
    const uint8_t* payload,
    size_t payload_len)
{
  az_iot_mqtt_message msg;
  memset(&msg, 0, sizeof(msg));
  msg.topic = topic;
  msg.payload = payload;
  msg.payload_len = payload_len;
  msg.correlation_data = corr;
  msg.correlation_data_len = corr_len;
  msg.qos = AZ_IOT_MQTT_QOS_1;
  az_iot_mqtt_event evt;
  memset(&evt, 0, sizeof(evt));
  evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  evt.message = &msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &evt));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

static void next_init_does_not_subscribe_a_redundant_methods_filter(void** state)
{
  fixture* fx = (fixture*)*state;
  open_next(fx);

  /* CONNACK alone buys only the presence filter. */
  assert_non_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE, "ih/ut-device/dev/#"));
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE, "ih/ut-device/dev/methods/+"));

  finish_birth_next(fx);

  /* And still nothing afterwards: ih/ut-device/dev/# already covers
   * ih/ut-device/dev/methods/+, so a second filter would be redundant.
   * Invocations still arrive -- see next_invocation_is_dispatched_to_the_handler. */
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE, "ih/ut-device/dev/methods/+"));
}

static void next_invocation_is_dispatched_to_the_handler(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  static const uint8_t payload[] = "{\"x\":1}";
  static const uint8_t corr[] = { 0xDE, 0xAD, 0xBE, 0xEF };
  inject_next_invocation(
      fx, "ih/ut-device/dev/methods/reboot", corr, sizeof(corr), payload, sizeof(payload) - 1);

  assert_true(rec.fired);
  assert_string_equal(rec.method_name, "reboot");
  assert_int_equal(rec.payload_len, sizeof(payload) - 1);
  assert_string_equal(rec.payload, "{\"x\":1}");
  assert_non_null(rec.request);
}

static void next_respond_publishes_to_the_service_topic_with_a_status_property(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  static const uint8_t corr[] = { 0x01, 0x02, 0x03, 0x04 };
  inject_next_invocation(fx, "ih/ut-device/dev/methods/reboot", corr, sizeof(corr), NULL, 0);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  static const uint8_t resp[] = "{\"ok\":true}";
  assert_int_equal(
      az_iot_direct_method_respond(rec.request, 200, resp, sizeof(resp) - 1), AZ_IOT_OK);

  /* Next carries the status as a user property, not in the topic, and the
   * response goes to the service-bound half of the topic space. */
  const az_iot_mock_call* c
      = find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, "ih/ut-device/srv/methods/reboot/response");
  assert_non_null(c);
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_1);
  assert_int_equal(c->payload_len, sizeof(resp) - 1);
  assert_memory_equal(c->payload, resp, sizeof(resp) - 1);
}

static void next_respond_echoes_the_correlation_data_back(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  static const uint8_t corr[] = { 0xAA, 0xBB, 0xCC };
  inject_next_invocation(fx, "ih/ut-device/dev/methods/ping", corr, sizeof(corr), NULL, 0);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);

  /* The service correlates the response to the invocation solely by this
   * value, so dropping it would strand the caller until it timed out. */
  const az_iot_mock_call* c
      = find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, "ih/ut-device/srv/methods/ping/response");
  assert_non_null(c);
  assert_int_equal(c->correlation_data_len, sizeof(corr));
  assert_memory_equal(c->correlation_data, corr, sizeof(corr));
}

static void next_correlation_data_past_the_maximum_is_truncated(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Oversized correlation data is clamped to the request's fixed buffer rather
   * than overrunning it; the invocation is still delivered. */
  uint8_t corr[AZ_IOT_DM_CORR_DATA_MAX + 8];
  for (size_t i = 0; i < sizeof(corr); ++i)
  {
    corr[i] = (uint8_t)(i & 0xFF);
  }
  inject_next_invocation(fx, "ih/ut-device/dev/methods/big", corr, sizeof(corr), NULL, 0);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);
  const az_iot_mock_call* c
      = find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, "ih/ut-device/srv/methods/big/response");
  assert_non_null(c);
  assert_int_equal(c->correlation_data_len, AZ_IOT_DM_CORR_DATA_MAX);
  assert_memory_equal(c->correlation_data, corr, AZ_IOT_DM_CORR_DATA_MAX);
}

static void next_invocation_without_correlation_data_is_still_delivered(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* A service that omits Correlation Data is still answerable: the response
   * simply carries none back. */
  inject_next_invocation(fx, "ih/ut-device/dev/methods/bare", NULL, 0, NULL, 0);
  assert_true(rec.fired);
  assert_string_equal(rec.method_name, "bare");

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);
  const az_iot_mock_call* c
      = find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, "ih/ut-device/srv/methods/bare/response");
  assert_non_null(c);
  assert_int_equal(c->correlation_data_len, 0);
}

static void next_topic_with_a_foreign_prefix_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Not the hub topic space at all. */
  inject_next_invocation(fx, "xx/ut-device/dev/methods/reboot", NULL, 0, NULL, 0);
  assert_false(rec.fired);
}

static void next_topic_without_the_methods_segment_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Right prefix, wrong feature: a twin topic must not reach the method
   * handler even though both arrive on the same device-scoped subscription. */
  inject_next_invocation(fx, "ih/ut-device/dev/twin/patch", NULL, 0, NULL, 0);
  assert_false(rec.fired);
}

static void next_topic_with_an_empty_method_name_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Nothing after the segment, and trailing slashes do not manufacture a name:
   * responding would build a topic with an empty method segment. */
  inject_next_invocation(fx, "ih/ut-device/dev/methods/", NULL, 0, NULL, 0);
  assert_false(rec.fired);
  inject_next_invocation(fx, "ih/ut-device/dev/methods///", NULL, 0, NULL, 0);
  assert_false(rec.fired);
}

static void next_method_name_past_the_bound_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* One past what the request's name buffer holds: dropped rather than
   * truncated, since a truncated name would be answered on the wrong topic. */
  char topic[64 + AZ_IOT_DM_METHOD_NAME_MAX + 8];
  int n = snprintf(topic, sizeof(topic), "%s", "ih/ut-device/dev/methods/");
  assert_true(n > 0);
  for (size_t i = 0; i < (size_t)AZ_IOT_DM_METHOD_NAME_MAX; ++i)
  {
    topic[(size_t)n + i] = 'x';
  }
  topic[(size_t)n + AZ_IOT_DM_METHOD_NAME_MAX] = '\0';
  inject_next_invocation(fx, topic, NULL, 0, NULL, 0);
  assert_false(rec.fired);
}

static void next_invocation_with_no_handler_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  /* No handler was ever set. The request must not be taken out of the pool,
   * or a later handler would start life with fewer slots than documented. */
  inject_next_invocation(fx, "ih/ut-device/dev/methods/reboot", NULL, 0, NULL, 0);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  for (int i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    char topic[64];
    snprintf(topic, sizeof(topic), "ih/ut-device/dev/methods/m%d", i);
    rec.fired = false;
    inject_next_invocation(fx, topic, NULL, 0, NULL, 0);
    assert_true(rec.fired);
  }
}

static void next_pool_exhaustion_drops_the_extra_invocation(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  invocation_record rec = { 0 };
  assert_int_equal(az_iot_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Hold every slot by never responding. */
  for (int i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    char topic[64];
    snprintf(topic, sizeof(topic), "ih/ut-device/dev/methods/m%d", i);
    rec.fired = false;
    inject_next_invocation(fx, topic, NULL, 0, NULL, 0);
    assert_true(rec.fired);
  }

  /* The overflow invocation is dropped rather than evicting a live request,
   * which would hand the application a request it could no longer answer. */
  rec.fired = false;
  inject_next_invocation(fx, "ih/ut-device/dev/methods/overflow", NULL, 0, NULL, 0);
  assert_false(rec.fired);
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
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test(init_rejects_a_null_connection),
    cmocka_unit_test_setup_teardown(the_subscription_uses_qos_0, setup, teardown),
    cmocka_unit_test(destroy_tolerates_null),
    cmocka_unit_test_setup_teardown(destroy_zeroes_the_client, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_second_client_on_the_same_connection_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(an_invocation_after_destroy_reaches_nobody, setup, teardown),
    cmocka_unit_test(set_handler_rejects_a_null_client),
    cmocka_unit_test_setup_teardown(a_later_set_handler_replaces_the_earlier_one, setup, teardown),
    cmocka_unit_test_setup_teardown(an_invocation_with_no_handler_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_topic_with_an_empty_rid_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_topic_with_an_empty_method_name_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_topic_with_the_wrong_prefix_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_method_name_past_the_bound_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_rid_past_the_bound_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_non_numeric_rid_is_accepted, setup, teardown),
    cmocka_unit_test_setup_teardown(an_invocation_with_an_empty_body_is_delivered, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_with_an_empty_payload_publishes_an_empty_body, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_after_the_handler_returned_still_publishes, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_while_disconnected_reports_not_connected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        next_init_does_not_subscribe_a_redundant_methods_filter, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_invocation_is_dispatched_to_the_handler, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_respond_publishes_to_the_service_topic_with_a_status_property, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_respond_echoes_the_correlation_data_back, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_correlation_data_past_the_maximum_is_truncated, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_invocation_without_correlation_data_is_still_delivered, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_topic_with_a_foreign_prefix_is_dropped, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_topic_without_the_methods_segment_is_dropped, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_topic_with_an_empty_method_name_is_dropped, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_method_name_past_the_bound_is_dropped, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_invocation_with_no_handler_is_dropped, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_pool_exhaustion_drops_the_extra_invocation, setup_next, teardown),
  };
  return cmocka_run_group_tests_name("direct_method_client", tests, NULL, NULL);
}
