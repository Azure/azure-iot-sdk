// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_iot_deadline: explicit none / armed / expired states, saturating arithmetic. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include "internal/deadline.h"

/* Zero-initialized is none: never passes, remaining is unbounded. */
static void a_zeroed_deadline_is_none(void** state)
{
  (void)state;
  az_iot_deadline deadline = { 0 };
  assert_false(az_iot_deadline__is_set(&deadline));
  assert_false(az_iot_deadline__has_passed(&deadline, 0u));
  assert_false(az_iot_deadline__has_passed(&deadline, UINT64_MAX));
  assert_true(az_iot_deadline__remaining_ms(&deadline, 123u) == UINT64_MAX);
}

/* Armed: passes exactly at its time, not a millisecond before. */
static void an_armed_deadline_passes_at_its_time(void** state)
{
  (void)state;
  az_iot_deadline deadline = { 0 };
  az_iot_deadline__arm_after_ms(&deadline, 1000u, 500u);
  assert_true(az_iot_deadline__is_set(&deadline));
  assert_false(az_iot_deadline__has_passed(&deadline, 1499u));
  assert_true(az_iot_deadline__has_passed(&deadline, 1500u));
  assert_true(az_iot_deadline__has_passed(&deadline, 1501u));
  assert_int_equal(az_iot_deadline__remaining_ms(&deadline, 1000u), 500u);
  assert_int_equal(az_iot_deadline__remaining_ms(&deadline, 1499u), 1u);
  assert_int_equal(az_iot_deadline__remaining_ms(&deadline, 1500u), 0u);
  assert_int_equal(az_iot_deadline__remaining_ms(&deadline, 9999u), 0u);
}

/* A zero duration passes at once; a deadline armed at time 0 is not none. */
static void a_zero_duration_passes_at_once(void** state)
{
  (void)state;
  az_iot_deadline deadline = { 0 };
  az_iot_deadline__arm_after_ms(&deadline, 0u, 0u);
  assert_true(az_iot_deadline__is_set(&deadline));
  assert_true(az_iot_deadline__has_passed(&deadline, 0u));
  assert_int_equal(az_iot_deadline__remaining_ms(&deadline, 0u), 0u);
}

/* Seconds convert to milliseconds, the largest uint32_t included. */
static void seconds_are_converted(void** state)
{
  (void)state;
  az_iot_deadline deadline = { 0 };
  az_iot_deadline__arm_after_seconds(&deadline, 10u, 60u);
  assert_int_equal(az_iot_deadline__remaining_ms(&deadline, 10u), 60000u);
  az_iot_deadline__arm_after_seconds(&deadline, 0u, UINT32_MAX);
  assert_true(az_iot_deadline__remaining_ms(&deadline, 0u) == (uint64_t)UINT32_MAX * 1000u);
}

/* A duration past the clock's range saturates instead of wrapping into the past. */
static void an_overflowing_duration_saturates(void** state)
{
  (void)state;
  az_iot_deadline deadline = { 0 };
  az_iot_deadline__arm_after_ms(&deadline, UINT64_MAX - 10u, 100u);
  assert_false(az_iot_deadline__has_passed(&deadline, UINT64_MAX - 1u));
  assert_true(az_iot_deadline__has_passed(&deadline, UINT64_MAX));
  assert_int_equal(az_iot_deadline__remaining_ms(&deadline, UINT64_MAX - 10u), 10u);
}

/* Expired passes whatever the clock says, even at time 0. */
static void an_expired_deadline_has_passed_at_any_time(void** state)
{
  (void)state;
  az_iot_deadline deadline = { 0 };
  az_iot_deadline__arm_after_ms(&deadline, 0u, 60000u);
  az_iot_deadline__expire_now(&deadline);
  assert_true(az_iot_deadline__is_set(&deadline));
  assert_true(az_iot_deadline__has_passed(&deadline, 0u));
  assert_int_equal(az_iot_deadline__remaining_ms(&deadline, 0u), 0u);
}

/* Clearing returns any state to none; re-arming leaves expired. */
static void clear_and_rearm_change_the_state(void** state)
{
  (void)state;
  az_iot_deadline deadline = { 0 };
  az_iot_deadline__expire_now(&deadline);
  az_iot_deadline__arm_after_ms(&deadline, 0u, 100u);
  assert_false(az_iot_deadline__has_passed(&deadline, 99u));
  az_iot_deadline__clear(&deadline);
  assert_false(az_iot_deadline__is_set(&deadline));
  assert_false(az_iot_deadline__has_passed(&deadline, UINT64_MAX));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_zeroed_deadline_is_none),
    cmocka_unit_test(an_armed_deadline_passes_at_its_time),
    cmocka_unit_test(a_zero_duration_passes_at_once),
    cmocka_unit_test(seconds_are_converted),
    cmocka_unit_test(an_overflowing_duration_saturates),
    cmocka_unit_test(an_expired_deadline_has_passed_at_any_time),
    cmocka_unit_test(clear_and_rearm_change_the_state),
  };
  return cmocka_run_group_tests_name("deadline", tests, NULL, NULL);
}
