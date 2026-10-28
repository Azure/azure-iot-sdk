// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/deadline.h"

#define DEADLINE_STATE_NONE 0u
#define DEADLINE_STATE_ARMED 1u
#define DEADLINE_STATE_EXPIRED 2u

void az_iot_deadline__clear(az_iot_deadline* deadline)
{
  deadline->_internal.at_ms = 0u;
  deadline->_internal.state = DEADLINE_STATE_NONE;
}

void az_iot_deadline__arm_after_ms(
    az_iot_deadline* deadline,
    uint64_t now_ms,
    uint64_t duration_ms)
{
  deadline->_internal.at_ms
      = (duration_ms > UINT64_MAX - now_ms) ? UINT64_MAX : now_ms + duration_ms;
  deadline->_internal.state = DEADLINE_STATE_ARMED;
}

void az_iot_deadline__arm_after_seconds(
    az_iot_deadline* deadline,
    uint64_t now_ms,
    uint32_t duration_seconds)
{
  az_iot_deadline__arm_after_ms(deadline, now_ms, (uint64_t)duration_seconds * 1000u);
}

void az_iot_deadline__expire_now(az_iot_deadline* deadline)
{
  deadline->_internal.at_ms = 0u;
  deadline->_internal.state = DEADLINE_STATE_EXPIRED;
}

bool az_iot_deadline__is_set(const az_iot_deadline* deadline)
{
  return deadline->_internal.state != DEADLINE_STATE_NONE;
}

bool az_iot_deadline__has_passed(const az_iot_deadline* deadline, uint64_t now_ms)
{
  return deadline->_internal.state == DEADLINE_STATE_EXPIRED
      || (deadline->_internal.state == DEADLINE_STATE_ARMED
          && now_ms >= deadline->_internal.at_ms);
}

uint64_t az_iot_deadline__remaining_ms(const az_iot_deadline* deadline, uint64_t now_ms)
{
  if (deadline->_internal.state == DEADLINE_STATE_NONE)
  {
    return UINT64_MAX;
  }
  if (az_iot_deadline__has_passed(deadline, now_ms))
  {
    return 0u;
  }
  return deadline->_internal.at_ms - now_ms;
}
