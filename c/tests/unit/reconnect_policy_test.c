// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Phase 2.2 - reconnect-policy unit tests for the pure delay calculator. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "internal/reconnect.h"

static void disabled_when_initial_delay_is_zero(void** state)
{
  (void)state;
  az_iot_reconnection_policy p = { 0 };
  uint64_t rng = 1;
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 1, &rng), 0);
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 5, &rng), 0);
}

static void no_jitter_doubles_until_cap(void** state)
{
  (void)state;
  az_iot_reconnection_policy p = { 0 };
  p.initial_delay_ms = 100;
  p.max_delay_ms = 1000;
  p.jitter_pct = 0;

  uint64_t rng = 42;
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 1, &rng), 100);
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 2, &rng), 200);
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 3, &rng), 400);
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 4, &rng), 800);
  /* 100 << 4 = 1600 > cap; clamped. */
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 5, &rng), 1000);
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 100, &rng), 1000);
}

/* Jitter varies AROUND the backoff, so the band is symmetric and the top of it
 * sits above max_delay_ms. It used to be clamped back to the cap, which folded
 * every positive draw onto the cap itself -- half the fleet retrying on the
 * same instant, which is what jitter exists to prevent. */
static void jitter_stays_within_band(void** state)
{
  (void)state;
  az_iot_reconnection_policy p = { 0 };
  p.initial_delay_ms = 1000;
  p.max_delay_ms = 1000;
  p.jitter_pct = 20; /* +/- 20% of base */

  uint64_t rng = 0xDEADBEEFCAFEBABEull;
  int above_cap = 0;
  for (int i = 0; i < 200; ++i)
  {
    uint32_t d = az_iot_reconnect_delay_ms(&p, 1, &rng);
    /* base = 1000, jitter +/- 200, floored at 1. So [800, 1200]. */
    assert_true(d >= 800);
    assert_true(d <= 1200);
    if (d > 1000)
    {
      above_cap++;
    }
  }
  /* The upper half of the band is reachable at all -- the property the old
   * clamp destroyed. */
  assert_true(above_cap > 0);
}

/* The distribution is centred on the backoff, not bunched under it. With the
 * old clamp the mean of a capped policy sat ~5% low and half of all draws
 * landed on exactly the cap. */
static void jitter_is_centred_on_the_backoff(void** state)
{
  (void)state;
  az_iot_reconnection_policy p = { 0 };
  p.initial_delay_ms = 5000;
  p.max_delay_ms = 5000; /* fixed interval: base == cap from attempt 1 */
  p.jitter_pct = 20;

  uint64_t rng = 0x12345678ull;
  const int n = 20000;
  double sum = 0;
  int on_cap = 0;
  for (int i = 0; i < n; ++i)
  {
    uint32_t d = az_iot_reconnect_delay_ms(&p, 1, &rng);
    sum += d;
    if (d == 5000)
    {
      on_cap++;
    }
  }
  double mean = sum / n;
  /* Centred within 1% of the nominal interval (it was 4749 before). */
  assert_true(mean > 4950.0);
  assert_true(mean < 5050.0);
  /* And no spike on the cap: a single value out of a 2001-wide band. */
  assert_true(on_cap < n / 100);
}

static void zero_max_delay_means_initial_is_the_cap(void** state)
{
  (void)state;
  az_iot_reconnection_policy p = { 0 };
  p.initial_delay_ms = 250;
  p.max_delay_ms = 0; /* unset; should be treated as = initial_delay */
  p.jitter_pct = 0;

  uint64_t rng = 7;
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 1, &rng), 250);
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 10, &rng), 250);
}

static void attempt_zero_treated_as_one(void** state)
{
  (void)state;
  az_iot_reconnection_policy p = { 0 };
  p.initial_delay_ms = 100;
  p.max_delay_ms = 1000;

  uint64_t rng = 1;
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 0, &rng), 100);
}

static void shift_saturates_no_ub(void** state)
{
  (void)state;
  az_iot_reconnection_policy p = { 0 };
  p.initial_delay_ms = 1;
  p.max_delay_ms = 60000;

  uint64_t rng = 1;
  /* Big attempts shouldn't UB or wrap; result must be the cap. */
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 1000, &rng), 60000);
  assert_int_equal(az_iot_reconnect_delay_ms(&p, UINT32_MAX, &rng), 60000);
}

/* A policy at the very top of the representable range must not wrap. The
 * jitter arithmetic is done in int64_t precisely so the difference of two
 * UINT32_MAX-sized values cannot be narrowed before the clamps.
 *
 * The detector: with base == UINT32_MAX and jitter_pct == 100 the correct band
 * is [0, 2*base] and the result is uniform across it, so about a quarter of
 * the draws land BELOW 2^31. Narrowing the
 * difference to int32_t bounds |jitter| at 2^31, which makes every result at
 * least base - 2^31 -- the low half of the band becomes unreachable. */
static void top_of_range_policy_does_not_wrap(void** state)
{
  (void)state;
  az_iot_reconnection_policy p = { 0 };
  p.initial_delay_ms = UINT32_MAX;
  p.max_delay_ms = UINT32_MAX;
  p.jitter_pct = 100;

  uint64_t rng = 0xA5A5A5A5A5A5A5A5ull;
  int below_half = 0;
  for (int i = 0; i < 2000; ++i)
  {
    uint32_t d = az_iot_reconnect_delay_ms(&p, 1, &rng);
    assert_true(d >= 1u); /* the floor still holds */
    if (d < 2147483648u)
    {
      below_half++;
    }
  }
  /* About 500 of 2000 when the arithmetic is correct; essentially zero when
   * the difference is narrowed, since |jitter| is then bounded at 2^31. */
  assert_true(below_half > 300);

  /* With no jitter the answer is exact, proving the cast path itself is sound
   * at the top of the range. */
  p.jitter_pct = 0;
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 1, &rng), UINT32_MAX);
  assert_int_equal(az_iot_reconnect_delay_ms(&p, 64, &rng), UINT32_MAX);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(disabled_when_initial_delay_is_zero),
    cmocka_unit_test(no_jitter_doubles_until_cap),
    cmocka_unit_test(jitter_stays_within_band),
    cmocka_unit_test(jitter_is_centred_on_the_backoff),
    cmocka_unit_test(top_of_range_policy_does_not_wrap),
    cmocka_unit_test(zero_max_delay_means_initial_is_the_cap),
    cmocka_unit_test(attempt_zero_treated_as_one),
    cmocka_unit_test(shift_saturates_no_ub),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
