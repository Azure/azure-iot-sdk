// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter: factories and clients, a registered factory destroyed after deinit(), the
 * arguments connect(), publish() and subscribe() refuse, and a CONNECT that cannot be encoded,
 * without a network; PUBACK classification against an in-process server.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET test_sock;
typedef int test_socklen;
#define TEST_INVALID_SOCK INVALID_SOCKET
#define test_closesock(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int test_sock;
typedef socklen_t test_socklen;
#define TEST_INVALID_SOCK (-1)
#define test_closesock(s) close(s)
#endif

#include <cmocka.h>

#include "az_iot_az_mqtt_config.h"
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include <az_mqtt5/az_mqtt5_codec.h>
#include <stdio.h>

static void check_client(az_iot_mqtt_factory* f, az_iot_mqtt_version version)
{
  assert_non_null(f);
  assert_int_equal(f->version, version);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  assert_int_equal(c->iface->version, version);
  assert_int_equal(c->iface->disconnect(c), AZ_IOT_ERR_NOT_CONNECTED);
  az_iot_mqtt_message msg = { 0 };
  msg.topic = "t";
  assert_int_equal(c->iface->publish(c, &msg, NULL), AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
  c->iface->destroy(c);
}

static void factories_create_clients_of_their_version(void** state)
{
  (void)state;
  az_iot_mqtt_factory* v3 = az_iot_az_mqtt_factory_create_v3_1_1();
  check_client(v3, AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_az_mqtt_factory_destroy(v3);
  az_iot_mqtt_factory* v5 = az_iot_az_mqtt_factory_create_v5();
  check_client(v5, AZ_IOT_MQTT_VERSION_5);
  az_iot_az_mqtt_factory_destroy(v5);
  az_iot_az_mqtt_factory_destroy(NULL);
}

static uint8_t g_connection_workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];

static void a_registered_factory_is_destroyed_by_the_application_after_deinit(void** state)
{
  (void)state;
  az_iot_connection_client conn;
  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  options.iothub_hostname = "hub.example";
  options.device_id = "az-mqtt-adapter-test";
  options.workspace = g_connection_workspace;
  options.workspace_size = sizeof(g_connection_workspace);
  assert_int_equal(az_iot_connection_client_init(&conn, &options), AZ_IOT_OK);
  az_iot_mqtt_factory* v5 = az_iot_az_mqtt_factory_create_v5();
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&conn, v5), AZ_IOT_OK);
  az_iot_connection_client_deinit(&conn);
  // Under a leak checker: nothing left, and no double free.
  az_iot_az_mqtt_factory_destroy(v5);
}

static az_iot_mqtt_connect_options valid_connect(void)
{
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "broker.example";
  o.client_id = "c";
  return o;
}

static void connect_refuses_malformed_pointer_count_pairs(void** state)
{
  (void)state;
  az_iot_mqtt_factory* fs[]
      = { az_iot_az_mqtt_factory_create_v3_1_1(), az_iot_az_mqtt_factory_create_v5() };
  for (size_t i = 0; i < 2; i++)
  {
    az_iot_mqtt_client* c = fs[i]->create(fs[i]->factory_ctx);
    assert_non_null(c);
    az_iot_mqtt_connect_options o = valid_connect();
    o.lwt.topic = "will";
    o.lwt.payload_len = 4; // No payload.
    assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
    o = valid_connect();
    o.user_properties_count = 1; // No properties.
    assert_int_equal(
        c->iface->connect(c, &o),
        fs[i]->version == AZ_IOT_MQTT_VERSION_5 ? AZ_IOT_ERR_INVALID_ARG : AZ_IOT_OK);
    if (fs[i]->version == AZ_IOT_MQTT_VERSION_3_1_1)
    {
      (void)c->iface->disconnect(c); // MQTT 3.1.1 ignores user properties: it started.
    }
    c->iface->destroy(c);
    az_iot_az_mqtt_factory_destroy(fs[i]);
  }
}

static void a_psa_client_key_is_refused_unless_alone_on_mbedtls(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v5();
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  az_iot_mqtt_connect_options o = valid_connect();
  o.tls.client_cert_pem = "CERT";
  o.tls.client_key_psa_id = 0x7FFF0001u;
  o.tls.client_key_pem = "KEY";
#if defined(AZ_IOT_AZ_MQTT_MBEDTLS)
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
  o.tls.client_key_pem = NULL;
  o.tls.client_key_uri = "pkcs11:object=k";
  o.tls.crypto_engine_id = "pkcs11";
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
#else
  o.tls.client_key_pem = NULL;
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_NOT_SUPPORTED);
#endif
  c->iface->destroy(c);
  az_iot_az_mqtt_factory_destroy(f);
}

static void invalid_qos_and_v3_password_without_user_are_refused(void** state)
{
  (void)state;
  az_iot_mqtt_factory* fs[]
      = { az_iot_az_mqtt_factory_create_v3_1_1(), az_iot_az_mqtt_factory_create_v5() };
  for (size_t i = 0; i < 2; i++)
  {
    az_iot_mqtt_client* c = fs[i]->create(fs[i]->factory_ctx);
    assert_non_null(c);
    az_iot_mqtt_message msg = { 0 };
    msg.topic = "t";
    msg.qos = (az_iot_mqtt_qos)3;
    assert_int_equal(c->iface->publish(c, &msg, NULL), AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(c->iface->subscribe(c, "t", (az_iot_mqtt_qos)3, NULL), AZ_IOT_ERR_INVALID_ARG);
    az_iot_mqtt_connect_options o = valid_connect();
    o.lwt.topic = "will";
    o.lwt.qos = (az_iot_mqtt_qos)3;
    assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
    o = valid_connect();
    o.password = "p";
    bool const v5 = fs[i]->version == AZ_IOT_MQTT_VERSION_5;
    assert_int_equal(c->iface->connect(c, &o), v5 ? AZ_IOT_OK : AZ_IOT_ERR_INVALID_ARG);
    if (v5)
    {
      (void)c->iface->disconnect(c);
    }
    c->iface->destroy(c);
    az_iot_az_mqtt_factory_destroy(fs[i]);
  }
}

typedef struct
{
  int connected_events;
  az_iot_result status;
} connect_result;

static void record_connect(const az_iot_mqtt_event* evt, void* ctx)
{
  connect_result* r = (connect_result*)ctx;
  if (evt->kind == AZ_IOT_MQTT_EVT_CONNECTED)
  {
    r->connected_events++;
    r->status = evt->status;
  }
}

/** @brief connect() accepts @p o; the first process_loop() reports @p expected; then a new
 * connect() is accepted. */
static void connect_fails_before_any_io(az_iot_mqtt_connect_options o, az_iot_result expected)
{
  az_iot_mqtt_factory* fs[]
      = { az_iot_az_mqtt_factory_create_v3_1_1(), az_iot_az_mqtt_factory_create_v5() };
  for (size_t i = 0; i < 2; i++)
  {
    az_iot_mqtt_client* c = fs[i]->create(fs[i]->factory_ctx);
    assert_non_null(c);
    connect_result r = { 0 };
    c->iface->set_inbound_cb(c, record_connect, &r);
    assert_int_equal(c->iface->connect(c, &o), AZ_IOT_OK);
    assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
    assert_int_equal(r.connected_events, 1);
    assert_int_equal(r.status, expected);
    // The attempt is over: a new one is accepted.
    az_iot_mqtt_connect_options const next = valid_connect();
    assert_int_equal(c->iface->connect(c, &next), AZ_IOT_OK);
    (void)c->iface->disconnect(c);
    c->iface->set_inbound_cb(c, NULL, NULL);
    c->iface->destroy(c);
    az_iot_az_mqtt_factory_destroy(fs[i]);
  }
}

static void a_connect_larger_than_the_send_buffer_fails_the_attempt(void** state)
{
  (void)state;
  // Five fields of 65,000 bytes (each under the MQTT limit): more than the send buffer.
  static char text[65001];
  if (5u * (sizeof(text) - 1) <= AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE)
  {
    skip(); // A send buffer this large holds any CONNECT.
  }
  memset(text, 'a', sizeof(text) - 1);
  az_iot_mqtt_connect_options o = valid_connect();
  o.client_id = text;
  o.username = text;
  o.password = text;
  o.lwt.topic = text;
  o.lwt.payload = (const uint8_t*)text;
  o.lwt.payload_len = sizeof(text) - 1;
  connect_fails_before_any_io(o, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void a_connect_the_encoder_refuses_fails_the_attempt(void** state)
{
  (void)state;
  static uint8_t will[65536]; // MQTT binary data: at most 65,535 bytes.
  az_iot_mqtt_connect_options o = valid_connect();
  o.lwt.topic = "will";
  o.lwt.payload = will;
  o.lwt.payload_len = sizeof(will);
  connect_fails_before_any_io(o, AZ_IOT_ERR_INVALID_ARG);
}

// ──────────────────────── PUBACK, against an in-process server ──────────────────

#define STEPS 500

typedef struct
{
  int connected;
  int acks;
  uint16_t packet_id;
  az_iot_result status;
  int32_t protocol_code;
} puback_recorder;

static void record_puback(const az_iot_mqtt_event* evt, void* ctx)
{
  puback_recorder* r = (puback_recorder*)ctx;
  if (evt->kind == AZ_IOT_MQTT_EVT_CONNECTED && evt->status == AZ_IOT_OK)
  {
    r->connected++;
  }
  else if (evt->kind == AZ_IOT_MQTT_EVT_PUBLISH_ACK)
  {
    r->acks++;
    r->packet_id = evt->packet_id;
    r->status = evt->status;
    r->protocol_code = evt->protocol_code;
  }
}

static bool readable(test_sock s)
{
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  struct timeval tv = { 0, 0 };
  return select((int)s + 1, &set, NULL, NULL, &tv) > 0;
}

static test_sock listen_loopback(uint16_t* port)
{
  test_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert_true(s != TEST_INVALID_SOCK);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert_int_equal(bind(s, (struct sockaddr*)&a, sizeof(a)), 0);
  assert_int_equal(listen(s, 1), 0);
  test_socklen len = (test_socklen)sizeof(a);
  assert_int_equal(getsockname(s, (struct sockaddr*)&a, &len), 0);
  *port = ntohs(a.sin_port);
  return s;
}

static void send_all(test_sock s, const uint8_t* p, size_t n)
{
  while (n > 0)
  {
    int sent = send(s, (const char*)p, (int)n, 0);
    assert_true(sent > 0);
    p += sent;
    n -= (size_t)sent;
  }
}

/** @brief Runs @p c until @p s has a connection to accept; returns it. */
static test_sock accept_client(az_iot_mqtt_client* c, test_sock s)
{
  test_sock conn = TEST_INVALID_SOCK;
  for (int step = 0; step < STEPS && conn == TEST_INVALID_SOCK; step++)
  {
    assert_int_equal(c->iface->process_loop(c, 10), AZ_IOT_OK);
    if (readable(s))
    {
      conn = accept(s, NULL, NULL);
    }
  }
  assert_true(conn != TEST_INVALID_SOCK);
  return conn;
}

/** @brief One whole packet (remaining length under 128) from the client, running it meanwhile. */
static size_t read_packet(az_iot_mqtt_client* c, test_sock s, uint8_t* buf, size_t size)
{
  size_t have = 0;
  size_t packet_len = 0;
  for (int step = 0; step < STEPS && packet_len == 0; step++)
  {
    if (have >= 2)
    {
      assert_true((buf[1] & 0x80) == 0);
      if (have >= 2u + buf[1])
      {
        assert_int_equal(have, 2u + buf[1]);
        packet_len = have;
        break;
      }
    }
    assert_int_equal(c->iface->process_loop(c, 10), AZ_IOT_OK);
    if (readable(s))
    {
      assert_true(have < size);
      int got = recv(s, (char*)buf + have, (int)(size - have), 0);
      assert_true(got > 0);
      have += (size_t)got;
    }
  }
  assert_true(packet_len > 0);
  return packet_len;
}

/** @brief Accepts the client's connection, answers its CONNECT, and waits for CONNECTED. */
static test_sock handshake(az_iot_mqtt_client* c, test_sock listener, puback_recorder* r)
{
  bool const v5 = c->iface->version == AZ_IOT_MQTT_VERSION_5;
  test_sock conn = accept_client(c, listener);
  uint8_t packet[128];
  (void)read_packet(c, conn, packet, sizeof(packet));
  assert_int_equal(packet[0], 0x10); // CONNECT.
  static const uint8_t connack_v3[] = { 0x20, 0x02, 0x00, 0x00 };
  static const uint8_t connack_v5[] = { 0x20, 0x03, 0x00, 0x00, 0x00 };
  send_all(conn, v5 ? connack_v5 : connack_v3, v5 ? sizeof(connack_v5) : sizeof(connack_v3));
  int const before = r->connected;
  for (int step = 0; step < STEPS && r->connected == before; step++)
  {
    assert_int_equal(c->iface->process_loop(c, 10), AZ_IOT_OK);
  }
  assert_int_equal(r->connected, before + 1);
  return conn;
}

/** @brief Publishes at QoS 1 and reads the PUBLISH; returns its packet id. */
static uint16_t publish_qos1(az_iot_mqtt_client* c, test_sock conn)
{
  az_iot_mqtt_message msg = { 0 };
  msg.topic = "t";
  msg.payload = (const uint8_t*)"x";
  msg.payload_len = 1;
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pid), AZ_IOT_OK);
  uint8_t packet[128];
  (void)read_packet(c, conn, packet, sizeof(packet));
  assert_int_equal(packet[0] & 0xF6, 0x32); // PUBLISH, QoS 1.
  size_t const at = 4u + (((size_t)packet[2] << 8) | packet[3]); // After the topic.
  assert_int_equal(((uint16_t)packet[at] << 8) | packet[at + 1], pid);
  return pid;
}

static void wait_for_ack(az_iot_mqtt_client* c, puback_recorder* r, int before)
{
  for (int step = 0; step < STEPS && r->acks == before; step++)
  {
    assert_int_equal(c->iface->process_loop(c, 10), AZ_IOT_OK);
  }
  assert_int_equal(r->acks, before + 1);
}

/** @brief Each PUBACK reason code becomes the status az_iot_mqtt_puback_result() gives it,
 * with the code itself as protocol_code. MQTT 3.1.1: no code, so only success. */
static void run_pubacks(az_iot_mqtt_factory* f)
{
  bool const v5 = f->version == AZ_IOT_MQTT_VERSION_5;
  uint16_t port = 0;
  test_sock listener = listen_loopback(&port);
  puback_recorder r = { 0 };
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  c->iface->set_inbound_cb(c, record_puback, &r);
  az_iot_mqtt_connect_options o = valid_connect();
  o.host = "127.0.0.1";
  o.port = port;
  o.clean_start = true;
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_OK);
  test_sock conn = handshake(c, listener, &r);

  static const struct
  {
    uint8_t code;
    az_iot_result status;
  } cases[] = {
    { 0x00, AZ_IOT_OK },
    { 0x10, AZ_IOT_OK }, // No matching subscribers.
    { 0x87, AZ_IOT_ERR_PUBLISH_REFUSED }, // Not authorized.
    { 0x90, AZ_IOT_ERR_PUBLISH_REFUSED }, // Topic Name invalid.
    { 0x97, AZ_IOT_ERR_BUSY }, // Quota exceeded.
    { 0x80, AZ_IOT_ERR_MQTT }, // Unspecified error.
  };
  size_t const count = v5 ? sizeof(cases) / sizeof(cases[0]) : 1;
  for (size_t i = 0; i < count; i++)
  {
    uint16_t const pid = publish_qos1(c, conn);
    uint8_t puback[] = { 0x40, 0x02, (uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF), cases[i].code };
    if (v5 && cases[i].code != 0) // MQTT 5: 0x00 may be omitted, as here.
    {
      puback[1] = 0x03;
    }
    int const before = r.acks;
    send_all(conn, puback, 2u + puback[1]);
    wait_for_ack(c, &r, before);
    assert_int_equal(r.packet_id, pid);
    assert_int_equal(r.status, cases[i].status);
    assert_int_equal(r.protocol_code, cases[i].code);
  }

  (void)c->iface->disconnect(c);
  c->iface->set_inbound_cb(c, NULL, NULL);
  c->iface->destroy(c);
  test_closesock(conn);
  test_closesock(listener);
}

static void puback_codes_are_classified_v3(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v3_1_1();
  assert_non_null(f);
  run_pubacks(f);
  az_iot_az_mqtt_factory_destroy(f);
}

static void puback_codes_are_classified_v5(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v5();
  assert_non_null(f);
  run_pubacks(f);
  az_iot_az_mqtt_factory_destroy(f);
}

/** @brief Two properties at cursors 0 and 2^31, ending at SIZE_MAX: valid, and past INT32_MAX. */
static az_iot_result wide_cursor_next(
    const void* source,
    size_t source_len,
    size_t* cursor,
    az_iot_mqtt_user_property_view* out)
{
  (void)source;
  (void)source_len;
  size_t const second = (size_t)INT32_MAX + 1u;
  if (*cursor != 0 && *cursor != second)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  bool const first = *cursor == 0;
  out->key = first ? "k1" : "k2";
  out->value = first ? "v1" : "v2";
  out->key_len = 2;
  out->value_len = 2;
  *cursor = first ? second : SIZE_MAX;
  return AZ_IOT_OK;
}

/* The view's cursor is a size_t: a value past INT32_MAX must not fail the publish. */
static void publish_accepts_a_user_property_cursor_past_int32_max(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v5();
  assert_non_null(f);
  uint16_t port = 0;
  test_sock listener = listen_loopback(&port);
  puback_recorder r = { 0 };
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  c->iface->set_inbound_cb(c, record_puback, &r);
  az_iot_mqtt_connect_options o = valid_connect();
  o.host = "127.0.0.1";
  o.port = port;
  o.clean_start = true;
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_OK);
  test_sock conn = handshake(c, listener, &r);

  az_iot_mqtt_message msg = { 0 };
  msg.topic = "t";
  msg.payload = (const uint8_t*)"x";
  msg.payload_len = 1;
  msg.qos = AZ_IOT_MQTT_QOS_0;
  msg.user_properties.next = wide_cursor_next;
  assert_int_equal(c->iface->publish(c, &msg, NULL), AZ_IOT_OK);
  uint8_t packet[128];
  size_t const len = read_packet(c, conn, packet, sizeof(packet));
  static const uint8_t expected[] = {
    0x30, 23,   0x00, 0x01, 't', 18, // PUBLISH QoS 0, topic "t", properties length
    0x26, 0x00, 0x02, 'k',  '1', 0x00, 0x02, 'v', '1', // User Property
    0x26, 0x00, 0x02, 'k',  '2', 0x00, 0x02, 'v', '2', // User Property
    'x',
  };
  assert_int_equal(len, sizeof(expected));
  assert_memory_equal(packet, expected, sizeof(expected));

  (void)c->iface->disconnect(c);
  c->iface->set_inbound_cb(c, NULL, NULL);
  c->iface->destroy(c);
  test_closesock(conn);
  test_closesock(listener);
  az_iot_az_mqtt_factory_destroy(f);
}

/* ------------------------------------------------------------------------- */
/* Vendored az_mqtt codec: User Properties without an array                   */
/* ------------------------------------------------------------------------- */

/* Properties block: Content Type "a", User Property k0=v0, Correlation Data
 * {1}, Subscription Identifier 5, User Property ""="" , Message Expiry 7. */
static const uint8_t k_props_block[] = {
  0x03, 0x00, 0x01, 'a', // Content Type
  0x26, 0x00, 0x02, 'k',  '0',  0x00, 0x02, 'v', '0', // User Property
  0x09, 0x00, 0x01, 0x01, // Correlation Data
  0x0B, 0x05, // Subscription Identifier
  0x26, 0x00, 0x00, 0x00, 0x00, // User Property, both empty
  0x02, 0x00, 0x00, 0x00, 0x07, // Message Expiry Interval
};

static void codec_next_user_property_skips_other_properties(void** state)
{
  (void)state;
  az_span props = az_span_create((uint8_t*)(uintptr_t)k_props_block, sizeof(k_props_block));
  int32_t cursor = 0;
  az_mqtt5_user_property p;

  assert_int_equal(az_mqtt5_codec_next_user_property(props, &cursor, &p), AZ_OK);
  assert_true(az_span_is_content_equal(p.key, AZ_SPAN_FROM_STR("k0")));
  assert_true(az_span_is_content_equal(p.value, AZ_SPAN_FROM_STR("v0")));
  assert_int_equal(az_mqtt5_codec_next_user_property(props, &cursor, &p), AZ_OK);
  assert_int_equal(az_span_size(p.key), 0);
  assert_int_equal(az_span_size(p.value), 0);
  assert_int_equal(az_mqtt5_codec_next_user_property(props, &cursor, &p), AZ_ERROR_ITEM_NOT_FOUND);
  assert_int_equal(cursor, (int32_t)sizeof(k_props_block));
  assert_int_equal(az_mqtt5_codec_next_user_property(props, &cursor, &p), AZ_ERROR_ITEM_NOT_FOUND);
  assert_int_equal(
      az_mqtt5_codec_next_user_property(AZ_SPAN_EMPTY, &(int32_t){ 0 }, &p),
      AZ_ERROR_ITEM_NOT_FOUND);
}

static void codec_next_user_property_reports_malformed_blocks(void** state)
{
  (void)state;
  static const uint8_t truncated[] = { 0x26, 0x00, 0x02, 'k' };
  static const uint8_t unknown_id[] = { 0x7F, 0x00 };
  static const uint8_t no_value[] = { 0x26, 0x00, 0x01, 'k' };
  static const uint8_t bad_utf8[] = { 0x26, 0x00, 0x01, 0xFF, 0x00, 0x00 };
  static const uint8_t truncated_skip[] = { 0x02, 0x00, 0x00 };
  struct
  {
    const uint8_t* bytes;
    int32_t len;
  } const cases[] = {
    { truncated, sizeof(truncated) },
    { unknown_id, sizeof(unknown_id) },
    { no_value, sizeof(no_value) },
    { bad_utf8, sizeof(bad_utf8) },
    { truncated_skip, sizeof(truncated_skip) },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    int32_t cursor = 0;
    az_mqtt5_user_property p;
    assert_int_equal(
        az_mqtt5_codec_next_user_property(
            az_span_create((uint8_t*)(uintptr_t)cases[i].bytes, cases[i].len), &cursor, &p),
        AZ_MQTT_ERROR_MALFORMED_PACKET);
  }
  int32_t past = 5;
  az_mqtt5_user_property p;
  assert_int_equal(
      az_mqtt5_codec_next_user_property(
          az_span_create((uint8_t*)(uintptr_t)truncated, sizeof(truncated)), &past, &p),
      AZ_ERROR_ARG);
}

/* Reader over N generated properties; `extra` makes the second pass differ. */
typedef struct
{
  int32_t count;
  int32_t passes;
  int32_t extra;
  az_result fail_with;
  char keys[40][4];
} reader_ctx;

static az_result read_props(void* context, int32_t* cursor, az_mqtt5_user_property* out)
{
  reader_ctx* r = (reader_ctx*)context;
  if (*cursor == 0)
  {
    r->passes++;
  }
  if (r->fail_with != AZ_OK && *cursor == 3)
  {
    return r->fail_with;
  }
  int32_t const n = r->count + (r->passes > 1 ? r->extra : 0);
  if (*cursor >= n)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  out->key = az_span_create_from_str(r->keys[*cursor]);
  out->value = AZ_SPAN_FROM_STR("v");
  (*cursor)++;
  return AZ_OK;
}

static void codec_encodes_user_properties_from_a_reader(void** state)
{
  (void)state;
  reader_ctx r = { .count = 30, .fail_with = AZ_OK };
  az_mqtt5_user_property array[30];
  for (int32_t i = 0; i < 30; ++i)
  {
    snprintf(r.keys[i], sizeof(r.keys[i]), "k%d", (int)i);
    array[i].key = az_span_create_from_str(r.keys[i]);
    array[i].value = AZ_SPAN_FROM_STR("v");
  }
  az_mqtt5_publish_options from_array = az_mqtt5_publish_options_default();
  from_array.topic = AZ_SPAN_FROM_STR("t");
  from_array.payload = AZ_SPAN_FROM_STR("p");
  from_array.user_properties = array;
  from_array.user_property_count = 30;
  az_mqtt5_publish_options from_reader = from_array;
  from_reader.user_properties = NULL;
  from_reader.user_property_count = 0;
  from_reader.user_property_next = read_props;
  from_reader.user_property_context = &r;

  uint8_t a[512];
  uint8_t b[512];
  az_span sa = AZ_SPAN_FROM_BUFFER(a);
  az_span sb = AZ_SPAN_FROM_BUFFER(b);
  assert_int_equal(az_mqtt5_codec_encode_publish(&sa, &from_array, 0), AZ_OK);
  assert_int_equal(az_mqtt5_codec_encode_publish(&sb, &from_reader, 0), AZ_OK);
  int32_t const len = (int32_t)sizeof(a) - az_span_size(sa);
  assert_int_equal(len, (int32_t)sizeof(b) - az_span_size(sb));
  assert_memory_equal(a, b, (size_t)len);

  // Decoding gives every property back through the raw view, none dropped.
  az_mqtt5_publish_data data = { 0 };
  uint8_t const* body = a + 3; // Fixed header byte and a 2-byte Remaining Length.
  assert_int_equal(
      az_mqtt5_codec_decode_publish(
          az_span_create((uint8_t*)(uintptr_t)body, len - 3), (uint8_t)(a[0] & 0x0F), &data),
      AZ_OK);
  assert_int_equal(data.user_property_count, 0); // No array given: the old path keeps none.
  int32_t cursor = 0;
  int32_t seen = 0;
  az_mqtt5_user_property p;
  while (az_mqtt5_codec_next_user_property(data.properties, &cursor, &p) == AZ_OK)
  {
    assert_true(az_span_is_content_equal(p.key, array[seen].key));
    seen++;
  }
  assert_int_equal(seen, 30);
}

static void codec_refuses_a_reader_that_changes_or_fails(void** state)
{
  (void)state;
  reader_ctx r = { .count = 4, .extra = 1, .fail_with = AZ_OK };
  for (int32_t i = 0; i < 40; ++i)
  {
    snprintf(r.keys[i], sizeof(r.keys[i]), "k%d", (int)i);
  }
  az_mqtt5_publish_options o = az_mqtt5_publish_options_default();
  o.topic = AZ_SPAN_FROM_STR("t");
  o.user_property_next = read_props;
  o.user_property_context = &r;
  uint8_t buf[256];
  az_span s = AZ_SPAN_FROM_BUFFER(buf);
  // More on the write pass than when sized.
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_ARG);

  // Fewer.
  r.passes = 0;
  r.count = 5;
  r.extra = -1;
  s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_ARG);

  // A reader error is the encode's result.
  r.passes = 0;
  r.extra = 0;
  r.fail_with = AZ_ERROR_NOT_SUPPORTED;
  s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_NOT_SUPPORTED);

  // A result that is neither AZ_OK, the end, nor an error is refused, not taken as the end.
  // (az_result)0 is such a result: AZ_OK is 0x10000.
  r.passes = 0;
  r.fail_with = (az_result)0;
  s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_ARG);

  // Too many for the buffer: not enough space, not a cut.
  r.fail_with = AZ_OK;
  r.passes = 0;
  r.count = 40;
  uint8_t small[64];
  s = AZ_SPAN_FROM_BUFFER(small);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_NOT_ENOUGH_SPACE);
}

/* Reader of 2047 properties with 65,535-byte keys and values: 268,310,525 bytes, under the
 * Variable Byte Integer maximum (268,435,455) on its own. */
static char s_big[65535];

static az_result read_big(void* context, int32_t* cursor, az_mqtt5_user_property* out)
{
  (void)context;
  if (*cursor >= 2047)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  out->key = az_span_create((uint8_t*)s_big, (int32_t)sizeof(s_big));
  out->value = out->key;
  (*cursor)++;
  return AZ_OK;
}

static void codec_refuses_a_remaining_length_past_the_vbi_maximum(void** state)
{
  (void)state;
  az_mqtt5_publish_options o = az_mqtt5_publish_options_default();
  o.topic = AZ_SPAN_FROM_STR("t");
  o.user_property_next = read_big;
  // Never read: the encode fails on the size alone.
  o.payload = az_span_create((uint8_t*)s_big, 200000);
  uint8_t buf[64];
  az_span s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_NOT_ENOUGH_SPACE);
}

/* Span sizes whose sum overflows int32_t: refused, not wrapped. Never read. */
static az_result read_huge(void* context, int32_t* cursor, az_mqtt5_user_property* out)
{
  (void)context;
  if (*cursor >= 1)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  out->key = az_span_create((uint8_t*)s_big, INT32_MAX);
  out->value = out->key;
  (*cursor)++;
  return AZ_OK;
}

static void codec_refuses_property_sizes_that_overflow(void** state)
{
  (void)state;
  az_mqtt5_publish_options o = az_mqtt5_publish_options_default();
  o.topic = AZ_SPAN_FROM_STR("t");
  o.user_property_next = read_huge;
  uint8_t buf[64];
  az_span s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_NOT_ENOUGH_SPACE);
}

/* Native property spans whose sizes overflow int32_t together: refused, not wrapped. Never read. */
static void codec_refuses_native_property_sizes_that_overflow(void** state)
{
  (void)state;
  az_mqtt5_publish_options o = az_mqtt5_publish_options_default();
  o.topic = AZ_SPAN_FROM_STR("t");
  o.content_type = az_span_create((uint8_t*)s_big, INT32_MAX);
  o.correlation_data = az_span_create((uint8_t*)s_big, INT32_MAX);
  uint8_t buf[64];
  az_span s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_NOT_ENOUGH_SPACE);
}

/* A Topic Name over 65,535 bytes is refused before it is read or sized. Never read. */
static void codec_refuses_an_oversized_topic(void** state)
{
  (void)state;
  az_mqtt5_publish_options o = az_mqtt5_publish_options_default();
  o.topic = az_span_create((uint8_t*)s_big, INT32_MAX);
  uint8_t buf[64];
  az_span s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_ARG);
}

/* A reader returning a property without increasing the cursor would loop forever. */
static az_result read_stuck(void* context, int32_t* cursor, az_mqtt5_user_property* out)
{
  (void)context;
  (void)cursor;
  out->key = AZ_SPAN_FROM_STR("k");
  out->value = AZ_SPAN_FROM_STR("v");
  return AZ_OK;
}

static void codec_refuses_a_reader_that_does_not_advance(void** state)
{
  (void)state;
  az_mqtt5_publish_options o = az_mqtt5_publish_options_default();
  o.topic = AZ_SPAN_FROM_STR("t");
  o.user_property_next = read_stuck;
  uint8_t buf[64];
  az_span s = AZ_SPAN_FROM_BUFFER(buf);
  assert_int_equal(az_mqtt5_codec_encode_publish(&s, &o, 0), AZ_ERROR_ARG);
}

int main(void)
{
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
  {
    return 1;
  }
#endif
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(factories_create_clients_of_their_version),
    cmocka_unit_test(codec_next_user_property_skips_other_properties),
    cmocka_unit_test(codec_next_user_property_reports_malformed_blocks),
    cmocka_unit_test(codec_encodes_user_properties_from_a_reader),
    cmocka_unit_test(codec_refuses_a_reader_that_changes_or_fails),
    cmocka_unit_test(codec_refuses_a_remaining_length_past_the_vbi_maximum),
    cmocka_unit_test(codec_refuses_property_sizes_that_overflow),
    cmocka_unit_test(codec_refuses_native_property_sizes_that_overflow),
    cmocka_unit_test(codec_refuses_an_oversized_topic),
    cmocka_unit_test(codec_refuses_a_reader_that_does_not_advance),
    cmocka_unit_test(a_registered_factory_is_destroyed_by_the_application_after_deinit),
    cmocka_unit_test(connect_refuses_malformed_pointer_count_pairs),
    cmocka_unit_test(a_psa_client_key_is_refused_unless_alone_on_mbedtls),
    cmocka_unit_test(invalid_qos_and_v3_password_without_user_are_refused),
    cmocka_unit_test(a_connect_larger_than_the_send_buffer_fails_the_attempt),
    cmocka_unit_test(a_connect_the_encoder_refuses_fails_the_attempt),
    cmocka_unit_test(puback_codes_are_classified_v3),
    cmocka_unit_test(puback_codes_are_classified_v5),
    cmocka_unit_test(publish_accepts_a_user_property_cursor_past_int32_max),
  };
  int const failed = cmocka_run_group_tests(tests, NULL, NULL);
#if defined(_WIN32)
  WSACleanup();
#endif
  return failed;
}
