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

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief How a failed operation is retried.
   *
   * The delay before attempt N is initial_delay_ms doubled N-1 times, capped at
   * max_delay_ms, then varied by +/-jitter_pct.
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
    /** Jitter, 0..100 percent, applied around each delay. */
    uint8_t jitter_pct;
  } az_iot_retry_policy;

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_RETRY_POLICY_H */
