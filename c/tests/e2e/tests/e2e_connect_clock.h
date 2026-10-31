// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file e2e_connect_clock.h
 * @brief Separate time budgets for DPS provisioning and the hub connect.
 *
 * DPS assignment on the shared e2e instances intermittently takes from tens of
 * seconds to over 90 s. Charged to the hub's connect budget, that ran it out
 * before the hub connect started.
 */
#ifndef E2E_CONNECT_CLOCK_H
#define E2E_CONNECT_CLOCK_H

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "azure/iot/az_iot.h"

/** @brief Seconds a connect may spend provisioning through DPS. */
#define E2E_DPS_PROVISIONING_TIMEOUT_S 180

/** @brief Time a connect spent provisioning through DPS and on the hub. */
typedef struct e2e_connect_clock
{
  time_t last;
  long dps_s;
  long hub_s;
} e2e_connect_clock;

/** @brief Starts @p clock with nothing charged; call right after open(). */
static inline void e2e_connect_clock_start(e2e_connect_clock* clock)
{
  clock->last = time(NULL);
  clock->dps_s = 0;
  clock->hub_s = 0;
}

/**
 * @brief Charges the time since the last call to DPS while @p client is provisioning,
 * otherwise to the hub.
 *
 * @return true while neither #E2E_DPS_PROVISIONING_TIMEOUT_S nor @p hub_budget_s is spent.
 */
static inline bool e2e_connect_clock_running(
    e2e_connect_clock* clock,
    const az_iot_connection_client* client,
    long hub_budget_s)
{
  time_t now = time(NULL);
  long elapsed = (long)(now - clock->last);
  clock->last = now;
  az_iot_connection_state dps = az_iot_connection_client_get_state(client, AZ_IOT_CONN_SCOPE_DPS);
  if (dps != AZ_IOT_CONN_STATE_IDLE && dps != AZ_IOT_CONN_STATE_FAULTED)
  {
    clock->dps_s += elapsed;
  }
  else
  {
    clock->hub_s += elapsed;
  }
  return clock->dps_s < E2E_DPS_PROVISIONING_TIMEOUT_S && clock->hub_s < hub_budget_s;
}

/** @brief Logs the time @p who spent provisioning through DPS and on the hub. */
static inline void e2e_connect_clock_log(const e2e_connect_clock* clock, const char* who)
{
  (void)fprintf(
      stderr,
      "[e2e] %s connect: %lds provisioning through DPS (budget %ds), %lds on the hub\n",
      who,
      clock->dps_s,
      E2E_DPS_PROVISIONING_TIMEOUT_S,
      clock->hub_s);
}

#endif /* E2E_CONNECT_CLOCK_H */
