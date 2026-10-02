// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Retry policy: delay calculator, enablement and retry state. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "internal/mono_time.h"
#include "internal/retry_policy.h"

static void disabled_when_initial_delay_is_zero(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  uint64_t rng = 1;
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1, &rng), 0);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 5, &rng), 0);
}

static void no_jitter_doubles_until_cap(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  p.initial_delay_ms = 100;
  p.max_delay_ms = 1000;
  p.jitter_pct = 0;

  uint64_t rng = 42;
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1, &rng), 100);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 2, &rng), 200);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 3, &rng), 400);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 4, &rng), 800);
  /* 100 << 4 = 1600 > cap; clamped. */
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 5, &rng), 1000);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 100, &rng), 1000);
}

/* Jitter varies AROUND the backoff, so the band is symmetric and the top of it
 * sits above max_delay_ms. It used to be clamped back to the cap, which folded
 * every positive draw onto the cap itself -- half the fleet retrying on the
 * same instant, which is what jitter exists to prevent. */
static void jitter_stays_within_band(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  p.initial_delay_ms = 1000;
  p.max_delay_ms = 1000;
  p.jitter_pct = 20; /* +/- 20% of base */

  uint64_t rng = 0xDEADBEEFCAFEBABEull;
  int above_cap = 0;
  for (int i = 0; i < 200; ++i)
  {
    uint32_t d = az_iot_retry_policy__delay_ms(&p, 1, &rng);
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
  az_iot_retry_policy p = { 0 };
  p.initial_delay_ms = 5000;
  p.max_delay_ms = 5000; /* fixed interval: base == cap from attempt 1 */
  p.jitter_pct = 20;

  uint64_t rng = 0x12345678ull;
  const int n = 20000;
  double sum = 0;
  int on_cap = 0;
  for (int i = 0; i < n; ++i)
  {
    uint32_t d = az_iot_retry_policy__delay_ms(&p, 1, &rng);
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
  az_iot_retry_policy p = { 0 };
  p.initial_delay_ms = 250;
  p.max_delay_ms = 0; /* unset; should be treated as = initial_delay */
  p.jitter_pct = 0;

  uint64_t rng = 7;
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1, &rng), 250);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 10, &rng), 250);
}

static void attempt_zero_treated_as_one(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  p.initial_delay_ms = 100;
  p.max_delay_ms = 1000;

  uint64_t rng = 1;
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 0, &rng), 100);
}

static void shift_saturates_no_ub(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  p.initial_delay_ms = 1;
  p.max_delay_ms = 60000;

  uint64_t rng = 1;
  /* Big attempts shouldn't UB or wrap; result must be the cap. */
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1000, &rng), 60000);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, UINT32_MAX, &rng), 60000);
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
/* A policy at the very top of the representable range must neither wrap nor
 * lose half its band.
 *
 * Two distinct failure modes are pinned here, because each survives the other's
 * check:
 *
 *  - Computing the jitter through a narrower signed type bounds it at 2^31, so
 *    the low part of the band becomes unreachable. Detected by `below_half`.
 *  - Dropping the saturation clamp lets a result above UINT32_MAX truncate on
 *    the cast. Those wrap to small values, which would only INCREASE
 *    `below_half` -- so that counter cannot see it. Detected by `on_max`:
 *    saturation puts a large spike on exactly UINT32_MAX, and truncation
 *    removes it. */
static void top_of_range_policy_does_not_wrap(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  p.initial_delay_ms = UINT32_MAX;
  p.max_delay_ms = UINT32_MAX;
  p.jitter_pct = 100;

  /* base == UINT32_MAX and span == base, so the mathematical result is uniform
   * over [0, 2*UINT32_MAX]: about half the draws exceed UINT32_MAX and must
   * saturate, and about a quarter fall below 2^31. */
  uint64_t rng = 0xA5A5A5A5A5A5A5A5ull;
  int below_half = 0;
  int on_max = 0;
  for (int i = 0; i < 2000; ++i)
  {
    uint32_t d = az_iot_retry_policy__delay_ms(&p, 1, &rng);
    assert_true(d >= 1u); /* the floor still holds */
    if (d < 2147483648u)
    {
      below_half++;
    }
    if (d == UINT32_MAX)
    {
      on_max++;
    }
  }
  /* About 500 of 2000 when the arithmetic is correct; essentially zero when
   * the difference is narrowed, since |jitter| is then bounded at 2^31. */
  assert_true(below_half > 300);
  /* About 1000 of 2000 when the oversized results saturate; essentially zero
   * when they truncate instead (a wrapped value hits UINT32_MAX only by a
   * 1-in-2^32 coincidence). */
  assert_true(on_max > 600);

  /* With no jitter the answer is exact, proving the cast path itself is sound
   * at the top of the range. */
  p.jitter_pct = 0;
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1, &rng), UINT32_MAX);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 64, &rng), UINT32_MAX);
}

/* The saturation clamp on its own, with a narrow band so the assertion is a
 * hard bound rather than a distribution shape.
 *
 * base is UINT32_MAX and the band is +/-1% of it, so every legitimate result
 * is at least base - base/100 = 4252017623, and the upper half of the band
 * lies above UINT32_MAX and must come back as exactly UINT32_MAX. A truncated
 * result would land far below the lower bound, which the per-draw assertion
 * catches outright. */
static void an_oversized_result_saturates_rather_than_wrapping(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  p.initial_delay_ms = UINT32_MAX;
  p.max_delay_ms = UINT32_MAX;
  p.jitter_pct = 1;

  uint64_t rng = 0x0123456789ABCDEFull;
  int on_max = 0;
  for (int i = 0; i < 500; ++i)
  {
    uint32_t d = az_iot_retry_policy__delay_ms(&p, 1, &rng);
    assert_true(d >= 4252017623u);
    if (d == UINT32_MAX)
    {
      on_max++;
    }
  }
  assert_true(on_max > 150); /* about half the band saturates */
}

/* The getters are not just field bundles: each names a curve, and the curve is
 * what an application is choosing. Assert the curve, not the struct. */
static void the_default_getter_backs_off_and_then_holds_at_the_cap(void** state)
{
  (void)state;
  az_iot_retry_policy p = az_iot_connection_client_get_default_retry_policy();
  p.jitter_pct = 0; /* the ladder, without the randomization on top */
  uint64_t rng = 99;

  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1, &rng), 1000);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 2, &rng), 2000);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 3, &rng), 4000);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 4, &rng), 8000);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 5, &rng), 16000);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 6, &rng), 32000);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 7, &rng), 60000); /* capped */
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 20, &rng), 60000);
}

static void the_retry_disabled_getter_yields_no_delay_at_all(void** state)
{
  (void)state;
  az_iot_retry_policy p = az_iot_connection_client_get_disabled_retry_policy();
  uint64_t rng = 1;

  /* 0 is how the caller of this function learns retrying is off. */
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1, &rng), 0);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 7, &rng), 0);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, UINT32_MAX, &rng), 0);
}

/* The mechanism is max_delay_ms == initial_delay_ms, which pins the backoff at
 * the first rung. Nothing in az_iot_retry_policy__delay_ms() special-cases it. */
static void the_fixed_interval_getter_yields_a_flat_curve(void** state)
{
  (void)state;
  az_iot_retry_policy p = az_iot_connection_client_get_fixed_interval_retry_policy(5000, 360);
  uint64_t rng = 4242;

  for (uint32_t attempt = 1; attempt <= 12; ++attempt)
  {
    assert_int_equal(az_iot_retry_policy__delay_ms(&p, attempt, &rng), 5000);
  }
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1000, &rng), 5000);
}

/* ---- az_iot_retry_policy__delay_ms: arguments and bounds ------------------ */

/* NULL arguments yield 0 ("no retry") rather than a crash. */
static void delay_with_null_arguments_is_zero(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 100, .max_delay_ms = 1000, .jitter_pct = 20 };
  uint64_t rng = 1;
  assert_int_equal(az_iot_retry_policy__delay_ms(NULL, 1, &rng), 0);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1, NULL), 0);
  assert_int_equal(rng, 1); /* not drawn from */
}

/* Neither the policy nor, without jitter, the PRNG state is modified. */
static void delay_leaves_policy_and_unused_rng_untouched(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 100, .max_delay_ms = 1000, .max_attempts = 3 };
  az_iot_retry_policy copy = p;
  uint64_t rng = 77;
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 3, &rng), 400);
  assert_memory_equal(&p, &copy, sizeof(p));
  assert_int_equal(rng, 77);
}

/* A cap below the initial delay wins from the first attempt. */
static void a_cap_below_the_initial_delay_wins(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 1000, .max_delay_ms = 500 };
  uint64_t rng = 1;
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1, &rng), 500);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 9, &rng), 500);
}

/* The doubling stops at 2^30, the largest shift applied: attempt 31 and every
 * later one get the same base. */
static void the_shift_is_capped_at_thirty(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 1, .max_delay_ms = UINT32_MAX };
  uint64_t rng = 1;
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 30, &rng), 1u << 29);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 31, &rng), 1u << 30);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, 32, &rng), 1u << 30);
  assert_int_equal(az_iot_retry_policy__delay_ms(&p, UINT32_MAX, &rng), 1u << 30);
}

/* ---- az_iot_retry_policy__delay_ms: jitter --------------------------------- */

/* jitter_pct above 100 behaves exactly as 100. */
static void jitter_above_one_hundred_percent_is_clamped(void** state)
{
  (void)state;
  az_iot_retry_policy clamped
      = { .initial_delay_ms = 1000, .max_delay_ms = 1000, .jitter_pct = 255 };
  az_iot_retry_policy full = clamped;
  full.jitter_pct = 100;
  uint64_t rng_a = 0x1234;
  uint64_t rng_b = 0x1234;
  for (int i = 0; i < 500; ++i)
  {
    uint32_t d = az_iot_retry_policy__delay_ms(&clamped, 1, &rng_a);
    assert_int_equal(d, az_iot_retry_policy__delay_ms(&full, 1, &rng_b));
    assert_true(d >= 1 && d <= 2000);
  }
}

/* A band narrower than 1 ms applies no jitter and draws nothing. */
static void a_sub_millisecond_band_applies_no_jitter(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 1, .max_delay_ms = 1, .jitter_pct = 50 };
  uint64_t rng = 99;
  for (int i = 0; i < 10; ++i)
  {
    assert_int_equal(az_iot_retry_policy__delay_ms(&p, 1, &rng), 1);
  }
  assert_int_equal(rng, 99);
}

/* The lowest jittered value (0) is floored at 1 ms, never "no delay". */
static void a_jittered_zero_is_floored_at_one(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 1, .max_delay_ms = 1, .jitter_pct = 100 };
  uint64_t rng = 0xC0FFEE;
  int ones = 0;
  int twos = 0;
  for (int i = 0; i < 600; ++i)
  {
    uint32_t d = az_iot_retry_policy__delay_ms(&p, 1, &rng);
    assert_true(d == 1 || d == 2);
    ones += (d == 1);
    twos += (d == 2);
  }
  /* Draws of 0 and 1 both give 1, so about 2/3 of results are 1. */
  assert_true(ones > 300);
  assert_true(twos > 100);
}

/* Every value in the band is reachable, and the band's ends are hit. */
static void every_value_in_the_band_is_reachable(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 100, .max_delay_ms = 100, .jitter_pct = 10 };
  uint64_t rng = 0x5EED;
  int seen[21] = { 0 };
  for (int i = 0; i < 5000; ++i)
  {
    uint32_t d = az_iot_retry_policy__delay_ms(&p, 1, &rng);
    assert_true(d >= 90 && d <= 110);
    seen[d - 90] = 1;
  }
  for (int i = 0; i < 21; ++i)
  {
    assert_true(seen[i]);
  }
}

/* The same seed gives the same sequence, so tests can pin a schedule. */
static void jitter_is_deterministic_for_a_seed(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 1000, .max_delay_ms = 60000, .jitter_pct = 20 };
  uint64_t rng_a = 42;
  uint64_t rng_b = 42;
  for (uint32_t attempt = 1; attempt <= 20; ++attempt)
  {
    assert_int_equal(
        az_iot_retry_policy__delay_ms(&p, attempt, &rng_a),
        az_iot_retry_policy__delay_ms(&p, attempt, &rng_b));
  }
  assert_int_equal(rng_a, rng_b);
}

/* A zero PRNG state, which xorshift cannot leave on its own, still jitters. */
static void a_zero_rng_state_still_jitters(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 1000, .max_delay_ms = 1000, .jitter_pct = 20 };
  uint64_t rng = 0;
  uint32_t first = az_iot_retry_policy__delay_ms(&p, 1, &rng);
  assert_int_not_equal(rng, 0);
  int differs = 0;
  for (int i = 0; i < 50; ++i)
  {
    differs += (az_iot_retry_policy__delay_ms(&p, 1, &rng) != first);
  }
  assert_true(differs > 0);
}

/* ---- az_iot_retry_policy_is_enabled ----------------------------------------- */

/* Only initial_delay_ms decides; the other fields do not. */
static void is_enabled_depends_only_on_the_initial_delay(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  assert_false(az_iot_retry_policy_is_enabled(NULL));
  assert_false(az_iot_retry_policy_is_enabled(&p));
  p.max_delay_ms = 1000;
  p.max_attempts = 5;
  p.jitter_pct = 20;
  assert_false(az_iot_retry_policy_is_enabled(&p));
  p.initial_delay_ms = 1;
  assert_true(az_iot_retry_policy_is_enabled(&p));
  p.initial_delay_ms = UINT32_MAX;
  assert_true(az_iot_retry_policy_is_enabled(&p));
}

/* ---- az_iot_retry_policy__next ------------------------------------------------ */

/* Exactly max_attempts retries are allowed; the counter still counts the refusal. */
static void next_allows_exactly_max_attempts(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 100, .max_delay_ms = 1000, .max_attempts = 3 };
  uint64_t rng = 1;
  uint32_t attempt = 0;
  uint32_t delay = 0;
  assert_true(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_int_equal(attempt, 1);
  assert_int_equal(delay, 100);
  assert_true(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_int_equal(delay, 200);
  assert_true(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_int_equal(attempt, 3);
  assert_int_equal(delay, 400);

  delay = 12345;
  assert_false(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_int_equal(attempt, 4);
  assert_int_equal(delay, 12345); /* not written on refusal */
  assert_false(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
}

/* max_attempts == 1: one retry, then none. */
static void next_with_a_single_attempt_budget(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 100, .max_attempts = 1 };
  uint64_t rng = 1;
  uint32_t attempt = 0;
  uint32_t delay = 0;
  assert_true(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_false(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
}

/* No limit: the counter saturates at UINT32_MAX instead of wrapping to the first rung. */
static void next_without_a_limit_saturates_the_counter(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 100, .max_delay_ms = 1000 };
  uint64_t rng = 1;
  uint32_t attempt = UINT32_MAX - 1u;
  uint32_t delay = 0;
  assert_true(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_int_equal(attempt, UINT32_MAX);
  assert_true(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_int_equal(attempt, UINT32_MAX);
  assert_int_equal(delay, 1000);
}

/* The largest finite budget is still finite, although the counter saturates. */
static void next_honours_a_budget_of_uint32_max(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 100, .max_attempts = UINT32_MAX };
  uint64_t rng = 1;
  uint32_t attempt = UINT32_MAX - 1u;
  uint32_t delay = 0;
  assert_true(az_iot_retry_policy__next(&p, &attempt, &rng, &delay)); /* the last allowed */
  assert_int_equal(attempt, UINT32_MAX);
  assert_false(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_false(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_int_equal(attempt, UINT32_MAX);
}

/* Enablement is the caller's check: a disabled policy counts and yields 0 ms. */
static void next_on_a_disabled_policy_yields_zero(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  uint64_t rng = 1;
  uint32_t attempt = 0;
  uint32_t delay = 99;
  assert_true(az_iot_retry_policy__next(&p, &attempt, &rng, &delay));
  assert_int_equal(attempt, 1);
  assert_int_equal(delay, 0);
}

/* ---- az_iot_retry_state ---------------------------------------------------- */

/* A zeroed state has nothing scheduled and nothing counted. */
static void a_zeroed_state_is_idle(void** state)
{
  (void)state;
  az_iot_retry_state s = { 0 };
  assert_int_equal(az_iot_retry_state__attempts(&s), 0);
  assert_false(az_iot_retry_state__pending(&s));
  assert_true(az_iot_retry_state__due(&s));
  assert_true(az_iot_retry_state__due(&s));
}

/* schedule() counts, sets the time from the policy, and leaves it pending. */
static void schedule_sets_the_due_time_from_the_policy(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 60000, .max_delay_ms = 120000 };
  uint64_t rng = 1;
  az_iot_retry_state s = { 0 };

  uint64_t before = az_iot_time_mono_ms();
  assert_true(az_iot_retry_state__schedule(&s, &p, &rng));
  uint64_t after = az_iot_time_mono_ms();
  assert_int_equal(az_iot_retry_state__attempts(&s), 1);
  assert_true(s._internal.due_ms >= before + 60000u && s._internal.due_ms <= after + 60000u);

  before = az_iot_time_mono_ms();
  assert_true(az_iot_retry_state__schedule(&s, &p, &rng));
  after = az_iot_time_mono_ms();
  assert_int_equal(az_iot_retry_state__attempts(&s), 2);
  assert_true(s._internal.due_ms >= before + 120000u && s._internal.due_ms <= after + 120000u);
}

/* pending() reports without consuming; due() refuses while pending, keeping
 * the time, then fires once. */
static void a_scheduled_retry_fires_exactly_once(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 60000 };
  uint64_t rng = 1;
  az_iot_retry_state s = { 0 };
  assert_true(az_iot_retry_state__schedule(&s, &p, &rng));
  uint64_t due = s._internal.due_ms;

  assert_true(az_iot_retry_state__pending(&s));
  assert_true(az_iot_retry_state__pending(&s));
  assert_false(az_iot_retry_state__due(&s));
  assert_int_equal(s._internal.due_ms, due);

  s._internal.due_ms = az_iot_time_mono_ms(); /* elapse */
  assert_false(az_iot_retry_state__pending(&s));
  assert_int_not_equal(s._internal.due_ms, 0); /* pending() did not consume */
  assert_true(az_iot_retry_state__due(&s));
  assert_int_equal(s._internal.due_ms, 0);
  assert_int_equal(az_iot_retry_state__attempts(&s), 1); /* due() does not reset the count */
}

/* A spent budget schedules nothing and drops a retry already scheduled. */
static void a_spent_budget_cancels_the_scheduled_retry(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 60000, .max_attempts = 1 };
  uint64_t rng = 1;
  az_iot_retry_state s = { 0 };
  assert_true(az_iot_retry_state__schedule(&s, &p, &rng));
  assert_true(az_iot_retry_state__pending(&s));
  assert_false(az_iot_retry_state__schedule(&s, &p, &rng));
  assert_int_equal(s._internal.due_ms, 0);
  assert_false(az_iot_retry_state__pending(&s));
  assert_int_equal(az_iot_retry_state__attempts(&s), 2);
}

/* A disabled policy's 0 ms delay is due at once, never pending. */
static void a_zero_delay_is_due_at_once(void** state)
{
  (void)state;
  az_iot_retry_policy p = { 0 };
  uint64_t rng = 1;
  az_iot_retry_state s = { 0 };
  assert_true(az_iot_retry_state__schedule(&s, &p, &rng));
  assert_false(az_iot_retry_state__pending(&s));
  assert_true(az_iot_retry_state__due(&s));
}

/* defer() only ever moves the retry later, and can schedule one where none was. */
static void defer_only_moves_the_retry_later(void** state)
{
  (void)state;
  az_iot_retry_state s = { 0 };
  s._internal.due_ms = 5000;
  az_iot_retry_state__defer(&s, 4000);
  assert_int_equal(s._internal.due_ms, 5000);
  az_iot_retry_state__defer(&s, 5000);
  assert_int_equal(s._internal.due_ms, 5000);
  az_iot_retry_state__defer(&s, 9000);
  assert_int_equal(s._internal.due_ms, 9000);

  az_iot_retry_state idle = { 0 };
  az_iot_retry_state__defer(&idle, az_iot_time_mono_ms() + 60000u);
  assert_true(az_iot_retry_state__pending(&idle));
  assert_int_equal(az_iot_retry_state__attempts(&idle), 0);
}

/* A service floor longer than the backoff holds the retry until the floor. */
static void defer_holds_a_scheduled_retry_past_its_backoff(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 1 };
  uint64_t rng = 1;
  az_iot_retry_state s = { 0 };
  assert_true(az_iot_retry_state__schedule(&s, &p, &rng));
  uint64_t floor_ms = az_iot_time_mono_ms() + 60000u;
  az_iot_retry_state__defer(&s, floor_ms);
  assert_int_equal(s._internal.due_ms, floor_ms);
  assert_false(az_iot_retry_state__due(&s));
}

/* reset() clears both the count and a scheduled retry, restarting the ladder. */
static void reset_restarts_the_ladder(void** state)
{
  (void)state;
  az_iot_retry_policy p = { .initial_delay_ms = 1000, .max_delay_ms = 60000, .max_attempts = 2 };
  uint64_t rng = 1;
  az_iot_retry_state s = { 0 };
  assert_true(az_iot_retry_state__schedule(&s, &p, &rng));
  assert_true(az_iot_retry_state__schedule(&s, &p, &rng));
  assert_false(az_iot_retry_state__schedule(&s, &p, &rng));

  az_iot_retry_state__reset(&s);
  assert_int_equal(az_iot_retry_state__attempts(&s), 0);
  assert_int_equal(s._internal.due_ms, 0);

  uint64_t before = az_iot_time_mono_ms();
  assert_true(az_iot_retry_state__schedule(&s, &p, &rng));
  assert_true(s._internal.due_ms < before + 2000u); /* first rung again */

  /* And a retry still pending is cancelled. */
  assert_true(az_iot_retry_state__pending(&s));
  az_iot_retry_state__reset(&s);
  assert_false(az_iot_retry_state__pending(&s));
  assert_int_equal(s._internal.due_ms, 0);
}

/* ---- getters (exact fields) ------------------------------------------------ */

static void the_default_getter_fields(void** state)
{
  (void)state;
  az_iot_retry_policy p = az_iot_connection_client_get_default_retry_policy();
  assert_int_equal(p.initial_delay_ms, 1000);
  assert_int_equal(p.max_delay_ms, 60000);
  assert_int_equal(p.max_attempts, 0);
  assert_int_equal(p.jitter_pct, 20);
  assert_true(az_iot_retry_policy_is_enabled(&p));
}

/* The disabled policy is byte-for-byte a zeroed one. */
static void the_disabled_getter_is_a_zeroed_policy(void** state)
{
  (void)state;
  az_iot_retry_policy p = az_iot_connection_client_get_disabled_retry_policy();
  az_iot_retry_policy zero;
  memset(&zero, 0, sizeof(zero));
  assert_int_equal(p.initial_delay_ms, zero.initial_delay_ms);
  assert_int_equal(p.max_delay_ms, zero.max_delay_ms);
  assert_int_equal(p.max_attempts, zero.max_attempts);
  assert_int_equal(p.jitter_pct, zero.jitter_pct);
  assert_false(az_iot_retry_policy_is_enabled(&p));
}

/* Fixed interval: passes both arguments through, no jitter, and clamps a zero
 * interval to 1 ms so it still retries. */
static void the_fixed_interval_getter_fields(void** state)
{
  (void)state;
  az_iot_retry_policy p
      = az_iot_connection_client_get_fixed_interval_retry_policy(UINT32_MAX, UINT32_MAX);
  assert_int_equal(p.initial_delay_ms, UINT32_MAX);
  assert_int_equal(p.max_delay_ms, UINT32_MAX);
  assert_int_equal(p.max_attempts, UINT32_MAX);
  assert_int_equal(p.jitter_pct, 0);

  p = az_iot_connection_client_get_fixed_interval_retry_policy(0, 0);
  assert_int_equal(p.initial_delay_ms, 1);
  assert_int_equal(p.max_delay_ms, 1);
  assert_int_equal(p.max_attempts, 0);
  assert_true(az_iot_retry_policy_is_enabled(&p));
}

/* ---- az_iot_time_mono_ms ----------------------------------------------------- */

/* The clock never goes backwards and advances. */
static void the_monotonic_clock_never_goes_backwards(void** state)
{
  (void)state;
  uint64_t start = az_iot_time_mono_ms();
  uint64_t prev = start;
  while (prev - start < 5u)
  {
    uint64_t now = az_iot_time_mono_ms();
    assert_true(now >= prev);
    prev = now;
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(disabled_when_initial_delay_is_zero),
    cmocka_unit_test(no_jitter_doubles_until_cap),
    cmocka_unit_test(jitter_stays_within_band),
    cmocka_unit_test(jitter_is_centred_on_the_backoff),
    cmocka_unit_test(top_of_range_policy_does_not_wrap),
    cmocka_unit_test(an_oversized_result_saturates_rather_than_wrapping),
    cmocka_unit_test(zero_max_delay_means_initial_is_the_cap),
    cmocka_unit_test(attempt_zero_treated_as_one),
    cmocka_unit_test(shift_saturates_no_ub),
    cmocka_unit_test(the_default_getter_backs_off_and_then_holds_at_the_cap),
    cmocka_unit_test(the_retry_disabled_getter_yields_no_delay_at_all),
    cmocka_unit_test(the_fixed_interval_getter_yields_a_flat_curve),
    cmocka_unit_test(delay_with_null_arguments_is_zero),
    cmocka_unit_test(delay_leaves_policy_and_unused_rng_untouched),
    cmocka_unit_test(a_cap_below_the_initial_delay_wins),
    cmocka_unit_test(the_shift_is_capped_at_thirty),
    cmocka_unit_test(jitter_above_one_hundred_percent_is_clamped),
    cmocka_unit_test(a_sub_millisecond_band_applies_no_jitter),
    cmocka_unit_test(a_jittered_zero_is_floored_at_one),
    cmocka_unit_test(every_value_in_the_band_is_reachable),
    cmocka_unit_test(jitter_is_deterministic_for_a_seed),
    cmocka_unit_test(a_zero_rng_state_still_jitters),
    cmocka_unit_test(is_enabled_depends_only_on_the_initial_delay),
    cmocka_unit_test(next_allows_exactly_max_attempts),
    cmocka_unit_test(next_with_a_single_attempt_budget),
    cmocka_unit_test(next_without_a_limit_saturates_the_counter),
    cmocka_unit_test(next_honours_a_budget_of_uint32_max),
    cmocka_unit_test(next_on_a_disabled_policy_yields_zero),
    cmocka_unit_test(a_zeroed_state_is_idle),
    cmocka_unit_test(schedule_sets_the_due_time_from_the_policy),
    cmocka_unit_test(a_scheduled_retry_fires_exactly_once),
    cmocka_unit_test(a_spent_budget_cancels_the_scheduled_retry),
    cmocka_unit_test(a_zero_delay_is_due_at_once),
    cmocka_unit_test(defer_only_moves_the_retry_later),
    cmocka_unit_test(defer_holds_a_scheduled_retry_past_its_backoff),
    cmocka_unit_test(reset_restarts_the_ladder),
    cmocka_unit_test(the_default_getter_fields),
    cmocka_unit_test(the_disabled_getter_is_a_zeroed_policy),
    cmocka_unit_test(the_fixed_interval_getter_fields),
    cmocka_unit_test(the_monotonic_clock_never_goes_backwards),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
