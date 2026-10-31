// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The telemetry watcher's capture ring and capture filter: the replay of a shared events
 * endpoint must not evict the messages a test waits for. Also its detection of a receiver the
 * service detached, and the drop of events a reconnect replays. No network is used. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "e2e_amqp.h"

/** @brief A watcher that was never connected, as the capture functions see it after begin. */
static e2e_amqp_telemetry* new_watcher(const char* capture_filter)
{
  e2e_amqp_telemetry* t = (e2e_amqp_telemetry*)calloc(1, sizeof(*t));
  assert_non_null(t);
  assert_true(e2e_amqp_telemetry_set_capture_filter(t, capture_filter));
  return t;
}

/** @brief Captures {"marker":"<marker>"}; returns the capture or NULL. */
static e2e_amqp_capture* receive_marker(e2e_amqp_telemetry* t, const char* marker)
{
  char body[256];
  const int n = snprintf(body, sizeof(body), "{\"marker\":\"%s\"}", marker);
  assert_true(n > 0 && (size_t)n < sizeof(body));
  return e2e_amqp_telemetry_capture_body(t, (const uint8_t*)body, (size_t)n);
}

/** @brief Receives @p count messages of other devices. */
static void receive_other_traffic(e2e_amqp_telemetry* t, int count)
{
  for (int i = 0; i < count; i++)
  {
    char marker[64];
    (void)snprintf(marker, sizeof(marker), "or-other-device-%d", i);
    assert_null(receive_marker(t, marker));
  }
}

/* Many more messages of other devices than the ring holds arrive between the device's own: the
 * device's are all still found, in arrival order. */
static void test_other_traffic_does_not_evict_filtered_captures(void** state)
{
  (void)state;
  e2e_amqp_telemetry* t = new_watcher("ci-device-1");
  assert_non_null(receive_marker(t, "or-ci-device-1-0"));
  receive_other_traffic(t, 4 * E2E_AMQP_CAPTURE_MAX);
  assert_non_null(receive_marker(t, "or-ci-device-1-1"));
  receive_other_traffic(t, 4 * E2E_AMQP_CAPTURE_MAX);

  const e2e_amqp_capture* first = e2e_amqp_telemetry_find(t, "or-ci-device-1-0");
  const e2e_amqp_capture* second = e2e_amqp_telemetry_find(t, "or-ci-device-1-1");
  assert_non_null(first);
  assert_non_null(second);
  assert_int_equal(first->seq, 0);
  assert_int_equal(second->seq, 1 + 4 * E2E_AMQP_CAPTURE_MAX);
  assert_int_equal(t->captured_count, 2);
  assert_int_equal(t->received_count, 2 + 8 * E2E_AMQP_CAPTURE_MAX);
  assert_null(e2e_amqp_telemetry_find(t, "or-other-device-0"));
  free(t);
}

/* Without a filter every message is captured, and the oldest is evicted once the ring is full. */
static void test_without_a_filter_the_oldest_capture_is_evicted(void** state)
{
  (void)state;
  e2e_amqp_telemetry* t = new_watcher(NULL);
  assert_non_null(receive_marker(t, "or-ci-device-1-0"));
  for (int i = 0; i < E2E_AMQP_CAPTURE_MAX - 1; i++)
  {
    char marker[64];
    (void)snprintf(marker, sizeof(marker), "or-other-device-%d", i);
    assert_non_null(receive_marker(t, marker));
  }
  assert_non_null(e2e_amqp_telemetry_find(t, "or-ci-device-1-0"));
  assert_false(t->eviction_reported);

  assert_non_null(receive_marker(t, "or-other-device-last"));
  assert_null(e2e_amqp_telemetry_find(t, "or-ci-device-1-0"));
  assert_true(t->eviction_reported);
  assert_int_equal(t->captured_count, E2E_AMQP_CAPTURE_MAX);
  free(t);
}

/* The filter is matched against the full body, past the prefix the capture keeps. */
static void test_the_filter_matches_past_the_captured_prefix(void** state)
{
  (void)state;
  static const char device_id[] = "ci-device-1";
  const size_t device_id_size = sizeof(device_id) - 1;
  e2e_amqp_telemetry* t = new_watcher(device_id);
  const size_t size = (size_t)4 * E2E_AMQP_CAPTURE_BODY_MAX;
  uint8_t* body = (uint8_t*)malloc(size);
  assert_non_null(body);
  memset(body, 'x', size);
  memcpy(body + size - device_id_size, device_id, device_id_size);

  const e2e_amqp_capture* c = e2e_amqp_telemetry_capture_body(t, body, size);
  assert_non_null(c);
  assert_int_equal(c->body_size, size);
  assert_int_equal(strlen(c->body), E2E_AMQP_CAPTURE_BODY_MAX - 1);
  free(body);
  free(t);
}

/* A filter that does not fit is refused and the previous one is kept; NULL clears it. */
static void test_a_too_long_filter_is_refused(void** state)
{
  (void)state;
  e2e_amqp_telemetry* t = new_watcher("ci-device-1");
  char too_long[E2E_AMQP_CAPTURE_FILTER_MAX + 1];
  memset(too_long, 'f', sizeof(too_long) - 1);
  too_long[sizeof(too_long) - 1] = '\0';

  assert_false(e2e_amqp_telemetry_set_capture_filter(t, too_long));
  assert_string_equal(t->capture_filter, "ci-device-1");
  assert_true(e2e_amqp_telemetry_set_capture_filter(t, NULL));
  assert_string_equal(t->capture_filter, "");
  assert_non_null(receive_marker(t, "or-other-device-0"));
  free(t);
}

/* Only a started watcher whose receiver went back to detached, as when the service refuses a
 * sixth reader of a partition, reports that partition for a reconnect. */
static void test_a_detached_receiver_is_found(void** state)
{
  (void)state;
  e2e_amqp_telemetry* t = new_watcher(NULL);
  t->partition_count = 3;
  for (int p = 0; p < t->partition_count; p++)
  {
    t->receivers[p].state = AZ_AMQP_LINK_STATE_ATTACHED;
  }
  t->receivers[0].state = AZ_AMQP_LINK_STATE_ATTACHING;
  t->receivers[2].state = AZ_AMQP_LINK_STATE_DETACHED;
  assert_int_equal(e2e_amqp_telemetry_find_detached_receiver(t), -1);

  t->phase = E2E_AMQP_TELEMETRY_PHASE_RECEIVING;
  assert_int_equal(e2e_amqp_telemetry_find_detached_receiver(t), 2);
  t->receivers[2].state = AZ_AMQP_LINK_STATE_ERROR;
  assert_int_equal(e2e_amqp_telemetry_find_detached_receiver(t), 2);
  t->receivers[2].state = AZ_AMQP_LINK_STATE_ATTACHED;
  assert_int_equal(e2e_amqp_telemetry_find_detached_receiver(t), -1);
  free(t);
}

/** @brief Delivers {"marker":"<marker>"} as event @p sequence_number of @p partition, as the
 * receive callback does: a replayed event is neither counted nor captured. */
static e2e_amqp_capture* deliver_event(
    e2e_amqp_telemetry* t,
    int partition,
    int64_t sequence_number,
    const char* marker)
{
  if (!e2e_amqp_telemetry_record_sequence_number(t, partition, sequence_number))
  {
    return NULL;
  }
  return receive_marker(t, marker);
}

/* A reconnect replays every event since the watch began, more than the ring holds, on two
 * partitions: the replay is dropped, so the captures and their arrival order stay those of the
 * first delivery, and the next new event follows them. */
static void test_a_replay_after_a_reconnect_is_dropped(void** state)
{
  (void)state;
  e2e_amqp_telemetry* t = new_watcher(NULL);
  const int count = 2 * E2E_AMQP_CAPTURE_MAX;
  char marker[32];
  for (int pass = 0; pass < 2; pass++)
  {
    for (int i = 0; i < count; i++)
    {
      (void)snprintf(marker, sizeof(marker), "m-%d", i);
      e2e_amqp_capture* c = deliver_event(t, i % 2, i / 2, marker);
      assert_true(pass == 0 ? c != NULL : c == NULL);
    }
  }
  assert_int_equal(t->received_count, count);
  const e2e_amqp_capture* oldest_kept = e2e_amqp_telemetry_find(t, "\"m-64\"");
  const e2e_amqp_capture* newest = e2e_amqp_telemetry_find(t, "\"m-127\"");
  assert_non_null(oldest_kept);
  assert_non_null(newest);
  assert_int_equal(oldest_kept->seq, 64);
  assert_int_equal(newest->seq, 127);

  const e2e_amqp_capture* next = deliver_event(t, 0, count / 2, "m-new");
  assert_non_null(next);
  assert_int_equal(next->seq, count);
  assert_non_null(deliver_event(t, 1, -1, "m-unknown-sequence"));
  free(t);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_other_traffic_does_not_evict_filtered_captures),
    cmocka_unit_test(test_without_a_filter_the_oldest_capture_is_evicted),
    cmocka_unit_test(test_the_filter_matches_past_the_captured_prefix),
    cmocka_unit_test(test_a_too_long_filter_is_refused),
    cmocka_unit_test(test_a_detached_receiver_is_found),
    cmocka_unit_test(test_a_replay_after_a_reconnect_is_dropped),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
