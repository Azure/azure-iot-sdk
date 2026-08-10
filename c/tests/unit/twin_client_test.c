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
  /* Set once the factory has been handed to the connection, which adopts it.
   * Read in teardown rather than the connection's own counter, because a test
   * may already have destroyed the connection. */
  bool factory_registered;
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
    /* A registered factory is adopted by the connection and freed from its
     * destroy(); an unregistered one is still ours. */
    if (!fx->factory_registered)
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
  fx->factory_registered = true;
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
  fx->factory_registered = true;
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

/* First recorded PUBLISH, or NULL. Callers that need the whole call (payload,
 * qos, correlation data) rather than just the topic use this. */
static const az_iot_mock_call* first_publish(az_iot_mock_mqtt_client* m)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i);
    if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH)
    {
      return c;
    }
  }
  return NULL;
}

static const az_iot_mock_call* find_subscribe(az_iot_mock_mqtt_client* m, const char* expected)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i);
    if (c->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE && strcmp(c->topic, expected) == 0)
    {
      return c;
    }
  }
  return NULL;
}

/* Same as open_to_connected(), but keeps the connect burst in the history so a
 * test can assert on the subscriptions it issued. */
static void open_to_connected_keeping_history(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  fx->factory_registered = true;
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* argument rejection and lifecycle                                          */
/* ------------------------------------------------------------------------- */

static void init_rejects_a_null_client(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_twin_client_init(NULL, &fx->conn), AZ_IOT_ERR_INVALID_ARG);
}

static void init_rejects_a_null_connection(void** state)
{
  (void)state;
  az_iot_twin_client local;
  assert_int_equal(az_iot_twin_client_init(&local, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void both_subscriptions_use_qos_0(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_keeping_history(fx);

  /* Classic twin traffic is qos 0 in both directions. Subscribing at qos 1
   * would make the hub retain and redeliver, which the client is not built to
   * de-duplicate. */
  const az_iot_mock_call* res = find_subscribe(fx->mock, "$iothub/twin/res/#");
  assert_non_null(res);
  assert_int_equal(res->qos, AZ_IOT_MQTT_QOS_0);
  const az_iot_mock_call* des = find_subscribe(fx->mock, "$iothub/twin/PATCH/properties/desired/#");
  assert_non_null(des);
  assert_int_equal(des->qos, AZ_IOT_MQTT_QOS_0);
}

static void destroy_tolerates_null(void** state)
{
  (void)state;
  az_iot_twin_client_destroy(NULL);
}

static void destroy_zeroes_the_client(void** state)
{
  fixture* fx = (fixture*)*state;

  /* Operates on the fixture's own client: a connection admits one twin client,
   * so a second init against the same connection is refused. */
  az_iot_twin_client_destroy(&fx->twin);

  /* Zeroed rather than merely flagged: a stale connection pointer left behind
   * is what a later get() would publish through. */
  const unsigned char* raw = (const unsigned char*)&fx->twin;
  for (size_t i = 0; i < sizeof(fx->twin); ++i)
  {
    assert_int_equal(raw[i], 0);
  }

  assert_int_equal(az_iot_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void destroy_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_twin_client_destroy(&fx->twin);
  /* The second call runs against a zeroed struct and must not follow the
   * now-NULL connection pointer into unregister. */
  az_iot_twin_client_destroy(&fx->twin);

  assert_int_equal(az_iot_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void destroy_unregisters_both_handlers(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record des = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &des), AZ_IOT_OK);

  az_iot_twin_client_destroy(&fx->twin);

  /* Both the response and the desired handler must be gone: either one left
   * behind would dispatch into a zeroed client. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  static const uint8_t body[] = "{\"x\":1}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock,
      "$iothub/twin/PATCH/properties/desired/?$version=5",
      body,
      sizeof(body) - 1,
      AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(des.fired);

  assert_int_equal(az_iot_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* get                                                                       */
/* ------------------------------------------------------------------------- */

static void get_rejects_a_null_client(void** state)
{
  (void)state;
  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(NULL, on_get, &rec), AZ_IOT_ERR_INVALID_ARG);
}

static void get_publishes_an_empty_body(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* The service specifies an empty message for GET. Sending a body has been
   * observed to make the hub ignore the request outright. */
  const az_iot_mock_call* c = first_publish(fx->mock);
  assert_non_null(c);
  assert_int_equal(c->payload_len, 0);
}

static void get_with_a_full_pending_table_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record recs[AZ_IOT_TWIN_MAX_PENDING];
  memset(recs, 0, sizeof(recs));
  for (size_t i = 0; i < (size_t)AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &recs[i]), AZ_IOT_OK);
  }

  /* Refused up front rather than published and then never correlated, which
   * would leave the caller waiting for a callback that could not arrive. */
  get_record overflow = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &overflow), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_false(overflow.fired);
}

static void a_publish_failure_releases_the_pending_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* A refused PUBLISH must hand the slot back. Leaking one per failure would
   * retire the pending table after AZ_IOT_TWIN_MAX_PENDING transient errors. */
  az_iot_mock_mqtt_client_set_next_result(
      fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_NOT_CONNECTED);
  get_record rec = { 0 };
  assert_int_not_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  for (size_t i = 0; i < (size_t)AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    get_record again = { 0 };
    assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &again), AZ_IOT_OK);
  }
}

/* ------------------------------------------------------------------------- */
/* patch reported                                                            */
/* ------------------------------------------------------------------------- */

static void patch_rejects_a_null_client(void** state)
{
  (void)state;
  static const uint8_t patch[] = "{\"x\":1}";
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_twin_client_patch_reported(NULL, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_ERR_INVALID_ARG);
}

static void patch_rejects_a_null_patch_with_a_length(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Rejecting the arguments must not consume a pending slot. */
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, NULL, 8, on_patch, &rec),
      AZ_IOT_ERR_INVALID_ARG);

  for (size_t i = 0; i < (size_t)AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    get_record again = { 0 };
    assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &again), AZ_IOT_OK);
  }
}

static void an_empty_patch_is_publishable(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* A zero-length patch is a legitimate no-op the service accepts; refusing it
   * would force callers to special-case an empty diff. */
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, NULL, 0, on_patch, &rec), AZ_IOT_OK);
  const az_iot_mock_call* c = first_publish(fx->mock);
  assert_non_null(c);
  assert_int_equal(c->payload_len, 0);
}

static void the_patch_body_is_forwarded_byte_for_byte(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Embedded NUL included: the payload is a length-counted blob, not a C
   * string, and truncating at the NUL would silently drop reported properties. */
  static const uint8_t patch[] = { '{', '"', 'a', '"', ':', '1', 0x00, '}' };
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch), on_patch, &rec),
      AZ_IOT_OK);

  const az_iot_mock_call* c = first_publish(fx->mock);
  assert_non_null(c);
  assert_int_equal(c->payload_len, sizeof(patch));
  assert_memory_equal(c->payload, patch, sizeof(patch));
}

/* ------------------------------------------------------------------------- */
/* response correlation                                                      */
/* ------------------------------------------------------------------------- */

static void a_get_response_does_not_satisfy_a_patch_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  /* A 200 with a body is a get answer. Handing it to the patch callback would
   * report an acknowledgement the service never sent. The slot is released
   * either way, so the caller is not left hanging. */
  static const uint8_t body[] = "{\"desired\":{}}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

static void concurrent_get_and_patch_correlate_independently(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record grec = { 0 };
  patch_record prec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &grec), AZ_IOT_OK);
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &prec),
      AZ_IOT_OK);

  /* Answered out of order: correlation is by rid, not by arrival order. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/204/?$rid=2&$version=7", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(prec.fired);
  assert_false(grec.fired);

  static const uint8_t body[] = "{\"desired\":{}}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(grec.fired);
  assert_int_equal(grec.status, AZ_IOT_OK);
  assert_true(prec.version == UINT64_C(7));
}

static void a_second_response_for_the_same_rid_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  static const uint8_t body[] = "{\"a\":1}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);

  /* The slot was released by the first answer and may already belong to
   * another request, so a duplicate must not fire the callback again. */
  rec.fired = false;
  static const uint8_t body2[] = "{\"a\":2}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", body2, sizeof(body2) - 1, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

static void a_response_topic_with_a_non_numeric_status_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/abc/?$rid=1", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

static void a_response_topic_with_no_query_string_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

static void a_response_topic_with_no_rid_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* A query string that carries everything but the correlator. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$version=3", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

static void a_response_topic_with_the_wrong_prefix_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/methods/res/200/?$rid=1", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

static void the_rid_counter_wraps_without_reusing_zero(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Rid 0 is the "no pending request" marker in the table, so handing it out
   * would make an unrelated response match a free slot. Driven by seeding the
   * counter rather than issuing four billion requests. */
  fx->twin._internal.next_rid = UINT32_MAX;

  get_record a = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &a), AZ_IOT_OK);
  const az_iot_mock_call* first = first_publish(fx->mock);
  assert_non_null(first);
  assert_string_equal(first->topic, "$iothub/twin/GET/?$rid=4294967295");

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  get_record b = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &b), AZ_IOT_OK);
  const az_iot_mock_call* second = first_publish(fx->mock);
  assert_non_null(second);
  assert_string_not_equal(second->topic, "$iothub/twin/GET/?$rid=0");
}

/* ------------------------------------------------------------------------- */
/* desired properties                                                        */
/* ------------------------------------------------------------------------- */

static void a_desired_topic_with_no_version_yields_zero(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* The patch still has to reach the application: a missing version is not a
   * reason to withhold the properties themselves. */
  static const uint8_t body[] = "{\"x\":3}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock,
      "$iothub/twin/PATCH/properties/desired/",
      body,
      sizeof(body) - 1,
      AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_true(rec.version == UINT64_C(0));
  assert_string_equal(rec.payload, "{\"x\":3}");
}

static void a_desired_version_past_32_bits_is_preserved(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* The callback takes a uint64_t; parsing through a 32-bit intermediate would
   * wrap this to 1 and make a fresh patch look older than one already seen. */
  static const uint8_t body[] = "{\"x\":4}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock,
      "$iothub/twin/PATCH/properties/desired/?$version=4294967297",
      body,
      sizeof(body) - 1,
      AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_true(rec.version == UINT64_C(4294967297));
}

static void every_subscriber_in_a_pool_is_notified(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Dispatch must not stop at the first occupied slot. */
  desired_record a = { 0 };
  desired_record b = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &a), AZ_IOT_OK);
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &b), AZ_IOT_OK);

  static const uint8_t body[] = "{\"x\":5}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock,
      "$iothub/twin/PATCH/properties/desired/?$version=6",
      body,
      sizeof(body) - 1,
      AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(a.fired);
  assert_true(b.fired);
}

static void subscribe_desired_rejects_a_null_client(void** state)
{
  (void)state;
  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(NULL, on_desired, &rec), AZ_IOT_ERR_INVALID_ARG);
}

static void subscribe_desired_rejects_a_null_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, NULL, &rec), AZ_IOT_ERR_INVALID_ARG);
}

static void the_same_callback_with_a_different_context_takes_a_second_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* The (callback, context) pair is the identity. Treating the function
   * pointer alone as the key would silently drop the second subscriber -- two
   * objects of the same type sharing one handler is the normal case. */
  desired_record a = { 0 };
  desired_record b = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &a), AZ_IOT_OK);
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &b), AZ_IOT_OK);

  /* The pool holds exactly AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS, so a third pair
   * proves the first two occupy distinct slots. */
  desired_record c = { 0 };
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &c), AZ_IOT_ERR_NOT_SUPPORTED);
}

static void unsubscribe_frees_the_slot_for_reuse(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record a = { 0 };
  desired_record b = { 0 };
  desired_record c = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &a), AZ_IOT_OK);
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &b), AZ_IOT_OK);
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &c), AZ_IOT_ERR_NOT_SUPPORTED);

  /* An unsubscribed slot is reusable; leaking it would retire the pool one
   * subscriber at a time across a long-running application. */
  assert_int_equal(az_iot_twin_client_unsubscribe_desired(&fx->twin, on_desired, &a), AZ_IOT_OK);
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &c), AZ_IOT_OK);
}

static void unsubscribing_an_unregistered_pair_leaves_the_others_alone(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record a = { 0 };
  desired_record other = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &a), AZ_IOT_OK);

  /* Removing something that was never added is reported as a bad argument and
   * must be a no-op, not a wildcard clear of the pool. */
  assert_int_equal(
      az_iot_twin_client_unsubscribe_desired(&fx->twin, on_desired, &other),
      AZ_IOT_ERR_INVALID_ARG);

  static const uint8_t body[] = "{\"x\":7}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock,
      "$iothub/twin/PATCH/properties/desired/?$version=8",
      body,
      sizeof(body) - 1,
      AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(a.fired);
}

/* Subscribes from inside the dispatch, which must be refused, and records the
 * result so the test can assert the guard was actually exercised. */
typedef struct reentrant_record
{
  az_iot_twin_client* twin;
  bool fired;
  az_iot_result subscribe_result;
} reentrant_record;

static void on_desired_subscribing_reentrantly(
    const uint8_t* payload,
    size_t payload_len,
    uint64_t version,
    void* user_ctx)
{
  (void)payload;
  (void)payload_len;
  (void)version;
  reentrant_record* r = (reentrant_record*)user_ctx;
  r->fired = true;
  desired_record scratch = { 0 };
  r->subscribe_result = az_iot_twin_client_subscribe_desired(r->twin, on_desired, &scratch);
}

static void the_dispatch_guard_is_cleared_after_a_dispatch(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  reentrant_record rec = { 0 };
  rec.twin = &fx->twin;
  assert_int_equal(
      az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_subscribing_reentrantly, &rec),
      AZ_IOT_OK);

  static const uint8_t body[] = "{\"x\":9}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock,
      "$iothub/twin/PATCH/properties/desired/?$version=10",
      body,
      sizeof(body) - 1,
      AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);
  assert_int_equal(rec.subscribe_result, AZ_IOT_ERR_BUSY);

  /* A guard left set would make the registry permanently read-only after the
   * first desired patch. */
  desired_record late = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &late), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* Hub-Next (AEG, MQTT v5) flavor                                            */
/* ------------------------------------------------------------------------- */

/* Fixture variant: a direct HUB_NEXT connection. The twin client then resolves
 * the Next profile at init(), which uses a split topic space
 * (srv/... outbound, dev/... inbound) and correlates by MQTT v5 Correlation
 * Data carrying the decimal rid instead of a `$rid` query parameter. */
static int setup_next(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.hub_protocol = AZ_IOT_HUB_PROTOCOL_NEXT;
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(fx->factory);

  assert_int_equal(az_iot_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static const az_iot_mock_call* find_publish(az_iot_mock_mqtt_client* m, const char* expected)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = n; i > 0; --i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i - 1);
    if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH && strcmp(c->topic, expected) == 0)
    {
      return c;
    }
  }
  return NULL;
}

/* Drive a Hub-Next session to CONNECTED. Unlike Classic, CONNACK alone does not
 * announce CONNECTED: the presence birth handshake has to complete first, and
 * the feature filters are only subscribed once it does. */
static void open_to_connected_next(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  fx->factory_registered = true;
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);

  az_iot_mqtt_event connack;
  memset(&connack, 0, sizeof(connack));
  connack.kind = AZ_IOT_MQTT_EVT_CONNECTED;
  connack.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &connack));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  /* Collect the packet ids first: injecting appends to the same history the
   * iteration walks, so acking in place would read a moving array. */
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

  const az_iot_mock_call* birth = find_publish(fx->mock, "ih/ut-device/srv/presence");
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

/* Deliver an inbound Next message carrying Correlation Data. */
static void inject_next_message(
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

static void next_init_subscribes_the_three_twin_filters(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  /* Next splits what Classic does with two wildcards into three exact,
   * device-scoped topics. */
  assert_non_null(find_subscribe(fx->mock, "ih/ut-device/dev/twin/get/response"));
  assert_non_null(find_subscribe(fx->mock, "ih/ut-device/dev/twin/reported/response"));
  assert_non_null(find_subscribe(fx->mock, "ih/ut-device/dev/twin/desired"));
}

static void next_get_publishes_to_the_service_topic_with_correlation_data(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* qos 1 on Next: the request is a command the service must not silently
   * lose, unlike the fire-and-forget Classic GET. */
  const az_iot_mock_call* c = find_publish(fx->mock, "ih/ut-device/srv/twin/get");
  assert_non_null(c);
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_1);
  assert_int_equal(c->payload_len, 0);
  /* The rid rides in Correlation Data as decimal text, not in the topic. */
  assert_int_equal(c->correlation_data_len, 1);
  assert_int_equal(c->correlation_data[0], '1');
  assert_false(rec.fired);
}

static void next_get_response_fires_the_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  static const uint8_t corr[] = "1";
  static const uint8_t body[] = "{\"desired\":{},\"reported\":{}}";
  inject_next_message(
      fx, "ih/ut-device/dev/twin/get/response", corr, sizeof(corr) - 1, body, sizeof(body) - 1);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_string_equal(rec.payload, "{\"desired\":{},\"reported\":{}}");
}

static void next_get_response_with_an_unknown_rid_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* Stale correlation data from an earlier session must not complete a live
   * request with someone else's twin document. */
  static const uint8_t corr[] = "99";
  static const uint8_t body[] = "{\"x\":1}";
  inject_next_message(
      fx, "ih/ut-device/dev/twin/get/response", corr, sizeof(corr) - 1, body, sizeof(body) - 1);
  assert_false(rec.fired);
}

static void next_get_response_without_correlation_data_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* With no correlator the rid reads as 0, which is the free-slot marker; the
   * response must not be allowed to match anything. */
  static const uint8_t body[] = "{\"x\":1}";
  inject_next_message(fx, "ih/ut-device/dev/twin/get/response", NULL, 0, body, sizeof(body) - 1);
  assert_false(rec.fired);
}

static void next_patch_publishes_to_the_service_topic(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  const az_iot_mock_call* c = find_publish(fx->mock, "ih/ut-device/srv/twin/reported");
  assert_non_null(c);
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_1);
  assert_int_equal(c->payload_len, sizeof(patch) - 1);
  assert_memory_equal(c->payload, patch, sizeof(patch) - 1);
  assert_int_equal(c->correlation_data_len, 1);
  assert_int_equal(c->correlation_data[0], '1');
}

static void next_reported_response_fires_the_ack_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  /* Next acknowledges on its own topic and carries no reported version yet, so
   * the ack reports zero rather than inventing one. */
  static const uint8_t corr[] = "1";
  inject_next_message(
      fx, "ih/ut-device/dev/twin/reported/response", corr, sizeof(corr) - 1, NULL, 0);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_true(rec.version == UINT64_C(0));
}

static void next_reported_response_with_an_unknown_rid_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  static const uint8_t corr[] = "77";
  inject_next_message(
      fx, "ih/ut-device/dev/twin/reported/response", corr, sizeof(corr) - 1, NULL, 0);
  assert_false(rec.fired);
}

static void next_a_get_response_does_not_satisfy_a_patch_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  /* Correlation data alone is not enough: the pending kind is checked too, so
   * a get answer cannot be reported as a patch acknowledgement. */
  static const uint8_t corr[] = "1";
  static const uint8_t body[] = "{\"desired\":{}}";
  inject_next_message(
      fx, "ih/ut-device/dev/twin/get/response", corr, sizeof(corr) - 1, body, sizeof(body) - 1);
  assert_false(rec.fired);
}

static void next_a_reported_response_does_not_satisfy_a_get_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  static const uint8_t corr[] = "1";
  inject_next_message(
      fx, "ih/ut-device/dev/twin/reported/response", corr, sizeof(corr) - 1, NULL, 0);
  assert_false(rec.fired);
}

static void next_desired_push_reaches_the_subscriber(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  desired_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* Next pushes desired properties on an exact topic with no version in it,
   * so the callback sees zero until the service starts carrying one. */
  static const uint8_t body[] = "{\"x\":11}";
  inject_next_message(fx, "ih/ut-device/dev/twin/desired", NULL, 0, body, sizeof(body) - 1);

  assert_true(rec.fired);
  assert_string_equal(rec.payload, "{\"x\":11}");
  assert_true(rec.version == UINT64_C(0));
}

static void next_correlation_data_longer_than_the_rid_buffer_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* Oversized correlation data is truncated into the rid buffer rather than
   * overrunning it, and the truncation must not accidentally parse back to a
   * live rid. */
  static const uint8_t corr[] = "111111111111111111111111111111111111111111111111111111111111";
  static const uint8_t body[] = "{\"x\":1}";
  inject_next_message(
      fx, "ih/ut-device/dev/twin/get/response", corr, sizeof(corr) - 1, body, sizeof(body) - 1);
  assert_false(rec.fired);
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
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_a_null_connection, setup, teardown),
    cmocka_unit_test_setup_teardown(both_subscriptions_use_qos_0, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_tolerates_null, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_zeroes_the_client, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_unregisters_both_handlers, setup, teardown),
    cmocka_unit_test_setup_teardown(get_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(get_publishes_an_empty_body, setup, teardown),
    cmocka_unit_test_setup_teardown(get_with_a_full_pending_table_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(a_publish_failure_releases_the_pending_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(patch_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(patch_rejects_a_null_patch_with_a_length, setup, teardown),
    cmocka_unit_test_setup_teardown(an_empty_patch_is_publishable, setup, teardown),
    cmocka_unit_test_setup_teardown(the_patch_body_is_forwarded_byte_for_byte, setup, teardown),
    cmocka_unit_test_setup_teardown(a_get_response_does_not_satisfy_a_patch_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(
        concurrent_get_and_patch_correlate_independently, setup, teardown),
    cmocka_unit_test_setup_teardown(a_second_response_for_the_same_rid_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_topic_with_a_non_numeric_status_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_topic_with_no_query_string_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_response_topic_with_no_rid_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_topic_with_the_wrong_prefix_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(the_rid_counter_wraps_without_reusing_zero, setup, teardown),
    cmocka_unit_test_setup_teardown(a_desired_topic_with_no_version_yields_zero, setup, teardown),
    cmocka_unit_test_setup_teardown(a_desired_version_past_32_bits_is_preserved, setup, teardown),
    cmocka_unit_test_setup_teardown(every_subscriber_in_a_pool_is_notified, setup, teardown),
    cmocka_unit_test_setup_teardown(subscribe_desired_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(subscribe_desired_rejects_a_null_callback, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_same_callback_with_a_different_context_takes_a_second_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(unsubscribe_frees_the_slot_for_reuse, setup, teardown),
    cmocka_unit_test_setup_teardown(
        unsubscribing_an_unregistered_pair_leaves_the_others_alone, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_dispatch_guard_is_cleared_after_a_dispatch, setup, teardown),
    cmocka_unit_test_setup_teardown(
        next_init_subscribes_the_three_twin_filters, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_get_publishes_to_the_service_topic_with_correlation_data, setup_next, teardown),
    cmocka_unit_test_setup_teardown(next_get_response_fires_the_callback, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_get_response_with_an_unknown_rid_is_dropped, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_get_response_without_correlation_data_is_dropped, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_patch_publishes_to_the_service_topic, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_reported_response_fires_the_ack_callback, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_reported_response_with_an_unknown_rid_is_dropped, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_a_get_response_does_not_satisfy_a_patch_slot, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_a_reported_response_does_not_satisfy_a_get_slot, setup_next, teardown),
    cmocka_unit_test_setup_teardown(next_desired_push_reaches_the_subscriber, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_correlation_data_longer_than_the_rid_buffer_is_dropped, setup_next, teardown),
  };
  return cmocka_run_group_tests_name("twin_client", tests, NULL, NULL);
}
