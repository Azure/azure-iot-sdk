// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file e2e_defensive_timeout.h
 * @brief Seconds-scale mqttv5 defensive-timeout schedule for e2e clients.
 *
 * The spec's schedule (5/6/8/10 min + 0-5 min jitter) would let one missed
 * birth-ack outlast every connect window. Same shape in seconds: a miss
 * recovers in 5-15 s and escalation stays exercised.
 */
#ifndef E2E_DEFENSIVE_TIMEOUT_H
#define E2E_DEFENSIVE_TIMEOUT_H

#include <stdint.h>

#include "azure/iot/az_iot.h"
#include "internal/connection_client_internal.h"

/** @brief Applies the e2e schedule to @p client; call after init. */
static inline az_iot_result e2e_use_short_defensive_timeouts(az_iot_connection_client* client)
{
  static const uint32_t base_ms_by_attempt[] = { 5000u, 6000u, 8000u, 10000u };
  return az_iot_connection_client__set_defensive_timeout_schedule(
      client,
      base_ms_by_attempt,
      sizeof(base_ms_by_attempt) / sizeof(base_ms_by_attempt[0]),
      5000u);
}

#endif /* E2E_DEFENSIVE_TIMEOUT_H */
