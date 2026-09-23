// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTT v5 twin client unit tests, driven through the public API and the
 * in-memory mock_mqtt_iface.
 *
 * The gen2 twin uses one topic each way -- srv/twin outbound, dev/twin inbound
 * -- with the message kind in a `type` user property and a protobuf body.
 * Device-initiated exchanges correlate on a per-attempt 16-byte UUID;
 * backend-initiated ones carry the connection's birth nonce instead.
 *
 * Bodies are written out as literal protobuf bytes rather than built with the
 * SDK's own encoder, so a test proves the wire format instead of agreeing with
 * whatever the encoder happens to emit. */
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

#define TWIN_SRV_TOPIC "ih/ut-device/srv/twin"
#define TWIN_DEV_TOPIC "ih/ut-device/dev/twin"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct section_record
{
  uint64_t version;
  char payload[64];
  size_t payload_len;
  bool has_payload;
} section_record;

static void copy_section(const az_iot_gen2_twin_section* src, section_record* dst)
{
  dst->version = src->version;
  dst->has_payload = (src->payload != NULL);
  if (src->payload && src->payload_len > 0 && src->payload_len < sizeof(dst->payload))
  {
    memcpy(dst->payload, src->payload, src->payload_len);
    dst->payload[src->payload_len] = '\0';
    dst->payload_len = src->payload_len;
  }
}

typedef struct get_record
{
  bool fired;
  az_iot_result status;
  bool had_twin;
  section_record desired;
  section_record reported;
} get_record;

static void on_get(az_iot_result status, const az_iot_gen2_twin_state* twin, void* user_ctx)
{
  get_record* r = (get_record*)user_ctx;
  r->fired = true;
  r->status = status;
  r->had_twin = (twin != NULL);
  if (twin)
  {
    copy_section(&twin->desired, &r->desired);
    copy_section(&twin->reported, &r->reported);
  }
}

typedef struct patch_record
{
  bool fired;
  az_iot_result status;
  bool had_result;
  az_iot_gen2_twin_patch_status verdict;
  uint64_t version;
} patch_record;

static void on_patch(
    az_iot_result status,
    const az_iot_gen2_twin_patch_result* result,
    void* user_ctx)
{
  patch_record* r = (patch_record*)user_ctx;
  r->fired = true;
  r->status = status;
  r->had_result = (result != NULL);
  if (result)
  {
    r->verdict = result->status;
    r->version = result->version;
  }
}

typedef struct push_record
{
  bool fired;
  section_record desired;
  section_record reported;
} push_record;

static void on_push(const az_iot_gen2_twin_state* twin, void* user_ctx)
{
  push_record* r = (push_record*)user_ctx;
  r->fired = true;
  copy_section(&twin->desired, &r->desired);
  copy_section(&twin->reported, &r->reported);
}

typedef struct desired_record
{
  bool fired;
  int count;
  char payload[64];
  size_t payload_len;
  uint64_t version;
} desired_record;

static void on_desired(const uint8_t* payload, size_t payload_len, uint64_t version, void* user_ctx)
{
  desired_record* r = (desired_record*)user_ctx;
  r->fired = true;
  r->count++;
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
  /* The birth nonce of the current connection, which backend-initiated
   * messages must carry to be accepted. */
  uint8_t nonce[16];
  uint8_t encode_buffer[256];
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
  assert_int_equal(
      az_iot_gen2_twin_client_set_encode_buffer(
          &fx->twin, fx->encode_buffer, sizeof(fx->encode_buffer)),
      AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_gen2_twin_client_deinit(&fx->twin);
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

/* Count the twin publishes so far, so a test can prove nothing new went out. */
static size_t count_twin_publishes(az_iot_mock_mqtt_client* m)
{
  size_t count = 0;
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i);
    if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH && strcmp(c->topic, TWIN_SRV_TOPIC) == 0)
    {
      count++;
    }
  }
  return count;
}

/* Drive an MQTT v5 session to CONNECTED, admitting the birth with a birth-ack
 * carrying @p desired and @p reported as the authoritative twin versions.
 *
 * Unlike Classic, CONNACK alone does not announce CONNECTED: the presence
 * wildcard and the birth handshake complete first. */
static void open_to_connected_with_versions(fixture* fx, uint64_t desired, uint64_t reported)
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
  memcpy(fx->nonce, birth->correlation_data, sizeof(fx->nonce));

  /* BirthAck { 10: desired_version, 11: reported_version }; proto3 omits a
   * zero, so an unset version simply is not written. */
  uint8_t ack_body[24];
  size_t ack_len = 0;
  if (desired)
  {
    ack_body[ack_len++] = 0x50;
    assert_true(desired < 128);
    ack_body[ack_len++] = (uint8_t)desired;
  }
  if (reported)
  {
    ack_body[ack_len++] = 0x58;
    assert_true(reported < 128);
    ack_body[ack_len++] = (uint8_t)reported;
  }

  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/ut-device/dev/presence";
  ack_msg.correlation_data = fx->nonce;
  ack_msg.correlation_data_len = sizeof(fx->nonce);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  ack_msg.payload = ack_len ? ack_body : NULL;
  ack_msg.payload_len = ack_len;
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

static void open_to_connected(fixture* fx) { open_to_connected_with_versions(fx, 0, 0); }

/* Deliver an inbound twin message: @p type in the `type` user property, @p corr
 * as Correlation Data. */
static void inject_twin_on(
    fixture* fx,
    const char* topic,
    const char* type,
    const uint8_t* corr,
    size_t corr_len,
    const uint8_t* payload,
    size_t payload_len)
{
  az_iot_mqtt_user_property type_prop = { "type", type };
  az_iot_mqtt_message msg;
  memset(&msg, 0, sizeof(msg));
  msg.topic = topic;
  msg.payload = payload;
  msg.payload_len = payload_len;
  msg.correlation_data = corr;
  msg.correlation_data_len = corr_len;
  msg.content_type = "application/protobuf";
  msg.qos = AZ_IOT_MQTT_QOS_0;
  if (type)
  {
    msg.user_properties = &type_prop;
    msg.user_properties_count = 1;
  }
  az_iot_mqtt_event evt;
  memset(&evt, 0, sizeof(evt));
  evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  evt.message = &msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &evt));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

static void inject_twin(
    fixture* fx,
    const char* type,
    const uint8_t* corr,
    const uint8_t* payload,
    size_t payload_len)
{
  inject_twin_on(fx, TWIN_DEV_TOPIC, type, corr, corr ? 16u : 0u, payload, payload_len);
}

/* Issue a GET and return the correlation id it published under. */
static void issue_get(fixture* fx, get_record* rec, uint8_t out_corr[16])
{
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, rec), AZ_IOT_OK);
  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  assert_non_null(pub);
  assert_int_equal(pub->correlation_data_len, 16);
  memcpy(out_corr, pub->correlation_data, 16);
}

/* ------------------------------------------------------------------------- */
/* topic binding                                                             */
/* ------------------------------------------------------------------------- */

static void init_subscribes_no_per_feature_twin_filter(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* The inbound twin topic is a strict subset of ih/ut-device/dev/#, which the
   * presence handshake already holds, so gen2 registers nothing of its own.
   * Messages still route by dispatch prefix -- see the tests below. */
  assert_non_null(find_subscribe(fx->mock, "ih/ut-device/dev/#"));
  assert_null(find_subscribe(fx->mock, TWIN_DEV_TOPIC));
}

/* ------------------------------------------------------------------------- */
/* get                                                                       */
/* ------------------------------------------------------------------------- */

/* The GET goes to the single service topic as a protobuf TwinGet, at QoS 0,
 * with its kind in the `type` user property and a 16-byte correlation id. */
static void get_publishes_a_conformant_request(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  assert_non_null(pub);
  assert_int_equal(pub->qos, AZ_IOT_MQTT_QOS_0);
  assert_string_equal(pub->user_type, "get:1");
  assert_string_equal(pub->content_type, "application/protobuf");
  assert_int_equal(pub->correlation_data_len, 16);

  /* TwinGet { 1: sections = BOTH }. */
  const uint8_t expect[] = { 0x08, 0x03 };
  assert_int_equal(pub->payload_len, sizeof(expect));
  assert_memory_equal(pub->payload, expect, sizeof(expect));
}

/* The correlation id is a fresh UUID per attempt, not a counter: two GETs must
 * not collide, or one response would complete the wrong request. */
static void each_request_carries_a_distinct_correlation_id(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record first = { 0 };
  get_record second = { 0 };
  uint8_t corr_a[16];
  uint8_t corr_b[16];
  issue_get(fx, &first, corr_a);
  issue_get(fx, &second, corr_b);

  assert_memory_not_equal(corr_a, corr_b, sizeof(corr_a));

  /* It is also a well-formed v4 UUID, which is what the service expects. */
  assert_int_equal(corr_a[6] & 0xF0, 0x40);
  assert_int_equal(corr_a[8] & 0xC0, 0x80);
}

/* The response is correlated by the request's UUID and decoded into the two
 * sections, each with its authoritative version. */
static void get_response_decodes_both_sections(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  /* TwinGetResponse { 1: desired_version=7, 2: reported_version=9,
   *                   3: desired_payload, 4: reported_payload } */
  const uint8_t body[] = {
    0x08, 0x07, 0x10, 0x09, 0x1A, 0x07, '{', '"', 'd', '"', ':',
    '1',  '}',  0x22, 0x07, '{',  '"',  'r', '"', ':', '2', '}',
  };
  inject_twin(fx, "get-response:1", corr, body, sizeof(body));

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_true(rec.had_twin);
  assert_int_equal(rec.desired.version, 7);
  assert_int_equal(rec.reported.version, 9);
  assert_string_equal(rec.desired.payload, "{\"d\":1}");
  assert_string_equal(rec.reported.payload, "{\"r\":2}");
}

/* A section the service omitted comes back with its version but a NULL payload:
 * that is how an if-not-match hit is reported, and it must be distinguishable
 * from an empty payload. */
static void a_get_response_may_omit_a_section_payload(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  /* TwinGetResponse { 1: desired_version=4, 2: reported_version=5 } */
  const uint8_t body[] = { 0x08, 0x04, 0x10, 0x05 };
  inject_twin(fx, "get-response:1", corr, body, sizeof(body));

  assert_true(rec.fired);
  assert_int_equal(rec.desired.version, 4);
  assert_int_equal(rec.reported.version, 5);
  assert_false(rec.desired.has_payload);
  assert_false(rec.reported.has_payload);
}

/* A field the SDK does not know must be skipped, not abandoned, or a later
 * service-side schema addition would hide every field behind it. */
static void a_get_response_skips_unknown_fields(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  const uint8_t body[] = {
    0x08, 0x07, /* f1 desired_version = 7                */
    0x2A, 0x03, 'a', 'b', 'c', /* f5 length-delimited, unknown           */
    0x35, 1,    2,   3,   4, /* f6 32-bit, unknown                     */
    0x10, 0x09, /* f2 reported_version = 9                */
  };
  inject_twin(fx, "get-response:1", corr, body, sizeof(body));

  assert_true(rec.fired);
  assert_int_equal(rec.desired.version, 7);
  assert_int_equal(rec.reported.version, 9);
}

/* A response whose correlation data belongs to no pending request is dropped. */
static void a_response_with_unknown_correlation_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  uint8_t wrong[16];
  memset(wrong, 0xAB, sizeof(wrong));
  const uint8_t body[] = { 0x08, 0x07 };
  inject_twin(fx, "get-response:1", wrong, body, sizeof(body));

  assert_false(rec.fired);
}

/* Correlation data is the only thing tying a response to its request, so a
 * message without it cannot be attributed and is dropped. */
static void a_response_without_correlation_data_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  const uint8_t body[] = { 0x08, 0x07 };
  inject_twin(fx, "get-response:1", NULL, body, sizeof(body));

  assert_false(rec.fired);
}

/* Correlation data of the wrong width is not one of ours, however its bytes
 * begin. */
static void correlation_data_of_the_wrong_length_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  /* The right bytes, one short. */
  inject_twin_on(fx, TWIN_DEV_TOPIC, "get-response:1", corr, 15u, NULL, 0);

  assert_false(rec.fired);
}

/* A message correlating to a pending request that cannot be read as its answer
 * -- no type, an unknown type, or the other request kind -- must release the
 * slot rather than strand it. The service sends one answer per request, so
 * waiting for a better one would hold the slot until the session ends, and
 * AZ_IOT_TWIN_MAX_PENDING of these would wedge the client. */
static void a_response_with_an_unusable_type_releases_the_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  inject_twin(fx, "who-knows:1", corr, NULL, 0);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_PROTOCOL);
  assert_false(rec.had_twin);
}

/* A `type` value shorter than the names it is compared against must not be
 * read past its terminator. strncmp stops at the first difference, so the
 * one-past check only runs when the whole prefix matched -- but the value comes
 * from the broker, so the boundary is pinned here and the suite runs under
 * ASan in CI. */
static void a_short_type_value_is_handled(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  inject_twin(fx, "g", corr, NULL, 0);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_PROTOCOL);
}

/* The schema version is pinned, not skipped. A future "get-response:2" is a
 * message this SDK does not know how to read; accepting it as v1 would complete
 * a live request from fields that may have been redefined. */
static void a_response_with_an_unsupported_schema_version_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  const uint8_t body[] = { 0x08, 0x07, 0x10, 0x09 };
  inject_twin(fx, "get-response:2", corr, body, sizeof(body));

  /* Not decoded as v1; the slot is released as a protocol error instead. */
  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_PROTOCOL);
  assert_false(rec.had_twin);
}

/* A section the service sent as explicitly empty is an update to empty, and
 * must stay distinguishable from one it omitted for an if-not-match hit. */
static void an_explicitly_empty_section_payload_is_present(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  /* TwinGetResponse { 1: desired_version=4, 2: reported_version=5,
   *                   3: desired_payload = "" }  -- field 3 present, empty. */
  const uint8_t body[] = { 0x08, 0x04, 0x10, 0x05, 0x1A, 0x00 };
  inject_twin(fx, "get-response:1", corr, body, sizeof(body));

  assert_true(rec.fired);
  assert_true(rec.desired.has_payload); /* present */
  assert_int_equal(rec.desired.payload_len, 0); /* and empty */
  assert_false(rec.reported.has_payload); /* omitted entirely */
}

static void a_response_without_a_type_releases_the_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  inject_twin(fx, NULL, corr, NULL, 0);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_PROTOCOL);
}

/* A patch response quoting a GET's correlation id answers no question the slot
 * asked, so it releases it as a protocol error rather than reporting a patch
 * verdict to a GET caller. */
static void a_patch_response_does_not_satisfy_a_get_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  const uint8_t body[] = { 0x08, 0x01 };
  inject_twin(fx, "reported-patch-response:1", corr, body, sizeof(body));

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_PROTOCOL);
}

/* Dispatch matches by prefix, so a message published below the twin topic is
 * delivered here; nothing lives below it in the protocol, so it is dropped. */
static void a_message_on_a_longer_topic_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  const uint8_t body[] = { 0x08, 0x07 };
  inject_twin_on(fx, TWIN_DEV_TOPIC "/extra", "get-response:1", corr, 16u, body, sizeof(body));

  assert_false(rec.fired);
}

static void get_rejects_a_null_client(void** state)
{
  (void)state;
  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(NULL, on_get, &rec), AZ_IOT_ERR_INVALID_ARG);
}

/* Before the connection resolves there is no device id, so the topic cannot be
 * built and the request is refused rather than published somewhere wrong. */
static void get_before_the_device_id_is_assigned_is_refused(void** state)
{
  (void)state;

  az_iot_connection_client_options opts = { 0 };
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  az_iot_connection_client conn;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_gen2_twin_client twin;
  assert_int_equal(az_iot_gen2_twin_client_init(&twin, &conn), AZ_IOT_OK);

  get_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_get(&twin, on_get, &rec), AZ_IOT_ERR_NOT_CONNECTED);

  az_iot_gen2_twin_client_deinit(&twin);
  az_iot_connection_client_destroy(&conn);
}

static void get_with_a_full_pending_table_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record recs[AZ_IOT_TWIN_MAX_PENDING];
  memset(recs, 0, sizeof(recs));
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &recs[i]), AZ_IOT_OK);
  }

  get_record overflow = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_get(&fx->twin, on_get, &overflow), AZ_IOT_ERR_NOT_SUPPORTED);
}

/* A refused publish must not leave its slot claimed, or the table would fill up
 * with requests that were never sent. */
static void a_publish_failure_releases_the_pending_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING + 2; ++i)
  {
    /* The override is one-shot, so it is re-armed for each attempt. */
    az_iot_mock_mqtt_client_set_next_result(
        fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_NOT_CONNECTED);
    assert_int_equal(
        az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_ERR_NOT_CONNECTED);
  }

  /* Every slot must still be free: the failures released them. */
  assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* reported patch                                                            */
/* ------------------------------------------------------------------------- */

/* The patch is wrapped in ReportedPatch{if_match, payload}, with if_match
 * seeded from the birth-ack so optimistic concurrency works on the first write
 * of a connection without the application tracking anything. */
static void patch_frames_if_match_from_the_birth_ack(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_with_versions(fx, 7, 9);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  assert_non_null(pub);
  assert_int_equal(pub->qos, AZ_IOT_MQTT_QOS_0);
  assert_string_equal(pub->user_type, "reported-patch:1");
  assert_string_equal(pub->content_type, "application/protobuf");
  assert_int_equal(pub->correlation_data_len, 16);

  /* ReportedPatch { 1: if_match = 9 (the reported version), 2: payload }. */
  const uint8_t expect[] = {
    0x08, 0x09, 0x12, 0x07, '{', '"', 'x', '"', ':', '1', '}',
  };
  assert_int_equal(pub->payload_len, sizeof(expect));
  assert_memory_equal(pub->payload, expect, sizeof(expect));
}

/* With no authoritative version yet, if_match is 0 and proto3 omits it, so the
 * body is the payload field alone. */
static void patch_without_a_known_version_omits_if_match(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  assert_non_null(pub);
  const uint8_t expect[] = { 0x12, 0x07, '{', '"', 'x', '"', ':', '1', '}' };
  assert_int_equal(pub->payload_len, sizeof(expect));
  assert_memory_equal(pub->payload, expect, sizeof(expect));
}

/* An accepted patch reports the verdict and the new authoritative version. */
static void a_patch_response_reports_the_verdict(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_with_versions(fx, 7, 9);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);
  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  uint8_t corr[16];
  memcpy(corr, pub->correlation_data, sizeof(corr));

  /* ReportedPatchResponse { 1: result = OK, 2: version = 10 } */
  const uint8_t body[] = { 0x08, 0x01, 0x10, 0x0A };
  inject_twin(fx, "reported-patch-response:1", corr, body, sizeof(body));

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_true(rec.had_result);
  assert_int_equal(rec.verdict, AZ_IOT_GEN2_TWIN_PATCH_OK);
  assert_int_equal(rec.version, 10);
}

/* A rejected write is not a failed exchange: the status is OK and the verdict
 * carries the reason. It used to be indistinguishable from success. The client
 * adopts the version the service returned, so the retry is correct without the
 * application tracking versions itself. */
static void a_version_mismatch_reaches_the_caller_and_corrects_the_retry(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_with_versions(fx, 7, 9);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);
  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  uint8_t corr[16];
  memcpy(corr, pub->correlation_data, sizeof(corr));

  /* ReportedPatchResponse { 1: result = VERSION_MISMATCH, 2: version = 12 } */
  const uint8_t body[] = { 0x08, 0x02, 0x10, 0x0C };
  inject_twin(fx, "reported-patch-response:1", corr, body, sizeof(body));

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
  assert_int_equal(rec.verdict, AZ_IOT_GEN2_TWIN_PATCH_VERSION_MISMATCH);
  assert_int_equal(rec.version, 12);

  /* The retry carries the corrected if_match. */
  patch_record retry = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &retry),
      AZ_IOT_OK);
  const az_iot_mock_call* second = find_publish(fx->mock, TWIN_SRV_TOPIC);
  assert_non_null(second);
  const uint8_t expect[] = {
    0x08, 0x0C, 0x12, 0x07, '{', '"', 'x', '"', ':', '1', '}',
  };
  assert_int_equal(second->payload_len, sizeof(expect));
  assert_memory_equal(second->payload, expect, sizeof(expect));
}

/* The SDK does not allocate, so without a buffer to frame the patch into there
 * is nothing to publish. Refused up front rather than sending an unframed body
 * the service would reject. */
static void patch_without_an_encode_buffer_reports_no_space(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  assert_int_equal(az_iot_gen2_twin_client_set_encode_buffer(&fx->twin, NULL, 0), AZ_IOT_OK);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(count_twin_publishes(fx->mock), 0);
}

/* A patch too large for the buffer is refused, and must not leave a slot
 * claimed for a request that never went out. */
static void a_patch_larger_than_the_encode_buffer_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  uint8_t small[AZ_IOT_GEN2_TWIN_ENCODE_OVERHEAD];
  assert_int_equal(
      az_iot_gen2_twin_client_set_encode_buffer(&fx->twin, small, sizeof(small)), AZ_IOT_OK);

  uint8_t big[AZ_IOT_GEN2_TWIN_ENCODE_OVERHEAD * 2];
  memset(big, 'x', sizeof(big));
  patch_record rec = { 0 };
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING + 2; ++i)
  {
    assert_int_equal(
        az_iot_gen2_twin_client_patch_reported(&fx->twin, big, sizeof(big), on_patch, &rec),
        AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  }

  /* The slots were released, so a patch that does fit still goes out. */
  assert_int_equal(
      az_iot_gen2_twin_client_set_encode_buffer(
          &fx->twin, fx->encode_buffer, sizeof(fx->encode_buffer)),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, big, sizeof(big), on_patch, &rec),
      AZ_IOT_OK);
}

/* The documented size is "your largest patch plus the overhead", so a buffer of
 * exactly the overhead is the valid minimum: it frames a zero-length patch. */
static void set_encode_buffer_accepts_exactly_the_documented_overhead(void** state)
{
  fixture* fx = (fixture*)*state;

  uint8_t buf[AZ_IOT_GEN2_TWIN_ENCODE_OVERHEAD];
  assert_int_equal(
      az_iot_gen2_twin_client_set_encode_buffer(&fx->twin, buf, sizeof(buf)), AZ_IOT_OK);
  assert_int_equal(
      az_iot_gen2_twin_client_set_encode_buffer(&fx->twin, buf, sizeof(buf) - 1),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void set_encode_buffer_rejects_a_null_client(void** state)
{
  (void)state;
  uint8_t buf[64];
  assert_int_equal(
      az_iot_gen2_twin_client_set_encode_buffer(NULL, buf, sizeof(buf)), AZ_IOT_ERR_INVALID_ARG);
}

static void patch_rejects_a_null_client(void** state)
{
  (void)state;
  static const uint8_t patch[] = "{}";
  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(NULL, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_ERR_INVALID_ARG);
}

static void patch_rejects_a_null_patch_with_a_length(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, NULL, 4, on_patch, &rec),
      AZ_IOT_ERR_INVALID_ARG);
}

/* An empty patch is legal: proto3 still emits the payload field, so the service
 * sees an explicitly-present empty patch rather than an absent one. */
static void an_empty_patch_is_publishable(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, NULL, 0, on_patch, &rec), AZ_IOT_OK);

  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  assert_non_null(pub);
  const uint8_t expect[] = { 0x12, 0x00 };
  assert_int_equal(pub->payload_len, sizeof(expect));
  assert_memory_equal(pub->payload, expect, sizeof(expect));
}

/* ------------------------------------------------------------------------- */
/* twin-push                                                                 */
/* ------------------------------------------------------------------------- */

/* A twin-push is backend-initiated: it carries the connection's birth nonce
 * rather than a request id, and delivers both sections. */
static void a_twin_push_reaches_the_push_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  push_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_set_push_callback(&fx->twin, on_push, &rec), AZ_IOT_OK);

  /* TwinPush { 1: Section{1: version=3, 2: payload}, 2: Section{1: version=5} } */
  const uint8_t body[] = {
    0x0A, 0x0B, 0x08, 0x03, 0x12, 0x07, '{', '"', 'd', '"', ':', '1', '}', 0x12, 0x02, 0x08, 0x05,
  };
  inject_twin(fx, "twin-push:1", fx->nonce, body, sizeof(body));

  assert_true(rec.fired);
  assert_int_equal(rec.desired.version, 3);
  assert_string_equal(rec.desired.payload, "{\"d\":1}");
  assert_int_equal(rec.reported.version, 5);
  assert_false(rec.reported.has_payload);
}

/* The nonce is what distinguishes traffic for this connection from traffic the
 * service dispatched on one that has since been replaced. A push that does not
 * carry it is not ours, and applying it would overwrite current state with
 * stale state. */
static void a_twin_push_from_another_connection_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  push_record rec = { 0 };
  assert_int_equal(az_iot_gen2_twin_client_set_push_callback(&fx->twin, on_push, &rec), AZ_IOT_OK);

  uint8_t stale[16];
  memset(stale, 0x5A, sizeof(stale));
  const uint8_t body[] = { 0x0A, 0x02, 0x08, 0x03 };
  inject_twin(fx, "twin-push:1", stale, body, sizeof(body));

  assert_false(rec.fired);
}

/* A push carries authoritative state, so it advances the version the next
 * reported patch anchors on -- whether or not anyone is listening for it. */
static void a_twin_push_advances_the_reported_version(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_with_versions(fx, 7, 9);

  /* TwinPush { 2: Section{1: version=21} } */
  const uint8_t body[] = { 0x12, 0x02, 0x08, 0x15 };
  inject_twin(fx, "twin-push:1", fx->nonce, body, sizeof(body));

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  assert_non_null(pub);
  const uint8_t expect[] = {
    0x08, 0x15, 0x12, 0x07, '{', '"', 'x', '"', ':', '1', '}',
  };
  assert_int_equal(pub->payload_len, sizeof(expect));
  assert_memory_equal(pub->payload, expect, sizeof(expect));
}

/* ------------------------------------------------------------------------- */
/* desired patches                                                           */
/* ------------------------------------------------------------------------- */

/* A desired-patch carries its version in the body, and reaches the handler with
 * it. */
static void a_desired_patch_reaches_the_handler_with_its_version(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* DesiredPatch { 1: version=4, 2: payload } */
  const uint8_t body[] = { 0x08, 0x04, 0x12, 0x07, '{', '"', 'd', '"', ':', '1', '}' };
  inject_twin(fx, "desired-patch:1", fx->nonce, body, sizeof(body));

  assert_true(rec.fired);
  assert_int_equal(rec.version, 4);
  assert_string_equal(rec.payload, "{\"d\":1}");
}

/* A payload-less desired-patch is a version probe, not an update: it tells the
 * device which version it should be at. Waking the application for it would
 * hand it an empty patch to merge. */
static void a_payload_less_desired_patch_is_not_dispatched(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* DesiredPatch { 1: version=4 } */
  const uint8_t body[] = { 0x08, 0x04 };
  inject_twin(fx, "desired-patch:1", fx->nonce, body, sizeof(body));

  assert_false(rec.fired);
}

/* An explicitly-present but empty payload IS an update, and must reach the
 * handler -- proto3 distinguishes it from an absent one, and so must this. */
static void an_empty_desired_payload_is_still_dispatched(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  /* DesiredPatch { 1: version=4, 2: payload = "" } */
  const uint8_t body[] = { 0x08, 0x04, 0x12, 0x00 };
  inject_twin(fx, "desired-patch:1", fx->nonce, body, sizeof(body));

  assert_true(rec.fired);
  assert_int_equal(rec.version, 4);
}

static void a_desired_patch_from_another_connection_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);

  uint8_t stale[16];
  memset(stale, 0x5A, sizeof(stale));
  const uint8_t body[] = { 0x08, 0x04, 0x12, 0x02, '{', '}' };
  inject_twin(fx, "desired-patch:1", stale, body, sizeof(body));

  assert_false(rec.fired);
}

static void set_desired_handler_rejects_a_null_client(void** state)
{
  (void)state;
  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(NULL, on_desired, &rec), AZ_IOT_ERR_INVALID_ARG);
}

static void set_push_callback_rejects_a_null_client(void** state)
{
  (void)state;
  push_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_push_callback(NULL, on_push, &rec), AZ_IOT_ERR_INVALID_ARG);
}

/* A NULL handler pauses delivery; the message is still consumed. */
static void a_null_desired_handler_pauses_delivery(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record rec = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &rec), AZ_IOT_OK);
  assert_int_equal(az_iot_gen2_twin_client_set_desired_handler(&fx->twin, NULL, NULL), AZ_IOT_OK);

  const uint8_t body[] = { 0x08, 0x04, 0x12, 0x02, '{', '}' };
  inject_twin(fx, "desired-patch:1", fx->nonce, body, sizeof(body));

  assert_false(rec.fired);
}

static void setting_a_desired_handler_replaces_the_previous_one(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record first = { 0 };
  desired_record second = { 0 };
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &first), AZ_IOT_OK);
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired_other, &second), AZ_IOT_OK);

  const uint8_t body[] = { 0x08, 0x04, 0x12, 0x02, '{', '}' };
  inject_twin(fx, "desired-patch:1", fx->nonce, body, sizeof(body));

  assert_false(first.fired);
  assert_true(second.fired);
}

/* ------------------------------------------------------------------------- */
/* versions across connections                                               */
/* ------------------------------------------------------------------------- */

/* Both exchanges are in flight at once and each completes on its own
 * correlation id. */
static void concurrent_get_and_patch_correlate_independently(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_with_versions(fx, 7, 9);

  get_record get_rec = { 0 };
  uint8_t get_corr[16];
  issue_get(fx, &get_rec, get_corr);

  patch_record patch_rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(
          &fx->twin, patch, sizeof(patch) - 1, on_patch, &patch_rec),
      AZ_IOT_OK);
  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  uint8_t patch_corr[16];
  memcpy(patch_corr, pub->correlation_data, sizeof(patch_corr));
  assert_memory_not_equal(get_corr, patch_corr, sizeof(get_corr));

  const uint8_t patch_body[] = { 0x08, 0x01, 0x10, 0x0A };
  inject_twin(fx, "reported-patch-response:1", patch_corr, patch_body, sizeof(patch_body));
  assert_true(patch_rec.fired);
  assert_false(get_rec.fired);

  const uint8_t get_body[] = { 0x08, 0x07, 0x10, 0x0A };
  inject_twin(fx, "get-response:1", get_corr, get_body, sizeof(get_body));
  assert_true(get_rec.fired);
  assert_int_equal(get_rec.desired.version, 7);
}

/* The versions belong to the connection they were admitted on. A reconnect
 * brings a new birth-ack, and the client must anchor on that rather than on
 * what it learned from a session that has ended. */
static void a_reconnect_reseeds_the_versions_from_the_new_birth_ack(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_with_versions(fx, 7, 9);

  /* Move the tracked version away from the birth-ack value. */
  const uint8_t push[] = { 0x12, 0x02, 0x08, 0x15 };
  inject_twin(fx, "twin-push:1", fx->nonce, push, sizeof(push));

  /* A second connection admits the birth with a different reported version. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
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
  memcpy(fx->nonce, birth->correlation_data, sizeof(fx->nonce));

  const uint8_t ack_body[] = { 0x50, 0x02, 0x58, 0x03 }; /* desired=2, reported=3 */
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/ut-device/dev/presence";
  ack_msg.correlation_data = fx->nonce;
  ack_msg.correlation_data_len = sizeof(fx->nonce);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  ack_msg.payload = ack_body;
  ack_msg.payload_len = sizeof(ack_body);
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &ack));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);

  /* The patch must anchor on 3, the new connection's authoritative version --
   * not 21, which the previous session's push had established. */
  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);
  const az_iot_mock_call* pub = find_publish(fx->mock, TWIN_SRV_TOPIC);
  assert_non_null(pub);
  const uint8_t expect[] = {
    0x08, 0x03, 0x12, 0x07, '{', '"', 'x', '"', ':', '1', '}',
  };
  assert_int_equal(pub->payload_len, sizeof(expect));
  assert_memory_equal(pub->payload, expect, sizeof(expect));
}

/* ------------------------------------------------------------------------- */
/* session end                                                               */
/* ------------------------------------------------------------------------- */

static void a_pending_get_is_failed_when_the_session_drops(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record rec = { 0 };
  uint8_t corr[16];
  issue_get(fx, &rec, corr);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_NOT_CONNECTED);
  assert_false(rec.had_twin);
}

static void every_pending_request_is_failed_not_just_the_first(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  get_record recs[AZ_IOT_TWIN_MAX_PENDING];
  memset(recs, 0, sizeof(recs));
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_int_equal(az_iot_gen2_twin_client_get(&fx->twin, on_get, &recs[i]), AZ_IOT_OK);
  }

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    assert_true(recs[i].fired);
    assert_int_equal(recs[i].status, AZ_IOT_ERR_NOT_CONNECTED);
  }
}

/* A pending patch is failed too, and reports no verdict: the exchange never
 * completed, so there is nothing the service decided. */
static void a_pending_patch_is_failed_with_no_verdict(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  patch_record rec = { 0 };
  static const uint8_t patch[] = "{\"x\":1}";
  assert_int_equal(
      az_iot_gen2_twin_client_patch_reported(&fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
      AZ_IOT_OK);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_NOT_CONNECTED);
  assert_false(rec.had_result);
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
  az_iot_gen2_twin_client_deinit(NULL);
}

static void destroy_zeroes_the_client(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_gen2_twin_client_deinit(&fx->twin);

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

  az_iot_gen2_twin_client_deinit(&fx->twin);
  /* The second call runs against a zeroed struct and must not follow the
   * now-NULL connection pointer into unregister. */
  az_iot_gen2_twin_client_deinit(&fx->twin);

  assert_int_equal(az_iot_gen2_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

static void destroy_unregisters_the_inbound_handler(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  desired_record des = { 0 };
  get_record get = { 0 };
  uint8_t corr[16];
  assert_int_equal(
      az_iot_gen2_twin_client_set_desired_handler(&fx->twin, on_desired, &des), AZ_IOT_OK);
  issue_get(fx, &get, corr);

  az_iot_gen2_twin_client_deinit(&fx->twin);

  /* The handler must be gone: left behind, it would dispatch into a zeroed
   * client. */
  const uint8_t body[] = { 0x08, 0x04, 0x12, 0x02, '{', '}' };
  inject_twin(fx, "desired-patch:1", fx->nonce, body, sizeof(body));
  inject_twin(fx, "get-response:1", corr, body, sizeof(body));
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
    az_iot_gen2_twin_client_deinit(&twins[i]);
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
  az_iot_gen2_twin_client_deinit(&fx->twin);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_false(rec.fired);

  assert_int_equal(az_iot_gen2_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_subscribes_no_per_feature_twin_filter, setup, teardown),
    cmocka_unit_test_setup_teardown(get_publishes_a_conformant_request, setup, teardown),
    cmocka_unit_test_setup_teardown(
        each_request_carries_a_distinct_correlation_id, setup, teardown),
    cmocka_unit_test_setup_teardown(get_response_decodes_both_sections, setup, teardown),
    cmocka_unit_test_setup_teardown(a_get_response_may_omit_a_section_payload, setup, teardown),
    cmocka_unit_test_setup_teardown(a_get_response_skips_unknown_fields, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_with_unknown_correlation_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_without_correlation_data_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        correlation_data_of_the_wrong_length_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_with_an_unusable_type_releases_the_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(a_short_type_value_is_handled, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_with_an_unsupported_schema_version_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_explicitly_empty_section_payload_is_present, setup, teardown),
    cmocka_unit_test_setup_teardown(a_response_without_a_type_releases_the_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(a_patch_response_does_not_satisfy_a_get_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(a_message_on_a_longer_topic_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(get_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(
        get_before_the_device_id_is_assigned_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(get_with_a_full_pending_table_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(a_publish_failure_releases_the_pending_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(patch_frames_if_match_from_the_birth_ack, setup, teardown),
    cmocka_unit_test_setup_teardown(patch_without_a_known_version_omits_if_match, setup, teardown),
    cmocka_unit_test_setup_teardown(a_patch_response_reports_the_verdict, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_version_mismatch_reaches_the_caller_and_corrects_the_retry, setup, teardown),
    cmocka_unit_test_setup_teardown(
        patch_without_an_encode_buffer_reports_no_space, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_patch_larger_than_the_encode_buffer_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        set_encode_buffer_accepts_exactly_the_documented_overhead, setup, teardown),
    cmocka_unit_test_setup_teardown(set_encode_buffer_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(patch_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(patch_rejects_a_null_patch_with_a_length, setup, teardown),
    cmocka_unit_test_setup_teardown(an_empty_patch_is_publishable, setup, teardown),
    cmocka_unit_test_setup_teardown(a_twin_push_reaches_the_push_callback, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_twin_push_from_another_connection_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_twin_push_advances_the_reported_version, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_desired_patch_reaches_the_handler_with_its_version, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_payload_less_desired_patch_is_not_dispatched, setup, teardown),
    cmocka_unit_test_setup_teardown(an_empty_desired_payload_is_still_dispatched, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_desired_patch_from_another_connection_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(set_desired_handler_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(set_push_callback_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(a_null_desired_handler_pauses_delivery, setup, teardown),
    cmocka_unit_test_setup_teardown(
        setting_a_desired_handler_replaces_the_previous_one, setup, teardown),
    cmocka_unit_test_setup_teardown(
        concurrent_get_and_patch_correlate_independently, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_reconnect_reseeds_the_versions_from_the_new_birth_ack, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_pending_get_is_failed_when_the_session_drops, setup, teardown),
    cmocka_unit_test_setup_teardown(
        every_pending_request_is_failed_not_just_the_first, setup, teardown),
    cmocka_unit_test_setup_teardown(a_pending_patch_is_failed_with_no_verdict, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test_setup_teardown(init_rejects_a_null_connection, setup, teardown),
    cmocka_unit_test_setup_teardown(init_against_a_classic_connection_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_tolerates_null, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_zeroes_the_client, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_unregisters_the_inbound_handler, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_frees_the_connect_time_bind_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_destroyed_twin_client_is_not_called_on_a_later_session_end, setup, teardown),
  };
  return cmocka_run_group_tests_name("gen2_twin_client", tests, NULL, NULL);
}
