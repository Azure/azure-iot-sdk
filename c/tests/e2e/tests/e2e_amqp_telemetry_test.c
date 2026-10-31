// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The telemetry watcher (service/e2e_amqp.c) against an in-memory peer that plays Event Hubs:
 * SASL, CBS authorization, receiver attach, detach and transfers with sequence-number
 * annotations. Covers replay dropping, the reconnect after a detached receiver and the setup
 * deadline. No network or Azure resources are used. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "e2e_amqp.h"
#include "e2e_amqp_test_peer.h"

#define DESC_SASL_MECHANISMS 0x40
#define DESC_SASL_INIT 0x41
#define DESC_SASL_OUTCOME 0x44
#define DESC_OPEN 0x10
#define DESC_BEGIN 0x11
#define DESC_ATTACH 0x12
#define DESC_FLOW 0x13
#define DESC_TRANSFER 0x14
#define DESC_DETACH 0x16

#define PEER_MAX_LINKS 8
#define PEER_FIELDS_MAX 10
#define SETUP_CALLS_MAX 50

/* ---- a peer that plays Event Hubs ----------------------------------------- */

typedef struct
{
  e2e_amqp_test_peer io; /* first: the transport's impl pointer is also the peer */
  size_t handled; /* bytes of io.from_client already answered */
  char link_names[PEER_MAX_LINKS][32]; /* by the client's link handle */
  const char* unanswered_attach; /* a link whose attach is never answered, or NULL */
  const char* refused_attach; /* a link whose attach is answered, then detached, or NULL */
  uint32_t next_delivery_id;
  int connections_opened;
} event_hubs_peer;

static void peer_send(
    event_hubs_peer* peer,
    uint8_t frame_type,
    az_amqp_encoder* e,
    az_span payload)
{
  e2e_amqp_test_peer_send_frame(&peer->io, frame_type, az_amqp_encoder_get_bytes(e), payload);
}

static void peer_begin_performative(az_amqp_encoder* e, az_span buffer, uint64_t descriptor)
{
  assert_int_equal(AZ_OK, az_amqp_encoder_init(e, buffer));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_descriptor_ulong(e, descriptor));
  assert_int_equal(AZ_OK, az_amqp_encoder_begin_list(e));
}

static int peer_find_link(const event_hubs_peer* peer, const char* name)
{
  for (int h = 0; h < PEER_MAX_LINKS; h++)
  {
    if (strcmp(peer->link_names[h], name) == 0)
    {
      return h;
    }
  }
  return -1;
}

static void peer_send_message(event_hubs_peer* peer, int handle, const az_amqp_message* message)
{
  uint8_t encoded[2048];
  az_span bytes;
  assert_int_equal(AZ_OK, az_amqp_message_encode(message, AZ_SPAN_FROM_BUFFER(encoded), &bytes));
  uint8_t buffer[128];
  uint8_t tag[4];
  uint32_t const id = peer->next_delivery_id++;
  memcpy(tag, &id, sizeof(tag));
  az_amqp_encoder e;
  peer_begin_performative(&e, AZ_SPAN_FROM_BUFFER(buffer), DESC_TRANSFER);
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, (uint32_t)handle));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, id));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_binary(&e, AZ_SPAN_FROM_BUFFER(tag)));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 0)); /* message-format */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_bool(&e, false)); /* settled */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_bool(&e, false)); /* more */
  assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
  peer_send(peer, E2E_AMQP_TEST_PEER_FRAME_TYPE_AMQP, &e, bytes);
}

static void peer_send_detach(event_hubs_peer* peer, int handle)
{
  uint8_t buffer[32];
  az_amqp_encoder e;
  peer_begin_performative(&e, AZ_SPAN_FROM_BUFFER(buffer), DESC_DETACH);
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, (uint32_t)handle));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_bool(&e, true)); /* closed */
  assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
  peer_send(peer, E2E_AMQP_TEST_PEER_FRAME_TYPE_AMQP, &e, AZ_SPAN_EMPTY);
}

/** @brief Answers a client ATTACH; grants credit when the client sends on the link. */
static void peer_answer_attach(event_hubs_peer* peer, const az_amqp_value* fields)
{
  uint32_t const handle = (uint32_t)fields[1].scalar.u64;
  bool const client_receives = fields[2].scalar.boolean;
  assert_true(handle < PEER_MAX_LINKS);
  int const name_length = az_span_size(fields[0].payload);
  assert_true(name_length < (int)sizeof(peer->link_names[0]));
  memcpy(peer->link_names[handle], az_span_ptr(fields[0].payload), (size_t)name_length);
  peer->link_names[handle][name_length] = '\0';
  const char* name = peer->link_names[handle];
  if (peer->unanswered_attach != NULL && strcmp(name, peer->unanswered_attach) == 0)
  {
    return;
  }

  uint8_t buffer[128];
  az_amqp_encoder e;
  peer_begin_performative(&e, AZ_SPAN_FROM_BUFFER(buffer), DESC_ATTACH);
  assert_int_equal(AZ_OK, az_amqp_encoder_append_string(&e, fields[0].payload));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, handle));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_bool(&e, !client_receives)); /* role */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_ubyte(&e, 0)); /* snd-settle-mode */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_ubyte(&e, 0)); /* rcv-settle-mode */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_null(&e)); /* source */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_null(&e)); /* target */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_null(&e)); /* unsettled */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_bool(&e, false)); /* incomplete-unsettled */
  assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 0)); /* initial-delivery-count */
  assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
  peer_send(peer, E2E_AMQP_TEST_PEER_FRAME_TYPE_AMQP, &e, AZ_SPAN_EMPTY);

  if (peer->refused_attach != NULL && strcmp(name, peer->refused_attach) == 0)
  {
    peer_send_detach(peer, (int)handle);
  }
  else if (!client_receives)
  {
    peer_begin_performative(&e, AZ_SPAN_FROM_BUFFER(buffer), DESC_FLOW);
    assert_int_equal(AZ_OK, az_amqp_encoder_append_null(&e)); /* next-incoming-id */
    assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 5000)); /* incoming-window */
    assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 0)); /* next-outgoing-id */
    assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 5000)); /* outgoing-window */
    assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, handle));
    assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 0)); /* delivery-count */
    assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 100)); /* link-credit */
    assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
    peer_send(peer, E2E_AMQP_TEST_PEER_FRAME_TYPE_AMQP, &e, AZ_SPAN_EMPTY);
  }
}

/** @brief Accepts every CBS put-token request with status 200. */
static void peer_answer_put_token(event_hubs_peer* peer)
{
  int const reply_handle = peer_find_link(peer, "cbs-receiver");
  assert_true(reply_handle >= 0);
  uint8_t properties[64];
  az_amqp_encoder e;
  assert_int_equal(AZ_OK, az_amqp_encoder_init(&e, AZ_SPAN_FROM_BUFFER(properties)));
  assert_int_equal(AZ_OK, az_amqp_encoder_begin_map(&e));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_string(&e, AZ_SPAN_FROM_STR("status-code")));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_int(&e, 200));
  assert_int_equal(AZ_OK, az_amqp_encoder_end_map(&e));
  az_amqp_message reply;
  assert_int_equal(AZ_OK, az_amqp_message_init(&reply));
  assert_int_equal(
      AZ_OK, az_amqp_message_set_application_properties(&reply, az_amqp_encoder_get_bytes(&e)));
  assert_int_equal(AZ_OK, az_amqp_message_set_body_data(&reply, AZ_SPAN_FROM_STR("ok")));
  peer_send_message(peer, reply_handle, &reply);
}

/** @brief Answers one client frame; frames the client needs no answer to are ignored. */
static void peer_answer_frame(
    event_hubs_peer* peer,
    uint8_t frame_type,
    const uint8_t* body,
    size_t size)
{
  if (size == 0)
  {
    return; /* heartbeat */
  }
  az_amqp_decoder decoder;
  az_amqp_value described;
  az_amqp_value descriptor;
  az_amqp_decoder performative;
  az_amqp_value list;
  az_amqp_decoder list_decoder;
  uint32_t count = 0;
  assert_int_equal(
      AZ_OK,
      az_amqp_decoder_init(&decoder, az_span_create((uint8_t*)(uintptr_t)body, (int32_t)size)));
  assert_int_equal(AZ_OK, az_amqp_decoder_decode(&decoder, &described));
  assert_int_equal(AZ_OK, az_amqp_value_get_described(&described, &descriptor, &performative));
  assert_int_equal(AZ_OK, az_amqp_decoder_decode(&performative, &list));
  assert_int_equal(AZ_OK, az_amqp_value_get_list_decoder(&list, &list_decoder, &count));
  az_amqp_value fields[PEER_FIELDS_MAX];
  for (uint32_t i = 0; i < count && i < PEER_FIELDS_MAX; i++)
  {
    assert_int_equal(AZ_OK, az_amqp_decoder_decode(&list_decoder, &fields[i]));
  }

  uint8_t buffer[64];
  az_amqp_encoder e;
  switch (frame_type == E2E_AMQP_TEST_PEER_FRAME_TYPE_SASL ? descriptor.scalar.u64 | 0x100
                                                           : descriptor.scalar.u64)
  {
    case DESC_SASL_INIT | 0x100:
      peer_begin_performative(&e, AZ_SPAN_FROM_BUFFER(buffer), DESC_SASL_OUTCOME);
      assert_int_equal(AZ_OK, az_amqp_encoder_append_ubyte(&e, 0)); /* ok */
      assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
      peer_send(peer, E2E_AMQP_TEST_PEER_FRAME_TYPE_SASL, &e, AZ_SPAN_EMPTY);
      break;
    case DESC_OPEN:
      peer->connections_opened++;
      peer_begin_performative(&e, AZ_SPAN_FROM_BUFFER(buffer), DESC_OPEN);
      assert_int_equal(AZ_OK, az_amqp_encoder_append_string(&e, AZ_SPAN_FROM_STR("peer")));
      assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
      peer_send(peer, E2E_AMQP_TEST_PEER_FRAME_TYPE_AMQP, &e, AZ_SPAN_EMPTY);
      break;
    case DESC_BEGIN:
      peer_begin_performative(&e, AZ_SPAN_FROM_BUFFER(buffer), DESC_BEGIN);
      assert_int_equal(AZ_OK, az_amqp_encoder_append_ushort(&e, 0)); /* remote-channel */
      assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 0)); /* next-outgoing-id */
      assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 5000)); /* incoming-window */
      assert_int_equal(AZ_OK, az_amqp_encoder_append_uint(&e, 5000)); /* outgoing-window */
      assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
      peer_send(peer, E2E_AMQP_TEST_PEER_FRAME_TYPE_AMQP, &e, AZ_SPAN_EMPTY);
      break;
    case DESC_ATTACH:
      assert_true(count >= 3);
      peer_answer_attach(peer, fields);
      break;
    case DESC_TRANSFER:
      if ((uint32_t)peer_find_link(peer, "cbs-sender") == (uint32_t)fields[0].scalar.u64)
      {
        peer_answer_put_token(peer);
      }
      break;
    default:
      break;
  }
}

/** @brief Answers the protocol headers and frames the client wrote since the last call. */
static void peer_answer(e2e_amqp_test_peer* io)
{
  event_hubs_peer* peer = (event_hubs_peer*)io;
  for (;;)
  {
    const uint8_t* p = io->from_client + peer->handled;
    size_t const available = io->from_client_length - peer->handled;
    if (available < 8)
    {
      break;
    }
    if (memcmp(p, "AMQP", 4) == 0)
    {
      e2e_amqp_test_peer_send_header(io, p[4]);
      if (p[4] == E2E_AMQP_TEST_PEER_PROTOCOL_ID_SASL)
      {
        uint8_t buffer[64];
        az_amqp_encoder e;
        peer_begin_performative(&e, AZ_SPAN_FROM_BUFFER(buffer), DESC_SASL_MECHANISMS);
        assert_int_equal(AZ_OK, az_amqp_encoder_append_symbol(&e, AZ_SPAN_FROM_STR("ANONYMOUS")));
        assert_int_equal(AZ_OK, az_amqp_encoder_end_list(&e));
        peer_send(peer, E2E_AMQP_TEST_PEER_FRAME_TYPE_SASL, &e, AZ_SPAN_EMPTY);
      }
      peer->handled += 8;
      continue;
    }
    size_t const size
        = ((size_t)p[0] << 24) | ((size_t)p[1] << 16) | ((size_t)p[2] << 8) | (size_t)p[3];
    size_t const body_offset = (size_t)p[4] * 4;
    assert_true(size >= 8 && body_offset >= 8 && body_offset <= size);
    if (available < size)
    {
      break;
    }
    peer_answer_frame(peer, p[5], p + body_offset, size - body_offset);
    peer->handled += size;
  }
  if (peer->handled == io->from_client_length)
  {
    peer->handled = 0;
    io->from_client_length = 0;
  }
}

/** @brief Forgets the closed connection, as a service does when the socket drops. */
static void peer_on_close(e2e_amqp_test_peer* io)
{
  event_hubs_peer* peer = (event_hubs_peer*)io;
  peer->handled = 0;
  io->from_client_length = 0;
  io->to_client_length = 0;
  io->to_client_position = 0;
  memset(peer->link_names, 0, sizeof(peer->link_names));
  peer->next_delivery_id = 0;
}

/** @brief Sends an event with @p body on @p partition, annotated with @p sequence_number. */
static void peer_send_event(
    event_hubs_peer* peer,
    int partition,
    int64_t sequence_number,
    const char* body)
{
  char name[32];
  (void)snprintf(name, sizeof(name), "e2e-recv-%d", partition);
  int const handle = peer_find_link(peer, name);
  assert_true(handle >= 0);
  uint8_t annotations[64];
  az_amqp_encoder e;
  assert_int_equal(AZ_OK, az_amqp_encoder_init(&e, AZ_SPAN_FROM_BUFFER(annotations)));
  assert_int_equal(AZ_OK, az_amqp_encoder_begin_map(&e));
  assert_int_equal(
      AZ_OK, az_amqp_encoder_append_symbol(&e, AZ_SPAN_FROM_STR("x-opt-sequence-number")));
  assert_int_equal(AZ_OK, az_amqp_encoder_append_long(&e, sequence_number));
  assert_int_equal(AZ_OK, az_amqp_encoder_end_map(&e));
  az_amqp_message message;
  assert_int_equal(AZ_OK, az_amqp_message_init(&message));
  assert_int_equal(
      AZ_OK, az_amqp_message_set_message_annotations(&message, az_amqp_encoder_get_bytes(&e)));
  assert_int_equal(
      AZ_OK, az_amqp_message_set_body_data(&message, az_span_create_from_str((char*)body)));
  peer_send_message(peer, handle, &message);
}

/* ---- watcher helpers ------------------------------------------------------ */

static event_hubs_peer g_peer;

static e2e_amqp_telemetry* watcher_begin(int partition_count)
{
  memset(&g_peer, 0, sizeof(g_peer));
  g_peer.io.answer = peer_answer;
  g_peer.io.on_close = peer_on_close;
  e2e_amqp_telemetry* t = (e2e_amqp_telemetry*)calloc(1, sizeof(*t));
  assert_non_null(t);
  t->transport_override = &e2e_amqp_test_peer_transport_vtable;
  t->transport_override_impl = &g_peer.io;
  const char* err = NULL;
  assert_true(e2e_amqp_telemetry_begin(
      t, "peer.example", "hub", NULL, 0, "sas", partition_count, NULL, &err));
  assert_int_equal(E2E_AMQP_TELEMETRY_PHASE_RECEIVING, t->phase);
  return t;
}

static void watcher_end(e2e_amqp_telemetry* t)
{
  e2e_amqp_telemetry_end(t);
  free(t);
}

/** @brief Delivers @p body to the watcher. */
static void deliver(e2e_amqp_telemetry* t, int partition, int64_t sequence_number, const char* body)
{
  peer_send_event(&g_peer, partition, sequence_number, body);
  assert_true(e2e_amqp_telemetry_do_work(t, 0));
}

/**
 * @brief Calls do_work until the setup reaches @p phase, checking each call advances it by at
 * most one step.
 *
 * @return The number of calls.
 */
static int work_until(e2e_amqp_telemetry* t, e2e_amqp_telemetry_phase phase)
{
  int calls = 0;
  while (t->phase != phase)
  {
    assert_true(calls < SETUP_CALLS_MAX);
    const e2e_amqp_telemetry_phase before = t->phase;
    assert_true(e2e_amqp_telemetry_do_work(t, 0));
    assert_true(t->phase == before || t->phase == before + 1);
    calls++;
  }
  return calls;
}

/* ---- tests ---------------------------------------------------------------- */

/* Sequence numbers are per partition: the same number on two partitions is two events, and a
 * transfer at or below the last number of its partition is a replay that changes nothing. */
static void test_replayed_transfers_are_dropped_by_partition_sequence_number(void** state)
{
  (void)state;
  e2e_amqp_telemetry* t = watcher_begin(2);

  deliver(t, 0, 5, "p0-a");
  deliver(t, 1, 5, "p1-a");
  assert_int_equal(2, t->received_count);
  assert_int_equal(2, t->captured_count);

  deliver(t, 0, 5, "p0-a replayed");
  deliver(t, 1, 3, "p1-older");
  assert_int_equal(2, t->received_count);
  assert_int_equal(2, t->captured_count);
  assert_false(e2e_amqp_telemetry_seen(t, "replayed"));
  assert_false(e2e_amqp_telemetry_seen(t, "p1-older"));

  deliver(t, 0, 6, "p0-b");
  assert_int_equal(3, t->received_count);
  const e2e_amqp_capture* capture = e2e_amqp_telemetry_find(t, "p0-b");
  assert_non_null(capture);
  assert_int_equal(2, capture->seq);
  watcher_end(t);
}

/* A detached receiver is found, and the restart sets the connection up again one step per
 * do_work call. Captures, watched needles, sequence numbers and arrival order survive it, and
 * the replay of what was already received is dropped. */
static void test_a_detached_receiver_reconnects_and_keeps_its_state(void** state)
{
  (void)state;
  e2e_amqp_telemetry* t = watcher_begin(2);
  assert_true(e2e_amqp_telemetry_watch_needle(t, "needle"));
  deliver(t, 0, 0, "needle-0");
  deliver(t, 1, 0, "event-1");
  assert_int_equal(-1, e2e_amqp_telemetry_find_detached_receiver(t));

  peer_send_detach(&g_peer, peer_find_link(&g_peer, "e2e-recv-1"));
  assert_true(e2e_amqp_telemetry_do_work(t, 0));
  assert_int_equal(1, e2e_amqp_telemetry_find_detached_receiver(t));

  assert_true(e2e_amqp_telemetry_restart(t));
  assert_int_equal(E2E_AMQP_TELEMETRY_PHASE_OPENING_CONNECTION, t->phase);
  assert_true(work_until(t, E2E_AMQP_TELEMETRY_PHASE_RECEIVING) >= 5);
  assert_int_equal(2, g_peer.connections_opened);
  assert_int_equal(-1, e2e_amqp_telemetry_find_detached_receiver(t));

  deliver(t, 0, 0, "needle-0");
  deliver(t, 1, 0, "event-1");
  deliver(t, 1, 1, "event-2");
  assert_int_equal(3, t->received_count);
  assert_int_equal(3, t->captured_count);
  assert_int_equal(1, t->next_sequence_number[0]);
  assert_int_equal(2, t->next_sequence_number[1]);
  assert_true(e2e_amqp_telemetry_watched_seen(t, "needle"));
  assert_int_equal(0, e2e_amqp_telemetry_find(t, "needle-0")->seq);
  assert_int_equal(1, e2e_amqp_telemetry_find(t, "event-1")->seq);
  assert_int_equal(2, e2e_amqp_telemetry_find(t, "event-2")->seq);
  watcher_end(t);
}

/* A receiver whose attach the service never answers keeps the setup open, so the setup
 * deadline still applies and fails it. */
static void test_the_setup_times_out_while_a_receiver_is_still_attaching(void** state)
{
  (void)state;
  e2e_amqp_telemetry* t = watcher_begin(2);
  g_peer.unanswered_attach = "e2e-recv-1";

  assert_true(e2e_amqp_telemetry_restart(t));
  (void)work_until(t, E2E_AMQP_TELEMETRY_PHASE_ATTACHING_RECEIVERS);
  for (int i = 0; i < 5; i++)
  {
    assert_true(e2e_amqp_telemetry_do_work(t, 0));
  }
  assert_int_equal(E2E_AMQP_TELEMETRY_PHASE_ATTACHING_RECEIVERS, t->phase);
  assert_int_equal(-1, e2e_amqp_telemetry_find_detached_receiver(t));

  t->setup_deadline = time(NULL) - 1;
  assert_false(e2e_amqp_telemetry_do_work(t, 0));
  assert_string_equal("telemetry: connection setup timed out", t->error);
  watcher_end(t);
}

/* An attach the service answers and then detaches (a refused sixth reader) ends the setup, and
 * the refused receiver is found for a reconnect. */
static void test_a_refused_receiver_ends_the_setup_and_is_found(void** state)
{
  (void)state;
  e2e_amqp_telemetry* t = watcher_begin(2);
  g_peer.refused_attach = "e2e-recv-0";

  assert_true(e2e_amqp_telemetry_restart(t));
  (void)work_until(t, E2E_AMQP_TELEMETRY_PHASE_RECEIVING);
  assert_int_equal(0, e2e_amqp_telemetry_find_detached_receiver(t));
  watcher_end(t);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_replayed_transfers_are_dropped_by_partition_sequence_number),
    cmocka_unit_test(test_a_detached_receiver_reconnects_and_keeps_its_state),
    cmocka_unit_test(test_the_setup_times_out_while_a_receiver_is_still_attaching),
    cmocka_unit_test(test_a_refused_receiver_ends_the_setup_and_is_found),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
