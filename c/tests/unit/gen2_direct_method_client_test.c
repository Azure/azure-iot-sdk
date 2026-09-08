// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Gen2 (Hub-Next / AEG, MQTT v5) direct method client unit tests, driven
 * through the public API and the in-memory mock_mqtt_iface.
 *
 * The Next topic space is ih/{device}/dev/methods/{name}, and the request id
 * travels in MQTT v5 Correlation Data rather than in an $rid query parameter.
 */
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
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/gen2/az_iot_direct_method_client.h"

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
  az_iot_gen2_direct_method_client dm;
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
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(fx->factory);

  assert_int_equal(az_iot_gen2_direct_method_client_init(&fx->dm, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_gen2_direct_method_client_destroy(&fx->dm);
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
static void open_conn(fixture* fx)
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
static void finish_birth(fixture* fx)
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
static void open_to_connected(fixture* fx)
{
  open_conn(fx);
  finish_birth(fx);
  /* gen2 feature delivery uses the presence wildcard; there are no later
   * per-feature SUBACKs to wait for. */
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
}

/* Deliver one invocation, optionally carrying Correlation Data. */
static void inject_invocation(
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

/* ------------------------------------------------------------------------- */
/* init and profile pinning                                                  */
/* ------------------------------------------------------------------------- */

static void init_rejects_a_null_client(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_gen2_direct_method_client_init(NULL, &fx->conn), AZ_IOT_ERR_INVALID_ARG);
}

static void init_rejects_a_null_connection(void** state)
{
  (void)state;
  az_iot_gen2_direct_method_client dm;
  assert_int_equal(az_iot_gen2_direct_method_client_init(&dm, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void init_against_a_classic_connection_is_rejected(void** state)
{
  (void)state;

  /* A direct connection declares its generation up front, so the pin can be
   * answered at init rather than deferred to connect. */
  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_CLASSIC;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_gen2_direct_method_client dm;
  assert_int_equal(
      az_iot_gen2_direct_method_client_init(&dm, &conn), AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);

  az_iot_connection_client_destroy(&conn);
}

static void init_does_not_subscribe_a_redundant_methods_filter(void** state)
{
  fixture* fx = (fixture*)*state;
  open_conn(fx);

  /* CONNACK alone buys only the presence filter. */
  assert_non_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE, "ih/ut-device/dev/#"));
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE, "ih/ut-device/dev/methods/+"));

  finish_birth(fx);

  /* And still nothing afterwards: ih/ut-device/dev/# already covers
   * ih/ut-device/dev/methods/+, so a second filter would be redundant.
   * Invocations still arrive -- see invocation_is_dispatched_to_the_handler. */
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE, "ih/ut-device/dev/methods/+"));
}

/* ------------------------------------------------------------------------- */
/* delivery and response                                                     */
/* ------------------------------------------------------------------------- */

static void invocation_is_dispatched_to_the_handler(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  static const uint8_t payload[] = "{\"x\":1}";
  static const uint8_t corr[] = { 0xDE, 0xAD, 0xBE, 0xEF };
  inject_invocation(
      fx, "ih/ut-device/dev/methods/reboot", corr, sizeof(corr), payload, sizeof(payload) - 1);

  assert_true(rec.fired);
  assert_string_equal(rec.method_name, "reboot");
  assert_int_equal(rec.payload_len, sizeof(payload) - 1);
  assert_string_equal(rec.payload, "{\"x\":1}");
  assert_non_null(rec.request);
}

static void respond_publishes_to_the_service_topic_with_a_status_property(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  static const uint8_t corr[] = { 0x01, 0x02, 0x03, 0x04 };
  inject_invocation(fx, "ih/ut-device/dev/methods/reboot", corr, sizeof(corr), NULL, 0);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  static const uint8_t resp[] = "{\"ok\":true}";
  assert_int_equal(
      az_iot_gen2_direct_method_respond(rec.request, 200, resp, sizeof(resp) - 1), AZ_IOT_OK);

  /* Next carries the status as a user property, not in the topic, and the
   * response goes to the service-bound half of the topic space. */
  const az_iot_mock_call* c
      = find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, "ih/ut-device/srv/methods/reboot/response");
  assert_non_null(c);
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_1);
  assert_int_equal(c->payload_len, sizeof(resp) - 1);
  assert_memory_equal(c->payload, resp, sizeof(resp) - 1);
}

static void respond_echoes_the_correlation_data_back(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  static const uint8_t corr[] = { 0xAA, 0xBB, 0xCC };
  inject_invocation(fx, "ih/ut-device/dev/methods/ping", corr, sizeof(corr), NULL, 0);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_gen2_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);

  /* The service correlates the response to the invocation solely by this
   * value, so dropping it would strand the caller until it timed out. */
  const az_iot_mock_call* c
      = find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, "ih/ut-device/srv/methods/ping/response");
  assert_non_null(c);
  assert_int_equal(c->correlation_data_len, sizeof(corr));
  assert_memory_equal(c->correlation_data, corr, sizeof(corr));
}

static void correlation_data_past_the_maximum_is_truncated(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Oversized correlation data is clamped to the request's fixed buffer rather
   * than overrunning it; the invocation is still delivered. */
  uint8_t corr[AZ_IOT_DM_CORR_DATA_MAX + 8];
  for (size_t i = 0; i < sizeof(corr); ++i)
  {
    corr[i] = (uint8_t)(i & 0xFF);
  }
  inject_invocation(fx, "ih/ut-device/dev/methods/big", corr, sizeof(corr), NULL, 0);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_gen2_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);
  const az_iot_mock_call* c
      = find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, "ih/ut-device/srv/methods/big/response");
  assert_non_null(c);
  assert_int_equal(c->correlation_data_len, AZ_IOT_DM_CORR_DATA_MAX);
  assert_memory_equal(c->correlation_data, corr, AZ_IOT_DM_CORR_DATA_MAX);
}

static void invocation_without_correlation_data_is_still_delivered(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* A service that omits Correlation Data is still answerable: the response
   * simply carries none back. */
  inject_invocation(fx, "ih/ut-device/dev/methods/bare", NULL, 0, NULL, 0);
  assert_true(rec.fired);
  assert_string_equal(rec.method_name, "bare");

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(az_iot_gen2_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);
  const az_iot_mock_call* c
      = find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, "ih/ut-device/srv/methods/bare/response");
  assert_non_null(c);
  assert_int_equal(c->correlation_data_len, 0);
}

/* ------------------------------------------------------------------------- */
/* topic parsing                                                             */
/* ------------------------------------------------------------------------- */

static void topic_with_a_foreign_prefix_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Not the hub topic space at all. */
  inject_invocation(fx, "xx/ut-device/dev/methods/reboot", NULL, 0, NULL, 0);
  assert_false(rec.fired);
}

static void topic_without_the_methods_segment_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Right prefix, wrong feature: a twin topic must not reach the method
   * handler even though both arrive on the same device-scoped subscription. */
  inject_invocation(fx, "ih/ut-device/dev/twin/patch", NULL, 0, NULL, 0);
  assert_false(rec.fired);
}

static void topic_with_an_empty_method_name_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Nothing after the segment, and trailing slashes do not manufacture a name:
   * responding would build a topic with an empty method segment. */
  inject_invocation(fx, "ih/ut-device/dev/methods/", NULL, 0, NULL, 0);
  assert_false(rec.fired);
  inject_invocation(fx, "ih/ut-device/dev/methods///", NULL, 0, NULL, 0);
  assert_false(rec.fired);
}

static void method_name_past_the_bound_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

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
  inject_invocation(fx, topic, NULL, 0, NULL, 0);
  assert_false(rec.fired);
}

/* ------------------------------------------------------------------------- */
/* in-flight request pool                                                    */
/* ------------------------------------------------------------------------- */

static void invocation_with_no_handler_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* No handler was ever set. The request must not be taken out of the pool,
   * or a later handler would start life with fewer slots than documented. */
  inject_invocation(fx, "ih/ut-device/dev/methods/reboot", NULL, 0, NULL, 0);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  for (int i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    char topic[64];
    snprintf(topic, sizeof(topic), "ih/ut-device/dev/methods/m%d", i);
    rec.fired = false;
    inject_invocation(fx, topic, NULL, 0, NULL, 0);
    assert_true(rec.fired);
  }
}

static void pool_exhaustion_drops_the_extra_invocation(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);

  /* Hold every slot by never responding. */
  for (int i = 0; i < AZ_IOT_DM_MAX_INFLIGHT; ++i)
  {
    char topic[64];
    snprintf(topic, sizeof(topic), "ih/ut-device/dev/methods/m%d", i);
    rec.fired = false;
    inject_invocation(fx, topic, NULL, 0, NULL, 0);
    assert_true(rec.fired);
  }

  /* The overflow invocation is dropped rather than evicting a live request,
   * which would hand the application a request it could no longer answer. */
  rec.fired = false;
  inject_invocation(fx, "ih/ut-device/dev/methods/overflow", NULL, 0, NULL, 0);
  assert_false(rec.fired);
}

static void responding_twice_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_direct_method_client_set_handler(&fx->dm, on_method, &rec), AZ_IOT_OK);
  inject_invocation(fx, "ih/ut-device/dev/methods/reboot", NULL, 0, NULL, 0);
  assert_true(rec.fired);

  assert_int_equal(az_iot_gen2_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_OK);

  /* The slot has been returned to the pool and may already belong to another
   * invocation, so a second answer would reply on someone else's behalf. */
  assert_int_equal(
      az_iot_gen2_direct_method_respond(rec.request, 200, NULL, 0), AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test(init_rejects_a_null_connection),
    cmocka_unit_test(init_against_a_classic_connection_is_rejected),
    cmocka_unit_test_setup_teardown(
        init_does_not_subscribe_a_redundant_methods_filter, setup, teardown),
    cmocka_unit_test_setup_teardown(invocation_is_dispatched_to_the_handler, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_publishes_to_the_service_topic_with_a_status_property, setup, teardown),
    cmocka_unit_test_setup_teardown(respond_echoes_the_correlation_data_back, setup, teardown),
    cmocka_unit_test_setup_teardown(
        correlation_data_past_the_maximum_is_truncated, setup, teardown),
    cmocka_unit_test_setup_teardown(
        invocation_without_correlation_data_is_still_delivered, setup, teardown),
    cmocka_unit_test_setup_teardown(topic_with_a_foreign_prefix_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(topic_without_the_methods_segment_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(topic_with_an_empty_method_name_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(method_name_past_the_bound_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(invocation_with_no_handler_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(pool_exhaustion_drops_the_extra_invocation, setup, teardown),
    cmocka_unit_test_setup_teardown(responding_twice_is_rejected, setup, teardown),
  };
  return cmocka_run_group_tests_name("gen2_direct_method_client", tests, NULL, NULL);
}
