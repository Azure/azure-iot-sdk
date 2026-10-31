// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv5 direct method budgets and the result deadline. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal/direct_method_budget.h"
#include "internal/mono_time.h"

/* A tenth of the response timeout, held within these bounds so a very short timeout still
 * reserves something and a very long one does not reserve minutes. */
#define DM_SAFETY_MARGIN_DIVISOR 10u
#define DM_SAFETY_MARGIN_MIN_SECONDS 1u
#define DM_SAFETY_MARGIN_MAX_SECONDS 10u

uint32_t az_iot_mqttv5_direct_method_client__safety_margin_seconds(
    uint32_t response_timeout_seconds)
{
  uint32_t margin = response_timeout_seconds / DM_SAFETY_MARGIN_DIVISOR;
  if (margin < DM_SAFETY_MARGIN_MIN_SECONDS)
  {
    margin = DM_SAFETY_MARGIN_MIN_SECONDS;
  }
  if (margin > DM_SAFETY_MARGIN_MAX_SECONDS)
  {
    margin = DM_SAFETY_MARGIN_MAX_SECONDS;
  }
  return margin;
}

uint32_t az_iot_mqttv5_direct_method_client__remaining_budget_seconds(
    uint32_t budget_seconds,
    uint64_t start_ms)
{
  if (budget_seconds == 0u)
  {
    return 0u;
  }
  uint64_t now = az_iot_time_mono_ms();
  uint64_t elapsed_ms = (now > start_ms) ? (now - start_ms) : 0u;
  uint64_t elapsed = elapsed_ms / AZ_IOT_MQTTV5_DM_MS_PER_SECOND;
  return (elapsed >= budget_seconds) ? 0u : (budget_seconds - (uint32_t)elapsed);
}

uint64_t az_iot_mqttv5_direct_method_client__result_deadline_ms(
    uint64_t received_at_ms,
    uint32_t exec_budget_seconds,
    uint32_t margin_seconds,
    uint32_t max_unconfirmed_delivery_timeout_ms)
{
  if (exec_budget_seconds == 0u)
  {
    return received_at_ms + max_unconfirmed_delivery_timeout_ms;
  }
  if (exec_budget_seconds <= margin_seconds)
  {
    return received_at_ms;
  }
  return received_at_ms
      + (uint64_t)(exec_budget_seconds - margin_seconds) * AZ_IOT_MQTTV5_DM_MS_PER_SECOND;
}

bool az_iot_mqttv5_direct_method_client__result_deadline_has_passed(
    const az_iot_mqttv5_direct_method_client* client,
    size_t index)
{
  return az_iot_time_mono_ms() >= client->_internal.exec_deadline_ms[index];
}
