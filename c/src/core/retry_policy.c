// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/retry_policy.h"

#include "azure/iot/az_iot_connection_client.h"
#include "internal/mono_time.h"

#include <stddef.h>

static uint64_t xorshift64(uint64_t* s)
{
  uint64_t x = *s;
  if (x == 0)
  {
    x = 0x9E3779B97F4A7C15ull; /* avoid the absorbing zero */
  }
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

uint32_t az_iot_retry_policy__delay_ms(
    const az_iot_retry_policy* policy,
    uint32_t attempt,
    uint64_t* rng_state)
{
  if (!policy || !rng_state)
  {
    return 0;
  }
  if (policy->initial_delay_ms == 0)
  {
    return 0; /* reconnect disabled */
  }
  if (attempt == 0)
  {
    attempt = 1;
  }

  uint32_t shift = attempt - 1;
  if (shift > 30)
  {
    shift = 30;
  }

  uint64_t base = (uint64_t)policy->initial_delay_ms << shift;
  uint32_t cap = policy->max_delay_ms ? policy->max_delay_ms : policy->initial_delay_ms;
  if (base > (uint64_t)cap)
  {
    base = cap;
  }

  uint8_t pct = policy->jitter_pct;
  if (pct > 100)
  {
    pct = 100;
  }

  uint64_t result = base;
  if (pct > 0)
  {
    /* Jitter the backoff by [-pct%, +pct%], computed WITHOUT a signed
     * intermediate.
     *
     * The obvious form is `base + (r - span)`, but that difference is signed
     * and spans [-span, +span]; with initial_delay_ms == max_delay_ms ==
     * UINT32_MAX and pct == 100 it does not fit in 32 bits, so it has to be
     * carried in a wider signed type and narrowed back -- which is exactly the
     * implementation-defined narrowing this used to get wrong.
     *
     * pct is clamped to 100 above, so span <= base and `base - span` cannot
     * underflow. Rearranging to (base - span) + r gives the identical
     * distribution over [base-span, base+span] in unsigned arithmetic only. */
    uint64_t span = (base * pct) / 100u;
    if (span > 0)
    {
      uint64_t r = xorshift64(rng_state) % (2u * span + 1u);
      result = (base - span) + r;
    }
  }

  if (result < 1u)
  {
    result = 1u;
  }
  /* Deliberately NOT clamped back to `cap`. `cap` bounds the BACKOFF -- it is
   * how far the doubling is allowed to climb -- and jitter varies around that,
   * so the delay may exceed it by up to jitter_pct.
   *
   * Clamping here used to fold the whole upper half of the distribution onto
   * the cap itself. Once the ladder reached the cap, half of all retries fired
   * at exactly max_delay_ms and the mean sat jitter_pct/4 below it. So jitter
   * stopped de-correlating a fleet precisely at steady state, which is the
   * thundering herd it exists to prevent, and it biased every fixed-interval
   * policy low (a nominal 5s interval averaged 4749 ms).
   *
   * Only the representable range is enforced, so a caller using the extreme
   * end of uint32_t saturates instead of wrapping on the cast below. */
  if (result > (uint64_t)UINT32_MAX)
  {
    result = (uint64_t)UINT32_MAX;
  }
  return (uint32_t)result;
}

bool az_iot_retry_policy_is_enabled(const az_iot_retry_policy* policy)
{
  return policy != NULL && policy->initial_delay_ms > 0u;
}

bool az_iot_retry_policy__next(
    const az_iot_retry_policy* policy,
    uint32_t* attempt,
    uint64_t* rng_state,
    uint32_t* delay_ms)
{
  /* Checked before counting: the counter saturates, so "count > max" could
   * never hold for max_attempts == UINT32_MAX. */
  bool spent = policy->max_attempts > 0u && *attempt >= policy->max_attempts;
  if (*attempt < UINT32_MAX)
  {
    (*attempt)++;
  }
  if (spent)
  {
    return false;
  }
  *delay_ms = az_iot_retry_policy__delay_ms(policy, *attempt, rng_state);
  return true;
}

bool az_iot_retry_state__schedule(
    az_iot_retry_state* state,
    const az_iot_retry_policy* policy,
    uint64_t* rng_state)
{
  uint32_t delay_ms = 0u;
  if (!az_iot_retry_policy__next(policy, &state->_internal.attempt, rng_state, &delay_ms))
  {
    state->_internal.due_ms = 0u;
    return false;
  }
  state->_internal.due_ms = az_iot_time_mono_ms() + (uint64_t)delay_ms;
  return true;
}

void az_iot_retry_state__defer(az_iot_retry_state* state, uint64_t not_before_ms)
{
  if (state->_internal.due_ms < not_before_ms)
  {
    state->_internal.due_ms = not_before_ms;
  }
}

bool az_iot_retry_state__due(az_iot_retry_state* state)
{
  if (state->_internal.due_ms != 0u && az_iot_time_mono_ms() < state->_internal.due_ms)
  {
    return false;
  }
  state->_internal.due_ms = 0u;
  return true;
}

bool az_iot_retry_state__pending(const az_iot_retry_state* state)
{
  return state->_internal.due_ms != 0u && az_iot_time_mono_ms() < state->_internal.due_ms;
}

uint32_t az_iot_retry_state__attempts(const az_iot_retry_state* state)
{
  return state->_internal.attempt;
}

void az_iot_retry_state__reset(az_iot_retry_state* state)
{
  state->_internal.attempt = 0u;
  state->_internal.due_ms = 0u;
}

az_iot_retry_policy az_iot_connection_client_get_default_retry_policy(void)
{
  az_iot_retry_policy p = {
    .initial_delay_ms = 1000u, /* first retry after 1s           */
    .max_delay_ms = 60000u, /* cap exponential backoff at 60s */
    .max_attempts = 0u, /* 0 = retry forever              */
    .jitter_pct = 20u, /* +/-20% randomization            */
  };
  return p;
}

az_iot_retry_policy az_iot_connection_client_get_disabled_retry_policy(void)
{
  /* initial_delay_ms == 0 is what disables retrying. Returning it from a named
   * getter is the whole point: the value is identical to a zeroed struct, so
   * the difference this makes is at the call site, not in the bytes. */
  az_iot_retry_policy p = {
    .initial_delay_ms = 0u,
    .max_delay_ms = 0u,
    .max_attempts = 0u,
    .jitter_pct = 0u,
  };
  return p;
}

az_iot_retry_policy az_iot_connection_client_get_fixed_interval_retry_policy(
    uint32_t interval_ms,
    uint32_t max_attempts)
{
  /* max_delay_ms == initial_delay_ms pins the backoff at the first rung, which
   * is exactly a fixed interval -- no separate code path needed.
   *
   * A zero interval is clamped to 1 ms rather than passed through: 0 in
   * initial_delay_ms is the sentinel that disables retrying, so honouring it
   * here would hand back a policy that never retries from a function whose
   * name promises the opposite. 1 ms is the smallest schedule the delay
   * calculator can represent (it floors every result at 1), so it is the
   * nearest thing to the caller's request that is still a retry. */
  uint32_t interval = interval_ms ? interval_ms : 1u;
  az_iot_retry_policy p = {
    .initial_delay_ms = interval,
    .max_delay_ms = interval,
    .max_attempts = max_attempts,
    .jitter_pct = 0u,
  };
  return p;
}
