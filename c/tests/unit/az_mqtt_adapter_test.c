// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter: MQTT 3.1.1 clients, the arguments connect(), publish() and subscribe()
 * refuse, and a CONNECT that cannot be encoded, without a network; PUBACK classification against an
 * in-process server.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
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

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include <az_mqtt/az_mqtt_core.h>

/** @brief @p c is a @p version client, not connected; destroys it. */
static void check_unconnected_client(az_iot_mqtt_client* c, az_iot_mqtt_version version)
{
  assert_non_null(c);
  assert_int_equal(c->iface->version, version);
  assert_int_equal(c->iface->disconnect(c), AZ_IOT_ERR_NOT_CONNECTED);
  az_iot_mqtt_message msg = { 0 };
  msg.topic = "t";
  assert_int_equal(c->iface->publish(c, &msg, NULL), AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
  c->iface->destroy(c);
}

static az_iot_mqtt_connect_options valid_connect(void)
{
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "broker.example";
  o.client_id = "c";
  return o;
}

static const az_iot_mqtt_version k_versions[] = { AZ_IOT_MQTT_VERSION_3_1_1 };
#define K_VERSION_COUNT (sizeof(k_versions) / sizeof(k_versions[0]))

/** @brief Default buffer sizes and requests in flight of these tests' clients. */
#define TEST_SEND_BUFFER_SIZE ((size_t)264u * 1024u)
#define TEST_RECEIVE_BUFFER_SIZE ((size_t)264u * 1024u)
#define TEST_MAX_IN_FLIGHT_MESSAGES 64u

/** @brief The default buffers with a send buffer of @p send_buffer_size bytes, a message store for
 * one packet, and room for the largest connect strings of these tests. */
static az_iot_mqtt_limits client_limits(size_t send_buffer_size)
{
  az_iot_mqtt_limits const limits = {
    .send_buffer_size = send_buffer_size,
    .receive_buffer_size = TEST_RECEIVE_BUFFER_SIZE,
    .max_in_flight_messages = (uint16_t)TEST_MAX_IN_FLIGHT_MESSAGES,
    .message_store_size = send_buffer_size + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD,
    .connect_strings_size = (size_t)512u * 1024u,
  };
  return limits;
}

/** @brief A @p version client the adapter builds with @p limits in a heap workspace, returned in
 * @p out_workspace and freed by destroy_client(). */
static az_iot_mqtt_client* create_client(
    az_iot_mqtt_version version,
    az_iot_mqtt_limits const* limits,
    uint8_t** out_workspace)
{
  size_t const size = az_iot_az_mqtt_get_workspace_size(limits);
  if (size == 0)
  {
    fail_msg("invalid limits");
  }
  *out_workspace = (uint8_t*)malloc(size);
  assert_non_null(*out_workspace);
  az_iot_mqtt_client* c = NULL;
  assert_int_equal(
      az_iot_az_mqtt_get_adapter()->init(version, limits, *out_workspace, size, &c), AZ_IOT_OK);
  return c;
}

/** @brief A @p version client with the default buffers; see create_client(). */
static az_iot_mqtt_client* create_default_client(
    az_iot_mqtt_version version,
    uint8_t** out_workspace)
{
  az_iot_mqtt_limits const limits = client_limits(TEST_SEND_BUFFER_SIZE);
  return create_client(version, &limits, out_workspace);
}

/** @brief Destroys @p c, then frees its @p workspace. */
static void destroy_client(az_iot_mqtt_client* c, uint8_t* workspace)
{
  c->iface->destroy(c);
  free(workspace);
}

static void the_adapter_builds_clients_of_the_version_asked(void** state)
{
  (void)state;
  for (size_t i = 0; i < K_VERSION_COUNT; i++)
  {
    uint8_t* workspace = NULL;
    check_unconnected_client(create_default_client(k_versions[i], &workspace), k_versions[i]);
    free(workspace);
  }
}

static void connect_refuses_malformed_pointer_count_pairs(void** state)
{
  (void)state;
  for (size_t i = 0; i < K_VERSION_COUNT; i++)
  {
    uint8_t* workspace = NULL;
    az_iot_mqtt_client* c = create_default_client(k_versions[i], &workspace);
    az_iot_mqtt_connect_options o = valid_connect();
    o.lwt.topic = "will";
    o.lwt.payload_len = 4; // No payload.
    assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
    destroy_client(c, workspace);
  }
}

static void a_psa_client_key_is_refused_unless_alone_on_mbedtls(void** state)
{
  (void)state;
  uint8_t* workspace = NULL;
  az_iot_mqtt_client* c = create_default_client(AZ_IOT_MQTT_VERSION_3_1_1, &workspace);
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
  destroy_client(c, workspace);
}

static void invalid_qos_and_v3_password_without_user_are_refused(void** state)
{
  (void)state;
  for (size_t i = 0; i < K_VERSION_COUNT; i++)
  {
    uint8_t* workspace = NULL;
    az_iot_mqtt_client* c = create_default_client(k_versions[i], &workspace);
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
    assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
    destroy_client(c, workspace);
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

/** @brief With a send buffer of @p send_buffer_size bytes, connect() accepts @p o; the first
 * process_loop() reports @p expected; then a new connect() is accepted. */
static void connect_fails_before_any_io(
    az_iot_mqtt_connect_options o,
    size_t send_buffer_size,
    az_iot_result expected)
{
  az_iot_mqtt_limits const limits = client_limits(send_buffer_size);
  for (size_t i = 0; i < K_VERSION_COUNT; i++)
  {
    uint8_t* workspace = NULL;
    az_iot_mqtt_client* c = create_client(k_versions[i], &limits, &workspace);
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
    destroy_client(c, workspace);
  }
}

static void a_connect_larger_than_the_send_buffer_fails_the_attempt(void** state)
{
  (void)state;
  // Five fields of 65,000 bytes (each under the MQTT limit): more than the 64 KiB send buffer.
  static char text[65001];
  memset(text, 'a', sizeof(text) - 1);
  az_iot_mqtt_connect_options o = valid_connect();
  o.client_id = text;
  o.username = text;
  o.password = text;
  o.lwt.topic = text;
  o.lwt.payload = (const uint8_t*)text;
  o.lwt.payload_len = sizeof(text) - 1;
  connect_fails_before_any_io(o, (size_t)64u * 1024u, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void a_connect_the_encoder_refuses_fails_the_attempt(void** state)
{
  (void)state;
  static uint8_t will[65536]; // MQTT binary data: at most 65,535 bytes.
  az_iot_mqtt_connect_options o = valid_connect();
  o.lwt.topic = "will";
  o.lwt.payload = will;
  o.lwt.payload_len = sizeof(will);
  connect_fails_before_any_io(o, TEST_SEND_BUFFER_SIZE, AZ_IOT_ERR_INVALID_ARG);
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
  test_sock conn = accept_client(c, listener);
  uint8_t packet[128];
  (void)read_packet(c, conn, packet, sizeof(packet));
  assert_int_equal(packet[0], 0x10); // CONNECT.
  static const uint8_t connack_v3[] = { 0x20, 0x02, 0x00, 0x00 };
  send_all(conn, connack_v3, sizeof(connack_v3));
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

/** @brief A PUBACK becomes the status az_iot_mqtt_puback_result() gives it. MQTT 3.1.1: no code,
 * so only success. */
static void run_pubacks(az_iot_mqtt_version version)
{
  uint16_t port = 0;
  test_sock listener = listen_loopback(&port);
  puback_recorder r = { 0 };
  uint8_t* workspace = NULL;
  az_iot_mqtt_client* c = create_default_client(version, &workspace);
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
  };
  size_t const count = sizeof(cases) / sizeof(cases[0]);
  for (size_t i = 0; i < count; i++)
  {
    uint16_t const pid = publish_qos1(c, conn);
    uint8_t puback[] = { 0x40, 0x02, (uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF), cases[i].code };
    int const before = r.acks;
    send_all(conn, puback, 2u + puback[1]);
    wait_for_ack(c, &r, before);
    assert_int_equal(r.packet_id, pid);
    assert_int_equal(r.status, cases[i].status);
    assert_int_equal(r.protocol_code, cases[i].code);
  }

  (void)c->iface->disconnect(c);
  c->iface->set_inbound_cb(c, NULL, NULL);
  destroy_client(c, workspace);
  test_closesock(conn);
  test_closesock(listener);
}

static void puback_codes_are_classified_v3(void** state)
{
  (void)state;
  run_pubacks(AZ_IOT_MQTT_VERSION_3_1_1);
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
    cmocka_unit_test(the_adapter_builds_clients_of_the_version_asked),
    cmocka_unit_test(connect_refuses_malformed_pointer_count_pairs),
    cmocka_unit_test(a_psa_client_key_is_refused_unless_alone_on_mbedtls),
    cmocka_unit_test(invalid_qos_and_v3_password_without_user_are_refused),
    cmocka_unit_test(a_connect_larger_than_the_send_buffer_fails_the_attempt),
    cmocka_unit_test(a_connect_the_encoder_refuses_fails_the_attempt),
    cmocka_unit_test(puback_codes_are_classified_v3),
  };
  int const failed = cmocka_run_group_tests(tests, NULL, NULL);
#if defined(_WIN32)
  WSACleanup();
#endif
  return failed;
}
