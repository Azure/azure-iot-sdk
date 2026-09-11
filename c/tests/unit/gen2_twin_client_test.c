// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTT v5 twin client unit tests, driven through the public API and the
 * in-memory mock_mqtt_iface.
 *
 * The gen2 twin uses a split topic space (srv/... outbound, dev/... inbound)
 * and correlates by MQTT v5 Correlation Data carrying the decimal request id
 * instead of a `$rid` query parameter. */
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
#include "azure/iot/gen2/az_iot_twin_client.h"

#include "support/mock_mqtt_iface.h"
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
  az_iot_gen2_twin_client twin;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock;
  /* Set once the factory has been handed to the connection, which adopts it.
   * Read in teardown rather than the connection's own counter, because a test
   * may already have destroyed the connection. */
  bool factory_registered;
} fixture;

static void init_connection(az_iot_connection_client* conn)
{
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  assert_int_equal(az_iot_connection_client_init(conn, &opts), AZ_IOT_OK);
}

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  init_connection(&fx->conn);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(fx->factory);

  assert_int_equal(az_iot_gen2_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_gen2_twin_client_destroy(&fx->twin);
    az_iot_connection_client_destroy(&fx->conn);
    if (!fx->factory_registered)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
    free(fx);
  }
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

/* Drive an MQTT v5 session to CONNECTED. Unlike Classic, CONNACK alone does not
 * announce CONNECTED: the presence wildcard and birth handshake complete first. */
static void open_to_connected(fixture* fx)
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

  /* gen2 feature delivery uses the presence wildcard; there are no later
   * per-feature SUBACKs to wait for. */
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
}

/* Deliver an inbound message carrying Correlation Data. */
static void inject_message(
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
/* topic binding                                                             */
/* ------------------------------------------------------------------------- */

static void init_subscribes_no_per_feature_twin_filters(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* All three twin topics are strict subsets of ih/ut-device/dev/#, which the
   * presence handshake already holds, so gen2 registers none of them. Responses
   * still route by dispatch prefix -- see the get/patch tests below. */
  assert_non_null(find_subscribe(fx->mock, "ih/ut-device/dev/#"));
  assert_null(find_subscribe(fx->mock, "ih/ut-device/dev/twin/get/response"));
  assert_null(find_subscribe(fx->mock, "ih/ut-device/dev/twin/reported/response"));
  assert_null(find_subscribe(fx->mock, "ih/ut-device/dev/twin/desired"));
}

/* ------------------------------------------------------------------------- */
/* get                                                                       */
/* ------------------------------------------------------------------------- */

static void get_publishes_to_the_service_topic_with_correlation_data(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* qos 1 on gen2: the request is a command the service must not silently
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

static void get_response_fires_the_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  static const uint8_t corr[] = "1";
  static const uint8_t body[] = "{\"desired\":{},\"reported\":{}}";
  inject_message(
      fx, "ih/ut-device/dev/twin/get/response", corr, sizeof(corr) - 1, body, sizeof(body) - 1);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_string_equal(rec.payload, "{\"desired\":{},\"reported\":{}}");
}

static void get_response_with_an_unknown_rid_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* Stale correlation data from an earlier session must not complete a live
   * request with someone else's twin document. */
  static const uint8_t corr[] = "99";
  static const uint8_t body[] = "{\"x\":1}";
  inject_message(
      fx, "ih/ut-device/dev/twin/get/response", corr, sizeof(corr) - 1, body, sizeof(body) - 1);
  assert_false(rec.fired);
}

static void get_response_without_correlation_data_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* With no correlator the rid reads as 0, which is the free-slot marker; the
   * response must not be allowed to match anything. */
  static const uint8_t body[] = "{\"x\":1}";
  inject_message(fx, "ih/ut-device/dev/twin/get/response", NULL, 0, body, sizeof(body) - 1);
  assert_false(rec.fired);
}

static void correlation_data_longer_than_the_rid_buffer_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* Oversized correlation data is truncated into the rid buffer rather than
   * overrunning it, and the truncation must not accidentally parse back to a
   * live rid. */
  static const uint8_t corr[] = "111111111111111111111111111111111111111111111111111111111111";
  static const uint8_t body[] = "{\"x\":1}";
  inject_message(
      fx, "ih/ut-device/dev/twin/get/response", corr, sizeof(corr) - 1, body, sizeof(body) - 1);
  assert_false(rec.fired);
}

static void get_rejects_a_null_client(void** state)
{
  (void)state;
  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(NULL, on_get, &rec), AZ_IOT_ERR_INVALID_ARG);
}

static void get_before_the_device_id_is_assigned_is_refused(void** state)
{
  (void)state;

  /* The request topic embeds the assigned device id, which a DPS connection
   * does not have until ASSIGNED. Publishing to a half-built topic would be
   * worse than refusing -- this is the case the connect-time bind exists for,
   * and the one the pre-split client got wrong by reading the device id inside
   * init(). */
  az_iot_connection_client conn;
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-reg";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_gen2_twin_client twin;
  assert_int_equal(az_iot_gen2_twin_client_init(&twin, &conn), AZ_IOT_OK);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&twin, on_get, &rec), AZ_IOT_ERR_NOT_CONNECTED);
  assert_false(rec.fired);

  patch_record prec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&twin, patch, sizeof(patch) - 1, on_patch, &prec),
      AZ_IOT_ERR_NOT_CONNECTED);
  assert_false(prec.fired);

  az_iot_gen2_twin_client_destroy(&twin);
  az_iot_connection_client_destroy(&conn);
}

static void get_with_a_full_pending_table_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record recs[AZ_IOT_TWIN_MAX_PENDING];
  memset(recs, 0, sizeof(recs));
  for (size_t i = 0; i < (size_t)AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &recs[i]), AZ_IOT_OK);
  }

  /* Refused up front rather than published and then never correlated, which
   * would leave the caller waiting for a callback that could not arrive. */
  get_record overflow = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_get(&fx->twin, on_get, &overflow), AZ_IOT_ERR_NOT_SUPPORTED);
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
  assert_int_not_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  for (size_t i = 0; i < (size_t)AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    get_record again = { 0 };
    assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &again), AZ_IOT_OK);
  }
}

static void the_rid_counter_wraps_without_reusing_zero(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Rid 0 is both the "no pending request" marker and what a message carrying
   * no correlation data decodes to, so handing it out would make an unrelated
   * response match a live slot. */
  fx->twin._internal.next_rid = UINT32_MAX;

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  get_record a = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &a), AZ_IOT_OK);
  const az_iot_mock_call* first = find_publish(fx->mock, "ih/ut-device/srv/twin/get");
  assert_non_null(first);
  assert_int_equal(first->correlation_data_len, 10);
  assert_memory_equal(first->correlation_data, "4294967295", 10);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  get_record b = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &b), AZ_IOT_OK);
  const az_iot_mock_call* second = find_publish(fx->mock, "ih/ut-device/srv/twin/get");
  assert_non_null(second);
  assert_int_equal(second->correlation_data_len, 1);
  assert_int_equal(second->correlation_data[0], '1');
}

/* ------------------------------------------------------------------------- */
/* patch reported                                                            */
/* ------------------------------------------------------------------------- */

static void patch_publishes_to_the_service_topic(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  const az_iot_mock_call* c = find_publish(fx->mock, "ih/ut-device/srv/twin/reported");
  assert_non_null(c);
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_1);
  assert_int_equal(c->payload_len, sizeof(patch) - 1);
  assert_memory_equal(c->payload, patch, sizeof(patch) - 1);
  assert_int_equal(c->correlation_data_len, 1);
  assert_int_equal(c->correlation_data[0], '1');
}

static void reported_response_fires_the_ack_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  /* gen2 acknowledges on its own topic and carries no reported version yet, so
   * the ack reports zero rather than inventing one. */
  static const uint8_t corr[] = "1";
  inject_message(fx, "ih/ut-device/dev/twin/reported/response", corr, sizeof(corr) - 1, NULL, 0);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_true(rec.version == UINT64_C(0));
}

static void reported_response_with_an_unknown_rid_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  static const uint8_t corr[] = "77";
  inject_message(fx, "ih/ut-device/dev/twin/reported/response", corr, sizeof(corr) - 1, NULL, 0);
  assert_false(rec.fired);
}

static void patch_rejects_a_null_client(void** state)
{
  (void)state;
  static const uint8_t patch[] = "{\"x\":1}";
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(NULL, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_ERR_INVALID_ARG);
}

static void patch_rejects_a_null_patch_with_a_length(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Rejecting the arguments must not consume a pending slot. */
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, NULL, 8, on_patch, &rec),
      AZ_IOT_ERR_INVALID_ARG);

  for (size_t i = 0; i < (size_t)AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    get_record again = { 0 };
    assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &again), AZ_IOT_OK);
  }
}

static void an_empty_patch_is_publishable(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);

  /* A zero-length patch is a legitimate no-op the service accepts; refusing it
   * would force callers to special-case an empty diff. */
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, NULL, 0, on_patch, &rec), AZ_IOT_OK);
  const az_iot_mock_call* c = find_publish(fx->mock, "ih/ut-device/srv/twin/reported");
  assert_non_null(c);
  assert_int_equal(c->payload_len, 0);
}

static void the_patch_body_is_forwarded_byte_for_byte(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);

  /* Embedded NUL included: the payload is a length-counted blob, not a C
   * string, and truncating at the NUL would silently drop reported properties. */
  static const uint8_t patch[] = { '{', '"', 'a', '"', ':', '1', 0x00, '}' };
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch), on_patch, &rec),
      AZ_IOT_OK);

  const az_iot_mock_call* c = find_publish(fx->mock, "ih/ut-device/srv/twin/reported");
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
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  /* Correlation data alone is not enough: the pending kind is checked too, so
   * a get answer cannot be reported as a patch acknowledgement. */
  static const uint8_t corr[] = "1";
  static const uint8_t body[] = "{\"desired\":{}}";
  inject_message(
      fx, "ih/ut-device/dev/twin/get/response", corr, sizeof(corr) - 1, body, sizeof(body) - 1);
  assert_false(rec.fired);
}

static void a_reported_response_does_not_satisfy_a_get_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  static const uint8_t corr[] = "1";
  inject_message(fx, "ih/ut-device/dev/twin/reported/response", corr, sizeof(corr) - 1, NULL, 0);
  assert_false(rec.fired);
}

static void concurrent_get_and_patch_correlate_independently(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record grec = { 0 };
  patch_record prec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &grec), AZ_IOT_OK);
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &prec),
      AZ_IOT_OK);

  /* Answered out of order: correlation is by request id, not arrival order. */
  static const uint8_t corr2[] = "2";
  inject_message(fx, "ih/ut-device/dev/twin/reported/response", corr2, sizeof(corr2) - 1, NULL, 0);
  assert_true(prec.fired);
  assert_false(grec.fired);

  static const uint8_t corr1[] = "1";
  static const uint8_t body[] = "{\"desired\":{}}";
  inject_message(
      fx, "ih/ut-device/dev/twin/get/response", corr1, sizeof(corr1) - 1, body, sizeof(body) - 1);
  assert_true(grec.fired);
  assert_int_equal(grec.status, AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* pending requests across a dropped session                                 */
/* ------------------------------------------------------------------------- */

static void a_pending_get_is_failed_when_the_session_drops(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
  assert_false(rec.fired);

  /* The response would have travelled on the session that just died, so it can
   * never arrive. Leaving the caller waiting is indistinguishable from a hang. */
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
    assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &gets[i]), AZ_IOT_OK);
  }

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_true(gets[i].fired);
    assert_int_equal(gets[i].status, AZ_IOT_ERR_NOT_CONNECTED);
  }
}

/* ------------------------------------------------------------------------- */
/* desired properties                                                        */
/* ------------------------------------------------------------------------- */

static void desired_push_reaches_the_handler(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* gen2 pushes desired properties on an exact topic with no version in it,
   * so the callback sees zero until the service starts carrying one. */
  static const uint8_t body[] = "{\"x\":11}";
  inject_message(fx, "ih/ut-device/dev/twin/desired", NULL, 0, body, sizeof(body) - 1);

  assert_true(rec.fired);
  assert_string_equal(rec.payload, "{\"x\":11}");
  assert_true(rec.version == UINT64_C(0));
}

static void set_desired_handler_rejects_a_null_client(void** state)
{
  (void)state;
  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(NULL, on_desired, &rec), AZ_IOT_ERR_INVALID_ARG);
}

static void a_null_desired_handler_pauses_delivery(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);
  assert_int_equal(az_iot_gen2_twin_client_set_desired_handler(&fx->twin, NULL, NULL), AZ_IOT_OK);

  static const uint8_t body[] = "{\"x\":1}";
  inject_message(fx, "ih/ut-device/dev/twin/desired", NULL, 0, body, sizeof(body) - 1);
  assert_false(rec.fired);

  /* Setting it again resumes delivery; the handler registration was never torn
   * down, only the dispatch target. */
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);
  static const uint8_t body2[] = "{\"x\":2}";
  inject_message(fx, "ih/ut-device/dev/twin/desired", NULL, 0, body2, sizeof(body2) - 1);
  assert_true(rec.fired);
  assert_string_equal(rec.payload, "{\"x\":2}");
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
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &first), AZ_IOT_OK);
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired_other, &second), AZ_IOT_OK);

  static const uint8_t body[] = "{\"x\":6}";
  inject_message(fx, "ih/ut-device/dev/twin/desired", NULL, 0, body, sizeof(body) - 1);

  assert_false(first.fired);
  assert_true(second.fired);
  assert_string_equal(second.payload, "{\"x\":6}");
}

/* ------------------------------------------------------------------------- */
/* argument rejection and lifecycle                                          */
/* ------------------------------------------------------------------------- */

static void init_rejects_a_null_client(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_gen2_twin_client_init(NULL, &fx->conn), AZ_IOT_ERR_INVALID_ARG);
}

static void init_rejects_a_null_connection(void** state)
{
  (void)state;
  az_iot_gen2_twin_client local;
  assert_int_equal(az_iot_gen2_twin_client_init(&local, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void init_against_a_classic_connection_is_rejected(void** state)
{
  (void)state;

  /* A direct connection declares its generation up front, so the pin can be
   * answered at init rather than deferred to connect. */
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_CLASSIC;
  az_iot_connection_client conn;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_gen2_twin_client twin;
  assert_int_equal(
      az_iot_gen2_twin_client_init(&twin, &conn), AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);

  az_iot_connection_client_destroy(&conn);
}

static void destroy_tolerates_null(void** state)
{
  (void)state;
  az_iot_gen2_twin_client_destroy(NULL);
}

static void destroy_zeroes_the_client(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_gen2_twin_client_destroy(&fx->twin);

  /* Zeroed rather than merely flagged: a stale connection pointer left behind
   * is what a later get() would publish through. */
  const unsigned char* raw = (const unsigned char*)&fx->twin;
  for (size_t i = 0; i < sizeof(fx->twin); ++i)
  {
    assert_int_equal(raw[i], 0);
  }

  assert_int_equal(az_iot_gen2_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void destroy_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_gen2_twin_client_destroy(&fx->twin);
  /* The second call runs against a zeroed struct and must not follow the
   * now-NULL connection pointer into unregister. */
  az_iot_gen2_twin_client_destroy(&fx->twin);

  assert_int_equal(az_iot_gen2_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void destroy_unregisters_every_handler(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record des = { 0 };
  get_record get = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &des), AZ_IOT_OK);
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &get), AZ_IOT_OK);

  az_iot_gen2_twin_client_destroy(&fx->twin);

  /* All three inbound handlers must be gone: any one left behind would
   * dispatch into a zeroed client. */
  static const uint8_t corr[] = "1";
  static const uint8_t body[] = "{\"x\":1}";
  inject_message(
      fx, "ih/ut-device/dev/twin/get/response", corr, sizeof(corr) - 1, body, sizeof(body) - 1);
  inject_message(fx, "ih/ut-device/dev/twin/reported/response", corr, sizeof(corr) - 1, NULL, 0);
  inject_message(fx, "ih/ut-device/dev/twin/desired", NULL, 0, body, sizeof(body) - 1);
  assert_false(des.fired);
  assert_false(get.fired);

  assert_int_equal(az_iot_gen2_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void destroy_frees_the_connect_time_bind_slot(void** state)
{
  (void)state;

  /* destroy() must withdraw the bind registration. Releasing the profile does
   * NOT do it. A leak retires one of the connection's AZ_IOT_MAX_FEATURE_CLIENT_BINDS
   * slots per cycle and -- far worse -- leaves the connection holding a callback
   * into a destroyed client to invoke on the next connect. Distinct addresses,
   * because the registry is keyed on the owner pointer and reusing one address
   * would overwrite the same slot and hide the leak. */
  az_iot_connection_client conn;
  init_connection(&conn);

  az_iot_gen2_twin_client twins[AZ_IOT_MAX_FEATURE_CLIENT_BINDS + 2];
  for (size_t i = 0; i < sizeof(twins) / sizeof(twins[0]); ++i)
  {
    assert_int_equal(az_iot_gen2_twin_client_init(&twins[i], &conn), AZ_IOT_OK);
    az_iot_gen2_twin_client_destroy(&twins[i]);
  }

  az_iot_connection_client_destroy(&conn);
}

static void a_destroyed_twin_client_is_not_called_on_a_later_session_end(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  /* destroy() must unhook the session-end handler too. If it did not, the
   * connection would call into a zeroed client -- and, worse, into whatever the
   * application had already freed behind the user context. */
  az_iot_gen2_twin_client_destroy(&fx->twin);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_false(rec.fired);

  assert_int_equal(az_iot_gen2_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_subscribes_no_per_feature_twin_filters, setup, teardown),
    cmocka_unit_test_setup_teardown(
        get_publishes_to_the_service_topic_with_correlation_data, setup, teardown),
    cmocka_unit_test_setup_teardown(get_response_fires_the_callback, setup, teardown),
    cmocka_unit_test_setup_teardown(get_response_with_an_unknown_rid_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        get_response_without_correlation_data_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        correlation_data_longer_than_the_rid_buffer_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(get_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(
        get_before_the_device_id_is_assigned_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(get_with_a_full_pending_table_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(a_publish_failure_releases_the_pending_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(the_rid_counter_wraps_without_reusing_zero, setup, teardown),
    cmocka_unit_test_setup_teardown(patch_publishes_to_the_service_topic, setup, teardown),
    cmocka_unit_test_setup_teardown(reported_response_fires_the_ack_callback, setup, teardown),
    cmocka_unit_test_setup_teardown(
        reported_response_with_an_unknown_rid_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(patch_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(patch_rejects_a_null_patch_with_a_length, setup, teardown),
    cmocka_unit_test_setup_teardown(an_empty_patch_is_publishable, setup, teardown),
    cmocka_unit_test_setup_teardown(the_patch_body_is_forwarded_byte_for_byte, setup, teardown),
    cmocka_unit_test_setup_teardown(a_get_response_does_not_satisfy_a_patch_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_reported_response_does_not_satisfy_a_get_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(
        concurrent_get_and_patch_correlate_independently, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_pending_get_is_failed_when_the_session_drops, setup, teardown),
    cmocka_unit_test_setup_teardown(
        every_pending_request_is_failed_not_just_the_first, setup, teardown),
    cmocka_unit_test_setup_teardown(desired_push_reaches_the_handler, setup, teardown),
    cmocka_unit_test_setup_teardown(set_desired_handler_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(a_null_desired_handler_pauses_delivery, setup, teardown),
    cmocka_unit_test_setup_teardown(
        setting_a_desired_handler_replaces_the_previous_one, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_a_null_connection, setup, teardown),
    cmocka_unit_test_setup_teardown(init_against_a_classic_connection_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_tolerates_null, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_zeroes_the_client, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_unregisters_every_handler, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_frees_the_connect_time_bind_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_destroyed_twin_client_is_not_called_on_a_later_session_end, setup, teardown),
  };
  return cmocka_run_group_tests_name("gen2_twin_client", tests, NULL, NULL);
}
