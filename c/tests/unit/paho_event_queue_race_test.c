// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Data-race suite for the Paho adapter's event queue.
 *
 * WHAT THIS EXISTS FOR
 *
 * The adapter is the one place in this SDK where two threads meet. Paho runs
 * its callbacks on its own threads; the application calls process_loop() on
 * its own; and the adapter's FIFO is what joins them. That is its documented
 * contract -- "Producer = Paho callback threads. Consumer = process_loop() on
 * the user thread" -- so it is a contract that has to be tested, not assumed.
 *
 * It had not been. The project's dynamic analysis was memcheck on Linux and
 * AddressSanitizer on Windows, and NEITHER detects a data race: both look at
 * what memory is touched, not at which thread touched it or under which lock.
 * A race that happens to be benign on the CI machine's scheduling is invisible
 * to them, and that is exactly how this one survived:
 *
 *   paho_msg_arrived() pushed the queued event, and THEN filled in its v5
 *   User Properties by walking back to the queue tail. Between those two
 *   steps the node was already visible to the consumer, which is free to pop
 *   it, hand it to the application and free() it. The producer then wrote
 *   into freed memory, and the application could see a message whose property
 *   array was still being built.
 *
 * That window is small, and every existing test passed straight through it.
 * These cases are written so that a tool that understands happens-before --
 * helgrind, DRD, or ThreadSanitizer -- reports it, and so that a sufficiently
 * unlucky schedule fails the assertions even without one. They are ordinary
 * cmocka tests: they run in the normal suite, and CI additionally runs this
 * one binary under helgrind (see the race-detector job in ci-c.yml).
 *
 * HOW
 *
 * az_iot_mqtt_paho.c is compiled against tests/support/mock_paho_async.c, so
 * the test IS the Paho thread: it calls the callbacks the adapter registered,
 * from threads it starts itself, while another thread drains process_loop().
 * No broker, no sockets and no Paho threads are involved, which keeps the only
 * concurrency in the picture the concurrency under test.
 *
 * WHAT IS DELIBERATELY NOT TESTED HERE
 *
 * destroy() racing against a producer. The adapter's contract is that the
 * application stops the client before destroying it -- destroy() tears the
 * mutex down -- so a test that drove them concurrently would be asserting a
 * guarantee the adapter never made, and would fail for a reason that is not a
 * defect. Producer/consumer concurrency is the part that IS promised.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "mock_paho_async.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE race_thread;
typedef DWORD(WINAPI* race_thread_fn)(LPVOID);
#else
#include <pthread.h>
#include <time.h>
typedef pthread_t race_thread;
typedef void* (*race_thread_fn)(void*);
#endif

/* Iterations per producer. Large enough that the schedule varies across the
 * run, small enough that the binary stays quick under a race detector, which
 * slows execution by roughly an order of magnitude. */
#define RACE_ITERATIONS 400
#define RACE_PRODUCERS 2

static void race_thread_start(race_thread* t, race_thread_fn fn, void* arg)
{
#if defined(_WIN32)
  *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
  assert_non_null(*t);
#else
  assert_int_equal(pthread_create(t, NULL, fn, arg), 0);
#endif
}

static void race_thread_join(race_thread t)
{
#if defined(_WIN32)
  assert_int_equal(WaitForSingleObject(t, INFINITE), WAIT_OBJECT_0);
  CloseHandle(t);
#else
  assert_int_equal(pthread_join(t, NULL), 0);
#endif
}

static void race_yield(void)
{
#if defined(_WIN32)
  Sleep(0);
#else
  struct timespec ts = { 0, 1000L };
  nanosleep(&ts, NULL);
#endif
}

/* ------------------------------------------------------------------------- */
/* fixture                                                                    */
/* ------------------------------------------------------------------------- */

/* The properties every injected message carries. Static storage with static
 * lifetime: the mock keeps pointers to these rather than copying them, and
 * every producer thread reads the same table without writing to it.
 *
 * "" is in here on purpose. A zero-length value is legal MQTT 5.0, and the
 * consumer below asserts it arrives as an empty string rather than as NULL --
 * the other half of the defect this file was written for. */
static const char* const k_props[] = { "type", "telemetry:1", "empty", "" };
#define K_PROPS_COUNT ((int)(sizeof(k_props) / sizeof(k_props[0]) / 2))

typedef struct race_fixture
{
  az_iot_mqtt_factory* factory;
  az_iot_mqtt_client* client;
  /* Consumer-thread state. Only the consumer thread touches these while the
   * producers run; the main thread reads them after every join. */
  size_t messages_seen;
  size_t events_seen;
  size_t malformed;
  /* Producers are done, so the consumer may stop once the queue is empty.
   * Written by the main thread after joining every producer, and read by the
   * consumer -- the join is the happens-before edge, so a plain bool is
   * correct here and a detector will agree. */
  bool producers_done;
} race_fixture;

/* The consumer's inbound callback. Runs on the consumer thread, inside
 * process_loop(), with the event still owned by the adapter.
 *
 * Every assertion is about SELF-CONSISTENCY, which is what a half-published
 * node fails: a message whose property count says two must have two usable
 * pairs, right now, on this thread. Counted rather than asserted inline
 * because cmocka's failure path longjmps, and doing that out of a non-main
 * thread would abandon the producers mid-flight and report the wrong thing. */
static void on_event(const az_iot_mqtt_event* evt, void* ctx)
{
  race_fixture* f = (race_fixture*)ctx;
  f->events_seen++;
  if (evt->kind != AZ_IOT_MQTT_EVT_MESSAGE)
  {
    return;
  }
  f->messages_seen++;

  const az_iot_mqtt_message* msg = evt->message;
  if (msg == NULL || msg->topic == NULL || strcmp(msg->topic, "race/topic") != 0)
  {
    f->malformed++;
    return;
  }
  if (msg->user_properties_count != (size_t)K_PROPS_COUNT)
  {
    f->malformed++;
    return;
  }
  for (size_t i = 0; i < msg->user_properties_count; ++i)
  {
    const az_iot_mqtt_user_property* up = &msg->user_properties[i];
    if (up->key == NULL || up->value == NULL)
    {
      f->malformed++;
      return;
    }
    if (strcmp(up->key, k_props[2 * i]) != 0 || strcmp(up->value, k_props[2 * i + 1]) != 0)
    {
      f->malformed++;
      return;
    }
  }
}

/* Bring up a connected v5 client whose callbacks the test can then drive. */
static void fixture_setup(race_fixture* f)
{
  memset(f, 0, sizeof(*f));
  mock_paho_reset();
  mock_paho_set_inbound_user_properties(k_props, K_PROPS_COUNT);

  f->factory = az_iot_paho_factory_create_v5();
  assert_non_null(f->factory);
  f->client = f->factory->create(f->factory->factory_ctx);
  assert_non_null(f->client);

  f->client->iface->set_inbound_cb(f->client, on_event, f);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "localhost";
  copts.port = 1883;
  copts.client_id = "race-client";
  copts.keep_alive_seconds = 30;
  assert_int_equal(f->client->iface->connect(f->client, &copts), AZ_IOT_OK);

  /* connect() is what registers them; without it there is nothing to drive and
   * the test would pass by doing nothing at all. */
  assert_non_null(mock_paho_message_arrived());
  assert_non_null(mock_paho_connection_lost());
  assert_ptr_equal(mock_paho_callback_context(), f->client);
}

static void fixture_teardown(race_fixture* f)
{
  f->client->iface->destroy(f->client);
  az_iot_paho_factory_destroy(f->factory);
}

/* Drain until the producers have stopped AND the queue has gone quiet. */
#if defined(_WIN32)
static DWORD WINAPI consumer_main(LPVOID arg)
#else
static void* consumer_main(void* arg)
#endif
{
  race_fixture* f = (race_fixture*)arg;
  for (;;)
  {
    size_t before = f->events_seen;
    (void)f->client->iface->process_loop(f->client, 0);
    if (f->producers_done && f->events_seen == before)
    {
      break;
    }
    race_yield();
  }
#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}

/* ------------------------------------------------------------------------- */
/* cases                                                                      */
/* ------------------------------------------------------------------------- */

/* Deliver one PUBLISH exactly as Paho does: a heap message the callback takes
 * ownership of, and a topic passed with an explicit length.
 *
 * `properties.count` is set because extract_v5_props() returns immediately on
 * an empty property set -- leaving it zero would skip the very code whose
 * publication order is under test. The mock answers the property queries from
 * its scripted table, so the value only has to be non-zero. */
static void deliver_one_message(void)
{
  static const char topic[] = "race/topic";
  static const uint8_t payload[] = { 'r', 'a', 'c', 'e' };

  MQTTAsync_message msg = MQTTAsync_message_initializer;
  msg.payload = (void*)(uintptr_t)payload;
  msg.payloadlen = (int)sizeof(payload);
  msg.qos = 0;
  msg.retained = 0;
  msg.properties.count = K_PROPS_COUNT;

  MQTTAsync_messageArrived* arrived = mock_paho_message_arrived();
  (void)arrived(mock_paho_callback_context(), (char*)(uintptr_t)topic, (int)strlen(topic), &msg);
}

#if defined(_WIN32)
static DWORD WINAPI message_producer_main(LPVOID arg)
#else
static void* message_producer_main(void* arg)
#endif
{
  (void)arg;
  for (int i = 0; i < RACE_ITERATIONS; ++i)
  {
    deliver_one_message();
    race_yield();
  }
#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}

/* The regression case.
 *
 * Producers deliver messages carrying v5 properties while the consumer drains
 * the queue. Against the defect described at the top of this file, a detector
 * reports the write to the queued node after it was published, and the
 * consumer can observe a property array that is still being filled in.
 *
 * Two producers rather than one because the adapter's own comment says
 * "Paho callback threads", plural: the queue has to be correct for concurrent
 * producers as well as for a producer against the consumer. */
static void inbound_messages_do_not_race_with_process_loop(void** state)
{
  (void)state;
  race_fixture f;
  fixture_setup(&f);

  race_thread consumer;
  race_thread producers[RACE_PRODUCERS];
  race_thread_start(&consumer, consumer_main, &f);
  for (int i = 0; i < RACE_PRODUCERS; ++i)
  {
    race_thread_start(&producers[i], message_producer_main, &f);
  }
  for (int i = 0; i < RACE_PRODUCERS; ++i)
  {
    race_thread_join(producers[i]);
  }
  f.producers_done = true;
  race_thread_join(consumer);

  /* Nothing may be lost and nothing may arrive half-built. The counts are the
   * weaker half of this: the property assertions inside on_event() are what
   * catch a node that was published before it was finished. */
  assert_int_equal(f.messages_seen, (size_t)(RACE_ITERATIONS * RACE_PRODUCERS));
  assert_int_equal(f.malformed, 0);

  fixture_teardown(&f);
}

/* Status events take a different route into the same queue (enqueue_status
 * rather than enqueue_message) and carry no heap payload, so they would not
 * exercise the publication order above. What they do exercise is the queue
 * itself under a producer mix -- and a status event interleaved between a
 * message and its properties is how the original defect reordered in
 * practice. */
#if defined(_WIN32)
static DWORD WINAPI status_producer_main(LPVOID arg)
#else
static void* status_producer_main(void* arg)
#endif
{
  (void)arg;
  MQTTAsync_connectionLost* lost = mock_paho_connection_lost();
  for (int i = 0; i < RACE_ITERATIONS; ++i)
  {
    lost(mock_paho_callback_context(), NULL);
    race_yield();
  }
#if defined(_WIN32)
  return 0;
#else
  return NULL;
#endif
}

static void mixed_producers_do_not_race_with_process_loop(void** state)
{
  (void)state;
  race_fixture f;
  fixture_setup(&f);

  race_thread consumer;
  race_thread message_producer;
  race_thread status_producer;
  race_thread_start(&consumer, consumer_main, &f);
  race_thread_start(&message_producer, message_producer_main, &f);
  race_thread_start(&status_producer, status_producer_main, &f);
  race_thread_join(message_producer);
  race_thread_join(status_producer);
  f.producers_done = true;
  race_thread_join(consumer);

  assert_int_equal(f.messages_seen, (size_t)RACE_ITERATIONS);
  assert_int_equal(f.events_seen, (size_t)(RACE_ITERATIONS * 2));
  assert_int_equal(f.malformed, 0);

  fixture_teardown(&f);
}

/* Events still queued when the client is destroyed must be freed, not leaked.
 *
 * Single-threaded on purpose -- see the note at the top of this file about
 * destroy() -- but it belongs with these cases because q_drain_all() is the
 * queue path nothing else reaches, and memcheck over this binary is what
 * proves the property copies inside those events are freed too. */
static void events_left_queued_at_destroy_are_freed(void** state)
{
  (void)state;
  race_fixture f;
  fixture_setup(&f);

  for (int i = 0; i < 16; ++i)
  {
    deliver_one_message();
  }
  /* No process_loop(): every event is still on the queue. */
  assert_int_equal(f.events_seen, 0);

  fixture_teardown(&f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(inbound_messages_do_not_race_with_process_loop),
    cmocka_unit_test(mixed_producers_do_not_race_with_process_loop),
    cmocka_unit_test(events_left_queued_at_destroy_are_freed),
  };
  return cmocka_run_group_tests_name("paho_event_queue_race_tests", tests, NULL, NULL);
}
