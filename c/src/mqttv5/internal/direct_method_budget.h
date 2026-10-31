// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file direct_method_budget.h
 * @brief MQTTv5 direct method budgets: the time the service counts down, in whole seconds, and the
 *        result deadline derived from it.
 */
#ifndef AZ_IOT_MQTTV5_DIRECT_METHOD_BUDGET_H
#define AZ_IOT_MQTTV5_DIRECT_METHOD_BUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/mqttv5/az_iot_direct_method_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Budgets travel in whole seconds; the monotonic clock counts milliseconds. */
#define AZ_IOT_MQTTV5_DM_MS_PER_SECOND 1000u

/**
 * @brief The service starts a timer and waits: a probe-ack or abandon with less than this many
 *        seconds left has missed that window. A result is held to its own deadline instead.
 */
#define AZ_IOT_MQTTV5_DM_MIN_USEFUL_BUDGET_SECONDS 1u

  /**
   * @brief Safety margin for the network transit a broker-adjusted message expiry cannot measure:
   *        a tenth of @p response_timeout_seconds, between 1 and 10 seconds.
   */
  uint32_t az_iot_mqttv5_direct_method_client__safety_margin_seconds(
      uint32_t response_timeout_seconds);

  /**
   * @brief Seconds left of a budget of @p budget_seconds started at @p start_ms.
   * @return 0 when @p budget_seconds is 0 (no expiry, not an exhausted one) or when it is spent.
   */
  uint32_t az_iot_mqttv5_direct_method_client__remaining_budget_seconds(
      uint32_t budget_seconds,
      uint64_t start_ms);

  /**
   * @brief Monotonic milliseconds at and after which no result may be published.
   *
   * @param received_at_ms When the exec arrived.
   * @param exec_budget_seconds The exec's message expiry; 0 means it carried none.
   * @param margin_seconds Safety margin for network transit.
   * @param max_unconfirmed_delivery_timeout_ms Bound used when the exec carried no expiry.
   * @return @p received_at_ms + @p max_unconfirmed_delivery_timeout_ms when
   *         @p exec_budget_seconds is 0; @p received_at_ms when the budget does not
   *         exceed the margin.
   */
  uint64_t az_iot_mqttv5_direct_method_client__result_deadline_ms(
      uint64_t received_at_ms,
      uint32_t exec_budget_seconds,
      uint32_t margin_seconds,
      uint32_t max_unconfirmed_delivery_timeout_ms);

  /** @brief Whether the result deadline of req_pool[@p index] has been reached. */
  bool az_iot_mqttv5_direct_method_client__result_deadline_has_passed(
      const az_iot_mqttv5_direct_method_client* client,
      size_t index);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV5_DIRECT_METHOD_BUDGET_H */
