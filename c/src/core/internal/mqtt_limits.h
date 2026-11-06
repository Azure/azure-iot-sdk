// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file mqtt_limits.h
 * @brief The az_iot_mqtt_limits every MQTT adapter accepts, so one set works with any of them.
 */
#ifndef AZ_IOT_MQTT_LIMITS_INTERNAL_H
#define AZ_IOT_MQTT_LIMITS_INTERNAL_H

#include <stdbool.h>

#include "azure/iot/az_iot_mqtt_iface.h"

/** @brief Bytes a message store needs per message beyond the packet (az_mqtt's overhead; that
 * adapter checks it is the same at compile time). */
#define AZ_IOT_MQTT__MESSAGE_STORE_ENTRY_OVERHEAD 18u

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Whether @p limits is valid for every adapter: buffers 1 to INT32_MAX bytes (receive at
   * most INT32_MAX - 3), 1 to 32767 in-flight messages, a message store of 0 or at least one
   * send buffer plus #AZ_IOT_MQTT__MESSAGE_STORE_ENTRY_OVERHEAD, connect strings at most
   * INT32_MAX.
   *
   * @param[in] limits Limits; NULL is invalid.
   */
  bool az_iot_mqtt__limits_are_valid(const az_iot_mqtt_limits* limits);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTT_LIMITS_INTERNAL_H */
