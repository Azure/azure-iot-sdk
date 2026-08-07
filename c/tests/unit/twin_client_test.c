// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Phase 3.3 - TwinClient unit tests, driven through the public API and the
 * in-memory mock_mqtt_iface. */
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
#include "azure/iot/az_iot_twin_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "internal/twin_client_internal.h"

#include "support/mock_mqtt_iface.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct get_record
{
  bool fired;
  az_iot_result status;
  char payload[64];
  size_t payload_len;
} get_record;

static void on_get(az_iot_result status, const uint8_t* payload, size_t payload_len, void* user_ctx)
{
  get_record* r = (get_record*)user_ctx;
  r->fired = true;
  r->status = status;
  if (payload && payload_len > 0 && payload_len < sizeof(r->payload))
  {
    memcpy(r->payload, payload, payload_len);
    r->payload[payload_len] = '\0';
    r->payload_len = payload_len;
  }
}

typedef struct patch_record
{
  bool fired;
  az_iot_result status;
  uint64_t version;
} patch_record;

static void on_patch(az_iot_result status, uint64_t version, void* user_ctx)
{
  patch_record* r = (patch_record*)user_ctx;
  r->fired = true;
  r->status = status;
  r->version = version;
}

typedef struct desired_record
{
  bool fired;
  char payload[64];
  size_t payload_len;
  uint64_t version;
} desired_record;

static void on_desired(const uint8_t* payload, size_t payload_len, uint64_t version, void* user_ctx)
{
  desired_record* r = (desired_record*)user_ctx;
  r->fired = true;
  r->version = version;
  if (payload && payload_len > 0 && payload_len < sizeof(r->payload))
  {
    memcpy(r->payload, payload, payload_len);
    r->payload[payload_len] = '\0';
    r->payload_len = payload_len;
  }
}

typedef struct fixture
{
  az_iot_connection_client conn;
  az_iot_twin_client twin;
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

  assert_int_equal(az_iot_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_twin_client_destroy(&fx->twin);
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

/* Start a second session on a client that is already back in IDLE. Deliberately
 * does NOT re-register the factory: registration appends, and destroy() calls
 * every registered entry's destroy hook, so registering the same factory twice
 * frees it twice. */
static void reopen_to_connected(fixture* fx)
{
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

/* Walk the mock call history; return the topic of the first PUBLISH found
 * (or NULL). */
static const char* first_publish_topic(az_iot_mock_mqtt_client* m)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i);
    if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH)
    {
      return c->topic;
    }
  }
  return NULL;
}

static bool history_has_subscribe(az_iot_mock_mqtt_client* m, const char* expected)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i);
    if (c->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE && strcmp(c->topic, expected) == 0)
    {
      return true;
    }
  }
  return false;
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void create_subscribes_response_and_desired(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(history_has_subscribe(fx->mock, "$iothub/twin/res/#"));
  assert_true(history_has_subscribe(fx->mock, "$iothub/twin/PATCH/properties/desired/#"));
}

static void get_publishes_and_response_fires_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  const char* topic = first_publish_topic(fx->mock);
  assert_non_null(topic);
  /* First rid is 1. */
  assert_string_equal(topic, "$iothub/twin/GET/?$rid=1");
  assert_false(rec.fired);

  /* Inject the response. */
  static const uint8_t body[] = "{\"desired\":{},\"reported\":{}}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_int_equal(rec.payload_len, sizeof(body) - 1);
  assert_string_equal(rec.payload, "{\"desired\":{},\"reported\":{}}");
}

static void patch_publishes_and_204_response_fires_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"reported\":{\"x\":1}}";
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  const char* topic = first_publish_topic(fx->mock);
  assert_non_null(topic);
  assert_string_equal(topic, "$iothub/twin/PATCH/properties/reported/?$rid=1");

  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/204/?$rid=1&$version=42", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  /* The service returns the new version of the reported-properties section
   * alongside the acknowledgement. An application that tracks it can tell an
   * applied update from a lost one. Compared at full width: a twin version is
   * a uint64_t and narrowing it here would hide a truncation bug. */
  assert_true(rec.version == UINT64_C(42));
}

static void a_patch_ack_without_a_version_reports_zero(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/204/?$rid=1", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_true(rec.version == UINT64_C(0));
}

static void a_failed_patch_reports_version_zero(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  /* A version only means something when the update was applied. Reporting one
   * from a rejected patch would let a caller record a version for something
   * the service never stored. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/429/?$rid=1&$version=77", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_BUSY);
  assert_true(rec.version == UINT64_C(0));
}

static void desired_message_dispatched_to_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  static const uint8_t body[] = "{\"x\":2}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock,
      "$iothub/twin/PATCH/properties/desired/?$version=99",
      body,
      sizeof(body) - 1,
      AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.payload_len, sizeof(body) - 1);
  assert_string_equal(rec.payload, "{\"x\":2}");
  assert_true(rec.version == UINT64_C(99));
}

static void unknown_rid_drops_response(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* No request issued; response with rid=99 should not crash. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=99", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  /* No assertion needed: surviving the call is the test. */
}

/* ---- service status mapping ---------------------------------------------- */

/* Drive one GET to completion against the given service status and hand back
 * the result the caller was given. */
static az_iot_result get_result_for_status(fixture* fx, const char* status_topic)
{
  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  assert_true(
      az_iot_mock_mqtt_client_inject_message(fx->mock, status_topic, NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);
  return rec.status;
}

static void throttled_status_is_reported_as_busy(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* 429 must be distinguishable from a locally full pending table, which is
   * what NOT_SUPPORTED means on this API. One says "back off and retry", the
   * other says "you have too many requests in flight"; a caller that cannot
   * tell them apart cannot do either correctly. */
  assert_int_equal(get_result_for_status(fx, "$iothub/twin/res/429/?$rid=1"), (int)AZ_IOT_ERR_BUSY);
}

static void bad_request_status_is_reported_as_invalid_arg(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* 400 is the service rejecting the reported-properties JSON this device
   * sent. Resending it unchanged cannot succeed, so it must not look like a
   * transport error the caller should retry. */
  assert_int_equal(
      get_result_for_status(fx, "$iothub/twin/res/400/?$rid=1"), (int)AZ_IOT_ERR_INVALID_ARG);
}

static void not_found_status_is_reported_as_not_found(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  assert_int_equal(
      get_result_for_status(fx, "$iothub/twin/res/404/?$rid=1"), (int)AZ_IOT_ERR_NOT_FOUND);
}

static void server_error_status_is_reported_as_an_mqtt_error(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  assert_int_equal(get_result_for_status(fx, "$iothub/twin/res/500/?$rid=1"), (int)AZ_IOT_ERR_MQTT);
}

static void success_statuses_are_reported_as_ok(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  assert_int_equal(get_result_for_status(fx, "$iothub/twin/res/200/?$rid=1"), (int)AZ_IOT_OK);
  assert_int_equal(get_result_for_status(fx, "$iothub/twin/res/204/?$rid=2"), (int)AZ_IOT_OK);
}

/* ---- pending requests across a dropped session --------------------------- */

static void a_pending_get_is_failed_when_the_session_drops(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  assert_false(rec.fired);

  /* The response would have travelled on the session that just died, so it can
   * never arrive. Leaving the caller waiting is indistinguishable from a hang. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_NOT_CONNECTED);
}

static void a_pending_patch_is_failed_when_the_session_drops(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);
  assert_false(rec.fired);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_NOT_CONNECTED);
}

static void every_pending_request_is_failed_not_just_the_first(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record gets[AZ_IOT_TWIN_MAX_PENDING];
  memset(gets, 0, sizeof(gets));
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &gets[i]), AZ_IOT_OK);
  }

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_true(gets[i].fired);
    assert_int_equal(gets[i].status, AZ_IOT_ERR_NOT_CONNECTED);
  }
}

static void the_pending_pool_is_reusable_after_a_dropped_session(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Fill the pool, drop the session, and fill it again. Before the slots were
   * released on teardown this second round returned NOT_SUPPORTED, so a device
   * on a flaky link lost the ability to talk to its twin at all after
   * AZ_IOT_TWIN_MAX_PENDING outages. */
  get_record first[AZ_IOT_TWIN_MAX_PENDING];
  memset(first, 0, sizeof(first));
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &first[i]), AZ_IOT_OK);
  }
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &first[0]), AZ_IOT_ERR_NOT_SUPPORTED);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  /* Reconnect and prove the whole pool came back. */
  reopen_to_connected(fx);
  get_record second[AZ_IOT_TWIN_MAX_PENDING];
  memset(second, 0, sizeof(second));
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &second[i]), AZ_IOT_OK);
  }
}

static void a_destroyed_twin_client_is_not_called_on_a_later_session_end(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* destroy() must unhook the handler. If it did not, the connection would
   * call into a zeroed client -- and, worse, into whatever the application had
   * already freed behind the user context. */
  az_iot_twin_client_destroy(&fx->twin);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_false(rec.fired);

  /* Re-init so the shared teardown() has a valid client to destroy. */
  assert_int_equal(az_iot_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void destroying_the_connection_does_not_complete_pending_requests(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* Same rule the QoS-1 acknowledgements follow: on destroy() the application
   * is tearing everything down and the context the callback closes over may
   * already be gone, so calling into it would turn cleanup into a
   * use-after-free. */
  az_iot_twin_client_destroy(&fx->twin);
  az_iot_connection_client_destroy(&fx->conn);
  assert_false(rec.fired);

  /* Rebuild what teardown() expects to tear down. */
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* desired-property subscriber registry                                      */
/* ------------------------------------------------------------------------- */

static int g_desired_seq;

typedef struct order_record
{
  int order; /* dispatch order, captured from g_desired_seq */
} order_record;

static void on_desired_feature(const uint8_t* p, size_t n, uint64_t v, void* ctx)
{
  (void)p;
  (void)n;
  (void)v;
  ((order_record*)ctx)->order = ++g_desired_seq;
}
static void on_desired_app_a(const uint8_t* p, size_t n, uint64_t v, void* ctx)
{
  (void)p;
  (void)n;
  (void)v;
  ((order_record*)ctx)->order = ++g_desired_seq;
}
static void on_desired_app_b(const uint8_t* p, size_t n, uint64_t v, void* ctx)
{
  (void)p;
  (void)n;
  (void)v;
  ((order_record*)ctx)->order = ++g_desired_seq;
}

static void inject_desired(fixture* fx, const char* body)
{
  char topic[] = "$iothub/twin/PATCH/properties/desired/?$version=7";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, topic, (const uint8_t*)body, strlen(body), AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

static void feature_subscribers_notified_before_app(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  g_desired_seq = 0;
  order_record feat = { 0 }, app_a = { 0 }, app_b = { 0 };

  /* Register application subs first, feature sub last, to prove ordering is
   * by pool (feature-before-app) and NOT by registration time. */
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &app_a), AZ_IOT_OK);
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_b, &app_b), AZ_IOT_OK);
  assert_int_equal(
      az_iot_twin_client__subscribe_desired(&fx->twin, on_desired_feature, &feat), AZ_IOT_OK);

  inject_desired(fx, "{\"a\":1}");

  assert_int_equal(feat.order, 1); /* feature pool first */
  assert_true(app_a.order == 2 && app_b.order == 3); /* app pool in reg order */
}

static void app_pool_full_returns_not_supported(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  int a = 0, b = 0, c = 0;
  /* Default app pool capacity is 2. */
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &a), AZ_IOT_OK);
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &b), AZ_IOT_OK);
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &c),
      AZ_IOT_ERR_NOT_SUPPORTED);
}

static void resubscribe_same_pair_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* Idempotent: only one slot consumed, so one more distinct sub still fits. */
  int other = 0;
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &other), AZ_IOT_OK);
}

static void unsubscribe_stops_delivery(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);
  assert_int_equal(az_iot_twin_client_unsubscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);
  /* Removing an absent entry reports invalid arg. */
  assert_int_equal(
      az_iot_twin_client_unsubscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_ERR_INVALID_ARG);

  inject_desired(fx, "{\"a\":1}");
  assert_false(rec.fired);
}

typedef struct busy_record
{
  az_iot_twin_client* twin;
  az_iot_result sub_result;
  az_iot_result unsub_result;
} busy_record;

static void on_desired_reentrant(const uint8_t* p, size_t n, uint64_t v, void* ctx)
{
  (void)p;
  (void)n;
  (void)v;
  busy_record* r = (busy_record*)ctx;
  /* Mutating the registry mid-dispatch MUST be rejected. */
  r->sub_result = az_iot_twin_client_subscribe_desired(r->twin, on_desired_app_a, NULL);
  r->unsub_result = az_iot_twin_client_unsubscribe_desired(r->twin, on_desired_reentrant, ctx);
}

static void mutating_registry_during_dispatch_is_busy(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  busy_record rec = { .twin = &fx->twin, .sub_result = AZ_IOT_OK, .unsub_result = AZ_IOT_OK };
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_reentrant, &rec), AZ_IOT_OK);

  inject_desired(fx, "{\"a\":1}");
  assert_int_equal(rec.sub_result, AZ_IOT_ERR_BUSY);
  assert_int_equal(rec.unsub_result, AZ_IOT_ERR_BUSY);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(create_subscribes_response_and_desired, setup, teardown),
    cmocka_unit_test_setup_teardown(get_publishes_and_response_fires_callback, setup, teardown),
    cmocka_unit_test_setup_teardown(
        patch_publishes_and_204_response_fires_callback, setup, teardown),
    cmocka_unit_test_setup_teardown(a_patch_ack_without_a_version_reports_zero, setup, teardown),
    cmocka_unit_test_setup_teardown(a_failed_patch_reports_version_zero, setup, teardown),
    cmocka_unit_test_setup_teardown(desired_message_dispatched_to_callback, setup, teardown),
    cmocka_unit_test_setup_teardown(feature_subscribers_notified_before_app, setup, teardown),
    cmocka_unit_test_setup_teardown(app_pool_full_returns_not_supported, setup, teardown),
    cmocka_unit_test_setup_teardown(resubscribe_same_pair_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(unsubscribe_stops_delivery, setup, teardown),
    cmocka_unit_test_setup_teardown(mutating_registry_during_dispatch_is_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(unknown_rid_drops_response, setup, teardown),
    cmocka_unit_test_setup_teardown(success_statuses_are_reported_as_ok, setup, teardown),
    cmocka_unit_test_setup_teardown(throttled_status_is_reported_as_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(bad_request_status_is_reported_as_invalid_arg, setup, teardown),
    cmocka_unit_test_setup_teardown(not_found_status_is_reported_as_not_found, setup, teardown),
    cmocka_unit_test_setup_teardown(
        server_error_status_is_reported_as_an_mqtt_error, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_pending_get_is_failed_when_the_session_drops, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_pending_patch_is_failed_when_the_session_drops, setup, teardown),
    cmocka_unit_test_setup_teardown(
        every_pending_request_is_failed_not_just_the_first, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_pending_pool_is_reusable_after_a_dropped_session, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_destroyed_twin_client_is_not_called_on_a_later_session_end, setup, teardown),
    cmocka_unit_test_setup_teardown(
        destroying_the_connection_does_not_complete_pending_requests, setup, teardown),
  };
  return cmocka_run_group_tests_name("twin_client", tests, NULL, NULL);
}
