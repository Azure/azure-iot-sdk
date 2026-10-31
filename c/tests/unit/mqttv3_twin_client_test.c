// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv3 hub twin client unit tests, driven through the public API and the
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
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/mqttv3/az_iot_twin_client.h"

#include "internal/connection_client_internal.h"
#include "internal/deadline.h"
#include "support/connection_test_harness.h"
#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"
#include "support/subscription_ack.h"

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

/* A second distinct handler, so a test can prove set_desired_handler() replaces
 * rather than appends. */
static void on_desired_other(
    const uint8_t* payload,
    size_t payload_len,
    uint64_t version,
    void* user_ctx)
{
  on_desired(payload, payload_len, version, user_ctx);
}

typedef struct fixture
{
  az_iot_connection_client conn;
  az_iot_mqttv3_twin_client twin;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock;
} fixture;

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.iothub_hostname = "broker.example";
  opts.port = 8883;
  opts.device_id = "ut-device";
  assert_int_equal(az_iot_test_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  assert_int_equal(az_iot_mqttv3_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_mqttv3_twin_client_deinit(&fx->twin);
    az_iot_test_connection_client_deinit(&fx->conn);
    az_iot_mock_mqtt_factory_destroy(fx->factory);
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

/* Same as open_to_connected(), but keeps the connect burst in the history so a
 * test can assert on the subscriptions it issued. */
static void open_to_connected_keeping_history(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
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

/* First recorded PUBLISH, or NULL. Callers that need the whole call (payload,
 * qos) rather than just the topic use this. */
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

static bool history_has_subscribe(az_iot_mock_mqtt_client* m, const char* expected)
{
  return find_subscribe(m, expected) != NULL;
}

static void inject_desired(fixture* fx, const char* topic, const char* body)
{
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, topic, (const uint8_t*)body, strlen(body), AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void create_subscribes_response_and_desired(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_keeping_history(fx);

  assert_true(history_has_subscribe(fx->mock, "$iothub/twin/res/#"));
  assert_true(history_has_subscribe(fx->mock, "$iothub/twin/PATCH/properties/desired/#"));
}

static void get_publishes_and_response_fires_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

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
      az_iot_mqttv3_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
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
      az_iot_mqttv3_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
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
      az_iot_mqttv3_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
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
  assert_int_equal(
      az_iot_mqttv3_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  inject_desired(fx, "$iothub/twin/PATCH/properties/desired/?$version=99", "{\"x\":2}");

  assert_true(rec.fired);
  assert_int_equal(rec.payload_len, 7);
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
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
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

/* GET publishes with a topic starting "$iothub/twin/GET/". */
static size_t count_get_publishes(const fixture* fx)
{
  size_t count = 0;
  size_t const n = az_iot_mock_mqtt_client_call_count(fx->mock);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(fx->mock, i);
    count += (c->kind == AZ_IOT_MOCK_CALL_PUBLISH
              && strncmp(c->topic, "$iothub/twin/GET/", strlen("$iothub/twin/GET/")) == 0)
        ? 1u
        : 0u;
  }
  return count;
}

/* Bring up the next session, keeping its history, pumping until held resends have gone out. */
static void reopen_keeping_history(fixture* fx)
{
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
  for (int i = 0; i < 3; ++i)
  {
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  }
}

/* Drop the session and bring up the next one, keeping its history. */
static void drop_and_reopen(fixture* fx)
{
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  reopen_keeping_history(fx);
}

/* A GET unanswered when its session ends is sent again, under its rid, on the next session. */
static void a_pending_get_is_sent_again_on_the_next_session(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  drop_and_reopen(fx);
  assert_false(rec.fired);
  const az_iot_mock_call* resent
      = az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(resent);
  assert_string_equal(resent->topic, "$iothub/twin/GET/?$rid=1");

  static const uint8_t body[] = "{}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
}

static void a_pending_patch_is_failed_when_the_session_drops(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_mqttv3_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);
  assert_false(rec.fired);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_NOT_CONNECTED);
}

static void every_pending_get_is_sent_again_not_just_the_first(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record gets[AZ_IOT_TWIN_MAX_PENDING];
  memset(gets, 0, sizeof(gets));
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &gets[i]), AZ_IOT_OK);
  }
  drop_and_reopen(fx);
  assert_int_equal(count_get_publishes(fx), AZ_IOT_TWIN_MAX_PENDING);
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_false(gets[i].fired);
  }
}

static void the_pending_pool_is_reusable_after_a_dropped_session(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Fill the pool with patches, which a dropped session fails, and fill it again: a device on
   * a flaky link must not lose its twin after AZ_IOT_TWIN_MAX_PENDING outages. */
  static const uint8_t patch[] = "{}";
  patch_record first[AZ_IOT_TWIN_MAX_PENDING];
  memset(first, 0, sizeof(first));
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(
        az_iot_mqttv3_twin_client_patch_reported(
            &fx->twin, patch, sizeof(patch) - 1, on_patch, &first[i]),
        AZ_IOT_OK);
  }
  get_record overflow = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &overflow), AZ_IOT_ERR_NOT_SUPPORTED);

  drop_and_reopen(fx);
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(first[i].status, AZ_IOT_ERR_NOT_CONNECTED);
  }
  get_record second[AZ_IOT_TWIN_MAX_PENDING];
  memset(second, 0, sizeof(second));
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &second[i]), AZ_IOT_OK);
  }
}

static void a_deinitialized_twin_client_is_not_called_on_a_later_session_end(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* deinit() must unhook the handler. If it did not, the connection would
   * call into a zeroed client -- and, worse, into whatever the application had
   * already freed behind the user context. */
  az_iot_mqttv3_twin_client_deinit(&fx->twin);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_false(rec.fired);

  /* Re-init so the shared teardown() has a valid client to destroy. */
  assert_int_equal(az_iot_mqttv3_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void deinitializing_the_connection_does_not_complete_pending_requests(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* Same rule the QoS-1 acknowledgements follow: on deinit() the application
   * is tearing everything down and the context the callback closes over may
   * already be gone, so calling into it would turn cleanup into a
   * use-after-free. */
  az_iot_mqttv3_twin_client_deinit(&fx->twin);
  az_iot_test_connection_client_deinit(&fx->conn);
  assert_false(rec.fired);

  /* Rebuild what teardown() expects to tear down. */
  az_iot_connection_client_options opts = { 0 };
  opts.iothub_hostname = "broker.example";
  opts.port = 8883;
  opts.device_id = "ut-device";
  assert_int_equal(az_iot_test_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_mqttv3_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void a_failed_get_withholds_the_service_error_body(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* A rejected GET carries a service error description, not a twin document.
   * az_iot_twin_get_callback documents twin_payload as NULL on failure, so
   * forwarding this body would hand the application an error blob to parse as
   * if it were the twin. */
  static const uint8_t body[] = "{\"Message\":\"ErrorCode:ArgumentInvalid\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/400/?$rid=1", body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(rec.payload_len, 0);
  assert_string_equal(rec.payload, "");
}

static void a_response_status_too_long_to_be_one_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* The status is untrusted topic text. Accumulating an unbounded digit run
   * into a signed int is undefined behaviour once it overflows, so a value
   * that does not fit is reported as unreadable rather than wrapped into a
   * status that would then be mapped to a result. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/99999999999999999999/?$rid=1", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

static void a_response_rid_with_trailing_characters_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* A prefix-tolerant parse would read "1x" as request id 1 and complete this
   * live request from a malformed topic. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1x", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

static void a_response_rid_past_32_bits_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Seeded so the live request id is 1; 4294967297 truncates to 1 in 32 bits,
   * which is exactly the collision a width-unchecked parse would allow. */
  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=4294967297", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_false(rec.fired);
}

/* ------------------------------------------------------------------------- */
/* argument rejection and lifecycle                                          */
/* ------------------------------------------------------------------------- */

static void init_rejects_a_null_client(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_mqttv3_twin_client_init(NULL, &fx->conn), AZ_IOT_ERR_INVALID_ARG);
}

static void init_rejects_a_null_connection(void** state)
{
  (void)state;
  az_iot_mqttv3_twin_client local;
  assert_int_equal(az_iot_mqttv3_twin_client_init(&local, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void init_against_an_mqtt_v5_connection_is_rejected(void** state)
{
  (void)state;

  /* A direct connection declares its generation up front, so the pin can be
   * answered at init rather than deferred to connect. */
  az_iot_connection_client_options opts = { 0 };
  opts.iothub_hostname = "broker.example";
  opts.device_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  az_iot_connection_client conn;
  assert_int_equal(az_iot_test_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_twin_client twin;
  assert_int_equal(
      az_iot_mqttv3_twin_client_init(&twin, &conn), AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);

  az_iot_test_connection_client_deinit(&conn);
}

static void both_subscriptions_use_qos_0(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_keeping_history(fx);

  /* MQTTv3 twin traffic is qos 0 in both directions. Subscribing at qos 1
   * would make the hub retain and redeliver, which the client is not built to
   * de-duplicate. */
  const az_iot_mock_call* res = find_subscribe(fx->mock, "$iothub/twin/res/#");
  assert_non_null(res);
  assert_int_equal(res->qos, AZ_IOT_MQTT_QOS_0);
  const az_iot_mock_call* des = find_subscribe(fx->mock, "$iothub/twin/PATCH/properties/desired/#");
  assert_non_null(des);
  assert_int_equal(des->qos, AZ_IOT_MQTT_QOS_0);
}

static void deinit_tolerates_null(void** state)
{
  (void)state;
  az_iot_mqttv3_twin_client_deinit(NULL);
}

static void deinit_zeroes_the_client(void** state)
{
  fixture* fx = (fixture*)*state;

  /* Operates on the fixture's own client: a connection admits one twin client,
   * so a second init against the same connection is refused. */
  az_iot_mqttv3_twin_client_deinit(&fx->twin);

  /* Zeroed rather than merely flagged: a stale connection pointer left behind
   * is what a later get() would publish through. */
  const unsigned char* raw = (const unsigned char*)&fx->twin;
  for (size_t i = 0; i < sizeof(fx->twin); ++i)
  {
    assert_int_equal(raw[i], 0);
  }

  assert_int_equal(az_iot_mqttv3_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void deinit_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_mqttv3_twin_client_deinit(&fx->twin);
  /* The second call runs against a zeroed struct and must not follow the
   * now-NULL connection pointer into unregister. */
  az_iot_mqttv3_twin_client_deinit(&fx->twin);

  assert_int_equal(az_iot_mqttv3_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void deinit_unregisters_both_handlers(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record des = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_set_desired_handler(&fx->twin, on_desired, &des), AZ_IOT_OK);

  az_iot_mqttv3_twin_client_deinit(&fx->twin);

  /* Both the response and the desired handler must be gone: either one left
   * behind would dispatch into a zeroed client. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", NULL, 0, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  inject_desired(fx, "$iothub/twin/PATCH/properties/desired/?$version=5", "{\"x\":1}");
  assert_false(des.fired);

  assert_int_equal(az_iot_mqttv3_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* get                                                                       */
/* ------------------------------------------------------------------------- */

static void get_rejects_a_null_client(void** state)
{
  (void)state;
  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(NULL, on_get, &rec), AZ_IOT_ERR_INVALID_ARG);
}

static void get_publishes_an_empty_body(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* The service specifies an empty message for GET. Sending a body has been
   * observed to make the hub ignore the request outright. */
  const az_iot_mock_call* c = first_publish(fx->mock);
  assert_non_null(c);
  assert_int_equal(c->payload_len, 0);
}

static void get_publishes_at_qos_0(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* MQTTv3 twin requests are fire-and-forget; the $rid correlation, not the
   * MQTT acknowledgement, is what tells the caller the request landed. */
  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  const az_iot_mock_call* c = first_publish(fx->mock);
  assert_non_null(c);
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_0);
}

static void get_with_a_full_pending_table_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record recs[AZ_IOT_TWIN_MAX_PENDING];
  memset(recs, 0, sizeof(recs));
  for (size_t i = 0; i < (size_t)AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &recs[i]), AZ_IOT_OK);
  }

  /* Refused up front rather than published and then never correlated, which
   * would leave the caller waiting for a callback that could not arrive. */
  get_record overflow = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &overflow), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_false(overflow.fired);
}

/* A refused GET publish keeps the request: the connection's next do_work() sends it again. A
 * refused reported patch is refused to the caller with AZ_IOT_ERR_BUSY and holds no slot. */
static void a_refused_get_is_sent_again_and_a_refused_patch_is_busy(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_mock_mqtt_client_set_next_result(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_BUSY);
  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  size_t const before = count_get_publishes(fx);
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_int_equal(count_get_publishes(fx), before + 1u);
  assert_false(rec.fired);

  static const uint8_t patch[] = "{}";
  patch_record refused = { 0 };
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING + 1; ++i)
  {
    az_iot_mock_mqtt_client_set_next_result(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_BUSY);
    assert_int_equal(
        az_iot_mqttv3_twin_client_patch_reported(
            &fx->twin, patch, sizeof(patch) - 1, on_patch, &refused),
        AZ_IOT_ERR_BUSY);
  }
  assert_false(refused.fired);
  for (size_t i = 1; i < (size_t)AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    get_record again = { 0 };
    assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &again), AZ_IOT_OK);
  }
}

/* A GET made while disconnected waits and is sent once connected; a reported patch is refused. */
static void a_get_while_disconnected_waits_and_a_patch_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  static const uint8_t patch[] = "{}";
  patch_record refused = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_patch_reported(
          &fx->twin, patch, sizeof(patch) - 1, on_patch, &refused),
      AZ_IOT_ERR_NOT_CONNECTED);

  reopen_keeping_history(fx);
  assert_int_equal(count_get_publishes(fx), 1u);
  static const uint8_t body[] = "{}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_false(refused.fired);
}

/* The request timeout, counted from the call, completes an unanswered request on the
 * connection's do_work(); 0 restores the default, and a NULL client is refused. */
static void an_unanswered_request_times_out(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_int_equal(az_iot_mqttv3_twin_client_set_request_timeout(NULL, 1), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      fx->twin._internal.request_timeout_ms, AZ_IOT_MQTTV3_TWIN_REQUEST_TIMEOUT_MS_DEFAULT);
  assert_int_equal(az_iot_mqttv3_twin_client_set_request_timeout(&fx->twin, 1), AZ_IOT_OK);

  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  az_iot_test_wait_ms(5);
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_TIMEOUT);

  assert_int_equal(az_iot_mqttv3_twin_client_set_request_timeout(&fx->twin, 0), AZ_IOT_OK);
  assert_int_equal(
      fx->twin._internal.request_timeout_ms, AZ_IOT_MQTTV3_TWIN_REQUEST_TIMEOUT_MS_DEFAULT);
}

/* Queue a GET whose first send is refused, and end its time limit: the next flush expires it. */
static void queue_an_expired_get(fixture* fx, az_iot_twin_get_callback cb, void* ctx)
{
  az_iot_mock_mqtt_client_set_next_result(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_BUSY);
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, cb, ctx), AZ_IOT_OK);
  for (size_t i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (fx->twin._internal.requests[i].kind != 0 && !fx->twin._internal.requests[i].sent)
    {
      az_iot_deadline__expire_now(&fx->twin._internal.tracker_entries[i]._internal.deadline);
    }
  }
}

typedef struct deinit_call
{
  fixture* fx;
  bool fired;
} deinit_call;

static void deinit_from_get_callback(
    az_iot_result status,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  (void)status;
  (void)payload;
  (void)payload_len;
  deinit_call* call = (deinit_call*)user_ctx;
  call->fired = true;
  az_iot_mqttv3_twin_client_deinit(&call->fx->twin);
}

/* A callback that deinitializes the client during a request call: the call returns
 * AZ_IOT_ERR_NOT_INITIALIZED without touching the client again. */
static void a_client_deinitialized_by_a_callback_during_a_request_call(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  deinit_call call = { fx, false };
  queue_an_expired_get(fx, deinit_from_get_callback, &call);

  get_record rec = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_ERR_NOT_INITIALIZED);
  assert_true(call.fired);
  assert_false(rec.fired);
}

/* A callback that deinitializes the client while an answered request is released: the answer is
 * not reported to a deinitialized client. */
static void a_client_deinitialized_by_a_callback_during_an_answer(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  get_record answered = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &answered), AZ_IOT_OK);
  deinit_call call = { fx, false };
  queue_an_expired_get(fx, deinit_from_get_callback, &call);

  static const uint8_t body[] = "{}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
  (void)az_iot_connection_client_do_work(&fx->conn, 0);
  assert_true(call.fired);
  assert_false(answered.fired);
}

static void count_puback(az_iot_result status, void* user_ctx)
{
  (void)status;
  ++*(int*)user_ctx;
}

/* With every shared PUBACK slot taken, a refused GET (QoS 0, needing none) still bounds the
 * connection's wait to the retry interval. */
static void a_refused_get_is_retried_soon_when_puback_slots_are_full(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  int acks = 0;
  az_iot_mqtt_message msg = { 0 };
  msg.topic = "devices/ut-device/messages/events/";
  msg.qos = AZ_IOT_MQTT_QOS_1;
  while (az_iot_connection_client__publish(&fx->conn, fx, &msg, count_puback, &acks) == AZ_IOT_OK)
  {
  }
  az_iot_mock_mqtt_client_set_next_result(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_BUSY);
  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  (void)az_iot_connection_client_do_work(&fx->conn, 60000u);
  assert_true(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms
      <= 1000u);
}

/* A GET awaiting its answer is an unsettled delivery, so a SAS renewal waits for it. */
static void a_get_awaiting_its_answer_is_unsettled(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_false(az_iot_test_has_unsettled_deliveries(&fx->conn));
  get_record rec = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  assert_true(az_iot_test_has_unsettled_deliveries(&fx->conn));
  static const uint8_t body[] = "{}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      fx->mock, "$iothub/twin/res/200/?$rid=1", body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  assert_true(rec.fired);
  assert_false(az_iot_test_has_unsettled_deliveries(&fx->conn));
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
      az_iot_mqttv3_twin_client_patch_reported(NULL, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_ERR_INVALID_ARG);
}

static void patch_rejects_a_null_patch_with_a_length(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Rejecting the arguments must not consume a pending slot. */
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_patch_reported(&fx->twin, NULL, 8, on_patch, &rec),
      AZ_IOT_ERR_INVALID_ARG);

  for (size_t i = 0; i < (size_t)AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    get_record again = { 0 };
    assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &again), AZ_IOT_OK);
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
      az_iot_mqttv3_twin_client_patch_reported(&fx->twin, NULL, 0, on_patch, &rec), AZ_IOT_OK);
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
      az_iot_mqttv3_twin_client_patch_reported(&fx->twin, patch, sizeof(patch), on_patch, &rec),
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
      az_iot_mqttv3_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
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
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &grec), AZ_IOT_OK);
  assert_int_equal(
      az_iot_mqttv3_twin_client_patch_reported(
          &fx->twin, patch, sizeof(patch) - 1, on_patch, &prec),
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
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

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
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

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
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

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
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

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
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

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
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &a), AZ_IOT_OK);
  const az_iot_mock_call* first = first_publish(fx->mock);
  assert_non_null(first);
  assert_string_equal(first->topic, "$iothub/twin/GET/?$rid=4294967295");

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  get_record b = { 0 };
  assert_int_equal(az_iot_mqttv3_twin_client_get(&fx->twin, on_get, &b), AZ_IOT_OK);
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
  assert_int_equal(
      az_iot_mqttv3_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* The patch still has to reach the application: a missing version is not a
   * reason to withhold the properties themselves. */
  inject_desired(fx, "$iothub/twin/PATCH/properties/desired/", "{\"x\":3}");

  assert_true(rec.fired);
  assert_true(rec.version == UINT64_C(0));
  assert_string_equal(rec.payload, "{\"x\":3}");
}

static void a_desired_version_past_32_bits_is_preserved(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* The callback takes a uint64_t; parsing through a 32-bit intermediate would
   * wrap this to 1 and make a fresh patch look older than one already seen. */
  inject_desired(fx, "$iothub/twin/PATCH/properties/desired/?$version=4294967297", "{\"x\":4}");

  assert_true(rec.fired);
  assert_true(rec.version == UINT64_C(4294967297));
}

static void set_desired_handler_rejects_a_null_client(void** state)
{
  (void)state;
  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_set_desired_handler(NULL, on_desired, &rec),
      AZ_IOT_ERR_INVALID_ARG);
}

static void a_null_desired_handler_pauses_delivery(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);
  assert_int_equal(az_iot_mqttv3_twin_client_set_desired_handler(&fx->twin, NULL, NULL), AZ_IOT_OK);

  /* Clearing the handler must not tear the subscription down -- the patches
   * keep arriving, they are simply not dispatched. */
  inject_desired(fx, "$iothub/twin/PATCH/properties/desired/?$version=1", "{\"x\":1}");
  assert_false(rec.fired);

  /* ...and setting it again resumes delivery on the same subscription. */
  assert_int_equal(
      az_iot_mqttv3_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);
  inject_desired(fx, "$iothub/twin/PATCH/properties/desired/?$version=2", "{\"x\":2}");
  assert_true(rec.fired);
  assert_true(rec.version == UINT64_C(2));
}

static void setting_a_desired_handler_replaces_the_previous_one(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* One handler, not a registry: the second call must displace the first
   * rather than leaving both to fire. */
  desired_record first = { 0 };
  desired_record second = { 0 };
  assert_int_equal(
      az_iot_mqttv3_twin_client_set_desired_handler(&fx->twin, on_desired, &first), AZ_IOT_OK);
  assert_int_equal(
      az_iot_mqttv3_twin_client_set_desired_handler(&fx->twin, on_desired_other, &second),
      AZ_IOT_OK);

  inject_desired(fx, "$iothub/twin/PATCH/properties/desired/?$version=4", "{\"x\":6}");

  assert_false(first.fired);
  assert_true(second.fired);
  assert_string_equal(second.payload, "{\"x\":6}");
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
    cmocka_unit_test_setup_teardown(unknown_rid_drops_response, setup, teardown),
    cmocka_unit_test_setup_teardown(success_statuses_are_reported_as_ok, setup, teardown),
    cmocka_unit_test_setup_teardown(throttled_status_is_reported_as_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(bad_request_status_is_reported_as_invalid_arg, setup, teardown),
    cmocka_unit_test_setup_teardown(not_found_status_is_reported_as_not_found, setup, teardown),
    cmocka_unit_test_setup_teardown(
        server_error_status_is_reported_as_an_mqtt_error, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_pending_get_is_sent_again_on_the_next_session, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_pending_patch_is_failed_when_the_session_drops, setup, teardown),
    cmocka_unit_test_setup_teardown(
        every_pending_get_is_sent_again_not_just_the_first, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_pending_pool_is_reusable_after_a_dropped_session, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_deinitialized_twin_client_is_not_called_on_a_later_session_end, setup, teardown),
    cmocka_unit_test_setup_teardown(
        deinitializing_the_connection_does_not_complete_pending_requests, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_a_null_connection, setup, teardown),
    cmocka_unit_test_setup_teardown(
        init_against_an_mqtt_v5_connection_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(both_subscriptions_use_qos_0, setup, teardown),
    cmocka_unit_test_setup_teardown(deinit_tolerates_null, setup, teardown),
    cmocka_unit_test_setup_teardown(deinit_zeroes_the_client, setup, teardown),
    cmocka_unit_test_setup_teardown(deinit_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(deinit_unregisters_both_handlers, setup, teardown),
    cmocka_unit_test_setup_teardown(get_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(get_publishes_an_empty_body, setup, teardown),
    cmocka_unit_test_setup_teardown(get_publishes_at_qos_0, setup, teardown),
    cmocka_unit_test_setup_teardown(get_with_a_full_pending_table_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_refused_get_is_sent_again_and_a_refused_patch_is_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_get_while_disconnected_waits_and_a_patch_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(an_unanswered_request_times_out, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_client_deinitialized_by_a_callback_during_a_request_call, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_client_deinitialized_by_a_callback_during_an_answer, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_refused_get_is_retried_soon_when_puback_slots_are_full, setup, teardown),
    cmocka_unit_test_setup_teardown(a_get_awaiting_its_answer_is_unsettled, setup, teardown),
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
    cmocka_unit_test_setup_teardown(a_failed_get_withholds_the_service_error_body, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_status_too_long_to_be_one_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_rid_with_trailing_characters_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_response_rid_past_32_bits_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_desired_topic_with_no_version_yields_zero, setup, teardown),
    cmocka_unit_test_setup_teardown(a_desired_version_past_32_bits_is_preserved, setup, teardown),
    cmocka_unit_test_setup_teardown(set_desired_handler_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(a_null_desired_handler_pauses_delivery, setup, teardown),
    cmocka_unit_test_setup_teardown(
        setting_a_desired_handler_replaces_the_previous_one, setup, teardown),
  };
  return cmocka_run_group_tests_name("mqttv3_twin_client", tests, NULL, NULL);
}
