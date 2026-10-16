// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_retry_policy.h
 * @brief Retry schedule: exponential backoff with jitter and an attempt bound.
 */
#ifndef AZ_IOT_RETRY_POLICY_H
#define AZ_IOT_RETRY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief How jitter_pct varies a retry delay. */
  typedef enum az_iot_retry_jitter
  {
    /** delay +/- jitter_pct (default). */
    AZ_IOT_RETRY_JITTER_SYMMETRIC = 0,
    /** delay + [0, jitter_pct]: never shorter than the delay. */
    AZ_IOT_RETRY_JITTER_ADDITIVE = 1,
  } az_iot_retry_jitter;

  /**
   * @brief How a failed operation is retried.
   *
   * The delay before attempt N is initial_delay_ms doubled N-1 times, capped at
   * max_delay_ms, then varied by jitter_pct as jitter_mode says.
   */
  typedef struct az_iot_retry_policy
  {
    /**
     * Delay before the first retry, in milliseconds.
     *
     * 0 DISABLES retrying: every failure is terminal, so a zero-initialized
     * policy retries nothing. There is no "retry immediately, forever".
     */
    uint32_t initial_delay_ms;
    /**
     * Cap on the doubling, in milliseconds; jitter may exceed it by up to
     * jitter_pct. 0, or equal to initial_delay_ms, gives a fixed interval.
     */
    uint32_t max_delay_ms;
    /** Retries before giving up; 0 = no limit. */
    uint32_t max_attempts;
    /** Jitter, 0..100 percent, applied to each delay as jitter_mode says. */
    uint8_t jitter_pct;
    /** How jitter_pct is applied. Zero-initialized: AZ_IOT_RETRY_JITTER_SYMMETRIC. */
    az_iot_retry_jitter jitter_mode;
  } az_iot_retry_policy;

  /**
   * @brief Progress along a retry schedule, kept by the SDK client that retries.
   *
   * Fields are internal and not for application use.
   */
  typedef struct az_iot_retry_state
  {
    struct
    {
      uint64_t due_ms; /**< Monotonic ms the retry may run at; 0 = none scheduled. */
      uint32_t attempt; /**< Failures counted since the last reset. */
      uint64_t due_unix_s; /**< due_ms in Unix seconds; 0 = unknown. */
      bool due_unix_aligned; /**< due_unix_s was set from the same instant as due_ms. */
    } _internal;
  } az_iot_retry_state;

  /**
   * @brief Whether @p policy retries at all.
   *
   * @param[in] policy The policy.
   * @return false if @p policy is NULL or its initial_delay_ms is 0.
   */
  bool az_iot_retry_policy_is_enabled(const az_iot_retry_policy* policy);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_RETRY_POLICY_H */
