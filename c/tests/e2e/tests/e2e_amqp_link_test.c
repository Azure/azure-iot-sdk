// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Receiver credit accounting of the vendored test-only AMQP client
 * (c/tests/deps/amqp), driven over an in-memory transport that plays the peer.
 * No network or Azure resources are used. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <string.h>

#include <azure/az_amqp.h>

#define PEER_INITIAL_DELIVERY_COUNT 100u
#define FRAME_TYPE_AMQP 0
#define DESC_OPEN 0x10
#define DESC_BEGIN 0x11
#define DESC_ATTACH 0x12
#define DESC_FLOW 0x13
#define DESC_TRANSFER 0x14

/* ---- in-memory transport -------------------------------------------------- */

typedef struct
{
  uint8_t to_client[8192];
  size_t to_client_len;
  size_t to_client_pos;
  uint8_t from_client[16384];
  size_t from_client_len;
} mem_transport;

static az_amqp_transport_status mem_ok(az_amqp_transport* t)
{
  (void)t;
  return AZ_AMQP_TRANSPORT_STATUS_OK;
}

static az_amqp_transport_status mem_read(az_amqp_transport* t, az_span dst, size_t* out)
{
  mem_transport* m = (mem_transport*)az_amqp_transport_get_impl(t);
  size_t n = m->to_client_len - m->to_client_pos;
  if (n > (size_t)az_span_size(dst))
  {
    n = (size_t)az_span_size(dst);
  }
  memcpy(az_span_ptr(dst), m->to_client + m->to_client_pos, n);
  m->to_client_pos += n;
  *out = n;
  return (n > 0) ? AZ_AMQP_TRANSPORT_STATUS_OK : AZ_AMQP_TRANSPORT_STATUS_WANT_READ;
}

static az_amqp_transport_status mem_write(az_amqp_transport* t, az_span src, size_t* out)
{
  mem_transport* m = (mem_transport*)az_amqp_transport_get_impl(t);
  size_t const n = (size_t)az_span_size(src);
  assert_true(m->from_client_len + n <= sizeof(m->from_client));
  memcpy(m->from_client + m->from_client_len, az_span_ptr(src), n);
  m->from_client_len += n;
  *out = n;
  return AZ_AMQP_TRANSPORT_STATUS_OK;
}

static const az_amqp_transport_vtable k_mem_vtable
    = { mem_ok, mem_ok, mem_read, mem_write, mem_ok, NULL };

/* ---- peer side ------------------------------------------------------------ */

typedef struct
{
  mem_transport mem;
  az_amqp_transport transport;
  az_amqp_connection connection;
  az_amqp_session session;
  az_amqp_link receiver;
  az_amqp_session* session_slots[1];
  az_amqp_link* link_slots[1];
  uint8_t incoming[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
  uint8_t outgoing[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
  uint8_t message_buffer[512];
  uint32_t next_delivery_id;
  uint32_t manual_credit; /* granted from the ATTACHED callback when nonzero */
} fixture;

/** @brief Queues one AMQP frame (performative + optional payload) to the client. */
static void peer_send(fixture* fx, az_span performative, az_span payload)
{
  mem_transport* m = &fx->mem;
  size_t const size = 8 + (size_t)az_span_size(performative) + (size_t)az_span_size(payload);
  assert_true(m->to_client_len + size <= sizeof(m->to_client));
  uint8_t* p = m->to_client + m->to_client_len;
  p[0] = (uint8_t)(size >> 24);
  p[1] = (uint8_t)(size >> 16);
  p[2] = (uint8_t)(size >> 8);
  p[3] = (uint8_t)size;
  p[4] = 2;
  p[5] = FRAME_TYPE_AMQP;
  p[6] = 0;
  p[7] = 0;
  memcpy(p + 8, az_span_ptr(performative), (size_t)az_span_size(performative));
  memcpy(p + 8 + az_span_size(performative), az_span_ptr(payload), (size_t)az_span_size(payload));
  m->to_client_len += size;
}

static void pump(fixture* fx)
{
  for (int i = 0; i < 4; i++)
  {
    az_amqp_connection_process_result r;
    assert_int_equal(AZ_OK, az_amqp_connection_process(&fx->connection, &r));
  }
}

static void peer_open(fixture* fx)
{
  static const uint8_t header[8] = { 'A', 'M', 'Q', 'P', 0, 1, 0, 0 };
  assert_true(fx->mem.to_client_len + sizeof(header) <= sizeof(fx->mem.to_client));
  memcpy(fx->mem.to_client + fx->mem.to_client_len, header, sizeof(header));
  fx->mem.to_client_len += sizeof(header);

  uint8_t buf[64];
  az_amqp_encoder e;
  assert_int_equal(AZ_OK, az_amqp_encoder_init(&e, AZ_SPAN_FROM_BUFFER(buf)));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_descriptor_ulong(&e, DESC_OPEN));
  assert_int_equal(AZ_OK, az_amqp_encoder_begin_list(&e));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_string(&e, AZ_SPAN_FROM_STR("peer")));
  assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
  peer_send(fx, az_amqp_encoder_get_bytes(&e), AZ_SPAN_EMPTY);
}

static void peer_begin(fixture* fx)
{
  uint8_t buf[64];
  az_amqp_encoder e;
  assert_int_equal(AZ_OK, az_amqp_encoder_init(&e, AZ_SPAN_FROM_BUFFER(buf)));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_descriptor_ulong(&e, DESC_BEGIN));
  assert_int_equal(AZ_OK, az_amqp_encoder_begin_list(&e));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_ushort(&e, 0)); /* remote-channel */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 0)); /* next-outgoing-id */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 1000)); /* incoming-window */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 1000)); /* outgoing-window */
  assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
  peer_send(fx, az_amqp_encoder_get_bytes(&e), AZ_SPAN_EMPTY);
}

static void peer_attach(fixture* fx)
{
  uint8_t buf[128];
  az_amqp_encoder e;
  assert_int_equal(AZ_OK, az_amqp_encoder_init(&e, AZ_SPAN_FROM_BUFFER(buf)));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_descriptor_ulong(&e, DESC_ATTACH));
  assert_int_equal(AZ_OK, az_amqp_encoder_begin_list(&e));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_string(&e, AZ_SPAN_FROM_STR("recv")));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 0)); /* handle */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_bool(&e, false)); /* role = sender */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_ubyte(&e, 0)); /* snd-settle-mode */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_ubyte(&e, 0)); /* rcv-settle-mode */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_null(&e)); /* source */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_null(&e)); /* target */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_null(&e)); /* unsettled */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_bool(&e, false)); /* incomplete-unsettled */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, PEER_INITIAL_DELIVERY_COUNT));
  assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
  peer_send(fx, az_amqp_encoder_get_bytes(&e), AZ_SPAN_EMPTY);
}

static void peer_transfer(fixture* fx)
{
  uint8_t perf[64];
  uint8_t tag[4];
  uint32_t const id = fx->next_delivery_id++;
  memcpy(tag, &id, sizeof(tag));
  az_amqp_encoder e;
  assert_int_equal(AZ_OK, az_amqp_encoder_init(&e, AZ_SPAN_FROM_BUFFER(perf)));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_descriptor_ulong(&e, DESC_TRANSFER));
  assert_int_equal(AZ_OK, az_amqp_encoder_begin_list(&e));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 0)); /* handle */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, id)); /* delivery-id */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_binary(&e, AZ_SPAN_FROM_BUFFER(tag)));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 0)); /* message-format */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_bool(&e, false)); /* settled */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_bool(&e, false)); /* more */
  assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));

  az_amqp_message message;
  uint8_t encoded[128];
  az_span body;
  assert_int_equal(AZ_OK, az_amqp_message_init(&message));
  assert_int_equal(AZ_OK, az_amqp_message_set_body_data(&message, AZ_SPAN_FROM_STR("x")));
  assert_int_equal(AZ_OK, az_amqp_message_encode(&message, AZ_SPAN_FROM_BUFFER(encoded), &body));
  peer_send(fx, az_amqp_encoder_get_bytes(&e), body);
}

/**
 * @brief Finds the last FLOW frame the client sent.
 *
 * @return true with its delivery-count and link-credit; false if there is none.
 */
static bool last_client_flow(fixture* fx, uint32_t* out_delivery_count, uint32_t* out_credit)
{
  bool found = false;
  size_t pos = 8; /* skip the protocol header */
  while (pos + 8 <= fx->mem.from_client_len)
  {
    const uint8_t* p = fx->mem.from_client + pos;
    size_t const size
        = ((size_t)p[0] << 24) | ((size_t)p[1] << 16) | ((size_t)p[2] << 8) | (size_t)p[3];
    assert_true(size >= 8 && pos + size <= fx->mem.from_client_len);
    size_t const body_off = (size_t)p[4] * 4;
    if (size > body_off)
    {
      az_amqp_decoder d;
      az_amqp_value described;
      az_amqp_value descriptor;
      az_amqp_decoder perf;
      az_amqp_value list;
      assert_int_equal(
          AZ_OK,
          az_amqp_decoder_init(
              &d, az_span_create((uint8_t*)(uintptr_t)(p + body_off), (int32_t)(size - body_off))));
      assert_int_equal(AZ_OK, az_amqp_decoder_decode(&d, &described));
      assert_int_equal(AZ_OK, az_amqp_value_get_described(&described, &descriptor, &perf));
      if (descriptor.scalar.u64 == DESC_FLOW)
      {
        assert_int_equal(AZ_OK, az_amqp_decoder_decode(&perf, &list));
        az_amqp_decoder fields;
        uint32_t count = 0;
        assert_int_equal(AZ_OK, az_amqp_value_get_list_decoder(&list, &fields, &count));
        assert_true(count >= 7);
        for (uint32_t i = 0; i < 7; i++)
        {
          az_amqp_value v;
          assert_int_equal(AZ_OK, az_amqp_decoder_decode(&fields, &v));
          if (i == 5)
          {
            *out_delivery_count = (uint32_t)v.scalar.u64;
          }
          else if (i == 6)
          {
            *out_credit = (uint32_t)v.scalar.u64;
          }
        }
        found = true;
      }
    }
    pos += size;
  }
  return found;
}

static void on_message(
    az_amqp_link* link,
    az_amqp_message const* message,
    az_amqp_delivery const* delivery,
    void* user_data)
{
  (void)message;
  (void)user_data;
  assert_int_equal(AZ_OK, az_amqp_link_accept(link, delivery->number));
}

static void on_link_state(
    az_amqp_link* link,
    az_amqp_link_state previous_state,
    az_amqp_link_state current_state,
    az_amqp_error_detail const* error,
    void* user_data)
{
  (void)previous_state;
  (void)error;
  fixture* fx = (fixture*)user_data;
  if (current_state == AZ_AMQP_LINK_STATE_ATTACHED && fx->manual_credit > 0)
  {
    assert_int_equal(AZ_OK, az_amqp_link_add_credit(link, fx->manual_credit));
  }
}

/** @brief Opens connection, session and a receiver link against the in-memory peer. */
static void attach_receiver(fixture* fx, uint32_t prefetch_credit)
{
  assert_int_equal(AZ_OK, az_amqp_transport_init(&fx->transport, &k_mem_vtable, &fx->mem));
  az_amqp_connection_storage cs = {
    .incoming_buffer = AZ_SPAN_FROM_BUFFER(fx->incoming),
    .outgoing_buffer = AZ_SPAN_FROM_BUFFER(fx->outgoing),
    .sessions = fx->session_slots,
    .sessions_capacity = 1,
  };
  az_amqp_connection_options co = az_amqp_connection_options_default();
  co.container_id = AZ_SPAN_FROM_STR("client");
  assert_int_equal(AZ_OK, az_amqp_connection_init(&fx->connection, &fx->transport, &cs, &co));
  assert_int_equal(AZ_OK, az_amqp_connection_open(&fx->connection));
  peer_open(fx);
  pump(fx);
  assert_int_equal(AZ_AMQP_CONNECTION_STATE_OPENED, az_amqp_connection_get_state(&fx->connection));

  az_amqp_session_storage ss = { .links = fx->link_slots, .links_capacity = 1 };
  assert_int_equal(AZ_OK, az_amqp_session_init(&fx->session, &fx->connection, &ss, NULL));
  assert_int_equal(AZ_OK, az_amqp_session_begin(&fx->session));
  pump(fx);
  peer_begin(fx);
  pump(fx);
  assert_int_equal(AZ_AMQP_SESSION_STATE_MAPPED, az_amqp_session_get_state(&fx->session));

  az_amqp_link_options lo = az_amqp_link_receiver_options_default(
      AZ_SPAN_FROM_STR("recv"),
      az_amqp_source_from_address(AZ_SPAN_FROM_STR("node")),
      AZ_AMQP_RECEIVER_SETTLE_MODE_FIRST,
      AZ_SPAN_FROM_BUFFER(fx->message_buffer),
      prefetch_credit);
  assert_int_equal(AZ_OK, az_amqp_link_init(&fx->receiver, &fx->session, &lo));
  az_amqp_link_set_message_callback(&fx->receiver, on_message, fx);
  az_amqp_link_set_state_callback(&fx->receiver, on_link_state, fx);
  assert_int_equal(AZ_OK, az_amqp_link_attach(&fx->receiver));
  pump(fx);
  peer_attach(fx);
  pump(fx);
  assert_int_equal(AZ_AMQP_LINK_STATE_ATTACHED, az_amqp_link_get_state(&fx->receiver));
}

/* ---- tests ---------------------------------------------------------------- */

/* Replenishing flows must advance delivery-count by the deliveries received, or
 * the sender computes zero new credit once the initial prefetch is used up. */
static void test_prefetch_replenish_advances_delivery_count(void** state)
{
  (void)state;
  static fixture fx;
  memset(&fx, 0, sizeof(fx));
  attach_receiver(&fx, 4);

  uint32_t delivery_count = 0;
  uint32_t credit = 0;
  assert_true(last_client_flow(&fx, &delivery_count, &credit));
  assert_int_equal(PEER_INITIAL_DELIVERY_COUNT, delivery_count);
  assert_int_equal(4, credit);

  for (uint32_t received = 1; received <= 12; received++)
  {
    peer_transfer(&fx);
    pump(&fx);
    assert_true(last_client_flow(&fx, &delivery_count, &credit));
    /* The sender may keep sending while the limit exceeds what it has sent. */
    assert_true(delivery_count + credit > PEER_INITIAL_DELIVERY_COUNT + received);
  }
  assert_int_equal(PEER_INITIAL_DELIVERY_COUNT + 12, delivery_count);
}

/* Credit granted from the ATTACHED callback must already use the peer's
 * initial-delivery-count. */
static void test_credit_from_attached_callback_uses_peer_initial_count(void** state)
{
  (void)state;
  static fixture fx;
  memset(&fx, 0, sizeof(fx));
  fx.manual_credit = 5;
  attach_receiver(&fx, 0);

  uint32_t delivery_count = 0;
  uint32_t credit = 0;
  assert_true(last_client_flow(&fx, &delivery_count, &credit));
  assert_int_equal(PEER_INITIAL_DELIVERY_COUNT, delivery_count);
  assert_int_equal(5, credit);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_prefetch_replenish_advances_delivery_count),
    cmocka_unit_test(test_credit_from_attached_callback_uses_peer_initial_count),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
