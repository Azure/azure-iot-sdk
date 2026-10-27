// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file presence_timeout.h
 * @brief Defensive timeout schedule for messages an mqttv5 connection waits on.
 *
 * Presence protocol schedule: base + uniform jitter in [0, 5 min]. The base is
 * 5 min for attempt 1, 6 min for attempt 2, 8 min for attempt 3, and 10 min
 * from attempt 4 on. Callers own the attempt count.
 */
#ifndef AZ_IOT_PRESENCE_TIMEOUT_INTERNAL_H
#define AZ_IOT_PRESENCE_TIMEOUT_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Largest jitter added to the base delay: 5 minutes. */
#define AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS 300000u

  /**
   * @brief Base delay of 1-based @p attempt (0 is treated as 1).
   *
   * @param[in] attempt Attempt number.
   * @return 300000, 360000, 480000, or 600000 milliseconds.
   */
  uint32_t az_iot_presence_timeout__base_ms(uint32_t attempt);

  /**
   * @brief Timeout of 1-based @p attempt with a given jitter.
   *
   * @param[in] attempt   Attempt number (0 is treated as 1).
   * @param[in] jitter_ms Jitter; capped at AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS.
   * @return Base delay plus the capped jitter, in milliseconds.
   */
  uint32_t az_iot_presence_timeout__ms(uint32_t attempt, uint32_t jitter_ms);

  /**
   * @brief Draw a jitter uniformly from [0, AZ_IOT_PRESENCE_TIMEOUT_JITTER_MAX_MS].
   *
   * @param[in,out] rng_state xorshift64 state; must be non-NULL.
   * @return The jitter in milliseconds.
   */
  uint32_t az_iot_presence_timeout__random_jitter_ms(uint64_t* rng_state);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_PRESENCE_TIMEOUT_INTERNAL_H */
