// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/reconnect.h"

#include <stddef.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
uint64_t az_iot_time_mono_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#include <time.h>
uint64_t az_iot_time_mono_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}
#endif

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

uint32_t az_iot_reconnect_delay_ms(
    const az_iot_reconnection_policy* policy,
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

  int32_t jitter = 0;
  uint8_t pct = policy->jitter_pct;
  if (pct > 100)
  {
    pct = 100;
  }
  if (pct > 0 && base > 0)
  {
    /* Range: [-pct%, +pct%] of base. */
    uint64_t span = ((uint64_t)base * pct) / 100u;
    if (span > 0)
    {
      uint64_t r = xorshift64(rng_state) % (2u * span + 1u);
      jitter = (int32_t)((int64_t)r - (int64_t)span);
    }
  }

  int64_t result = (int64_t)base + jitter;
  if (result < 1)
  {
    result = 1;
  }
  /* Deliberately NOT clamped back to `cap`. `cap` bounds the BACKOFF -- it is
   * how far the doubling is allowed to climb -- and jitter varies around that,
   * so the delay may exceed it by up to jitter_pct.
   *
   * Clamping here used to fold the whole upper half of the distribution onto
   * the cap itself. Once the ladder reached the cap, half of all retries fired
   * at exactly max_delay_ms: measured over 200000 draws of the default policy,
   * 50.1% landed on 30000 ms and the mean sat at 28507 ms instead of 30000. So
   * jitter stopped de-correlating a fleet precisely at steady state, which is
   * the thundering herd it exists to prevent, and it biased every
   * fixed-interval policy low (a nominal 5s interval averaged 4749 ms).
   *
   * Only the representable range is enforced, so a caller using the extreme
   * end of uint32_t cannot wrap. */
  if (result > (int64_t)UINT32_MAX)
  {
    result = (int64_t)UINT32_MAX;
  }
  return (uint32_t)result;
}

az_iot_reconnection_policy az_iot_reconnection_policy_default(void)
{
  az_iot_reconnection_policy p = {
    .initial_delay_ms = 1000u, /* first retry after 1s           */
    .max_delay_ms = 30000u, /* cap exponential backoff at 30s */
    .max_attempts = 0u, /* 0 = retry forever              */
    .jitter_pct = 20u, /* +/-20% randomization            */
  };
  return p;
}
