// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/defensive_timeout.h"

#include <stddef.h>

static const uint32_t defensive_timeout_base_ms[] = { 300000u, 360000u, 480000u, 600000u };

#define DEFENSIVE_TIMEOUT_BASE_COUNT \
  (sizeof(defensive_timeout_base_ms) / sizeof(defensive_timeout_base_ms[0]))

uint32_t az_iot_defensive_timeout__base_ms(uint32_t attempt)
{
  size_t index = attempt == 0u ? 0u : (size_t)attempt - 1u;
  if (index >= DEFENSIVE_TIMEOUT_BASE_COUNT)
  {
    index = DEFENSIVE_TIMEOUT_BASE_COUNT - 1u;
  }
  return defensive_timeout_base_ms[index];
}

uint32_t az_iot_defensive_timeout__ms(uint32_t attempt, uint64_t random_value)
{
  uint32_t jitter
      = (uint32_t)(random_value % ((uint64_t)AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS + 1u));
  return az_iot_defensive_timeout__base_ms(attempt) + jitter;
}
