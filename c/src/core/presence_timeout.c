// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/presence_timeout.h"

#include <stddef.h>

#define PRESENCE_TIMEOUT_MINUTE_MS 60000u

uint32_t az_iot_presence_timeout__base_ms(uint32_t attempt)
{
  switch (attempt)
  {
    case 0:
    case 1:
      return 5u * PRESENCE_TIMEOUT_MINUTE_MS;
    case 2:
      return 6u * PRESENCE_TIMEOUT_MINUTE_MS;
    case 3:
      return 8u * PRESENCE_TIMEOUT_MINUTE_MS;
    default:
      return 10u * PRESENCE_TIMEOUT_MINUTE_MS;
  }
}

uint32_t az_iot_presence_timeout__ms(uint32_t attempt, uint32_t jitter_ms)
{
  if (jitter_ms > AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS)
  {
    jitter_ms = AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS;
  }
  return az_iot_presence_timeout__base_ms(attempt) + jitter_ms;
}

uint32_t az_iot_presence_timeout__random_jitter_ms(uint64_t* rng_state)
{
  if (rng_state == NULL)
  {
    return 0;
  }
  uint64_t x = *rng_state;
  if (x == 0)
  {
    x = 0x9E3779B97F4A7C15ull; /* avoid the absorbing zero */
  }
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *rng_state = x;
  /* The modulo bias over 64 bits is negligible for this range. */
  return (uint32_t)(x % ((uint64_t)AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS + 1u));
}
