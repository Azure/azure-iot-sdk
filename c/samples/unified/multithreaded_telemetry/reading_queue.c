// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "reading_queue.h"

#include <string.h>

/** @brief Take @p q's lock. */
static void queue_lock(reading_queue* q)
{
#if defined(_WIN32)
  EnterCriticalSection(&q->lock);
#else
  (void)pthread_mutex_lock(&q->lock);
#endif
}

/** @brief Release @p q's lock. */
static void queue_unlock(reading_queue* q)
{
#if defined(_WIN32)
  LeaveCriticalSection(&q->lock);
#else
  (void)pthread_mutex_unlock(&q->lock);
#endif
}

bool reading_queue_init(reading_queue* q)
{
  memset(q, 0, sizeof(*q));
#if defined(_WIN32)
  InitializeCriticalSection(&q->lock);
  return true;
#else
  return pthread_mutex_init(&q->lock, NULL) == 0;
#endif
}

void reading_queue_deinit(reading_queue* q)
{
#if defined(_WIN32)
  DeleteCriticalSection(&q->lock);
#else
  (void)pthread_mutex_destroy(&q->lock);
#endif
}

void reading_queue_push(reading_queue* q, reading r)
{
  queue_lock(q);
  if (q->count == READING_QUEUE_CAPACITY)
  {
    q->head = (q->head + 1u) % READING_QUEUE_CAPACITY;
    q->count--;
    q->dropped++;
  }
  q->items[(q->head + q->count) % READING_QUEUE_CAPACITY] = r;
  q->count++;
  queue_unlock(q);
}

bool reading_queue_pop(reading_queue* q, reading* out)
{
  bool found = false;
  queue_lock(q);
  if (q->count > 0u)
  {
    *out = q->items[q->head];
    q->head = (q->head + 1u) % READING_QUEUE_CAPACITY;
    q->count--;
    found = true;
  }
  queue_unlock(q);
  return found;
}

unsigned reading_queue_dropped(reading_queue* q)
{
  queue_lock(q);
  unsigned dropped = q->dropped;
  queue_unlock(q);
  return dropped;
}

void reading_queue_stop(reading_queue* q)
{
  queue_lock(q);
  q->stopping = true;
  queue_unlock(q);
}

bool reading_queue_stopping(reading_queue* q)
{
  queue_lock(q);
  bool stopping = q->stopping;
  queue_unlock(q);
  return stopping;
}
