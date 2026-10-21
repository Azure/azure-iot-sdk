// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file reading_queue.h
 * @brief Application-owned, thread-safe hand-off of readings to the SDK thread.
 *
 * The SDK is single-threaded: this queue is the application's side of that
 * contract. Sensor threads push; only the thread that calls into the SDK pops. */
#ifndef READING_QUEUE_H
#define READING_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

/** @brief Readings held before the oldest unsent ones are dropped. */
#define READING_QUEUE_CAPACITY 16u

/** @brief One sensor reading. */
typedef struct reading
{
  unsigned sensor;
  int temperature;
} reading;

/** @brief Bounded FIFO guarded by one lock; every field is read and written under it. */
typedef struct reading_queue
{
#if defined(_WIN32)
  CRITICAL_SECTION lock;
#else
  pthread_mutex_t lock;
#endif
  reading items[READING_QUEUE_CAPACITY];
  size_t head;
  size_t count;
  unsigned dropped;
  bool stopping;
} reading_queue;

/** @brief Initialize @p q empty. @return false if the lock could not be created. */
bool reading_queue_init(reading_queue* q);

/** @brief Release @p q. No thread may still use it. */
void reading_queue_deinit(reading_queue* q);

/** @brief Append @p r; when full, drop the oldest reading to make room. Any thread. */
void reading_queue_push(reading_queue* q, reading r);

/** @brief Take the oldest reading into @p out. @return false if empty. Any thread. */
bool reading_queue_pop(reading_queue* q, reading* out);

/** @brief Readings dropped because the queue was full. Any thread. */
unsigned reading_queue_dropped(reading_queue* q);

/** @brief Ask producers to stop. Any thread. */
void reading_queue_stop(reading_queue* q);

/** @brief Whether reading_queue_stop() was called. Any thread. */
bool reading_queue_stopping(reading_queue* q);

#endif /* READING_QUEUE_H */
