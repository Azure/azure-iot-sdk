// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Presence defensive-timeout schedule: base per attempt plus jitter in [0, 5 min]. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#include <cmocka.h>

#include "internal/presence_timeout.h"

#define MINUTE_MS 60000u

static void the_base_follows_the_schedule(void** state)
{
  (void)state;
  assert_int_equal(az_iot_presence_timeout__base_ms(0), 5u * MINUTE_MS);
  assert_int_equal(az_iot_presence_timeout__base_ms(1), 5u * MINUTE_MS);
  assert_int_equal(az_iot_presence_timeout__base_ms(2), 6u * MINUTE_MS);
  assert_int_equal(az_iot_presence_timeout__base_ms(3), 8u * MINUTE_MS);
  assert_int_equal(az_iot_presence_timeout__base_ms(4), 10u * MINUTE_MS);
  assert_int_equal(az_iot_presence_timeout__base_ms(UINT32_MAX), 10u * MINUTE_MS);
}

static void the_timeout_adds_the_jitter_capped_at_five_minutes(void** state)
{
  (void)state;
  assert_int_equal(az_iot_presence_timeout__ms(1, 0), 5u * MINUTE_MS);
  assert_int_equal(az_iot_presence_timeout__ms(2, 1234u), 6u * MINUTE_MS + 1234u);
  assert_int_equal(
      az_iot_presence_timeout__ms(3, AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS), 13u * MINUTE_MS);
  assert_int_equal(az_iot_presence_timeout__ms(4, UINT32_MAX), 15u * MINUTE_MS);
}

static void random_jitter_stays_in_range_and_varies(void** state)
{
  (void)state;
  uint64_t rng = 42u;
  uint32_t lowest = UINT32_MAX;
  uint32_t highest = 0;
  for (int i = 0; i < 10000; ++i)
  {
    uint32_t jitter = az_iot_presence_timeout__random_jitter_ms(&rng);
    assert_true(jitter <= AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS);
    lowest = jitter < lowest ? jitter : lowest;
    highest = jitter > highest ? jitter : highest;
  }
  /* Uniform over 5 minutes: 10000 draws reach both ends of the range. */
  assert_true(lowest < AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS / 100u);
  assert_true(highest > AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS / 100u * 99u);
}

static void random_jitter_is_deterministic_for_a_seed_and_survives_a_zero_state(void** state)
{
  (void)state;
  uint64_t a = 7u;
  uint64_t b = 7u;
  assert_int_equal(
      az_iot_presence_timeout__random_jitter_ms(&a), az_iot_presence_timeout__random_jitter_ms(&b));

  uint64_t zero = 0;
  (void)az_iot_presence_timeout__random_jitter_ms(&zero);
  assert_true(zero != 0);
  assert_int_equal(az_iot_presence_timeout__random_jitter_ms(NULL), 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(the_base_follows_the_schedule),
    cmocka_unit_test(the_timeout_adds_the_jitter_capped_at_five_minutes),
    cmocka_unit_test(random_jitter_stays_in_range_and_varies),
    cmocka_unit_test(random_jitter_is_deterministic_for_a_seed_and_survives_a_zero_state),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
