// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file su_retry_internal.h
 * @brief Retry schedule of a FAILED workflow: waits, clock and
 * persisted form. NOT part of the public API. */
#ifndef AZ_IOT_SU_RETRY_INTERNAL_H
#define AZ_IOT_SU_RETRY_INTERNAL_H

#include <stdint.h>

#include "azure/iot/az_iot_su.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Bytes of the persisted retry schedule. */
#define AZ_IOT_SU_RETRY_RECORD_SIZE 16u

  /**
   * @brief Current Unix time in seconds.
   *
   * @param get_time Time source; NULL uses time().
   * @param ctx Passed to @p get_time.
   * @return Seconds; 0 when unknown (no time, or before 2024-01-01, an unsynced clock).
   */
  uint64_t az_iot_su__unix_now(az_iot_unix_time_callback get_time, void* ctx);

  /** @brief Forget the schedule: the workflow succeeded, or another one was taken. */
  void az_iot_su__workflow_retry_reset(az_iot_su_workflow_retry* retry);

  /** @brief Milliseconds before the next attempt may start; 0 when it may start now. */
  uint32_t az_iot_su__workflow_retry_remaining_ms(const az_iot_su_workflow_retry* retry);

  /**
   * @brief Count a FAILED attempt and schedule the next one.
   *
   * The n-th consecutive failure waits min(1 h * 2^(n-1), 24 h) plus a uniform
   * jitter of up to a tenth of that; jitter only lengthens the wait.
   *
   * @param retry Schedule to update.
   * @param rng Jitter PRNG state; seeded when 0.
   * @param seed Mixed into a zero @p rng.
   * @param get_time Unix time source; NULL uses time().
   * @param ctx Passed to @p get_time.
   * @return The wait, in milliseconds.
   */
  uint64_t az_iot_su__workflow_retry_schedule(
      az_iot_su_workflow_retry* retry,
      uint64_t* rng,
      uint64_t seed,
      az_iot_unix_time_callback get_time,
      void* ctx);

  /**
   * @brief Write the schedule as AZ_IOT_SU_RETRY_RECORD_SIZE bytes: failures, seconds
   * left, Unix deadline (low, high); all u32 little-endian.
   */
  void az_iot_su__workflow_retry_write(const az_iot_su_workflow_retry* retry, uint8_t* out);

  /**
   * @brief Restore the schedule written by az_iot_su__workflow_retry_write().
   *
   * The wait left comes from the stored Unix deadline when it and the current
   * Unix time are both known, else from the seconds left when written; at most
   * 24 h plus jitter. A usable stored deadline is kept as stored.
   */
  void az_iot_su__workflow_retry_read(
      az_iot_su_workflow_retry* retry,
      const uint8_t* in,
      az_iot_unix_time_callback get_time,
      void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SU_RETRY_INTERNAL_H */
