// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_amqp_test_peer.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <string.h>

static az_amqp_transport_status test_peer_ok(az_amqp_transport* transport)
{
  (void)transport;
  return AZ_AMQP_TRANSPORT_STATUS_OK;
}

static az_amqp_transport_status test_peer_read(
    az_amqp_transport* transport,
    az_span destination,
    size_t* out_bytes)
{
  e2e_amqp_test_peer* peer = (e2e_amqp_test_peer*)az_amqp_transport_get_impl(transport);
  if (peer->answer != NULL)
  {
    peer->answer(peer);
  }
  size_t n = peer->to_client_length - peer->to_client_position;
  if (n > (size_t)az_span_size(destination))
  {
    n = (size_t)az_span_size(destination);
  }
  memcpy(az_span_ptr(destination), peer->to_client + peer->to_client_position, n);
  peer->to_client_position += n;
  if (peer->to_client_position == peer->to_client_length)
  {
    peer->to_client_position = 0;
    peer->to_client_length = 0;
  }
  *out_bytes = n;
  return (n > 0) ? AZ_AMQP_TRANSPORT_STATUS_OK : AZ_AMQP_TRANSPORT_STATUS_WANT_READ;
}

static az_amqp_transport_status test_peer_write(
    az_amqp_transport* transport,
    az_span source,
    size_t* out_bytes)
{
  e2e_amqp_test_peer* peer = (e2e_amqp_test_peer*)az_amqp_transport_get_impl(transport);
  size_t const n = (size_t)az_span_size(source);
  assert_true(peer->from_client_length + n <= sizeof(peer->from_client));
  memcpy(peer->from_client + peer->from_client_length, az_span_ptr(source), n);
  peer->from_client_length += n;
  *out_bytes = n;
  return AZ_AMQP_TRANSPORT_STATUS_OK;
}

static az_amqp_transport_status test_peer_close(az_amqp_transport* transport)
{
  e2e_amqp_test_peer* peer = (e2e_amqp_test_peer*)az_amqp_transport_get_impl(transport);
  if (peer->on_close != NULL)
  {
    peer->on_close(peer);
  }
  return AZ_AMQP_TRANSPORT_STATUS_OK;
}

const az_amqp_transport_vtable e2e_amqp_test_peer_transport_vtable
    = { test_peer_ok, test_peer_ok, test_peer_read, test_peer_write, test_peer_close, NULL };

void e2e_amqp_test_peer_send_header(e2e_amqp_test_peer* peer, uint8_t protocol_id)
{
  const uint8_t header[8] = { 'A', 'M', 'Q', 'P', protocol_id, 1, 0, 0 };
  assert_true(peer->to_client_length + sizeof(header) <= sizeof(peer->to_client));
  memcpy(peer->to_client + peer->to_client_length, header, sizeof(header));
  peer->to_client_length += sizeof(header);
}

void e2e_amqp_test_peer_send_frame(
    e2e_amqp_test_peer* peer,
    uint8_t frame_type,
    az_span performative,
    az_span payload)
{
  size_t const size = 8 + (size_t)az_span_size(performative) + (size_t)az_span_size(payload);
  assert_true(peer->to_client_length + size <= sizeof(peer->to_client));
  uint8_t* p = peer->to_client + peer->to_client_length;
  p[0] = (uint8_t)(size >> 24);
  p[1] = (uint8_t)(size >> 16);
  p[2] = (uint8_t)(size >> 8);
  p[3] = (uint8_t)size;
  p[4] = 2;
  p[5] = frame_type;
  p[6] = 0;
  p[7] = 0;
  memcpy(p + 8, az_span_ptr(performative), (size_t)az_span_size(performative));
  if (az_span_size(payload) > 0)
  {
    memcpy(p + 8 + az_span_size(performative), az_span_ptr(payload), (size_t)az_span_size(payload));
  }
  peer->to_client_length += size;
}
