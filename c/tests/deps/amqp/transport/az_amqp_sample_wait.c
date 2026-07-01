// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include "az_amqp_sample_wait.h"

#include <azure/amqp/az_amqp_common.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#endif

void az_amqp_sample_wait_for_io(
    az_amqp_sample_transport* transport,
    az_amqp_io_interest io_interest,
    int32_t timeout_milliseconds)
{
  int64_t const raw = az_amqp_sample_transport_get_socket(transport);
  if (raw < 0)
  {
    return;
  }

#ifdef _WIN32
  SOCKET const sock = (SOCKET)raw;
#else
  int const sock = (int)raw;
#endif

  fd_set read_fds;
  fd_set write_fds;
  FD_ZERO(&read_fds);
  FD_ZERO(&write_fds);

  if ((io_interest & AZ_AMQP_IO_INTEREST_READ) || io_interest == AZ_AMQP_IO_INTEREST_NONE)
  {
    FD_SET(sock, &read_fds);
  }
  if (io_interest & AZ_AMQP_IO_INTEREST_WRITE)
  {
    FD_SET(sock, &write_fds);
  }

  // Cap an "indefinite" wait at 1s so heartbeats/timeouts stay responsive.
  long const ms = (timeout_milliseconds < 0 || timeout_milliseconds > 1000) ? 1000
                                                                            : timeout_milliseconds;
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;

  (void)select((int)(sock + 1), &read_fds, &write_fds, NULL, &tv);
}
