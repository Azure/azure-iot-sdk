// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Contract tests for az_iot_mqtt_iface, exercised through the in-memory mock.
 * These tests only depend on the public iface header + the mock; no real broker. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_mqtt_iface.h"

#include "../support/mock_mqtt_iface.h"

typedef struct collected_events
{
  size_t count;
  az_iot_mqtt_event_kind kinds[8];
  az_iot_result statuses[8];
  char topics[8][AZ_IOT_MOCK_TOPIC_MAX];
  uint8_t payloads[8][AZ_IOT_MOCK_PAYLOAD_MAX];
  size_t payload_lens[8];
} collected_events;

static void on_event(const az_iot_mqtt_event* evt, void* ctx)
{
  collected_events* col = (collected_events*)ctx;
  if (col->count >= 8)
  {
    return;
  }
  size_t i = col->count++;
  col->kinds[i] = evt->kind;
  col->statuses[i] = evt->status;
  if (evt->message)
  {
    if (evt->message->topic)
    {
      size_t n = strlen(evt->message->topic);
      if (n >= AZ_IOT_MOCK_TOPIC_MAX)
      {
        n = AZ_IOT_MOCK_TOPIC_MAX - 1;
      }
      memcpy(col->topics[i], evt->message->topic, n);
      col->topics[i][n] = '\0';
    }
    size_t plen = evt->message->payload_len;
    if (plen > AZ_IOT_MOCK_PAYLOAD_MAX)
    {
      plen = AZ_IOT_MOCK_PAYLOAD_MAX;
    }
    if (plen)
    {
      memcpy(col->payloads[i], evt->message->payload, plen);
    }
    col->payload_lens[i] = plen;
  }
}

/* ------------------------------------------------------------------------- */

static void factory_advertises_version(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(f);
  assert_int_equal(f->version, AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_mock_mqtt_factory_destroy(f);
}

static void client_carries_iface_pointer_with_version(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  assert_non_null(c->iface);
  assert_int_equal(c->iface->version, AZ_IOT_MQTT_VERSION_5);
  /* Every vtable slot must be populated. */
  assert_non_null(c->iface->connect);
  assert_non_null(c->iface->disconnect);
  assert_non_null(c->iface->subscribe);
  assert_non_null(c->iface->unsubscribe);
  assert_non_null(c->iface->publish);
  assert_non_null(c->iface->process_loop);
  assert_non_null(c->iface->set_inbound_cb);
  assert_non_null(c->iface->destroy);
  az_iot_mock_mqtt_factory_destroy(f);
}

static void publish_records_topic_payload_and_assigns_packet_id(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);

  az_iot_mqtt_message msg = { 0 };
  msg.topic = "devices/dev1/messages/events/";
  static const uint8_t body[] = { 'h', 'i' };
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;

  uint16_t pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pid), AZ_IOT_OK);
  assert_int_not_equal(pid, 0);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_client_from(c);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(m), 1);
  const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(m, 0);
  assert_int_equal(call->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(call->topic, "devices/dev1/messages/events/");
  assert_int_equal(call->payload_len, sizeof(body));
  assert_memory_equal(call->payload, body, sizeof(body));
  assert_int_equal(call->qos, AZ_IOT_MQTT_QOS_1);
  assert_int_equal(call->packet_id, pid);

  az_iot_mock_mqtt_factory_destroy(f);
}

static void scripted_failure_propagates_to_caller(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_client_from(c);

  az_iot_mock_mqtt_client_set_next_result(m, AZ_IOT_MOCK_CALL_CONNECT, AZ_IOT_ERR_MQTT);
  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "example.invalid";
  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_ERR_MQTT);
  /* Override is one-shot; second call returns AZ_IOT_OK again. */
  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);

  az_iot_mock_mqtt_factory_destroy(f);
}

static void process_loop_drains_one_event_per_call(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_client_from(c);

  collected_events col = { 0 };
  c->iface->set_inbound_cb(c, on_event, &col);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  static const uint8_t body[] = { 'p', 'a', 'y' };
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m, "devices/dev1/methods/POST/reboot/?$rid=42", body, sizeof(body), AZ_IOT_MQTT_QOS_0));

  assert_int_equal(col.count, 0);
  assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
  assert_int_equal(col.count, 1);
  assert_int_equal(col.kinds[0], AZ_IOT_MQTT_EVT_CONNECTED);
  assert_int_equal(col.statuses[0], AZ_IOT_OK);

  assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
  assert_int_equal(col.count, 2);
  assert_int_equal(col.kinds[1], AZ_IOT_MQTT_EVT_MESSAGE);
  assert_string_equal(col.topics[1], "devices/dev1/methods/POST/reboot/?$rid=42");
  assert_int_equal(col.payload_lens[1], sizeof(body));
  assert_memory_equal(col.payloads[1], body, sizeof(body));

  /* No more events queued; process_loop is still safe to call. */
  assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
  assert_int_equal(col.count, 2);

  az_iot_mock_mqtt_factory_destroy(f);
}

static void destroy_via_iface_is_recorded(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  /* Calling destroy through the iface frees the client; the factory's
   * last_client back-pointer is cleared. */
  c->iface->destroy(c);
  assert_null(az_iot_mock_mqtt_factory_last_client(f));
  az_iot_mock_mqtt_factory_destroy(f);
}

/* ---- CONNACK code mapping -------------------------------------------------
 * The core re-provisions through DPS on AZ_IOT_ERR_IDENTITY_REJECTED and only
 * on that result, so which side of the identity/transport line each code falls
 * on is behaviour, not cosmetics. */

static void connack_success_maps_to_ok(void** state)
{
  (void)state;
  assert_int_equal(az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_3_1_1, 0), AZ_IOT_OK);
  assert_int_equal(az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_5, 0), AZ_IOT_OK);
}

static void connack_v3_identity_codes_map_to_identity_rejected(void** state)
{
  (void)state;
  /* 2 identifier rejected, 4 bad user name or password, 5 not authorized. */
  static const int codes[] = { 2, 4, 5 };
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
  {
    assert_int_equal(
        az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_3_1_1, codes[i]),
        AZ_IOT_ERR_IDENTITY_REJECTED);
  }
}

static void connack_v3_transport_codes_map_to_mqtt(void** state)
{
  (void)state;
  /* 1 unacceptable protocol version, 3 server unavailable. Neither is a
   * verdict on the device identity, so neither may trigger re-provisioning. */
  static const int codes[] = { 1, 3 };
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
  {
    assert_int_equal(
        az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_3_1_1, codes[i]), AZ_IOT_ERR_MQTT);
  }
}

static void connack_v5_identity_codes_map_to_identity_rejected(void** state)
{
  (void)state;
  /* 0x85 client identifier not valid, 0x86 bad user name or password,
   * 0x87 not authorized, 0x8C bad authentication method. */
  static const int codes[] = { 0x85, 0x86, 0x87, 0x8C };
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
  {
    assert_int_equal(
        az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_5, codes[i]), AZ_IOT_ERR_IDENTITY_REJECTED);
  }
}

static void connack_v5_transport_codes_map_to_mqtt(void** state)
{
  (void)state;
  /* 0x80 unspecified, 0x88 server unavailable, 0x89 server busy,
   * 0x97 quota exceeded -- all retryable against the same identity. */
  static const int codes[] = { 0x80, 0x88, 0x89, 0x97 };
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
  {
    assert_int_equal(az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_5, codes[i]), AZ_IOT_ERR_MQTT);
  }
}

static void connack_negative_codes_map_to_mqtt(void** state)
{
  (void)state;
  /* Adapters report their own failures (socket refused, TLS handshake) with
   * negative codes. Those never reached a broker, so they say nothing about
   * the identity no matter which MQTT version is in play. */
  assert_int_equal(az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_3_1_1, -1), AZ_IOT_ERR_MQTT);
  assert_int_equal(az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_5, -1), AZ_IOT_ERR_MQTT);
}

/* An unrecognized version must not be interpreted as either scheme. The codes
 * overlap numerically -- 2, 4 and 5 are identity refusals in v3.1.1 and mean
 * something else in v5 -- so picking a scheme would be picking whether to
 * re-provision. The values below are exactly the ones that WOULD map to
 * IDENTITY_REJECTED if an unknown version silently fell through to v3.1.1. */
static void connack_unknown_version_never_rejects_the_identity(void** state)
{
  (void)state;
  const az_iot_mqtt_version bogus = (az_iot_mqtt_version)99;
  /* Exactly the v3.1.1 identity codes -- 2, 4, 5 -- which is what an unknown
   * version would have been scored against had it fallen through to v3.1.1. */
  static const int codes[] = { 2, 4, 5 };
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
  {
    assert_int_equal(az_iot_mqtt_connack_result(bogus, codes[i]), AZ_IOT_ERR_MQTT);
  }

  /* A success code is still success: it carries no scheme-specific meaning. */
  assert_int_equal(az_iot_mqtt_connack_result(bogus, 0), AZ_IOT_OK);
}

static void mapped_connack_status_reaches_the_inbound_callback(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_client_from(c);

  collected_events col = { 0 };
  c->iface->set_inbound_cb(c, on_event, &col);

  /* Stand in for an adapter that saw CONNACK 0x87 and mapped it before
   * reporting: the discriminated status must survive the event plumbing
   * intact, since that is the only thing the core gets to look at. */
  assert_true(az_iot_mock_mqtt_client_inject_connected(
      m, az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_5, 0x87)));
  assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);

  assert_int_equal(col.count, 1);
  assert_int_equal(col.kinds[0], AZ_IOT_MQTT_EVT_CONNECTED);
  assert_int_equal(col.statuses[0], AZ_IOT_ERR_IDENTITY_REJECTED);

  az_iot_mock_mqtt_factory_destroy(f);
}

/* ---- SUBACK code mapping ---------------------------------------------------
 * The gate that withholds CONNECTED until every persistent filter is SUBACKed
 * acts on this split: a refusal a retry cannot change fails the session, while
 * a transient one reconnects. Flattening the two -- which is what the adapters
 * did before this mapper existed -- turns a permanently refused filter into a
 * reconnect loop with no exit. */

static void suback_granted_qos_maps_to_ok(void** state)
{
  (void)state;
  /* A grant BELOW the QoS requested is still a grant: the subscription exists
   * and delivery is min(publish QoS, granted QoS). Reading it as a refusal
   * would fail a session no broker objected to. */
  for (int code = 0; code <= 2; ++code)
  {
    assert_int_equal(az_iot_mqtt_suback_result(AZ_IOT_MQTT_VERSION_3_1_1, code), AZ_IOT_OK);
    assert_int_equal(az_iot_mqtt_suback_result(AZ_IOT_MQTT_VERSION_5, code), AZ_IOT_OK);
  }
}

static void suback_v3_failure_maps_to_subscription_refused(void** state)
{
  (void)state;
  /* 0x80 Failure is the only refusal MQTT 3.1.1 can express and it carries no
   * reason, so the classification comes from what a Classic device can ask for:
   * a topic set fixed at compile time, which cannot become acceptable later. */
  assert_int_equal(
      az_iot_mqtt_suback_result(AZ_IOT_MQTT_VERSION_3_1_1, 0x80),
      AZ_IOT_ERR_SUBSCRIPTION_REFUSED);
}

static void suback_v5_permanent_codes_map_to_subscription_refused(void** state)
{
  (void)state;
  /* 0x87 not authorized, 0x8F topic filter invalid, 0x9E shared subscriptions
   * not supported, 0xA1 subscription identifiers not supported, 0xA2 wildcard
   * subscriptions not supported. Re-issuing any of these is refused again. */
  static const int codes[] = { 0x87, 0x8F, 0x9E, 0xA1, 0xA2 };
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
  {
    assert_int_equal(
        az_iot_mqtt_suback_result(AZ_IOT_MQTT_VERSION_5, codes[i]),
        AZ_IOT_ERR_SUBSCRIPTION_REFUSED);
  }
}

static void suback_v5_transient_codes_map_to_mqtt(void** state)
{
  (void)state;
  /* 0x80 unspecified, 0x83 implementation specific, 0x91 packet identifier in
   * use, 0x97 quota exceeded -- how a service-side fault presents, and
   * re-subscribing is the right answer to every one of them. */
  static const int codes[] = { 0x80, 0x83, 0x91, 0x97 };
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
  {
    assert_int_equal(az_iot_mqtt_suback_result(AZ_IOT_MQTT_VERSION_5, codes[i]), AZ_IOT_ERR_MQTT);
  }
}

static void suback_negative_codes_map_to_mqtt(void** state)
{
  (void)state;
  /* Adapter-internal failures never reached a broker, so they carry no verdict
   * about the filter and must not fail the session. */
  assert_int_equal(az_iot_mqtt_suback_result(AZ_IOT_MQTT_VERSION_3_1_1, -1), AZ_IOT_ERR_MQTT);
  assert_int_equal(az_iot_mqtt_suback_result(AZ_IOT_MQTT_VERSION_5, -1), AZ_IOT_ERR_MQTT);
}

static void suback_unknown_version_never_fails_the_session(void** state)
{
  (void)state;
  const az_iot_mqtt_version bogus = (az_iot_mqtt_version)99;
  /* 0x80 is a permanent refusal in v3.1.1 and a transient one in v5, so an
   * unknown version cannot score it either way. Retrying is the safe half. */
  assert_int_equal(az_iot_mqtt_suback_result(bogus, 0x80), AZ_IOT_ERR_MQTT);
  assert_int_equal(az_iot_mqtt_suback_result(bogus, 0x87), AZ_IOT_ERR_MQTT);
  /* A grant carries no scheme-specific meaning. */
  assert_int_equal(az_iot_mqtt_suback_result(bogus, 1), AZ_IOT_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(factory_advertises_version),
    cmocka_unit_test(client_carries_iface_pointer_with_version),
    cmocka_unit_test(publish_records_topic_payload_and_assigns_packet_id),
    cmocka_unit_test(scripted_failure_propagates_to_caller),
    cmocka_unit_test(process_loop_drains_one_event_per_call),
    cmocka_unit_test(destroy_via_iface_is_recorded),
    cmocka_unit_test(connack_success_maps_to_ok),
    cmocka_unit_test(connack_v3_identity_codes_map_to_identity_rejected),
    cmocka_unit_test(connack_v3_transport_codes_map_to_mqtt),
    cmocka_unit_test(connack_v5_identity_codes_map_to_identity_rejected),
    cmocka_unit_test(connack_v5_transport_codes_map_to_mqtt),
    cmocka_unit_test(connack_negative_codes_map_to_mqtt),
    cmocka_unit_test(connack_unknown_version_never_rejects_the_identity),
    cmocka_unit_test(mapped_connack_status_reaches_the_inbound_callback),
    cmocka_unit_test(suback_granted_qos_maps_to_ok),
    cmocka_unit_test(suback_v3_failure_maps_to_subscription_refused),
    cmocka_unit_test(suback_v5_permanent_codes_map_to_subscription_refused),
    cmocka_unit_test(suback_v5_transient_codes_map_to_mqtt),
    cmocka_unit_test(suback_negative_codes_map_to_mqtt),
    cmocka_unit_test(suback_unknown_version_never_fails_the_session),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
