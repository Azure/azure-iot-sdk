// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Phase 3.1 - TelemetryClient unit tests, driven through the public API and
 * the in-memory mock_mqtt_iface. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_telemetry_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "support/mock_mqtt_iface.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct send_record
{
  bool fired;
  az_iot_result status;
} send_record;

static void on_send(az_iot_result status, void* user_ctx)
{
  send_record* r = (send_record*)user_ctx;
  r->fired = true;
  r->status = status;
}

typedef struct fixture
{
  az_iot_connection_client conn;
  az_iot_telemetry_client tc;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock; /* convenience alias */
  /* Set once the factory has been handed to the connection, which adopts it.
   * Tracked here rather than read back from the connection, which a test may
   * already have destroyed. */
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

  assert_int_equal(az_iot_telemetry_client_init(&fx->tc, &fx->conn), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_telemetry_client_destroy(&fx->tc);
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

/* Drive the connection through CONNECTING -> CONNECTED on the mock. */
static void open_to_connected(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  fx->factory_registered = true;
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  /* Drop the CONNECT call from the history so PUBLISH lands at index 0. */
  az_iot_mock_mqtt_client_clear_calls(fx->mock);

  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  /* process_loop is called from inside do_work() to deliver the injected
   * event; that adds a PROCESS_LOOP call to the mock history. Clear again so
   * the test can index PUBLISH at 0. */
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void init_rejects_nulls(void** state)
{
  (void)state;
  az_iot_telemetry_client tc;
  az_iot_connection_client* dummy = (az_iot_connection_client*)(uintptr_t)1;
  assert_int_equal(az_iot_telemetry_client_init(NULL, dummy), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_telemetry_client_init(&tc, NULL), AZ_IOT_ERR_INVALID_ARG);
  /* deinit(NULL) is a no-op. */
  az_iot_telemetry_client_destroy(NULL);
}

static void send_before_connect_returns_not_connected(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  fx->factory_registered = true;
  /* Not opened yet - publish must refuse. */
  static const uint8_t payload[] = "hello";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  assert_int_equal(
      az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_ERR_NOT_CONNECTED);
}

static void send_publishes_qos1(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  static const uint8_t payload[] = "hello";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;

  send_record r = { 0 };
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &r), AZ_IOT_OK);

  /* Always QoS 1 -> publish() called once with the expected D2C topic. */
  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(c0->topic, "devices/ut-device/messages/events/");
  assert_int_equal(c0->payload_len, 5);
  assert_memory_equal(c0->payload, "hello", 5);
  assert_int_equal(c0->qos, AZ_IOT_MQTT_QOS_1);

  /* QoS 1 -> ack callback NOT yet fired (deferred until PUBACK). */
  assert_false(r.fired);
}

static void send_qos1_defers_cb_until_puback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  static const uint8_t payload[] = "{\"t\":1}";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;

  send_record r = { 0 };
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &r), AZ_IOT_OK);

  /* publish() was called and a packet_id was assigned. */
  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_int_equal(c0->qos, AZ_IOT_MQTT_QOS_1);
  uint16_t pid = c0->packet_id;
  assert_true(pid != 0);

  /* QoS 1 -> ack callback NOT yet fired. */
  assert_false(r.fired);

  /* Inject the matching PUBLISH_ACK. */
  az_iot_mqtt_event evt = { 0 };
  evt.kind = AZ_IOT_MQTT_EVT_PUBLISH_ACK;
  evt.packet_id = pid;
  evt.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &evt));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(r.fired);
  assert_int_equal(r.status, AZ_IOT_OK);
}

static void send_propagates_content_type_and_properties(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_telemetry_property props[] = { { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
                                        { AZ_IOT_MSG_PROP_CONTENT_ENCODING, "utf-8" },
                                        { "k1", "v1" },
                                        { "k2", "v2" } };
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 1;
  msg.properties = props;
  msg.properties_count = 4;

  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  /* IoT Hub Classic topic with system + application properties appended.
   * Both halves of each pair are percent-encoded, so the system keys arrive
   * as %24.ct / %24.ce -- the same bytes azure-sdk-for-c emits -- and the
   * '/' in the content type as %2F. */
  assert_string_equal(
      c0->topic,
      "devices/ut-device/messages/events/%24.ct=application%2Fjson&%24.ce=utf-8&k1=v1&k2=v2");
}

static void send_url_encodes_property_values(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Every character that carries meaning in a property bag has to survive as
   * content rather than structure, and the RFC 3986 unreserved set has to
   * pass through untouched. Keys go through the same encoder as values. */
  az_iot_telemetry_property props[]
      = { { "note", "a&b=c d%e" }, { "keep", "-_.~AZaz09" }, { "$.mid", "id-1" } };
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 1;
  msg.properties = props;
  msg.properties_count = 3;

  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(
      c0->topic,
      "devices/ut-device/messages/events/"
      "note=a%26b%3Dc%20d%25e&keep=-_.~AZaz09&%24.mid=id-1");
}

static void system_property_keys_match_the_azure_sdk_wire_form(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* azure-sdk-for-c spells these constants pre-encoded
   * (AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE is "%24.ct"). Our constants are
   * the readable form and the encoder produces the same bytes, so a device
   * built on either SDK publishes an identical topic. */
  az_iot_telemetry_property props[]
      = { { AZ_IOT_MSG_PROP_MESSAGE_ID, "m1" }, { AZ_IOT_MSG_PROP_CORRELATION_ID, "c1" } };
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 1;
  msg.properties = props;
  msg.properties_count = 2;

  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(c0->topic, "devices/ut-device/messages/events/%24.mid=m1&%24.cid=c1");
}

static void send_reports_not_enough_space_when_the_bag_overflows(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Each 'x' encodes to itself, so this exceeds the topic buffer on length
   * alone. The build has to report the overflow rather than truncate. */
  char big[1024];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';

  az_iot_telemetry_property props[] = { { "k", big } };
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 1;
  msg.properties = props;
  msg.properties_count = 1;

  assert_int_equal(
      az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  /* Nothing was published. */
  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 0);
}

static void send_rejects_invalid_args(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_telemetry_message msg = { 0 };
  msg.payload_len = 4; /* but payload is NULL */
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_telemetry_client_send(NULL, &msg, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, NULL, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
}

/* ---- wire invariants IoT Hub enforces ------------------------------------ */

static void the_publish_never_uses_qos_2(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* IoT Hub does not support QoS 2: a device that publishes one has its
   * network connection closed. Nothing else in the SDK stops this, so pin it
   * where the application's data actually goes out. */
  az_iot_telemetry_property props[] = { { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" } };
  static const uint8_t payload[] = "{}";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 2;
  msg.properties = props;
  msg.properties_count = 1;
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

  size_t n = az_iot_mock_mqtt_client_call_count(fx->mock);
  size_t publishes = 0;
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(fx->mock, i);
    if (c->kind != AZ_IOT_MOCK_CALL_PUBLISH)
    {
      continue;
    }
    publishes++;
    assert_true(c->qos == AZ_IOT_MQTT_QOS_0 || c->qos == AZ_IOT_MQTT_QOS_1);
  }
  /* Without this the test would also pass if send() stopped publishing at all,
   * which is the one way "no QoS 2 was used" could be true and useless. */
  assert_int_equal(publishes, 1);
}

static void the_publish_never_sets_retain(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* IoT Hub does not persist retained messages. It converts the flag into an
   * "mqtt-retain" application property and hands the message to the backend,
   * so setting it would quietly change the shape of every message a routing
   * query sees. */
  static const uint8_t payload[] = "{}";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 2;
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_false(c->retain);
}

static void a_routing_content_type_survives_encoding(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* To route on the message BODY, IoT Hub requires the content type to be
   * exactly "application/json;charset=utf-8" once decoded. Both the '/' and
   * the ';' have to arrive percent-encoded or the property bag is misparsed
   * and body-based routing silently stops matching. */
  az_iot_telemetry_property props[]
      = { { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json;charset=utf-8" } };
  static const uint8_t payload[] = "{}";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = 2;
  msg.properties = props;
  msg.properties_count = 1;
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

  assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
  assert_int_equal(c->kind, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_string_equal(
      c->topic, "devices/ut-device/messages/events/%24.ct=application%2Fjson%3Bcharset%3Dutf-8");
}

/* First recorded PUBLISH, or NULL. */
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

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

static void init_zeroes_a_reused_instance(void** state)
{
  fixture* fx = (fixture*)*state;

  /* A stack instance carrying a previous client's bytes must not keep them:
   * an inherited connection pointer would publish through a dead session. */
  az_iot_telemetry_client local;
  memset(&local, 0xEE, sizeof(local));
  assert_int_equal(az_iot_telemetry_client_init(&local, &fx->conn), AZ_IOT_OK);
  assert_ptr_equal(local._internal.conn, &fx->conn);

  az_iot_telemetry_client_destroy(&local);
}

static void destroy_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_telemetry_client local;
  assert_int_equal(az_iot_telemetry_client_init(&local, &fx->conn), AZ_IOT_OK);

  az_iot_telemetry_client_destroy(&local);
  /* The second call runs against a zeroed struct and must not follow the
   * now-NULL connection pointer. */
  az_iot_telemetry_client_destroy(&local);
}

static void send_after_destroy_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_telemetry_client local;
  assert_int_equal(az_iot_telemetry_client_init(&local, &fx->conn), AZ_IOT_OK);
  az_iot_telemetry_client_destroy(&local);

  /* destroy() zeroed the connection pointer; the send must report that rather
   * than dereference it. */
  static const uint8_t payload[] = "hello";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  assert_int_equal(
      az_iot_telemetry_client_send(&local, &msg, NULL, NULL), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_null(first_publish(fx->mock));
}

/* ------------------------------------------------------------------------- */
/* send arguments and failures                                               */
/* ------------------------------------------------------------------------- */

static void send_accepts_a_null_payload_with_a_zero_length(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* An empty heartbeat is a valid message: the hub records the arrival and the
   * properties even with no body. */
  az_iot_telemetry_message msg = { 0 };
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);
  const az_iot_mock_call* c = first_publish(fx->mock);
  assert_non_null(c);
  assert_int_equal(c->payload_len, 0);
}

static void an_adapter_publish_failure_reaches_the_caller(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_mock_mqtt_client_set_next_result(
      fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_NOT_CONNECTED);

  static const uint8_t payload[] = "hello";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  send_record rec = { 0 };
  assert_int_equal(
      az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &rec), AZ_IOT_ERR_NOT_CONNECTED);

  /* Reported once, through the return value. Also firing the callback would
   * make a caller that handles both count the failure twice. */
  assert_false(rec.fired);
}

static void the_callback_receives_the_context_it_was_given(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  static const uint8_t payload[] = "hello";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  send_record rec = { 0 };
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &rec), AZ_IOT_OK);

  const az_iot_mock_call* c = first_publish(fx->mock);
  assert_non_null(c);
  assert_true(az_iot_mock_mqtt_client_inject_puback(fx->mock, c->packet_id, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  /* on_send writes through the pointer it was handed; a mismatched context
   * would show up as a record that never fired. */
  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_OK);
}

static void the_callback_reports_not_connected_when_the_session_drops_first(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  static const uint8_t payload[] = "hello";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  send_record rec = { 0 };
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &rec), AZ_IOT_OK);
  assert_false(rec.fired);

  /* The puback died with the session. Completing the send is what lets the
   * caller release its buffer instead of waiting forever. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

  assert_true(rec.fired);
  assert_int_equal(rec.status, AZ_IOT_ERR_NOT_CONNECTED);
}

static void a_full_puback_table_is_reported_after_the_publish_went_out(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  static const uint8_t payload[] = "x";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;

  send_record recs[AZ_IOT_MAX_PENDING_PUBACKS];
  memset(recs, 0, sizeof(recs));
  for (size_t i = 0; i < (size_t)AZ_IOT_MAX_PENDING_PUBACKS; ++i)
  {
    assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &recs[i]), AZ_IOT_OK);
  }

  /* The table is full, but the PUBLISH is already on the wire by the time the
   * slot is needed, so the caller is told the completion cannot be tracked --
   * not that the message was never sent. Retrying on this code duplicates
   * telemetry. */
  send_record overflow = { 0 };
  size_t before = az_iot_mock_mqtt_client_call_count(fx->mock);
  assert_int_equal(
      az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &overflow), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_true(az_iot_mock_mqtt_client_call_count(fx->mock) > before);
  assert_false(overflow.fired);
}

/* ------------------------------------------------------------------------- */
/* property bag edges                                                        */
/* ------------------------------------------------------------------------- */

/* Returns the published topic for a message carrying `props`. */
static const char* topic_for_properties(
    fixture* fx,
    const az_iot_telemetry_property* props,
    size_t count)
{
  static const uint8_t payload[] = "p";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  msg.properties = props;
  msg.properties_count = count;
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);
  const az_iot_mock_call* c = first_publish(fx->mock);
  assert_non_null(c);
  return c->topic;
}

static void the_remaining_system_property_keys_match_the_azure_sdk_wire_form(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* These three are declared in the public header but were never asserted.
   * A device that sets them expects the same wire form the other SDKs use. */
  const az_iot_telemetry_property props[] = {
    { AZ_IOT_MSG_PROP_USER_ID, "u1" },
    { AZ_IOT_MSG_PROP_CREATION_TIME, "2026-01-01T00:00:00Z" },
    { AZ_IOT_MSG_PROP_COMPONENT_NAME, "thermostat" },
  };
  const char* topic = topic_for_properties(fx, props, 3);
  assert_non_null(strstr(topic, "%24.uid=u1"));
  assert_non_null(strstr(topic, "%24.ctime=2026-01-01T00%3A00%3A00Z"));
  assert_non_null(strstr(topic, "%24.sub=thermostat"));
}

static void a_property_with_a_null_value_emits_a_bare_key(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* A valueless property is a flag. Emitting "key=" instead would make it an
   * empty string, which is a different thing to the service. */
  const az_iot_telemetry_property props[] = { { "flag", NULL } };
  const char* topic = topic_for_properties(fx, props, 1);
  assert_non_null(strstr(topic, "flag"));
  assert_null(strstr(topic, "flag="));
}

static void a_property_with_an_empty_value_emits_key_and_equals(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  const az_iot_telemetry_property props[] = { { "empty", "" } };
  const char* topic = topic_for_properties(fx, props, 1);
  assert_non_null(strstr(topic, "empty="));
}

static void a_null_key_is_skipped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* A pair with no key cannot be expressed in the bag; emitting "=value"
   * would produce a property the service cannot address. */
  const az_iot_telemetry_property props[] = { { NULL, "orphan" } };
  const char* topic = topic_for_properties(fx, props, 1);
  assert_null(strstr(topic, "orphan"));
}

static void an_empty_key_is_skipped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  const az_iot_telemetry_property props[] = { { "", "orphan" } };
  const char* topic = topic_for_properties(fx, props, 1);
  assert_null(strstr(topic, "orphan"));
}

static void a_skipped_key_leaves_no_dangling_separator(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* The separator belongs to the pair that was dropped. A stray "&&" or a
   * trailing "&" makes the bag unparsable for the receiver. */
  const az_iot_telemetry_property props[] = {
    { "a", "1" },
    { NULL, "dropped" },
    { "b", "2" },
  };
  const char* topic = topic_for_properties(fx, props, 3);
  assert_non_null(strstr(topic, "a=1"));
  assert_non_null(strstr(topic, "b=2"));
  assert_null(strstr(topic, "&&"));
  size_t len = strlen(topic);
  assert_true(len > 0);
  assert_int_not_equal(topic[len - 1], '&');
}

/* ------------------------------------------------------------------------- */
/* Hub-Next (AEG, MQTT v5) flavor                                            */
/* ------------------------------------------------------------------------- */

/* Fixture variant: a direct HUB_NEXT connection. Telemetry then takes the Next
 * path, which puts everything the Classic path percent-encodes into the topic
 * onto MQTT v5 User Properties instead. */
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

  assert_int_equal(az_iot_telemetry_client_init(&fx->tc, &fx->conn), AZ_IOT_OK);

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

/* Drive a Hub-Next session to CONNECTED: CONNACK, a SUBACK for every filter,
 * then echo the birth nonce back. Classic announces CONNECTED on CONNACK; Next
 * waits for the presence handshake. */
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

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

/* Publish `props` on the Next path and return the recorded PUBLISH. */
static const az_iot_mock_call* next_send(
    fixture* fx,
    const az_iot_telemetry_property* props,
    size_t count)
{
  static const uint8_t payload[] = "{\"t\":21}";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  msg.properties = props;
  msg.properties_count = count;
  assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);
  const az_iot_mock_call* c = find_publish(fx->mock, "ih/ut-device/srv/telemetry");
  assert_non_null(c);
  return c;
}

static void next_send_uses_the_flat_service_topic(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  /* One flat topic per device rather than the Classic path's trailing property
   * bag: on Next the metadata travels beside the message, not inside the name. */
  const az_iot_mock_call* c = next_send(fx, NULL, 0);
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_1);
  assert_false(c->retain);
  assert_int_equal(c->payload_len, 8);
  assert_memory_equal(c->payload, "{\"t\":21}", 8);
}

static void next_send_marks_the_message_type(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  /* The service demultiplexes on this property, so a message without it is
   * discarded rather than routed. */
  const az_iot_mock_call* c = next_send(fx, NULL, 0);
  assert_string_equal(c->user_type, "telemetry:1");
}

static void next_send_defaults_the_content_type_to_json(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  const az_iot_mock_call* c = next_send(fx, NULL, 0);
  const char* ct = az_iot_mock_call_user_property(c, "content-type");
  assert_non_null(ct);
  assert_string_equal(ct, "application/json");
  assert_string_equal(c->content_type, "application/json");
}

static void next_send_honours_the_content_type_system_property(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  /* $.ct is how a caller declares the body format on both flavors; on Next it
   * becomes the real MQTT v5 Content Type rather than a bag entry. */
  const az_iot_telemetry_property props[] = { { AZ_IOT_MSG_PROP_CONTENT_TYPE, "text/plain" } };
  const az_iot_mock_call* c = next_send(fx, props, 1);
  const char* ct = az_iot_mock_call_user_property(c, "content-type");
  assert_non_null(ct);
  assert_string_equal(ct, "text/plain");
  assert_string_equal(c->content_type, "text/plain");
}

static void next_send_forwards_application_properties(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  const az_iot_telemetry_property props[] = { { "site", "plant-3" }, { "unit", "celsius" } };
  const az_iot_mock_call* c = next_send(fx, props, 2);

  const char* site = az_iot_mock_call_user_property(c, "site");
  const char* unit = az_iot_mock_call_user_property(c, "unit");
  assert_non_null(site);
  assert_non_null(unit);
  assert_string_equal(site, "plant-3");
  assert_string_equal(unit, "celsius");
}

static void next_send_does_not_forward_system_properties_as_user_properties(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  /* System properties have first-class MQTT v5 homes on Next. Forwarding the
   * "$."-prefixed spelling as well would put the same value on the wire twice,
   * under a name the service does not recognise. */
  const az_iot_telemetry_property props[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "text/plain" },
    { AZ_IOT_MSG_PROP_MESSAGE_ID, "m-1" },
    { "site", "plant-3" },
  };
  const az_iot_mock_call* c = next_send(fx, props, 3);

  assert_null(az_iot_mock_call_user_property(c, AZ_IOT_MSG_PROP_CONTENT_TYPE));
  assert_null(az_iot_mock_call_user_property(c, AZ_IOT_MSG_PROP_MESSAGE_ID));
  assert_non_null(az_iot_mock_call_user_property(c, "site"));
}

static void next_send_skips_properties_with_no_key(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  /* Same rule as the Classic bag: a pair with no key cannot be addressed, so
   * it is dropped rather than sent with an empty name. */
  const az_iot_telemetry_property props[] = {
    { NULL, "orphan" },
    { "", "also-orphan" },
    { "kept", "yes" },
  };
  const az_iot_mock_call* c = next_send(fx, props, 3);

  /* type + content-type + "kept". */
  assert_int_equal(c->user_properties_count, 3);
  const char* kept = az_iot_mock_call_user_property(c, "kept");
  assert_non_null(kept);
  assert_string_equal(kept, "yes");
}

static void next_send_gives_a_valueless_property_an_empty_value(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected_next(fx);

  /* MQTT v5 User Properties are key/value pairs with no "bare key" form, so a
   * NULL value has to become an empty string rather than a NULL pointer the
   * adapter would have to guard. */
  const az_iot_telemetry_property props[] = { { "flag", NULL } };
  const az_iot_mock_call* c = next_send(fx, props, 1);
  const char* flag = az_iot_mock_call_user_property(c, "flag");
  assert_non_null(flag);
  assert_string_equal(flag, "");
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(init_rejects_nulls),
    cmocka_unit_test_setup_teardown(send_before_connect_returns_not_connected, setup, teardown),
    cmocka_unit_test_setup_teardown(send_publishes_qos1, setup, teardown),
    cmocka_unit_test_setup_teardown(send_qos1_defers_cb_until_puback, setup, teardown),
    cmocka_unit_test_setup_teardown(send_propagates_content_type_and_properties, setup, teardown),
    cmocka_unit_test_setup_teardown(send_url_encodes_property_values, setup, teardown),
    cmocka_unit_test_setup_teardown(
        system_property_keys_match_the_azure_sdk_wire_form, setup, teardown),
    cmocka_unit_test_setup_teardown(
        send_reports_not_enough_space_when_the_bag_overflows, setup, teardown),
    cmocka_unit_test_setup_teardown(send_rejects_invalid_args, setup, teardown),
    cmocka_unit_test_setup_teardown(the_publish_never_uses_qos_2, setup, teardown),
    cmocka_unit_test_setup_teardown(the_publish_never_sets_retain, setup, teardown),
    cmocka_unit_test_setup_teardown(a_routing_content_type_survives_encoding, setup, teardown),
    cmocka_unit_test_setup_teardown(init_zeroes_a_reused_instance, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(send_after_destroy_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        send_accepts_a_null_payload_with_a_zero_length, setup, teardown),
    cmocka_unit_test_setup_teardown(an_adapter_publish_failure_reaches_the_caller, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_callback_receives_the_context_it_was_given, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_callback_reports_not_connected_when_the_session_drops_first, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_full_puback_table_is_reported_after_the_publish_went_out, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_remaining_system_property_keys_match_the_azure_sdk_wire_form, setup, teardown),
    cmocka_unit_test_setup_teardown(a_property_with_a_null_value_emits_a_bare_key, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_property_with_an_empty_value_emits_key_and_equals, setup, teardown),
    cmocka_unit_test_setup_teardown(a_null_key_is_skipped, setup, teardown),
    cmocka_unit_test_setup_teardown(an_empty_key_is_skipped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_skipped_key_leaves_no_dangling_separator, setup, teardown),
    cmocka_unit_test_setup_teardown(next_send_uses_the_flat_service_topic, setup_next, teardown),
    cmocka_unit_test_setup_teardown(next_send_marks_the_message_type, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_send_defaults_the_content_type_to_json, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_send_honours_the_content_type_system_property, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_send_forwards_application_properties, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_send_does_not_forward_system_properties_as_user_properties, setup_next, teardown),
    cmocka_unit_test_setup_teardown(next_send_skips_properties_with_no_key, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        next_send_gives_a_valueless_property_an_empty_value, setup_next, teardown),
  };
  return cmocka_run_group_tests_name("telemetry_client", tests, NULL, NULL);
}
