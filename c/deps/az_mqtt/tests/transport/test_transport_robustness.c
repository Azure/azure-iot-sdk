// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_transport_robustness.c
 * @brief Bounded connect, receive timeouts and peer resets, against an in-process peer.
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cmocka.h>

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include "az_mqtt_socket_posix.h"
#include "test_server.h"

#if defined(AZ_MQTT_TEST_BACKEND_NONE)
#define TLS_BACKEND 0
#else
#define TLS_BACKEND 1
#endif

static int64_t _now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void _sleep_ms(int ms)
{
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

static az_mqtt_transport* _transport_new(void)
{
  az_mqtt_transport* t = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  assert_non_null(t);
  assert_int_equal(az_mqtt_transport_init(t), AZ_OK);
  return t;
}

static void _transport_free(az_mqtt_transport* t)
{
  az_mqtt_transport_close(t);
  free(t);
}

static az_span _str(char const* s) { return az_span_create_from_str((char*)(uintptr_t)s); }

static az_mqtt_tls_options _trusting(test_server const* s)
{
  az_mqtt_tls_options t = az_mqtt_tls_options_default();
  t.ca_cert_path = _str(test_server_ca_path(s));
  return t;
}

/** @brief Send CONNECT and wait for the CONNACK. */
static void _mqtt_handshake(az_mqtt_transport* t)
{
  static uint8_t connect_v5[] = { 0x10, 0x0E, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x05,
                                  0x02, 0x00, 0x3C, 0x00, 0x00, 0x01, 'c' };
  assert_int_equal(az_mqtt_transport_send(t, AZ_SPAN_FROM_BUFFER(connect_v5)), AZ_OK);
  uint8_t buf[8];
  az_span got = AZ_SPAN_EMPTY;
  assert_int_equal(az_mqtt_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 3000, &got), AZ_OK);
  assert_true(az_span_size(got) > 0);
  assert_int_equal(buf[0], 0x20);
}

// ──────────────────────── Bounded connect ────────────────────

static void tls_handshake_with_a_silent_peer_times_out(void** state)
{
  (void)state;
  if (!TLS_BACKEND)
  {
    skip();
  }
  test_server_options o = test_server_options_default();
  o.behavior = TEST_SERVER_SILENT;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt_transport* t = _transport_new();
  az_mqtt_tls_options tls = _trusting(s);

  assert_int_equal(
      az_mqtt_transport_connect_start(t, _str("127.0.0.1"), test_server_port(s), &tls), AZ_OK);
  for (int i = 0; i < 2; i++)
  {
    int64_t t0 = _now_ms();
    assert_int_equal(az_mqtt_transport_connect_poll(t, 300), AZ_MQTT_ERROR_TIMEOUT);
    int64_t elapsed = _now_ms() - t0;
    assert_true(elapsed >= 250 && elapsed < 1500);
  }
  assert_int_equal(test_server_accepted(s), 1);

  _transport_free(t);
  test_server_stop(s);
}

static void tcp_connect_to_a_full_backlog_times_out(void** state)
{
  (void)state;
  // A listener that never accepts, with its backlog already full, drops SYNs:
  // the same as an address that never answers.
  int l = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(a);
  assert_int_equal(bind(l, (struct sockaddr*)&a, sizeof(a)), 0);
  assert_int_equal(listen(l, 0), 0);
  assert_int_equal(getsockname(l, (struct sockaddr*)&a, &alen), 0);
  int fillers[2];
  for (int i = 0; i < 2; i++)
  {
    fillers[i] = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(fillers[i], F_SETFL, O_NONBLOCK);
    (void)connect(fillers[i], (struct sockaddr*)&a, sizeof(a));
  }
  _sleep_ms(50);

  az_mqtt_transport* t = _transport_new();
  assert_int_equal(az_mqtt_transport_connect_start(t, _str("127.0.0.1"), ntohs(a.sin_port), NULL), AZ_OK);
  int64_t t0 = _now_ms();
  assert_int_equal(az_mqtt_transport_connect_poll(t, 300), AZ_MQTT_ERROR_TIMEOUT);
  assert_true(_now_ms() - t0 < 1500);

  _transport_free(t);
  close(fillers[0]);
  close(fillers[1]);
  close(l);
}

static void connect_poll_completes_a_plain_connection(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.tls = false;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt_transport* t = _transport_new();

  // "localhost" may resolve to ::1 first, where nothing listens: the next address must be tried.
  assert_int_equal(az_mqtt_transport_connect_start(t, _str("localhost"), test_server_port(s), NULL), AZ_OK);
  az_result rc;
  int64_t const deadline = _now_ms() + 3000;
  while ((rc = az_mqtt_transport_connect_poll(t, 50)) == AZ_MQTT_ERROR_TIMEOUT
         && _now_ms() < deadline)
  {
  }
  assert_int_equal(rc, AZ_OK);
  _mqtt_handshake(t);

  _transport_free(t);
  test_server_stop(s);
}

static void connect_to_a_closed_port_fails(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.tls = false;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  uint16_t port = test_server_port(s);
  test_server_stop(s);

  az_mqtt_transport* t = _transport_new();
  az_result rc = az_mqtt_transport_connect(t, _str("127.0.0.1"), port, NULL);
  assert_int_equal(rc, AZ_MQTT_ERROR_TRANSPORT);
  _transport_free(t);
}

// ──────────────────────── I/O ────────────────────────────────

static void _receive_waits_for_the_timeout(bool tls)
{
  test_server_options o = test_server_options_default();
  o.tls = tls;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt_transport* t = _transport_new();
  az_mqtt_tls_options topt = _trusting(s);
  assert_int_equal(
      az_mqtt_transport_connect(t, _str("localhost"), test_server_port(s), tls ? &topt : NULL),
      AZ_OK);
  _mqtt_handshake(t);

  uint8_t buf[16];
  az_span got = AZ_SPAN_EMPTY;
  int64_t t0 = _now_ms();
  assert_int_equal(az_mqtt_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 300, &got), AZ_OK);
  int64_t elapsed = _now_ms() - t0;
  assert_int_equal(az_span_size(got), 0);
  assert_true(elapsed >= 250 && elapsed < 1500);

  assert_int_equal(az_mqtt_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 0, &got), AZ_OK);
  assert_int_equal(az_span_size(got), 0);

  _transport_free(t);
  test_server_stop(s);
}

static void plain_receive_waits_for_the_timeout(void** state)
{
  (void)state;
  _receive_waits_for_the_timeout(false);
}

static void tls_receive_waits_for_the_timeout(void** state)
{
  (void)state;
  if (!TLS_BACKEND)
  {
    skip();
  }
  _receive_waits_for_the_timeout(true);
}

/**
 * @brief Writing to a connection the peer has closed must return an error, not
 * kill the process with SIGPIPE (SIGPIPE is left at its default action here).
 */
static void _peer_close_is_an_error_not_a_signal(bool tls)
{
  test_server_options o = test_server_options_default();
  o.tls = tls;
  o.behavior = TEST_SERVER_CLOSE_AFTER_CONNACK;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt_transport* t = _transport_new();
  az_mqtt_tls_options topt = _trusting(s);
  assert_int_equal(
      az_mqtt_transport_connect(t, _str("localhost"), test_server_port(s), tls ? &topt : NULL),
      AZ_OK);
  _mqtt_handshake(t);
  _sleep_ms(400);

  static uint8_t chunk[16 * 1024];
  az_result rc = AZ_OK;
  for (int i = 0; i < 64 && az_result_succeeded(rc); i++)
  {
    rc = az_mqtt_transport_send(t, AZ_SPAN_FROM_BUFFER(chunk));
  }
  assert_true(az_result_failed(rc));

  uint8_t buf[16];
  az_span got = AZ_SPAN_EMPTY;
  assert_true(az_result_failed(az_mqtt_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 100, &got)));

  _transport_free(t);
  test_server_stop(s);
}

static void plain_peer_close_is_an_error_not_a_signal(void** state)
{
  (void)state;
  _peer_close_is_an_error_not_a_signal(false);
}

static void tls_peer_close_is_an_error_not_a_signal(void** state)
{
  (void)state;
  if (!TLS_BACKEND)
  {
    skip();
  }
  _peer_close_is_an_error_not_a_signal(true);
}

// ──────────────────────── Address fallback ───────────────────

/** @brief A loopback IPv4 address list entry for @p port. */
static void _address(struct addrinfo* ai, struct sockaddr_in* sa, uint16_t port, struct addrinfo* next)
{
  memset(sa, 0, sizeof(*sa));
  sa->sin_family = AF_INET;
  sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa->sin_port = htons(port);
  memset(ai, 0, sizeof(*ai));
  ai->ai_family = AF_INET;
  ai->ai_socktype = SOCK_STREAM;
  ai->ai_addr = (struct sockaddr*)sa;
  ai->ai_addrlen = sizeof(*sa);
  ai->ai_next = next;
}

/** @brief Poll until connected or @p budget_ms; returns the last result. */
static az_result _poll_tcp(_az_mqtt_tcp_connect* c, int budget_ms)
{
  az_result rc;
  int64_t const end = _now_ms() + budget_ms;
  while ((rc = _az_mqtt_tcp_connect_poll(c, 50)) == AZ_MQTT_ERROR_TIMEOUT && _now_ms() < end)
  {
  }
  return rc;
}

static void a_refused_address_falls_back_to_the_next(void** state)
{
  (void)state;
  test_server_options o = test_server_options_default();
  o.tls = false;
  test_server* closed = test_server_start(&o);
  test_server* s = test_server_start(&o);
  assert_non_null(closed);
  assert_non_null(s);
  uint16_t const closed_port = test_server_port(closed);
  test_server_stop(closed);

  struct sockaddr_in sa[2];
  struct addrinfo ai[2];
  _address(&ai[1], &sa[1], test_server_port(s), NULL);
  _address(&ai[0], &sa[0], closed_port, &ai[1]);

  _az_mqtt_tcp_connect c;
  _az_mqtt_tcp_connect_init(&c);
  assert_int_equal(_az_mqtt_tcp_connect_start_addresses(&c, &ai[0]), AZ_OK);
  assert_int_equal(_poll_tcp(&c, 3000), AZ_OK);
  int fd = c.fd;
  _az_mqtt_tcp_connect_init(&c); // Handed over.
  for (int i = 0; i < 40 && test_server_accepted(s) == 0; i++)
  {
    _sleep_ms(25);
  }
  assert_int_equal(test_server_accepted(s), 1);
  close(fd);
  test_server_stop(s);
}

static void an_unanswered_address_falls_back_after_its_attempt_budget(void** state)
{
  (void)state;
  // First address: a listener with a full backlog drops SYNs (never answers).
  int l = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in hole;
  memset(&hole, 0, sizeof(hole));
  hole.sin_family = AF_INET;
  hole.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(hole);
  assert_int_equal(bind(l, (struct sockaddr*)&hole, sizeof(hole)), 0);
  assert_int_equal(listen(l, 0), 0);
  assert_int_equal(getsockname(l, (struct sockaddr*)&hole, &alen), 0);
  int fillers[2];
  for (int i = 0; i < 2; i++)
  {
    fillers[i] = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(fillers[i], F_SETFL, O_NONBLOCK);
    (void)connect(fillers[i], (struct sockaddr*)&hole, sizeof(hole));
  }
  _sleep_ms(50);

  test_server_options o = test_server_options_default();
  o.tls = false;
  test_server* s = test_server_start(&o);
  assert_non_null(s);

  struct sockaddr_in sa[2];
  struct addrinfo ai[2];
  _address(&ai[1], &sa[1], test_server_port(s), NULL);
  _address(&ai[0], &sa[0], ntohs(hole.sin_port), &ai[1]);

  _az_mqtt_tcp_connect c;
  _az_mqtt_tcp_connect_init(&c);
  int64_t t0 = _now_ms();
  assert_int_equal(_az_mqtt_tcp_connect_start_addresses(&c, &ai[0]), AZ_OK);
  assert_int_equal(_poll_tcp(&c, 6000), AZ_OK);
  int64_t elapsed = _now_ms() - t0;
  assert_true(elapsed >= AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS - 100);
  assert_true(elapsed < AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS + 1500);
  close(c.fd);

  test_server_stop(s);
  close(fillers[0]);
  close(fillers[1]);
  close(l);
}

static void tls_receive_of_a_partial_record_returns_at_the_deadline(void** state)
{
  (void)state;
  if (!TLS_BACKEND)
  {
    skip();
  }
  test_server_options o = test_server_options_default();
  o.behavior = TEST_SERVER_PARTIAL_TLS_RECORD;
  test_server* s = test_server_start(&o);
  assert_non_null(s);
  az_mqtt_transport* t = _transport_new();
  az_mqtt_tls_options topt = _trusting(s);
  assert_int_equal(az_mqtt_transport_connect(t, _str("localhost"), test_server_port(s), &topt), AZ_OK);
  _mqtt_handshake(t);
  _sleep_ms(100); // The partial record is now in the socket buffer.

  uint8_t buf[16];
  az_span got = AZ_SPAN_EMPTY;
  int64_t t0 = _now_ms();
  assert_int_equal(az_mqtt_transport_receive(t, AZ_SPAN_FROM_BUFFER(buf), 300, &got), AZ_OK);
  int64_t elapsed = _now_ms() - t0;
  assert_int_equal(az_span_size(got), 0);
  assert_true(elapsed >= 250 && elapsed < 1500);

  _transport_free(t);
  test_server_stop(s);
}

int main(void)
{
  signal(SIGPIPE, SIG_DFL);
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(tls_handshake_with_a_silent_peer_times_out),
    cmocka_unit_test(tcp_connect_to_a_full_backlog_times_out),
    cmocka_unit_test(connect_poll_completes_a_plain_connection),
    cmocka_unit_test(connect_to_a_closed_port_fails),
    cmocka_unit_test(plain_receive_waits_for_the_timeout),
    cmocka_unit_test(tls_receive_waits_for_the_timeout),
    cmocka_unit_test(plain_peer_close_is_an_error_not_a_signal),
    cmocka_unit_test(tls_peer_close_is_an_error_not_a_signal),
    cmocka_unit_test(a_refused_address_falls_back_to_the_next),
    cmocka_unit_test(an_unanswered_address_falls_back_after_its_attempt_budget),
    cmocka_unit_test(tls_receive_of_a_partial_record_returns_at_the_deadline),
  };
  return cmocka_run_group_tests_name("transport_robustness", tests, NULL, NULL);
}
