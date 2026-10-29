// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file twin_client_internal.h
 * @brief Internal test seams of the mqttv5 twin client. Not part of the public API.
 */
#ifndef AZ_IOT_MQTTV5_TWIN_CLIENT_INTERNAL_H
#define AZ_IOT_MQTTV5_TWIN_CLIENT_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "azure/iot/mqttv5/az_iot_twin_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Make an armed twin-push wait expire on the next
   *        az_iot_mqttv5_twin_client_do_work(). No-op when not armed.
   */
  void az_iot_mqttv5_twin_client__twin_push_force_timeout(az_iot_mqttv5_twin_client* twin);

  /**
   * @brief Whether the twin-push wait is set (armed or expired).
   *
   * @param[out] out_remaining_ms Receives the time left (0 once passed); may be NULL.
   * @return true when set.
   */
  bool az_iot_mqttv5_twin_client__twin_push_remaining(
      const az_iot_mqttv5_twin_client* twin,
      uint64_t* out_remaining_ms);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV5_TWIN_CLIENT_INTERNAL_H */
