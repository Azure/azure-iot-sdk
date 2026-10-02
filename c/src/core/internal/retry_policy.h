// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file retry_policy.h
 * @brief Internal retry scheduling over an az_iot_retry_policy.
 *
 * The policy is immutable; each schedule's progress lives in an
 * az_iot_retry_state owned by the module that retries. Jitter draws from a
 * caller-owned PRNG state so a client can share one generator and tests can
 * seed it.
 */
#ifndef AZ_IOT_RETRY_POLICY_INTERNAL_H
#define AZ_IOT_RETRY_POLICY_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "azure/iot/az_iot_retry_policy.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Delay before 1-based attempt @p attempt (0 is treated as 1).
   *
   * base = min(cap, initial_delay_ms << min(attempt - 1, 30)), cap being
   * max_delay_ms (initial_delay_ms when 0); result = base +/- jitter_pct,
   * not clamped back to cap, at least 1 and at most UINT32_MAX.
   *
   * @param[in] policy The policy.
   * @param[in] attempt Attempt number.
   * @param[in,out] rng_state xorshift64 state for jitter; must be non-NULL.
   * @return The delay in milliseconds; 0 if retrying is disabled or an argument is NULL.
   */
  uint32_t az_iot_retry_policy__delay_ms(
      const az_iot_retry_policy* policy,
      uint32_t attempt,
      uint64_t* rng_state);

  /**
   * @brief Count a failure on @p attempt and compute the delay before the retry.
   *
   * Does not check whether retrying is enabled; see az_iot_retry_policy_is_enabled().
   *
   * @param[in] policy The policy.
   * @param[in,out] attempt Retry counter; incremented, saturating.
   * @param[in,out] rng_state xorshift64 state for jitter.
   * @param[out] delay_ms Delay before the retry.
   * @return false if max_attempts is spent, so no retry is due.
   */
  bool az_iot_retry_policy__next(
      const az_iot_retry_policy* policy,
      uint32_t* attempt,
      uint64_t* rng_state,
      uint32_t* delay_ms);

  /**
   * @brief Count a failure and schedule the retry az_iot_retry_policy__next() allows.
   *
   * @return false if max_attempts is spent; nothing is then scheduled.
   */
  bool az_iot_retry_state__schedule(
      az_iot_retry_state* state,
      const az_iot_retry_policy* policy,
      uint64_t* rng_state);

  /** @brief Hold the scheduled retry until at least @p not_before_ms (az_iot_time_mono_ms()). */
  void az_iot_retry_state__defer(az_iot_retry_state* state, uint64_t not_before_ms);

  /** @brief Whether a retry may run now. Consumes the scheduled time, so it fires once. */
  bool az_iot_retry_state__due(az_iot_retry_state* state);

  /** @brief Whether a retry is scheduled and not yet due. Does not consume it. */
  bool az_iot_retry_state__pending(const az_iot_retry_state* state);

  /** @brief Failures counted since the last reset. */
  uint32_t az_iot_retry_state__attempts(const az_iot_retry_state* state);

  /** @brief Clear the count and any scheduled retry. */
  void az_iot_retry_state__reset(az_iot_retry_state* state);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_RETRY_POLICY_INTERNAL_H */
