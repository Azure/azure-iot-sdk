// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter clients with a receive buffer larger than their send buffer, against an
 * in-process server: a PUBLISH whose payload and topic each exceed the send size arrives whole, and
 * a packet of exactly the receive size arrives. */
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

/* The clients' limits: a receive buffer larger than the send buffer. */
#define SEND_BUFFER_SIZE 1024
#define RECEIVE_BUFFER_SIZE 8192
#define MAX_IN_FLIGHT 8

/* Payload and topic each over the send size; together under the receive size. The topic is copied
 * out of the packet, so it also covers its buffer. */
#define PAYLOAD_LEN 2000
#define TOPIC_LEN 1500
#if PAYLOAD_LEN <= SEND_BUFFER_SIZE || TOPIC_LEN <= SEND_BUFFER_SIZE \
    || PAYLOAD_LEN + TOPIC_LEN + 64 > RECEIVE_BUFFER_SIZE
#error "Each must be over the send size, and all under the receive size"
#endif
#define STEPS 500

typedef struct
{
  int connected;
  int disconnected;
  az_iot_result disconnect_status;
  int messages;
  char topic[TOPIC_LEN + 1];
  uint8_t payload[RECEIVE_BUFFER_SIZE];
  size_t payload_len;
} recorder;

/* Copies NUL-terminated @p src into @p dst of @p size bytes; fails when it does not fit. */
static void copy_text(char* dst, size_t size, const char* src)
{
  size_t const len = strlen(src);
  assert_true(len < size);
  memcpy(dst, src, len + 1);
}

static void on_event(const az_iot_mqtt_event* evt, void* ctx)
{
  recorder* r = (recorder*)ctx;
  if (evt->kind == AZ_IOT_MQTT_EVT_CONNECTED && evt->status == AZ_IOT_OK)
  {
    r->connected++;
  }
  else if (evt->kind == AZ_IOT_MQTT_EVT_DISCONNECTED)
  {
    r->disconnected++;
    r->disconnect_status = evt->status;
  }
  else if (evt->kind == AZ_IOT_MQTT_EVT_MESSAGE)
  {
    // Copied: the message is valid only during the callback.
    const az_iot_mqtt_message* m = evt->message;
    r->messages++;
    copy_text(r->topic, sizeof(r->topic), m->topic);
    assert_true(m->payload_len <= sizeof(r->payload));
    memcpy(r->payload, m->payload, m->payload_len);
    r->payload_len = m->payload_len;
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

/* MQTT Variable Byte Integer at p[*i], advancing *i. */
static size_t read_varint(const uint8_t* p, size_t n, size_t* i)
{
  size_t value = 0;
  uint8_t b = 0;
  for (int shift = 0; shift == 0 || (b & 0x80) != 0; shift += 7)
  {
    assert_true(shift < 28 && *i < n);
    b = p[(*i)++];
    value |= (size_t)(b & 0x7F) << shift;
  }
  return value;
}

static size_t write_varint(uint8_t* p, size_t value)
{
  size_t i = 0;
  do
  {
    uint8_t b = (uint8_t)(value & 0x7F);
    value >>= 7;
    p[i++] = (uint8_t)(b | (value > 0 ? 0x80 : 0));
  } while (value > 0);
  return i;
}

/* One whole packet from the client into buf, running the client meanwhile. */
static size_t read_packet(az_iot_mqtt_client* c, test_sock s, uint8_t* buf, size_t size)
{
  size_t have = 0;
  size_t packet_len = 0;
  for (int step = 0; step < STEPS && packet_len == 0; step++)
  {
    if (have >= 2)
    {
      size_t i = 1;
      bool complete_length = false;
      for (size_t k = 1; k < have && k < 5; k++)
      {
        if ((buf[k] & 0x80) == 0)
        {
          complete_length = true;
          break;
        }
      }
      if (complete_length)
      {
        size_t remaining = read_varint(buf, have, &i);
        if (have >= i + remaining)
        {
          packet_len = i + remaining;
          break;
        }
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

/** @brief A client connected to an in-process server. */
typedef struct
{
  az_iot_mqtt_client* client;
  uint8_t* workspace; /**< The client's heap workspace, freed by close_session(); NULL: none. */
  test_sock listener;
  test_sock connection;
} session;

static recorder s_recorder;

/** @brief Connects @p c to an in-process server and accepts it with a CONNACK. */
static session open_session_of(az_iot_mqtt_client* c)
{
  session s;
  s.workspace = NULL;
  uint16_t port = 0;
  test_sock listener = listen_loopback(&port);
  memset(&s_recorder, 0, sizeof(s_recorder));
  recorder* r = &s_recorder;
  assert_non_null(c);
  c->iface->set_inbound_cb(c, on_event, r);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "127.0.0.1";
  o.port = port;
  o.client_id = "sizes";
  o.clean_start = true;
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_OK);

  test_sock conn = TEST_INVALID_SOCK;
  for (int step = 0; step < STEPS && conn == TEST_INVALID_SOCK; step++)
  {
    assert_int_equal(c->iface->process_loop(c, 10), AZ_IOT_OK);
    if (readable(listener))
    {
      conn = accept(listener, NULL, NULL);
    }
  }
  assert_true(conn != TEST_INVALID_SOCK);

  static uint8_t packet[512];
  (void)read_packet(c, conn, packet, sizeof(packet));
  assert_int_equal(packet[0], 0x10); // CONNECT.
  static const uint8_t connack_v3[] = { 0x20, 0x02, 0x00, 0x00 };
  send_all(conn, connack_v3, sizeof(connack_v3));
  for (int step = 0; step < STEPS && r->connected == 0; step++)
  {
    assert_int_equal(c->iface->process_loop(c, 10), AZ_IOT_OK);
  }
  assert_int_equal(r->connected, 1);
  s.client = c;
  s.listener = listener;
  s.connection = conn;
  return s;
}

/** @brief A @p version client built by the adapter in a heap workspace, connected with the
 * limits of this test. */
static session open_session(az_iot_mqtt_version version)
{
  az_iot_mqtt_limits const limits = {
    .send_buffer_size = SEND_BUFFER_SIZE,
    .receive_buffer_size = RECEIVE_BUFFER_SIZE,
    .max_in_flight_messages = MAX_IN_FLIGHT,
    .message_store_size = 0,
    .connect_strings_size = 256,
  };
  size_t const size = az_iot_az_mqtt_get_workspace_size(&limits);
  uint8_t* workspace = (uint8_t*)malloc(size);
  assert_non_null(workspace);
  az_iot_mqtt_client* c = NULL;
  assert_int_equal(
      az_iot_az_mqtt_get_adapter()->init(version, &limits, workspace, size, &c), AZ_IOT_OK);
  session s = open_session_of(c);
  s.workspace = workspace;
  return s;
}

static void close_session(session* s)
{
  (void)s->client->iface->disconnect(s->client);
  s->client->iface->destroy(s->client);
  free(s->workspace);
  test_closesock(s->connection);
  test_closesock(s->listener);
}

/** @brief Runs the client until @p count messages arrived. */
static void run_until_messages(az_iot_mqtt_client* c, int count)
{
  for (int step = 0; step < STEPS && s_recorder.messages < count; step++)
  {
    assert_int_equal(c->iface->process_loop(c, 10), AZ_IOT_OK);
  }
  assert_int_equal(s_recorder.messages, count);
}

static void run_client(az_iot_mqtt_version version)
{
  session s = open_session(version);
  az_iot_mqtt_client* c = s.client;
  test_sock conn = s.connection;
  recorder* r = &s_recorder;

  // QoS 0 PUBLISH.
  static char topic[TOPIC_LEN + 1];
  for (size_t k = 0; k < TOPIC_LEN; k++)
  {
    topic[k] = (char)('a' + k % 26);
  }
  static uint8_t publish[PAYLOAD_LEN + TOPIC_LEN + 64];
  size_t const remaining = 2 + TOPIC_LEN + (size_t)PAYLOAD_LEN;
  size_t i = 0;
  publish[i++] = 0x30;
  i += write_varint(publish + i, remaining);
  publish[i++] = (uint8_t)(TOPIC_LEN >> 8);
  publish[i++] = (uint8_t)(TOPIC_LEN & 0xFF);
  memcpy(publish + i, topic, TOPIC_LEN);
  i += TOPIC_LEN;
  for (size_t k = 0; k < PAYLOAD_LEN; k++)
  {
    publish[i++] = (uint8_t)k;
  }
  send_all(conn, publish, i);
  run_until_messages(c, 1);
  assert_string_equal(r->topic, topic);
  assert_int_equal(r->payload_len, PAYLOAD_LEN);
  for (size_t k = 0; k < PAYLOAD_LEN; k++)
  {
    assert_int_equal(r->payload[k], (uint8_t)k);
  }

  close_session(&s);
}

/** @brief Writes a QoS 0 PUBLISH to topic "t" of @p total bytes into @p out; returns its size. */
static size_t make_publish_of_size(uint8_t* out, size_t total)
{
  // Remaining Length of 2 bytes for every size used here (128..16,383).
  size_t const remaining = total - 3;
  size_t i = 0;
  out[i++] = 0x30;
  assert_int_equal(write_varint(out + i, remaining), 2);
  i += 2;
  out[i++] = 0;
  out[i++] = 1;
  out[i++] = 't';
  memset(out + i, 'p', total - i);
  return total;
}

static void receives_a_packet_of_exactly_the_receive_size(az_iot_mqtt_version version)
{
  session s = open_session(version);
  static uint8_t publish[RECEIVE_BUFFER_SIZE];
  size_t const n = make_publish_of_size(publish, sizeof(publish));
  send_all(s.connection, publish, n);
  run_until_messages(s.client, 1);
  assert_string_equal(s_recorder.topic, "t");
  assert_int_equal(s_recorder.payload_len, n - 6);
  assert_int_equal(s_recorder.disconnected, 0);
  close_session(&s);
}

static void receives_a_packet_of_exactly_the_receive_size_v3(void** state)
{
  (void)state;
  receives_a_packet_of_exactly_the_receive_size(AZ_IOT_MQTT_VERSION_3_1_1);
}

static void receives_more_than_it_can_send_v3(void** state)
{
  (void)state;
  run_client(AZ_IOT_MQTT_VERSION_3_1_1);
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
    cmocka_unit_test(receives_more_than_it_can_send_v3),
    cmocka_unit_test(receives_a_packet_of_exactly_the_receive_size_v3),
  };
  int const failed = cmocka_run_group_tests_name("az_mqtt_adapter_sizes", tests, NULL, NULL);
#if defined(_WIN32)
  WSACleanup();
#endif
  return failed;
}
