// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Escalating, jittered mqttv5 defensive-timeout schedule. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include "internal/defensive_timeout.h"

static const uint32_t k_base_ms[] = { 300000u, 360000u, 480000u, 600000u };

#define MINUTES_MS(m) ((uint32_t)(m) * 60000u)

/* Attempts 1-4 use 5, 6, 8 and 10 min bases. */
static void each_attempt_has_its_base(void** state)
{
  (void)state;
  assert_int_equal(az_iot_defensive_timeout__base_ms(1), MINUTES_MS(5));
  assert_int_equal(az_iot_defensive_timeout__base_ms(2), MINUTES_MS(6));
  assert_int_equal(az_iot_defensive_timeout__base_ms(3), MINUTES_MS(8));
  assert_int_equal(az_iot_defensive_timeout__base_ms(4), MINUTES_MS(10));
}

/* Every later attempt stays at the 10 min base; 0 counts as the first. */
static void later_attempts_stay_at_ten_minutes(void** state)
{
  (void)state;
  assert_int_equal(az_iot_defensive_timeout__base_ms(0), MINUTES_MS(5));
  assert_int_equal(az_iot_defensive_timeout__base_ms(5), MINUTES_MS(10));
  assert_int_equal(az_iot_defensive_timeout__base_ms(100), MINUTES_MS(10));
  assert_int_equal(az_iot_defensive_timeout__base_ms(UINT32_MAX), MINUTES_MS(10));
}

/* Each attempt's range is [base, base + 5 min], both ends reachable. */
static void each_attempt_spans_its_range(void** state)
{
  (void)state;
  for (uint32_t attempt = 1; attempt <= 4u; ++attempt)
  {
    uint32_t base = k_base_ms[attempt - 1u];
    assert_int_equal(az_iot_defensive_timeout__ms(attempt, 0), base);
    assert_int_equal(az_iot_defensive_timeout__ms(attempt, 1), base + 1u);
    assert_int_equal(
        az_iot_defensive_timeout__ms(attempt, AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS),
        base + MINUTES_MS(5));
    /* One past the largest jitter wraps to the base. */
    assert_int_equal(
        az_iot_defensive_timeout__ms(attempt, AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS + 1u), base);
  }
  assert_int_equal(az_iot_defensive_timeout__ms(9, 0), MINUTES_MS(10));
  assert_int_equal(
      az_iot_defensive_timeout__ms(9, AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS), MINUTES_MS(15));
}

/* Any 64-bit random value lands inside the range. */
static void any_random_value_stays_in_range(void** state)
{
  (void)state;
  const uint64_t values[] = { UINT64_MAX, UINT64_MAX - 1u, 0x8000000000000000ull, 0x123456789ull };
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
  {
    for (uint32_t attempt = 1; attempt <= 5u; ++attempt)
    {
      uint32_t base = az_iot_defensive_timeout__base_ms(attempt);
      uint32_t t = az_iot_defensive_timeout__ms(attempt, values[i]);
      assert_true(t >= base);
      assert_true(t <= base + AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS);
    }
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(each_attempt_has_its_base),
    cmocka_unit_test(later_attempts_stay_at_ten_minutes),
    cmocka_unit_test(each_attempt_spans_its_range),
    cmocka_unit_test(any_random_value_stays_in_range),
  };
  return cmocka_run_group_tests_name("defensive_timeout", tests, NULL, NULL);
}
