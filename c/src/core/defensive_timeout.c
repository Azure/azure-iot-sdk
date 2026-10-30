// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/defensive_timeout.h"

static const uint32_t defensive_timeout_base_ms[] = { 300000u, 360000u, 480000u, 600000u };

#define DEFENSIVE_TIMEOUT_BASE_COUNT \
  (sizeof(defensive_timeout_base_ms) / sizeof(defensive_timeout_base_ms[0]))

static uint32_t schedule_base_ms(
    const uint32_t* base_ms_by_attempt,
    size_t attempt_count,
    uint32_t attempt)
{
  size_t index = attempt == 0u ? 0u : (size_t)attempt - 1u;
  if (index >= attempt_count)
  {
    index = attempt_count - 1u;
  }
  return base_ms_by_attempt[index];
}

uint32_t az_iot_defensive_timeout__base_ms(uint32_t attempt)
{
  return schedule_base_ms(defensive_timeout_base_ms, DEFENSIVE_TIMEOUT_BASE_COUNT, attempt);
}

uint32_t az_iot_defensive_timeout__ms(uint32_t attempt, uint64_t random_value)
{
  return az_iot_defensive_timeout__schedule_ms(
      defensive_timeout_base_ms,
      DEFENSIVE_TIMEOUT_BASE_COUNT,
      AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS,
      attempt,
      random_value);
}

uint32_t az_iot_defensive_timeout__schedule_ms(
    const uint32_t* base_ms_by_attempt,
    size_t attempt_count,
    uint32_t jitter_max_ms,
    uint32_t attempt,
    uint64_t random_value)
{
  uint32_t base = schedule_base_ms(base_ms_by_attempt, attempt_count, attempt);
  uint32_t jitter = (uint32_t)(random_value % ((uint64_t)jitter_max_ms + 1u));
  return jitter > UINT32_MAX - base ? UINT32_MAX : base + jitter;
}

uint32_t az_iot_defensive_timeout__next_attempt(uint32_t consecutive_misses)
{
  return consecutive_misses < UINT32_MAX ? consecutive_misses + 1u : UINT32_MAX;
}

void az_iot_defensive_timeout__count_miss(uint32_t* consecutive_misses)
{
  if (*consecutive_misses < UINT32_MAX)
  {
    ++*consecutive_misses;
  }
}
