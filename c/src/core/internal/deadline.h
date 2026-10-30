// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file deadline.h
 * @brief Operations on az_iot_deadline.
 *
 * A deadline is in one of three explicit states, so no time value doubles as a
 * sentinel:
 * - none: nothing to wait for; never passes;
 * - armed: passes once the monotonic clock reaches its time;
 * - expired: already passed, whatever the clock says (az_iot_deadline__expire_now()).
 *
 * Arithmetic saturates: a duration that would overflow arms the deadline at
 * UINT64_MAX, which never passes in practice.
 */
#ifndef AZ_IOT_DEADLINE_INTERNAL_H
#define AZ_IOT_DEADLINE_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "azure/iot/az_iot_deadline.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief Remove the deadline (state none). */
  void az_iot_deadline__clear(az_iot_deadline* deadline);

  /**
   * @brief Arm the deadline @p duration_ms after @p now_ms.
   *
   * @param[out] deadline The deadline.
   * @param[in] now_ms Current monotonic time.
   * @param[in] duration_ms Time from @p now_ms; 0 arms it at @p now_ms (passed at once).
   */
  void az_iot_deadline__arm_after_ms(
      az_iot_deadline* deadline,
      uint64_t now_ms,
      uint64_t duration_ms);

  /** @brief As az_iot_deadline__arm_after_ms(), with @p duration_seconds in seconds. */
  void az_iot_deadline__arm_after_seconds(
      az_iot_deadline* deadline,
      uint64_t now_ms,
      uint32_t duration_seconds);

  /** @brief Mark the deadline passed, regardless of the clock. */
  void az_iot_deadline__expire_now(az_iot_deadline* deadline);

  /** @brief Whether the deadline is armed or expired (not none). */
  bool az_iot_deadline__is_set(const az_iot_deadline* deadline);

  /**
   * @brief Whether the deadline has passed at @p now_ms.
   *
   * @return true when expired, or armed and @p now_ms is at or past its time; false when none.
   */
  bool az_iot_deadline__has_passed(const az_iot_deadline* deadline, uint64_t now_ms);

  /**
   * @brief Milliseconds left at @p now_ms.
   *
   * @return UINT64_MAX when none; 0 when passed; otherwise the time left.
   */
  uint64_t az_iot_deadline__remaining_ms(const az_iot_deadline* deadline, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DEADLINE_INTERNAL_H */
