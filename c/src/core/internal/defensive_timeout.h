// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file defensive_timeout.h
 * @brief Escalating, jittered schedule for mqttv5 defensive timeouts.
 *
 * Bounds the wait for a service message that may never come (the presence
 * birth-ack, a twin push). Attempt N waits base(N) plus a uniform jitter in
 * [0, AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS]:
 *
 * | Attempt | Base   | Range      |
 * |---------|--------|------------|
 * | 1       | 5 min  | 5-10 min   |
 * | 2       | 6 min  | 6-11 min   |
 * | 3       | 8 min  | 8-13 min   |
 * | 4+      | 10 min | 10-15 min  |
 *
 * The caller owns the attempt count and the random source, so tests can make
 * the result deterministic.
 */
#ifndef AZ_IOT_DEFENSIVE_TIMEOUT_INTERNAL_H
#define AZ_IOT_DEFENSIVE_TIMEOUT_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Largest jitter added to the base, in milliseconds (5 min). */
#define AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS 300000u

  /**
   * @brief Base timeout of 1-based attempt @p attempt (0 is treated as 1).
   *
   * @param[in] attempt Attempt number.
   * @return 300000, 360000, 480000, then 600000 for every later attempt.
   */
  uint32_t az_iot_defensive_timeout__base_ms(uint32_t attempt);

  /**
   * @brief Timeout of 1-based attempt @p attempt: base plus jitter.
   *
   * @param[in] attempt Attempt number (0 is treated as 1).
   * @param[in] random_value Uniformly distributed value; the jitter is
   *                         @p random_value modulo
   *                         (AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS + 1).
   * @return The timeout in milliseconds, in
   *         [base, base + AZ_IOT_DEFENSIVE_TIMEOUT_JITTER_MAX_MS].
   */
  uint32_t az_iot_defensive_timeout__ms(uint32_t attempt, uint64_t random_value);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DEFENSIVE_TIMEOUT_INTERNAL_H */
