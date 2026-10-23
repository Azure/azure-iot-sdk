// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The hand-off queue of samples/unified/multithreaded_telemetry: application
 * code that makes a single-threaded SDK usable from several threads, so it has
 * to be race-free itself. Producers push while a consumer pops; nothing may be
 * lost except what the queue reports as dropped, and each producer's readings
 * stay in order. Labelled "race": CI also runs it under helgrind and DRD. */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "reading_queue.h"

#if !defined(_WIN32)
#include <time.h>
#endif

#define PRODUCERS 3u
#define PUSHES_PER_PRODUCER 500u

typedef struct
{
  reading_queue* q;
  unsigned id;
} producer_args;

/* Written by the consumer only; read by the main thread after joining it. */
typedef struct
{
  reading_queue* q;
  unsigned popped;
  unsigned out_of_order;
  int last[PRODUCERS];
} consumer_state;

#if defined(_WIN32)
typedef HANDLE test_thread;
typedef DWORD(WINAPI* thread_fn)(LPVOID);
#define THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
#define THREAD_RETURN return 0
#else
typedef pthread_t test_thread;
typedef void* (*thread_fn)(void*);
#define THREAD_FN(name) static void* name(void* arg)
#define THREAD_RETURN return NULL
#endif

static void thread_start(test_thread* t, thread_fn fn, void* arg)
{
#if defined(_WIN32)
  *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
  assert_non_null(*t);
#else
  assert_int_equal(0, pthread_create(t, NULL, fn, arg));
#endif
}

static void thread_yield(void)
{
#if defined(_WIN32)
  Sleep(1);
#else
  struct timespec ts = { 0, 1000000L };
  (void)nanosleep(&ts, NULL);
#endif
}

static void thread_join(test_thread t)
{
#if defined(_WIN32)
  assert_int_equal(WAIT_OBJECT_0, WaitForSingleObject(t, INFINITE));
  CloseHandle(t);
#else
  assert_int_equal(0, pthread_join(t, NULL));
#endif
}

THREAD_FN(producer)
{
  const producer_args* p = (const producer_args*)arg;
  for (unsigned i = 0; i < PUSHES_PER_PRODUCER; ++i)
  {
    reading r;
    r.sensor = p->id;
    r.temperature = (int)i;
    reading_queue_push(p->q, r);
  }
  THREAD_RETURN;
}

/* Record one popped reading: per producer, sequence numbers must increase. */
static void consume(consumer_state* c, reading r)
{
  if (r.sensor >= PRODUCERS || r.temperature <= c->last[r.sensor])
  {
    c->out_of_order++;
  }
  else
  {
    c->last[r.sensor] = r.temperature;
  }
  c->popped++;
}

THREAD_FN(consumer)
{
  consumer_state* c = (consumer_state*)arg;
  reading r;
  /* Stop is requested only after every producer was joined, so once it is seen
   * the next empty pop means the queue is drained. */
  for (;;)
  {
    bool const stopping = reading_queue_stopping(c->q);
    if (reading_queue_pop(c->q, &r))
    {
      consume(c, r);
    }
    else if (stopping)
    {
      break;
    }
    else
    {
      thread_yield(); /* empty: let the producers run */
    }
  }
  THREAD_RETURN;
}

static void producers_and_consumer_lose_nothing_unreported(void** state)
{
  (void)state;
  reading_queue q;
  assert_true(reading_queue_init(&q));

  consumer_state c;
  c.q = &q;
  c.popped = 0;
  c.out_of_order = 0;
  for (unsigned i = 0; i < PRODUCERS; ++i)
  {
    c.last[i] = -1;
  }
  producer_args args[PRODUCERS];
  test_thread producers[PRODUCERS];
  test_thread consumer_thread;

  thread_start(&consumer_thread, consumer, &c);
  for (unsigned i = 0; i < PRODUCERS; ++i)
  {
    args[i].q = &q;
    args[i].id = i;
    thread_start(&producers[i], producer, &args[i]);
  }
  for (unsigned i = 0; i < PRODUCERS; ++i)
  {
    thread_join(producers[i]);
  }
  reading_queue_stop(&q);
  thread_join(consumer_thread);

  reading r;
  assert_false(reading_queue_pop(&q, &r));
  assert_int_equal(0, c.out_of_order);
  assert_int_equal(PRODUCERS * PUSHES_PER_PRODUCER, c.popped + reading_queue_dropped(&q));
  reading_queue_deinit(&q);
}

static void a_full_queue_drops_the_oldest(void** state)
{
  (void)state;
  reading_queue q;
  assert_true(reading_queue_init(&q));
  unsigned const pushed = READING_QUEUE_CAPACITY + 4u;
  for (unsigned i = 0; i < pushed; ++i)
  {
    reading r;
    r.sensor = 0;
    r.temperature = (int)i;
    reading_queue_push(&q, r);
  }
  assert_int_equal(4, reading_queue_dropped(&q));
  reading r;
  for (unsigned i = 4; i < pushed; ++i)
  {
    assert_true(reading_queue_pop(&q, &r));
    assert_int_equal((int)i, r.temperature);
  }
  assert_false(reading_queue_pop(&q, &r));
  assert_false(reading_queue_stopping(&q));
  reading_queue_stop(&q);
  assert_true(reading_queue_stopping(&q));
  reading_queue_deinit(&q);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(producers_and_consumer_lose_nothing_unreported),
    cmocka_unit_test(a_full_queue_drops_the_oldest),
  };
  return cmocka_run_group_tests_name("sample_reading_queue_race_tests", tests, NULL, NULL);
}
