// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* A write by the harness AMQP transport to a peer that has closed the
 * connection returns an error instead of raising SIGPIPE, with SIGPIPE at its
 * default disposition (az_mqtt does not ignore it process-wide). Uses a
 * loopback peer; no Azure resources. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <azure/az_amqp.h>

#include "az_amqp_sample_transport.h"

static int listen_loopback(uint16_t* out_port)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert_true(fd >= 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert_int_equal(bind(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
  assert_int_equal(listen(fd, 1), 0);
  socklen_t len = sizeof(addr);
  assert_int_equal(getsockname(fd, (struct sockaddr*)&addr, &len), 0);
  *out_port = ntohs(addr.sin_port);
  return fd;
}

static void a_write_to_a_closed_peer_fails_without_sigpipe(void** state)
{
  (void)state;
  assert_true(signal(SIGPIPE, SIG_DFL) != SIG_ERR);

  uint16_t port = 0;
  int listener = listen_loopback(&port);

  az_amqp_transport_options options;
  memset(&options, 0, sizeof(options));
  options.host_name = AZ_SPAN_FROM_STR("127.0.0.1");
  options.port = port;
  options.tls_enabled = false;

  static az_amqp_sample_transport storage;
  az_amqp_transport transport;
  assert_int_equal(az_amqp_sample_transport_init(&transport, &storage, &options), AZ_OK);

  az_amqp_transport_status st = transport.vtable->open(&transport);
  for (int i = 0; i < 200 && st != AZ_AMQP_TRANSPORT_STATUS_OK; ++i)
  {
    assert_int_not_equal(st, AZ_AMQP_TRANSPORT_STATUS_ERROR);
    struct timespec pause = { 0, 5000000 };
    (void)nanosleep(&pause, NULL);
    st = transport.vtable->process(&transport);
  }
  assert_int_equal(st, AZ_AMQP_TRANSPORT_STATUS_OK);

  int peer = accept(listener, NULL, NULL);
  assert_true(peer >= 0);
  assert_int_equal(close(peer), 0);
  assert_int_equal(close(listener), 0);

  /* The first write can succeed before the peer's reset arrives; a later one
   * hits the closed connection. Reaching the asserts means no SIGPIPE. */
  uint8_t data[4096];
  memset(data, 0x41, sizeof(data));
  for (int i = 0; i < 200 && st != AZ_AMQP_TRANSPORT_STATUS_ERROR; ++i)
  {
    size_t written = 0;
    st = transport.vtable->write(&transport, AZ_SPAN_FROM_BUFFER(data), &written);
    struct timespec pause = { 0, 5000000 };
    (void)nanosleep(&pause, NULL);
  }
  assert_int_equal(st, AZ_AMQP_TRANSPORT_STATUS_ERROR);

  sigset_t mask;
  sigset_t pending;
  sigemptyset(&mask);
  sigemptyset(&pending);
  assert_int_equal(sigprocmask(SIG_BLOCK, NULL, &mask), 0);
  assert_false(sigismember(&mask, SIGPIPE));
  assert_int_equal(sigpending(&pending), 0);
  assert_false(sigismember(&pending, SIGPIPE));

  (void)transport.vtable->close(&transport);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_write_to_a_closed_peer_fails_without_sigpipe),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
