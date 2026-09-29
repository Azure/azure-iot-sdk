// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv5 (MQTTv5 hub, MQTT v5) direct method client unit tests, driven
 * through the public API and the in-memory mock_mqtt_iface.
 *
 * The MQTTv5 protocol is a probe / probe-ack / exec / result handshake over two
 * flat topics -- ih/{device}/dev/methods inbound and ih/{device}/srv/methods
 * outbound -- discriminated by a `type` user property, with the request id in
 * MQTT v5 Correlation Data and protobuf payloads. Neither topic names the
 * method: the name arrives in the probe and has to survive until the exec.
 *
 * The protobuf frames here are hand-built from common/Protos/directmethods.proto
 * rather than produced by the SDK's own codec, so these tests fail if the two
 * disagree instead of agreeing with each other by construction.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/mqttv5/az_iot_direct_method_client.h"

#include "support/mock_mqtt_iface.h"
#include "support/subscription_ack.h"

#define DEV_TOPIC "ih/ut-device/dev/methods"
#define SRV_TOPIC "ih/ut-device/srv/methods"

/* Two tests turn on a budget actually elapsing, and the budgets are whole
 * seconds off a monotonic clock with no test seam behind it. Anything under a
 * second would be measuring timer granularity instead of the behaviour. */
static void sleep_past_one_second(void)
{
#if defined(_WIN32)
  Sleep(1200);
#else
  struct timespec remaining;
  remaining.tv_sec = 1;
  remaining.tv_nsec = 200000000L;
  /* nanosleep() returns early when a signal arrives, which would leave the
   * budget unexpired and fail the assertion for the wrong reason. */
  while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR)
  {
  }
#endif
}

/* ------------------------------------------------------------------------- */
/* protobuf frames, hand-built from directmethods.proto                      */
/* ------------------------------------------------------------------------- */

/* Probe { method_name = 1, response_timeout_seconds = 2 } */
static size_t build_probe(uint8_t* out, const char* method_name, uint32_t timeout_seconds)
{
  size_t n = 0;
  if (method_name != NULL)
  {
    size_t name_len = strlen(method_name);
    out[n++] = 0x0A; /* field 1, length-delimited */
    out[n++] = (uint8_t)name_len; /* every name used here is under 128 bytes */
    memcpy(out + n, method_name, name_len);
    n += name_len;
  }
  if (timeout_seconds != 0)
  {
    out[n++] = 0x10; /* field 2, varint */
    uint32_t value = timeout_seconds;
    while (value >= 0x80u)
    {
      out[n++] = (uint8_t)(value | 0x80u);
      value >>= 7;
    }
    out[n++] = (uint8_t)value;
  }
  return n;
}

/* Exec { ready_id = 1, params = 2 } */
static size_t build_exec(
    uint8_t* out,
    const uint8_t* ready_id,
    size_t ready_id_len,
    const uint8_t* params,
    size_t params_len)
{
  size_t n = 0;
  out[n++] = 0x0A;
  out[n++] = (uint8_t)ready_id_len;
  memcpy(out + n, ready_id, ready_id_len);
  n += ready_id_len;
  if (params_len > 0)
  {
    out[n++] = 0x12;
    out[n++] = (uint8_t)params_len;
    memcpy(out + n, params, params_len);
    n += params_len;
  }
  return n;
}

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct invocation_record
{
  bool fired;
  int fire_count;
  char method_name[64];
  char payload[64];
  size_t payload_len;
  /* By value: still meaningful after the pool reuses the slot, which is the
   * whole point of the handle change. */
  az_iot_direct_method_request request;
} invocation_record;

typedef struct probe_record
{
  int call_count;
  char method_name[64];
  uint32_t response_timeout_seconds;
  az_iot_mqttv5_direct_method_probe_result answer;
  bool stall; /* burn the connect budget before answering */
} probe_record;

static az_iot_mqttv5_direct_method_probe_result on_probe(
    const az_iot_mqttv5_direct_method_probe* probe,
    void* user_ctx)
{
  probe_record* r = (probe_record*)user_ctx;
  r->call_count++;
  snprintf(r->method_name, sizeof(r->method_name), "%s", probe->method_name);
  r->response_timeout_seconds = probe->response_timeout_seconds;
  if (r->stall)
  {
    sleep_past_one_second();
  }
  return r->answer;
}

typedef struct fixture
{
  az_iot_connection_client conn;
  az_iot_mqttv5_direct_method_client dm;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock;
  /* Where the registered handler records what it was given. */
  invocation_record* rec;
} fixture;

static void on_method(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (fx->rec == NULL)
  {
    return;
  }
  invocation_record* r = fx->rec;
  r->fired = true;
  r->fire_count++;
  snprintf(r->method_name, sizeof(r->method_name), "%s", method_name);
  r->payload[0] = '\0';
  r->payload_len = 0;
  if (payload && payload_len > 0 && payload_len < sizeof(r->payload))
  {
    memcpy(r->payload, payload, payload_len);
    r->payload[payload_len] = '\0';
    r->payload_len = payload_len;
  }
  r->request = request;
}

/* A second, unrelated handler. Routing is only observable if two methods can
 * land somewhere different. */
typedef struct alt_record
{
  int fire_count;
  char method_name[64];
  az_iot_mqttv5_direct_method_client* dm; /* respond() needs the client that delivered it */
} alt_record;

static void on_method_alt(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  (void)payload;
  (void)payload_len;
  alt_record* a = (alt_record*)user_ctx;
  a->fire_count++;
  snprintf(a->method_name, sizeof(a->method_name), "%s", method_name);
  (void)az_iot_mqttv5_direct_method_respond(a->dm, request, 200, NULL, 0);
}

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

  assert_int_equal(az_iot_mqttv5_direct_method_client_init(&fx->dm, &fx->conn), AZ_IOT_OK);

  /* Probes are answered from the declared list, so the names these tests use
   * have to be on it. Tests about undeclared names declare nothing extra. */
  static const char* const declared[] = { "reboot", "ping", "dump", "slow" };
  for (size_t i = 0; i < sizeof(declared) / sizeof(declared[0]); ++i)
  {
    assert_int_equal(
        az_iot_mqttv5_direct_method_client_register_method(&fx->dm, declared[i], 0, on_method, fx),
        AZ_IOT_OK);
  }

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_mqttv5_direct_method_client_deinit(&fx->dm);
    /* A registered factory is adopted by the client and freed from deinit();
     * an unregistered one is still ours. */
    bool adopted = (fx->conn.factory_count > 0);
    az_iot_connection_client_deinit(&fx->conn);
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

/* Most recent PUBLISH on the service topic carrying the given `type`. */
static const az_iot_mock_call* find_phase(az_iot_mock_mqtt_client* m, const char* type_value)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = n; i > 0; --i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i - 1);
    if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH && strcmp(c->topic, SRV_TOPIC) == 0
        && strcmp(c->user_type, type_value) == 0)
    {
      return c;
    }
  }
  return NULL;
}

/* Read the ready id out of a ProbeAck{ready}: 0x0A len 0x0A 0x10 <16 bytes>. */
static void probe_ack_ready_id(const az_iot_mock_call* c, uint8_t out[16])
{
  assert_non_null(c);
  assert_int_equal(c->payload_len, 20);
  assert_int_equal(c->payload[0], 0x0A);
  assert_int_equal(c->payload[1], 0x12);
  assert_int_equal(c->payload[2], 0x0A);
  assert_int_equal(c->payload[3], 0x10);
  memcpy(out, c->payload + 4, 16);
}

/* Read the reason out of a ProbeAck{rejected}: 0x12 len [0x08 reason]. */
static int probe_ack_rejected_reason(const az_iot_mock_call* c)
{
  assert_non_null(c);
  assert_true(c->payload_len >= 2);
  assert_int_equal(c->payload[0], 0x12);
  if (c->payload[1] == 0x00)
  {
    return 0; /* proto3 omits the default reason */
  }
  assert_int_equal(c->payload_len, 4);
  assert_int_equal(c->payload[2], 0x08);
  return c->payload[3];
}

/* Register the factory, open, and deliver CONNACK. On MQTTv5 this leaves the
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

/* Drive an MQTTv5 session all the way to CONNECTED. Unlike the MQTTv3 path,
 * CONNACK alone does not announce CONNECTED. */
static void open_to_connected(fixture* fx)
{
  open_conn(fx);
  finish_birth(fx);
  /* mqttv5 feature delivery uses the presence wildcard; there are no later
   * per-feature SUBACKs to wait for. */
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
}

/* Deliver one service-to-device method message. */
static void inject_dm(
    fixture* fx,
    const char* topic,
    const char* type_value,
    const uint8_t* corr,
    size_t corr_len,
    const uint8_t* payload,
    size_t payload_len,
    uint32_t expiry_seconds)
{
  az_iot_mqtt_user_property type_prop = { "type", type_value };
  az_iot_mqtt_message msg;
  memset(&msg, 0, sizeof(msg));
  msg.topic = topic;
  msg.payload = payload;
  msg.payload_len = payload_len;
  msg.correlation_data = corr;
  msg.correlation_data_len = corr_len;
  msg.qos = AZ_IOT_MQTT_QOS_1;
  msg.content_type = "application/protobuf";
  msg.message_expiry_seconds = expiry_seconds;
  if (type_value != NULL)
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

/* A request id: 16 bytes, all `seed`, so each invocation is distinguishable. */
static void make_request_id(uint8_t out[16], uint8_t seed) { memset(out, seed, 16); }

/* Probe `method_name` and return the ready id the client answered with. */
static void probe_and_accept(
    fixture* fx,
    const uint8_t request_id[16],
    const char* method_name,
    uint32_t response_timeout_seconds,
    uint8_t out_ready_id[16])
{
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, method_name, response_timeout_seconds);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 0);
  probe_ack_ready_id(find_phase(fx->mock, "probe-ack:1"), out_ready_id);
}

/* Several failures are reportable only as a log line -- nothing goes on the
 * wire and the return code is shared with other causes -- so the text is the
 * contract and gets asserted like one. */
typedef struct log_capture
{
  int count;
  az_iot_log_level last_level;
  char last[AZ_IOT_LOG_MESSAGE_MAX];
  /* Every message, joined. A failure that also makes the connection log means
   * `last` belongs to whoever spoke second. */
  char all[AZ_IOT_LOG_MESSAGE_MAX * 4];
} log_capture;

static void capture_log(
    void* user_ctx,
    az_iot_log_level level,
    const char* file,
    int line,
    const char* msg)
{
  log_capture* c = (log_capture*)user_ctx;
  (void)file;
  (void)line;
  if (msg == NULL)
  {
    return;
  }
  c->count++;
  c->last_level = level;
  snprintf(c->last, sizeof(c->last), "%s", msg);
  size_t used = strlen(c->all);
  if (used + 1 < sizeof(c->all))
  {
    snprintf(c->all + used, sizeof(c->all) - used, "%s\n", msg);
  }
}

static void install_capture(log_capture* c, az_iot_log_level min_level)
{
  memset(c, 0, sizeof(*c));
  az_iot_log_sink sink;
  sink.sink = capture_log;
  sink.user_ctx = c;
  sink.min_level = min_level;
  az_iot_log_set_global_sink(&sink);
}

/* Deliver the PUBACK for `packet_id`. Publishing only hands a message to the
 * adapter; this is the broker's answer. */
static void inject_puback(fixture* fx, uint16_t packet_id, az_iot_result status)
{
  az_iot_mqtt_event evt;
  memset(&evt, 0, sizeof(evt));
  evt.kind = AZ_IOT_MQTT_EVT_PUBLISH_ACK;
  evt.status = status;
  evt.packet_id = packet_id;
  assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &evt));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

/* Drive one invocation to the point where the application holds a request it
 * has not answered yet. */
static void run_one_invocation(fixture* fx, invocation_record* rec, uint8_t seed)
{
  uint8_t request_id[16];
  make_request_id(request_id, seed);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "dump", 300, ready_id);
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  rec->fired = false;
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_true(rec->fired);
}

/* ------------------------------------------------------------------------- */
/* init and profile pinning                                                  */
/* ------------------------------------------------------------------------- */

static void init_rejects_a_null_client(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_init(NULL, &fx->conn), AZ_IOT_ERR_INVALID_ARG);
}

static void init_rejects_a_null_connection(void** state)
{
  (void)state;
  az_iot_mqttv5_direct_method_client dm;
  assert_int_equal(az_iot_mqttv5_direct_method_client_init(&dm, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void init_against_an_mqtt_v3_connection_is_rejected(void** state)
{
  (void)state;

  /* A direct connection declares its generation up front, so the pin can be
   * answered at init rather than deferred to connect. */
  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv5_direct_method_client dm;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_init(&dm, &conn), AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);

  az_iot_connection_client_deinit(&conn);
}

static void a_device_id_too_long_for_the_topics_says_so(void** state)
{
  (void)state;

  /* Both topics are "ih/" + device id + a direction tail, so a long enough
   * device id does not fit. */
  char long_id[AZ_IOT_MQTTV5_DM_TOPIC_MAX + 8];
  memset(long_id, 'd', sizeof(long_id) - 1);
  long_id[sizeof(long_id) - 1] = '\0';

  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = long_id;
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv5_direct_method_client dm;
  assert_int_equal(az_iot_mqttv5_direct_method_client_init(&dm, &conn), AZ_IOT_OK);

  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(factory);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&conn, factory), AZ_IOT_OK);

  /* Binding a truncated topic would listen on another device's name and answer
   * where the service is not reading, so the connect attempt fails instead --
   * and names the limit, which is the only way to act on it. */
  log_capture cap;
  install_capture(&cap, AZ_IOT_LOG_LEVEL_ERROR);
  az_iot_result opened = az_iot_connection_client_open(&conn);
  az_iot_log_set_global_sink(NULL);

  assert_int_not_equal(opened, AZ_IOT_OK);
  assert_true(cap.count >= 1);
  assert_non_null(strstr(cap.all, "AZ_IOT_MQTTV5_DM_TOPIC_MAX"));

  az_iot_mqttv5_direct_method_client_deinit(&dm);
  az_iot_connection_client_deinit(&conn);
}

static void init_does_not_subscribe_a_redundant_methods_filter(void** state)
{
  fixture* fx = (fixture*)*state;
  open_conn(fx);

  /* CONNACK alone buys only the presence filter. */
  assert_non_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE, "ih/ut-device/dev/#"));
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE, DEV_TOPIC));

  finish_birth(fx);

  /* And still nothing afterwards: ih/ut-device/dev/# already covers
   * ih/ut-device/dev/methods, so a second filter would be redundant.
   * Messages still arrive -- see a_probe_is_accepted_and_answered_with_a_ready_id. */
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE, DEV_TOPIC));
}

/* ------------------------------------------------------------------------- */
/* probe                                                                     */
/* ------------------------------------------------------------------------- */

static void a_probe_is_accepted_and_answered_with_a_ready_id(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  probe.answer = AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  uint8_t request_id[16];
  make_request_id(request_id, 0x11);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);

  /* The name and the caller's timeout come out of the payload -- the topic
   * carries neither. */
  assert_int_equal(probe.call_count, 1);
  assert_string_equal(probe.method_name, "reboot");
  assert_int_equal(probe.response_timeout_seconds, 300);

  /* A probe is a question, not an invocation: nothing runs yet. */
  assert_false(rec.fired);

  const az_iot_mock_call* ack = find_phase(fx->mock, "probe-ack:1");
  assert_non_null(ack);
  uint8_t ready_id[16];
  probe_ack_ready_id(ack, ready_id);
}

static void the_probe_ack_carries_the_protocol_envelope(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0x22);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);

  const az_iot_mock_call* ack = find_phase(fx->mock, "probe-ack:1");
  assert_non_null(ack);
  /* Everything the service needs to route the answer back to the invocation it
   * is holding open: the phase, the request id, the payload format, and a QoS
   * that survives a reconnect. */
  assert_int_equal(ack->qos, AZ_IOT_MQTT_QOS_1);
  assert_string_equal(ack->content_type, "application/protobuf");
  assert_int_equal(ack->correlation_data_len, 16);
  assert_memory_equal(ack->correlation_data, request_id, 16);
  /* The ack inherits what is left of the probe's budget, so it expires with
   * the connect timeout the service is still counting down. */
  assert_int_equal(ack->message_expiry_seconds, 30);
}

static void a_declared_method_is_accepted_without_a_probe_handler(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* The declared list is the answer to a probe. A probe handler is for
   * conditions the SDK cannot know, so an application that has none should
   * still serve the methods it declared. */
  uint8_t request_id[16];
  make_request_id(request_id, 0x33);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, ready_id);
}

static void an_undeclared_method_is_rejected_and_kept_out(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  probe.answer = AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  uint8_t request_id[16];
  make_request_id(request_id, 0x34);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "not-declared", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND);

  /* The application is not even asked -- the device knows it does not have
   * this method, and answering costs two small messages and no state. */
  assert_int_equal(probe.call_count, 0);

  /* No token was issued, so a matching exec finds nothing to run. */
  uint8_t any_ready_id[16];
  memset(any_ready_id, 0xEE, sizeof(any_ready_id));
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, any_ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_false(rec.fired);
}

static void an_undeclared_method_costs_no_capacity(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* This is the whole point of answering by name. A caller spraying invented
   * methods must not consume the slots the declared ones need, or a
   * service-side bug could starve a working device. */
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "invented", 300);
  for (int i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT * 4; ++i)
  {
    uint8_t request_id[16];
    make_request_id(request_id, (uint8_t)(0x40 + i));
    inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);
  }

  uint8_t real_id[16];
  make_request_id(real_id, 0x3F);
  uint8_t ready_id[16];
  probe_and_accept(fx, real_id, "reboot", 300, ready_id);
}

static void each_declared_method_runs_its_own_handler(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  alt_record alt = { 0 };
  alt.dm = &fx->dm;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(&fx->dm, "alt", 0, on_method_alt, &alt),
      AZ_IOT_OK);

  /* "reboot" was declared with on_method, "alt" with on_method_alt. Declaring
   * the handler alongside the name is what makes this routable at all -- one
   * catch-all would hand both to the same place and leave the application to
   * re-dispatch on a string the SDK already matched. */
  uint8_t reboot_id[16];
  make_request_id(reboot_id, 0xF1);
  uint8_t reboot_ready[16];
  probe_and_accept(fx, reboot_id, "reboot", 300, reboot_ready);
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, reboot_ready, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", reboot_id, 16, exec, exec_len, 300);

  assert_int_equal(rec.fire_count, 1);
  assert_string_equal(rec.method_name, "reboot");
  assert_int_equal(alt.fire_count, 0);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);

  uint8_t alt_id[16];
  make_request_id(alt_id, 0xF2);
  uint8_t alt_ready[16];
  probe_and_accept(fx, alt_id, "alt", 300, alt_ready);
  exec_len = build_exec(exec, alt_ready, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", alt_id, 16, exec, exec_len, 300);

  assert_int_equal(alt.fire_count, 1);
  assert_string_equal(alt.method_name, "alt");
  assert_int_equal(rec.fire_count, 1);
}

static void withdrawing_one_method_leaves_the_others_declared(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  probe.answer = AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  assert_int_equal(
      az_iot_mqttv5_direct_method_client_unregister_method(&fx->dm, "reboot"), AZ_IOT_OK);

  /* The withdrawn one is gone, and the probe handler is not consulted: it
   * narrows what the device will take on, it cannot put back a method the
   * device has said it no longer has. */
  uint8_t request_id[16];
  make_request_id(request_id, 0x35);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND);
  assert_int_equal(probe.call_count, 0);

  /* And withdrawing one method must not take the others with it. */
  uint8_t other_id[16];
  make_request_id(other_id, 0x3B);
  uint8_t ready_id[16];
  probe_and_accept(fx, other_id, "ping", 300, ready_id);
  assert_int_equal(probe.call_count, 1);
}

static void a_response_timeout_under_the_declared_minimum_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(&fx->dm, "long-job", 100, on_method, fx),
      AZ_IOT_OK);

  /* Accepting a timeout the method was never going to meet would spend the
   * caller's whole budget to arrive at the same answer, so it is refused with
   * the reason that says the timeout, not the device, is the problem. */
  uint8_t request_id[16];
  make_request_id(request_id, 0x36);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "long-job", 30);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_INSUFFICIENT_TIME);

  /* A timeout that does cover it is accepted. */
  uint8_t roomy_id[16];
  make_request_id(roomy_id, 0x37);
  uint8_t ready_id[16];
  probe_and_accept(fx, roomy_id, "long-job", 300, ready_id);
}

static void exec_admission_counts_the_declared_minimum(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(&fx->dm, "long-job", 5, on_method, fx),
      AZ_IOT_OK);

  uint8_t request_id[16];
  make_request_id(request_id, 0x38);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "long-job", 300, ready_id);

  /* Twelve seconds clears the ten second safety margin on its own, so without
   * the method's declared five second floor this would start -- and then miss
   * the deadline with the work half done. */
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 12);

  assert_false(rec.fired);
  const az_iot_mock_call* abandon = find_phase(fx->mock, "abandon:1");
  assert_non_null(abandon);
  assert_int_equal(abandon->payload[19], 2); /* INSUFFICIENT_TIME */
}

static void declaring_a_method_twice_updates_it(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* Re-declaring is how a device revises what a method needs; consuming a
   * second slot each time would exhaust the list instead. */
  for (int i = 0; i < AZ_IOT_MQTTV5_DM_MAX_METHODS * 2; ++i)
  {
    assert_int_equal(
        az_iot_mqttv5_direct_method_client_register_method(&fx->dm, "reboot", 0, on_method, fx),
        AZ_IOT_OK);
  }
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(&fx->dm, "reboot", 100, on_method, fx),
      AZ_IOT_OK);

  uint8_t request_id[16];
  make_request_id(request_id, 0x39);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 30);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_INSUFFICIENT_TIME);
}

static void the_method_list_reports_when_it_is_full(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)state;

  /* setup() already declared four. */
  char name[16];
  int declared = 4;
  for (int i = declared; i < AZ_IOT_MQTTV5_DM_MAX_METHODS; ++i)
  {
    snprintf(name, sizeof(name), "extra%d", i);
    assert_int_equal(
        az_iot_mqttv5_direct_method_client_register_method(&fx->dm, name, 0, on_method, fx),
        AZ_IOT_OK);
  }

  /* Silently dropping one would leave the device answering METHOD_NOT_FOUND
   * for something the application believes it offers. */
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(&fx->dm, "one-too-many", 0, on_method, fx),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void the_method_list_rejects_names_it_could_never_match(void** state)
{
  fixture* fx = (fixture*)*state;

  char long_name[AZ_IOT_DM_METHOD_NAME_MAX + 1];
  memset(long_name, 'x', sizeof(long_name) - 1);
  long_name[sizeof(long_name) - 1] = '\0';

  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(NULL, "reboot", 0, on_method, fx),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(&fx->dm, NULL, 0, on_method, fx),
      AZ_IOT_ERR_INVALID_ARG);
  /* A declared name with nothing behind it is a promise the device cannot
   * keep, which is the whole reason the handler is declared with it. */
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(&fx->dm, "handlerless", 0, NULL, fx),
      AZ_IOT_ERR_INVALID_ARG);
  /* An empty name cannot appear in a probe, and a name past the bound could
   * never be matched against one -- accepting either would be a promise the
   * device could not keep. */
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(&fx->dm, "", 0, on_method, fx),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(&fx->dm, long_name, 0, on_method, fx),
      AZ_IOT_ERR_INVALID_ARG);

  assert_int_equal(
      az_iot_mqttv5_direct_method_client_unregister_method(&fx->dm, "never-declared"),
      AZ_IOT_ERR_NOT_FOUND);
}

static void withdrawing_a_method_leaves_its_live_token_alone(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0x3A);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, ready_id);

  /* The device already agreed to run this one. Withdrawing the method stops
   * new probes, but reneging on an outstanding token would strand a caller
   * that was told yes. */
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_unregister_method(&fx->dm, "reboot"), AZ_IOT_OK);

  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_true(rec.fired);
}

static void a_rejected_probe_keeps_no_ready_state(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  probe.answer = AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND;
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  uint8_t request_id[16];
  make_request_id(request_id, 0x55);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND);

  /* An exec for a rejected probe must find nothing to run against, or the
   * rejection was only advisory. */
  uint8_t any_ready_id[16];
  memset(any_ready_id, 0xEE, sizeof(any_ready_id));
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, any_ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_false(rec.fired);
}

static void each_rejection_reason_reaches_the_wire(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  /* The reasons are the only thing that distinguishes "you asked for something
   * I do not have" from "ask me later", so each has to survive encoding. */
  static const az_iot_mqttv5_direct_method_probe_result answers[] = {
    AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND,
    AZ_IOT_MQTTV5_DM_PROBE_REJECT_INSUFFICIENT_TIME,
    AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY,
  };
  for (size_t i = 0; i < sizeof(answers) / sizeof(answers[0]); ++i)
  {
    probe.answer = answers[i];
    uint8_t request_id[16];
    make_request_id(request_id, (uint8_t)(0x60 + i));
    uint8_t frame[128];
    size_t frame_len = build_probe(frame, "reboot", 300);
    az_iot_mock_mqtt_client_clear_calls(fx->mock);
    inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);
    assert_int_equal(
        probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")), (int)answers[i]);
  }
}

static void a_redelivered_probe_is_not_answered_twice(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  probe.answer = AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  uint8_t request_id[16];
  make_request_id(request_id, 0x77);
  uint8_t first_ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, first_ready_id);
  assert_int_equal(probe.call_count, 1);

  /* QoS 1 can redeliver the probe. Re-asking the application would be visible
   * to it, and a fresh ready id would invalidate the one the service is about
   * to quote back. */
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);

  assert_int_equal(probe.call_count, 1);
  assert_null(find_phase(fx->mock, "probe-ack:1"));

  /* And the original token still works. */
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, first_ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_true(rec.fired);
}

static void a_probe_beyond_the_ready_capacity_is_rejected_as_busy(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  for (int i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    uint8_t request_id[16];
    make_request_id(request_id, (uint8_t)(0x80 + i));
    uint8_t ready_id[16];
    probe_and_accept(fx, request_id, "reboot", 300, ready_id);
  }

  /* Accepting past capacity would promise a token there is no room to hold, so
   * the caller is told to try later instead of being left to time out. */
  uint8_t overflow_id[16];
  make_request_id(overflow_id, 0x8F);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", overflow_id, 16, frame, frame_len, 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY);
}

static void a_method_name_past_the_bound_is_rejected_not_truncated(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* A truncated name would be handed to the application as if it were the one
   * the caller asked for. */
  char long_name[AZ_IOT_DM_METHOD_NAME_MAX + 1];
  memset(long_name, 'x', sizeof(long_name) - 1);
  long_name[sizeof(long_name) - 1] = '\0';

  uint8_t request_id[16];
  make_request_id(request_id, 0x90);
  uint8_t frame[AZ_IOT_DM_METHOD_NAME_MAX + 16];
  size_t frame_len = build_probe(frame, long_name, 300);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND);
}

static void a_method_name_with_an_embedded_nul_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* A protobuf string field is a byte count, not a C string, so it can carry a
   * NUL. Handing the tail-truncated name to the application would let it match
   * and run "reboot" when the caller authorized something else entirely. */
  static const uint8_t frame[]
      = { 0x0A, 0x0B, 'r', 'e', 'b', 'o', 'o', 't', 0x00, 'e', 'v', 'i', 'l' };
  uint8_t request_id[16];
  make_request_id(request_id, 0x94);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, sizeof(frame), 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND);
}

static void a_probe_whose_payload_is_not_protobuf_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  /* Claims a 32-byte method name in a 4-byte payload. Answering would mean
   * guessing what was asked. */
  static const uint8_t garbage[] = { 0x0A, 0x20, 0xFF, 0xFF };
  uint8_t request_id[16];
  make_request_id(request_id, 0x91);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, garbage, sizeof(garbage), 30);

  assert_int_equal(probe.call_count, 0);
  assert_null(find_phase(fx->mock, "probe-ack:1"));
}

static void a_probe_answered_past_the_connect_budget_sends_no_ack(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  probe.answer = AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
  probe.stall = true;
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  /* One second of connect budget, and the application takes longer than that
   * to decide. The service has stopped waiting, so an acceptance would offer a
   * token for an invocation that no longer exists -- and would hold ready
   * capacity until it timed out. */
  uint8_t request_id[16];
  make_request_id(request_id, 0x92);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 1);

  assert_int_equal(probe.call_count, 1);
  assert_null(find_phase(fx->mock, "probe-ack:1"));

  /* The slot went back, so the next probe is not answered DEVICE_BUSY. */
  probe.stall = false;
  uint8_t next_id[16];
  make_request_id(next_id, 0x93);
  uint8_t ready_id[16];
  probe_and_accept(fx, next_id, "reboot", 300, ready_id);
}

/* ------------------------------------------------------------------------- */
/* exec                                                                      */
/* ------------------------------------------------------------------------- */

static void an_exec_runs_the_method_named_by_its_probe(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xA1);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, ready_id);

  static const uint8_t params[] = "{\"x\":1}";
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, params, sizeof(params) - 1);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);

  /* Neither topic nor exec payload carries the name; it only exists because
   * the probe was remembered. */
  assert_true(rec.fired);
  assert_string_equal(rec.method_name, "reboot");
  assert_int_equal(rec.payload_len, sizeof(params) - 1);
  assert_string_equal(rec.payload, "{\"x\":1}");
  assert_true(rec.fired);
}

static void an_exec_for_a_method_with_no_arguments_still_runs(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xA2);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "ping", 300, ready_id);

  /* proto3 omits an empty params field entirely; that is a call with no
   * arguments, not a malformed exec. */
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);

  assert_true(rec.fired);
  assert_string_equal(rec.method_name, "ping");
  assert_int_equal(rec.payload_len, 0);
}

static void an_exec_with_no_matching_ready_token_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* No probe was ever accepted for this request id. Running it would execute a
   * method this device never agreed to. */
  uint8_t request_id[16];
  make_request_id(request_id, 0xA3);
  uint8_t ready_id[16];
  memset(ready_id, 0xEE, sizeof(ready_id));
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);

  assert_false(rec.fired);
}

static void an_exec_with_the_wrong_ready_id_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xA4);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, ready_id);

  /* Half the execution token. A request id alone is service-generated and
   * therefore guessable from a replayed probe; the ready id is what proves the
   * exec belongs to the acceptance this device actually issued. */
  uint8_t wrong_id[16];
  memcpy(wrong_id, ready_id, sizeof(wrong_id));
  wrong_id[0] = (uint8_t)(wrong_id[0] ^ 0xFF);
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, wrong_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);

  assert_false(rec.fired);

  /* The real token is untouched. */
  exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_true(rec.fired);
}

static void a_redelivered_exec_does_not_run_the_method_twice(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xA5);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, ready_id);

  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_int_equal(rec.fire_count, 1);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);

  /* This is the whole reason for the ready token: QoS 1 may deliver the same
   * exec twice, and rebooting twice because the network stuttered is not an
   * acceptable outcome. */
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_int_equal(rec.fire_count, 1);
}

static void an_exec_whose_payload_is_not_protobuf_leaves_the_token_alive(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xA6);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, ready_id);

  static const uint8_t garbage[] = { 0x0A, 0x40, 0x01 };
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, garbage, sizeof(garbage), 300);
  assert_false(rec.fired);

  /* A corrupt frame says nothing about the token, and the service may still
   * redeliver a good copy of the same exec. */
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_true(rec.fired);
}

static void an_exec_inside_the_safety_margin_is_abandoned_rather_than_started(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xA7);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, ready_id);

  /* The exec sat in the broker until only a second of budget was left, inside
   * the safety margin for a 300 second response timeout. Starting work that
   * cannot get a result back burns the device's time and still leaves the
   * caller waiting, so the token is given up and the service is told why. */
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 1);

  assert_false(rec.fired);

  const az_iot_mock_call* abandon = find_phase(fx->mock, "abandon:1");
  assert_non_null(abandon);
  /* Abandon { ready_id = 1, reason = 2 (INSUFFICIENT_TIME) } */
  assert_int_equal(abandon->payload_len, 20);
  assert_int_equal(abandon->payload[0], 0x0A);
  assert_int_equal(abandon->payload[1], 0x10);
  assert_memory_equal(abandon->payload + 2, ready_id, 16);
  assert_int_equal(abandon->payload[18], 0x10);
  assert_int_equal(abandon->payload[19], 2);
  assert_memory_equal(abandon->correlation_data, request_id, 16);
}

static void a_ready_token_that_is_never_used_is_abandoned(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* No connect budget and no response timeout leaves only the one second
   * minimum safety margin, so the wait is the shortest this client will ever
   * hold a token for. */
  uint8_t request_id[16];
  make_request_id(request_id, 0xA8);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 0, ready_id);

  sleep_past_one_second();

  /* There is no timer behind this: the sweep runs when the next method message
   * arrives, which is when the capacity matters. Without it a token whose exec
   * never came would hold a ready slot for the life of the process. */
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  uint8_t next_id[16];
  make_request_id(next_id, 0xA9);
  uint8_t next_ready_id[16];
  probe_and_accept(fx, next_id, "reboot", 0, next_ready_id);

  const az_iot_mock_call* abandon = find_phase(fx->mock, "abandon:1");
  assert_non_null(abandon);
  assert_memory_equal(abandon->payload + 2, ready_id, 16);
  assert_int_equal(abandon->payload[19], 1); /* READY_WAIT_TIMEOUT */
  assert_memory_equal(abandon->correlation_data, request_id, 16);

  /* And the expired token is gone, so its exec no longer runs anything. */
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_false(rec.fired);
}

/* ------------------------------------------------------------------------- */
/* result                                                                    */
/* ------------------------------------------------------------------------- */

static void respond_publishes_a_protobuf_result_on_the_service_topic(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xB1);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, ready_id);
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  static const uint8_t body[] = "{\"ok\":true}";
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, body, sizeof(body) - 1),
      AZ_IOT_OK);

  const az_iot_mock_call* c = find_phase(fx->mock, "result:1");
  assert_non_null(c);
  assert_int_equal(c->qos, AZ_IOT_MQTT_QOS_1);
  assert_string_equal(c->content_type, "application/protobuf");

  /* Result { status = 200, body }: the status is a protobuf field now, not a
   * user property, and the body is nested rather than being the whole
   * payload. */
  assert_int_equal(c->payload_len, 3 + 2 + (sizeof(body) - 1));
  assert_int_equal(c->payload[0], 0x08);
  assert_int_equal(c->payload[1], 0xC8);
  assert_int_equal(c->payload[2], 0x01);
  assert_int_equal(c->payload[3], 0x12);
  assert_int_equal(c->payload[4], sizeof(body) - 1);
  assert_memory_equal(c->payload + 5, body, sizeof(body) - 1);
}

static void respond_echoes_the_request_id(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xB2);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "ping", 300, ready_id);
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_true(rec.fired);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);

  /* The service pairs the result with the invocation solely by this value, so
   * dropping it would strand the caller until it timed out. */
  const az_iot_mock_call* c = find_phase(fx->mock, "result:1");
  assert_non_null(c);
  assert_int_equal(c->correlation_data_len, 16);
  assert_memory_equal(c->correlation_data, request_id, 16);
}

static void respond_keeps_the_request_when_the_body_does_not_fit(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xB3);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "dump", 300, ready_id);
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_true(rec.fired);

  /* Framing needs a contiguous buffer and this client does not allocate, so an
   * oversized body is refused outright rather than truncated into a Result the
   * service would decode as a shorter, wrong answer. */
  static uint8_t body[AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX + 1];
  memset(body, 'x', sizeof(body));
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  log_capture cap;
  install_capture(&cap, AZ_IOT_LOG_LEVEL_ERROR);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, body, sizeof(body)),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  az_iot_log_set_global_sink(NULL);
  assert_null(find_phase(fx->mock, "result:1"));

  /* Refusing has to leave the request answerable. Nothing was sent and nothing
   * consumed it, so releasing the slot here would destroy a result the
   * application still holds -- over a mistake it could fix in one line. */
  assert_int_equal(cap.count, 1);
  assert_non_null(strstr(cap.last, "shorter"));
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(
          &fx->dm, rec.request, 200, body, AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX),
      AZ_IOT_OK);
  const az_iot_mock_call* sent = find_phase(fx->mock, "result:1");
  assert_non_null(sent);
  assert_true(sent->payload_len > AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX);
}

static void a_broker_rejected_result_is_reported(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;
  run_one_invocation(fx, &rec, 0xB6);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);
  const az_iot_mock_call* sent = find_phase(fx->mock, "result:1");
  assert_non_null(sent);

  /* Publishing only hands the message to the adapter, so AZ_IOT_OK above says
   * nothing about the broker. If it refuses the result, the caller's timeout
   * would otherwise be the only symptom, on a device that believes it
   * answered. */
  log_capture cap;
  install_capture(&cap, AZ_IOT_LOG_LEVEL_ERROR);
  inject_puback(fx, sent->packet_id, AZ_IOT_ERR_MQTT);
  az_iot_log_set_global_sink(NULL);

  assert_int_equal(cap.count, 1);
  assert_int_equal(cap.last_level, AZ_IOT_LOG_LEVEL_ERROR);
  assert_non_null(strstr(cap.last, "result:1"));
  assert_non_null(strstr(cap.last, "rejected"));
}

static void an_accepted_result_is_not_reported_as_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;
  run_one_invocation(fx, &rec, 0xB7);

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);
  const az_iot_mock_call* sent = find_phase(fx->mock, "result:1");
  assert_non_null(sent);

  /* The quiet path has to stay quiet, or the report above is noise nobody
   * will read. */
  log_capture cap;
  install_capture(&cap, AZ_IOT_LOG_LEVEL_WARN);
  inject_puback(fx, sent->packet_id, AZ_IOT_OK);
  az_iot_log_set_global_sink(NULL);
  assert_int_equal(cap.count, 0);
}

static void a_result_that_cannot_be_published_says_why(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;
  run_one_invocation(fx, &rec, 0xB8);

  /* A device that drops off between the handler finishing and the answer going
   * out discards the result. Neither this client nor the connection logged
   * anything, so the only trace was a return code the sample ignores. */
  az_iot_mock_mqtt_client_set_next_result(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_MQTT);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  log_capture cap;
  install_capture(&cap, AZ_IOT_LOG_LEVEL_WARN);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_ERR_MQTT);
  az_iot_log_set_global_sink(NULL);

  assert_int_equal(cap.count, 1);
  assert_non_null(strstr(cap.last, "result:1"));
  assert_non_null(strstr(cap.last, "publish"));
}

static void respond_names_the_argument_it_was_given_wrong(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;
  run_one_invocation(fx, &rec, 0xB9);

  /* This is the one INVALID_ARG that used to return in silence, so a caller
   * that passed a length without a payload had nothing at all to go on. */
  log_capture cap;
  install_capture(&cap, AZ_IOT_LOG_LEVEL_ERROR);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 4),
      AZ_IOT_ERR_INVALID_ARG);
  az_iot_log_set_global_sink(NULL);
  assert_int_equal(cap.count, 1);
  assert_non_null(strstr(cap.last, "payload_len"));

  install_capture(&cap, AZ_IOT_LOG_LEVEL_ERROR);
  az_iot_direct_method_request no_client = rec.request;
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(NULL, no_client, 200, NULL, 0), AZ_IOT_ERR_INVALID_ARG);
  az_iot_log_set_global_sink(NULL);
  assert_int_equal(cap.count, 1);

  /* Neither attempt consumed the request. */
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);
}

static void respond_after_the_response_timeout_sends_nothing(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xB4);
  uint8_t ready_id[16];
  /* A five second response timeout puts the safety margin at its one second
   * floor, so a two second exec budget is admissible. */
  probe_and_accept(fx, request_id, "slow", 5, ready_id);

  /* Then the application takes longer than the budget to answer. */
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 2);
  assert_true(rec.fired);

  sleep_past_one_second();
  sleep_past_one_second();

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_ERR_TIMEOUT);

  /* The service stopped waiting and would discard this anyway; publishing it
   * only spends the device's radio. */
  assert_null(find_phase(fx->mock, "result:1"));
}

/* Occupy every request slot with an invocation the application never answers.
 * A 5 second response timeout puts the safety margin at its 1 second floor, so
 * a 2 second exec budget is admissible. */
static void fill_pool_with_unanswered(fixture* fx, invocation_record* rec, uint32_t exec_budget)
{
  for (int i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    uint8_t request_id[16];
    make_request_id(request_id, (uint8_t)(0xC0 + i));
    uint8_t ready_id[16];
    probe_and_accept(fx, request_id, "dump", 5, ready_id);
    uint8_t exec[64];
    size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
    rec->fired = false;
    inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, exec_budget);
    assert_true(rec->fired);
  }
}

/* Probe once more and report how the client answered: -1 when it accepted. */
static int probe_again(fixture* fx, uint8_t seed)
{
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  uint8_t request_id[16];
  make_request_id(request_id, seed);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "dump", 5);
  inject_dm(fx, DEV_TOPIC, "probe:1", request_id, 16, frame, frame_len, 0);
  const az_iot_mock_call* ack = find_phase(fx->mock, "probe-ack:1");
  assert_non_null(ack);
  return (ack->payload_len == 20) ? -1 : probe_ack_rejected_reason(ack);
}

/* The mqttv5 half of D-2. Without the sweep these four slots are held for the
 * life of the client and the device never accepts another method. */
static void an_unanswered_exec_slot_is_reclaimed_by_a_later_message(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  fill_pool_with_unanswered(fx, &rec, 2);
  assert_int_equal(probe_again(fx, 0x01), AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY);

  sleep_past_one_second();
  sleep_past_one_second();

  /* The sweep runs on message arrival, so this probe both frees the capacity
   * and then uses it. */
  assert_int_equal(probe_again(fx, 0x02), -1);
}

/* Negative control: a sweep that fired regardless of the budget would satisfy
 * the test above just as well. */
static void an_exec_slot_inside_its_budget_is_not_reclaimed(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  fill_pool_with_unanswered(fx, &rec, 300);
  sleep_past_one_second();

  assert_int_equal(probe_again(fx, 0x03), AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY);
}

static void a_reclaimed_exec_slot_names_the_method(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xD1);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "dump", 5, ready_id);
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 2);
  assert_true(rec.fired);

  sleep_past_one_second();
  sleep_past_one_second();

  log_capture cap;
  install_capture(&cap, AZ_IOT_LOG_LEVEL_WARN);
  (void)probe_again(fx, 0x04);
  az_iot_log_set_global_sink(NULL);

  assert_non_null(strstr(cap.all, "reclaiming"));
  assert_non_null(strstr(cap.all, "'dump'"));
}

static void respond_rejects_a_request_this_client_never_handed_out(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* A request names a slot by index and sequence, so one this client never
   * issued has to be rejected on the sequence rather than on identity: the
   * index alone is a perfectly plausible slot. */
  az_iot_direct_method_request forged;
  memset(&forged, 0, sizeof(forged));
  forged._internal.slot = 0u;
  forged._internal.seq = 0xDEADBEEFu;
  forged._internal.profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  log_capture cap;
  install_capture(&cap, AZ_IOT_LOG_LEVEL_ERROR);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, forged, 200, NULL, 0), AZ_IOT_ERR_INVALID_ARG);
  az_iot_log_set_global_sink(NULL);
  assert_null(find_phase(fx->mock, "result:1"));

  assert_int_equal(cap.count, 1);
  assert_non_null(strstr(cap.last, "no longer live"));
}

/* A request naming a slot index past the pool must be rejected before it is
 * used to subscript the pool or the parallel budget arrays. */
static void respond_rejects_a_request_naming_a_slot_out_of_range(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  az_iot_direct_method_request forged;
  memset(&forged, 0, sizeof(forged));
  forged._internal.slot = AZ_IOT_MQTTV5_DM_MAX_CONCURRENT + 7u;
  forged._internal.seq = 1u;
  forged._internal.profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;

  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, forged, 200, NULL, 0), AZ_IOT_ERR_INVALID_ARG);
}

/* A request built by the mqttv3 client must not be answerable here: mqttv3 answers
 * on a $rid, mqttv5 on MQTT v5 correlation data, so this one addresses nothing. */
static void respond_rejects_a_request_from_the_other_generation(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;
  run_one_invocation(fx, &rec, 0xF1);

  az_iot_direct_method_request foreign = rec.request;
  foreign._internal.profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3;

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, foreign, 200, NULL, 0),
      AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);
  assert_null(find_phase(fx->mock, "result:1"));

  /* The refusal must not have consumed the invocation. */
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);
}

/* The sequence counter must never hand out 0, because a zeroed request has to
 * stay un-matchable. Driven by winding the counter to its wrap point. */
static void the_sequence_counter_skips_zero_on_wrap(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  fx->dm._internal.next_seq = 0xFFFFFFFFu;
  run_one_invocation(fx, &rec, 0xF2);

  assert_int_not_equal(rec.request._internal.seq, 0u);
  assert_int_equal(fx->dm._internal.next_seq, 1u);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);

  /* AZ_IOT_CONNECTION_PROFILE_MQTT_V3 is 0 -- "also the absent/null default" --
   * so a zeroed request reads as an mqttv3 one and is refused on the profile
   * before the sequence is ever consulted. Refused either way, and nothing
   * reaches the wire; on mqttv3, where the profile does match, the sequence is
   * what catches it. */
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  az_iot_direct_method_request zeroed;
  memset(&zeroed, 0, sizeof(zeroed));
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, zeroed, 200, NULL, 0),
      AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);
  assert_null(find_phase(fx->mock, "result:1"));
}

static void deinit_tolerates_null(void** state)
{
  (void)state;
  az_iot_mqttv5_direct_method_client_deinit(NULL);
}

static void set_probe_handler_rejects_a_null_client(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(NULL, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

static void unregister_method_validates_its_arguments(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  assert_int_equal(
      az_iot_mqttv5_direct_method_client_unregister_method(NULL, "reboot"), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_unregister_method(&fx->dm, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void responding_twice_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  uint8_t request_id[16];
  make_request_id(request_id, 0xB5);
  uint8_t ready_id[16];
  probe_and_accept(fx, request_id, "reboot", 300, ready_id);
  uint8_t exec[64];
  size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
  inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
  assert_true(rec.fired);

  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);

  /* The slot has been returned to the pool and may already belong to another
   * invocation, so a second answer would reply on someone else's behalf. */
  log_capture cap;
  install_capture(&cap, AZ_IOT_LOG_LEVEL_ERROR);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0),
      AZ_IOT_ERR_INVALID_ARG);
  az_iot_log_set_global_sink(NULL);

  /* Three unrelated mistakes share INVALID_ARG, so the log is the only thing
   * that tells them apart. */
  assert_int_equal(cap.count, 1);
  assert_non_null(strstr(cap.last, "already answered"));
}

/* The alias this handle shape exists to close, on the MQTTv5 side.
 *
 * Answer an invocation, then run enough further invocations that the pool
 * cycles back and another one occupies that slot. With a pointer handle the
 * second answer passed every check and published under the *newer* correlation
 * id, completing a call the application had never seen. */
static void answering_after_the_slot_was_reused_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  run_one_invocation(fx, &rec, 0xE0);
  az_iot_direct_method_request first = rec.request;
  assert_int_equal(az_iot_mqttv5_direct_method_respond(&fx->dm, first, 200, NULL, 0), AZ_IOT_OK);

  /* Cycle the pool right back around to the slot `first` named, leaving these
   * unanswered so that slot is genuinely occupied when the stale answer
   * arrives. Answering them would free every slot and the refusal below would
   * hold for the wrong reason. */
  for (int i = 1; i <= AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    rec.fired = false;
    run_one_invocation(fx, &rec, (uint8_t)(0xE0 + i));
    assert_true(rec.fired);
  }
  az_iot_direct_method_request occupant = rec.request;

  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, first, 200, NULL, 0), AZ_IOT_ERR_INVALID_ARG);
  assert_null(find_phase(fx->mock, "result:1"));

  /* And the invocation that legitimately holds that slot is still answerable:
   * the refusal must not have consumed someone else's request. */
  assert_int_equal(az_iot_mqttv5_direct_method_respond(&fx->dm, occupant, 200, NULL, 0), AZ_IOT_OK);
}

static void in_flight_invocations_use_up_the_probe_capacity(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* Run every slot up to the limit and never answer, so the whole request pool
   * stays held while every ready slot goes back. */
  for (int i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    uint8_t request_id[16];
    make_request_id(request_id, (uint8_t)(0xC0 + i));
    uint8_t ready_id[16];
    probe_and_accept(fx, request_id, "reboot", 300, ready_id);
    uint8_t exec[64];
    size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
    rec.fired = false;
    inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
    assert_true(rec.fired);
  }

  /* The ready pool is empty again, but admitting on that alone would promise an
   * execution there is no request slot left to run -- and would ship the
   * method's parameters to a device that could only abandon them. Answering it
   * here, before the payload moves, is what the probe phase is for. */
  uint8_t overflow_id[16];
  make_request_id(overflow_id, 0xCF);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", overflow_id, 16, frame, frame_len, 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY);

  /* DEVICE_BUSY invites the caller back, so answering one invocation has to
   * actually make room. */
  assert_int_equal(
      az_iot_mqttv5_direct_method_respond(&fx->dm, rec.request, 200, NULL, 0), AZ_IOT_OK);
  uint8_t next_id[16];
  make_request_id(next_id, 0xCE);
  uint8_t next_ready_id[16];
  probe_and_accept(fx, next_id, "reboot", 300, next_ready_id);
}

static void ready_tokens_and_in_flight_invocations_share_one_budget(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  fx->rec = &rec;

  /* Half the budget spent on invocations that are still running. */
  int running = AZ_IOT_MQTTV5_DM_MAX_CONCURRENT / 2;
  for (int i = 0; i < running; ++i)
  {
    uint8_t request_id[16];
    make_request_id(request_id, (uint8_t)(0xE0 + i));
    uint8_t ready_id[16];
    probe_and_accept(fx, request_id, "reboot", 300, ready_id);
    uint8_t exec[64];
    size_t exec_len = build_exec(exec, ready_id, 16, NULL, 0);
    rec.fired = false;
    inject_dm(fx, DEV_TOPIC, "exec:1", request_id, 16, exec, exec_len, 300);
    assert_true(rec.fired);
  }

  /* The rest spent on tokens still waiting for their exec. */
  for (int i = running; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    uint8_t request_id[16];
    make_request_id(request_id, (uint8_t)(0xE0 + i));
    uint8_t ready_id[16];
    probe_and_accept(fx, request_id, "reboot", 300, ready_id);
  }

  /* Neither pool is full on its own, but together they account for every
   * invocation this device has committed to. */
  uint8_t overflow_id[16];
  make_request_id(overflow_id, 0xEF);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", overflow_id, 16, frame, frame_len, 30);

  assert_int_equal(
      probe_ack_rejected_reason(find_phase(fx->mock, "probe-ack:1")),
      AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY);
}

/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------- */

static void a_message_with_no_type_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  /* The phase is the only thing that says whether a message asks a question or
   * authorizes a call, and the two topics no longer differ. Guessing would
   * mean either running an unauthorized method or ignoring a real one. */
  uint8_t request_id[16];
  make_request_id(request_id, 0xD1);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, NULL, request_id, 16, frame, frame_len, 30);

  assert_int_equal(probe.call_count, 0);
  assert_false(rec.fired);
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, SRV_TOPIC));
}

static void a_type_version_this_client_does_not_know_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  /* The version rides in the type for exactly this reason: a later revision may
   * change the payload, so decoding a probe:2 as a probe:1 would read the
   * wrong shape and answer a question that was not asked. */
  uint8_t request_id[16];
  make_request_id(request_id, 0xD2);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:2", request_id, 16, frame, frame_len, 30);
  inject_dm(fx, DEV_TOPIC, "exec:2", request_id, 16, frame, frame_len, 30);

  assert_int_equal(probe.call_count, 0);
  assert_false(rec.fired);
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, SRV_TOPIC));
}

static void a_device_bound_phase_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  /* probe-ack, abandon and result are what this device sends. Seeing one
   * inbound means something is echoing traffic back, not that work arrived. */
  uint8_t request_id[16];
  make_request_id(request_id, 0xD3);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe-ack:1", request_id, 16, NULL, 0, 30);
  inject_dm(fx, DEV_TOPIC, "abandon:1", request_id, 16, NULL, 0, 30);
  inject_dm(fx, DEV_TOPIC, "result:1", request_id, 16, NULL, 0, 30);

  assert_int_equal(probe.call_count, 0);
  assert_false(rec.fired);
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, SRV_TOPIC));
}

static void a_request_id_of_the_wrong_size_is_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  /* The request id is a 16-byte binary UUID and is the only key the ready
   * state is filed under; a short one could collide with a real invocation. */
  static const uint8_t short_id[] = { 0xDE, 0xAD, 0xBE, 0xEF };
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC, "probe:1", short_id, sizeof(short_id), frame, frame_len, 30);
  inject_dm(fx, DEV_TOPIC, "probe:1", NULL, 0, frame, frame_len, 30);

  assert_int_equal(probe.call_count, 0);
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, SRV_TOPIC));
}

static void a_message_below_the_methods_topic_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  /* Delivery is by topic prefix, so a per-method topic still reaches this
   * handler. The protocol has exactly one inbound topic; anything under it is
   * not part of it and must not be answered as though it were. */
  uint8_t request_id[16];
  make_request_id(request_id, 0xD4);
  uint8_t frame[128];
  size_t frame_len = build_probe(frame, "reboot", 300);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, DEV_TOPIC "/reboot", "probe:1", request_id, 16, frame, frame_len, 30);

  assert_int_equal(probe.call_count, 0);
  assert_null(find_call(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, SRV_TOPIC));
}

static void another_feature_on_the_shared_subscription_is_not_dispatched(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  invocation_record rec = { 0 };
  probe_record probe = { 0 };
  fx->rec = &rec;
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, &probe), AZ_IOT_OK);

  /* Right device, wrong feature: twin and methods arrive on the same
   * device-scoped wildcard. */
  uint8_t request_id[16];
  make_request_id(request_id, 0xD5);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
  inject_dm(fx, "ih/ut-device/dev/twin/patch", "probe:1", request_id, 16, NULL, 0, 30);
  inject_dm(fx, "xx/ut-device/dev/methods", "probe:1", request_id, 16, NULL, 0, 30);

  assert_int_equal(probe.call_count, 0);
  assert_false(rec.fired);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_rejects_a_null_client, setup, teardown),
    cmocka_unit_test(init_rejects_a_null_connection),
    cmocka_unit_test(init_against_an_mqtt_v3_connection_is_rejected),
    cmocka_unit_test(a_device_id_too_long_for_the_topics_says_so),
    cmocka_unit_test_setup_teardown(
        init_does_not_subscribe_a_redundant_methods_filter, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_probe_is_accepted_and_answered_with_a_ready_id, setup, teardown),
    cmocka_unit_test_setup_teardown(the_probe_ack_carries_the_protocol_envelope, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_declared_method_is_accepted_without_a_probe_handler, setup, teardown),
    cmocka_unit_test_setup_teardown(an_undeclared_method_is_rejected_and_kept_out, setup, teardown),
    cmocka_unit_test_setup_teardown(an_undeclared_method_costs_no_capacity, setup, teardown),
    cmocka_unit_test_setup_teardown(each_declared_method_runs_its_own_handler, setup, teardown),
    cmocka_unit_test_setup_teardown(
        withdrawing_one_method_leaves_the_others_declared, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_timeout_under_the_declared_minimum_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(exec_admission_counts_the_declared_minimum, setup, teardown),
    cmocka_unit_test_setup_teardown(declaring_a_method_twice_updates_it, setup, teardown),
    cmocka_unit_test_setup_teardown(the_method_list_reports_when_it_is_full, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_method_list_rejects_names_it_could_never_match, setup, teardown),
    cmocka_unit_test_setup_teardown(
        withdrawing_a_method_leaves_its_live_token_alone, setup, teardown),
    cmocka_unit_test_setup_teardown(a_rejected_probe_keeps_no_ready_state, setup, teardown),
    cmocka_unit_test_setup_teardown(each_rejection_reason_reaches_the_wire, setup, teardown),
    cmocka_unit_test_setup_teardown(a_redelivered_probe_is_not_answered_twice, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_probe_beyond_the_ready_capacity_is_rejected_as_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_method_name_past_the_bound_is_rejected_not_truncated, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_method_name_with_an_embedded_nul_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_probe_whose_payload_is_not_protobuf_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_probe_answered_past_the_connect_budget_sends_no_ack, setup, teardown),
    cmocka_unit_test_setup_teardown(an_exec_runs_the_method_named_by_its_probe, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_exec_for_a_method_with_no_arguments_still_runs, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_exec_with_no_matching_ready_token_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(an_exec_with_the_wrong_ready_id_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_redelivered_exec_does_not_run_the_method_twice, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_exec_whose_payload_is_not_protobuf_leaves_the_token_alive, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_exec_inside_the_safety_margin_is_abandoned_rather_than_started, setup, teardown),
    cmocka_unit_test_setup_teardown(a_ready_token_that_is_never_used_is_abandoned, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_publishes_a_protobuf_result_on_the_service_topic, setup, teardown),
    cmocka_unit_test_setup_teardown(respond_echoes_the_request_id, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_keeps_the_request_when_the_body_does_not_fit, setup, teardown),
    cmocka_unit_test_setup_teardown(a_broker_rejected_result_is_reported, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_accepted_result_is_not_reported_as_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(a_result_that_cannot_be_published_says_why, setup, teardown),
    cmocka_unit_test_setup_teardown(respond_names_the_argument_it_was_given_wrong, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_after_the_response_timeout_sends_nothing, setup, teardown),
    cmocka_unit_test_setup_teardown(responding_twice_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_rejects_a_request_from_the_other_generation, setup, teardown),
    cmocka_unit_test_setup_teardown(the_sequence_counter_skips_zero_on_wrap, setup, teardown),
    cmocka_unit_test(deinit_tolerates_null),
    cmocka_unit_test(set_probe_handler_rejects_a_null_client),
    cmocka_unit_test_setup_teardown(unregister_method_validates_its_arguments, setup, teardown),
    cmocka_unit_test_setup_teardown(
        answering_after_the_slot_was_reused_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_rejects_a_request_this_client_never_handed_out, setup, teardown),
    cmocka_unit_test_setup_teardown(
        respond_rejects_a_request_naming_a_slot_out_of_range, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_unanswered_exec_slot_is_reclaimed_by_a_later_message, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_exec_slot_inside_its_budget_is_not_reclaimed, setup, teardown),
    cmocka_unit_test_setup_teardown(a_reclaimed_exec_slot_names_the_method, setup, teardown),
    cmocka_unit_test_setup_teardown(
        in_flight_invocations_use_up_the_probe_capacity, setup, teardown),
    cmocka_unit_test_setup_teardown(
        ready_tokens_and_in_flight_invocations_share_one_budget, setup, teardown),
    cmocka_unit_test_setup_teardown(a_message_with_no_type_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_type_version_this_client_does_not_know_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(a_device_bound_phase_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(a_request_id_of_the_wrong_size_is_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_message_below_the_methods_topic_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(
        another_feature_on_the_shared_subscription_is_not_dispatched, setup, teardown),
  };
  return cmocka_run_group_tests_name("mqttv5_direct_method_client", tests, NULL, NULL);
}
