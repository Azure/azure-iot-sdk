// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Shared fixture for the connection-client unit suites.
 *
 * Header-only (static inline) so each suite can shape its own options without a
 * separate library. Every helper drives the client through the PUBLIC API and
 * observes it through the in-memory mock adapter -- no white-box state pokes
 * beyond the documented internal test seams.
 *
 * Include AFTER <cmocka.h>: the helpers assert with cmocka.
 */
#ifndef AZ_IOT_CONNECTION_TEST_HARNESS_H
#define AZ_IOT_CONNECTION_TEST_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "internal/reconnect.h"

#include "support/mock_mqtt_iface.h"

/* Transitions observed through the state callback. */
#define AZ_IOT_TEST_MAX_STATES 32

typedef struct az_iot_test_state_log
{
  az_iot_connection_state states[AZ_IOT_TEST_MAX_STATES];
  az_iot_result reasons[AZ_IOT_TEST_MAX_STATES];
  size_t count;
} az_iot_test_state_log;

static inline void az_iot_test_on_state(
    az_iot_connection_state state,
    az_iot_result reason,
    void* user_ctx)
{
  az_iot_test_state_log* log = (az_iot_test_state_log*)user_ctx;
  if (log->count < AZ_IOT_TEST_MAX_STATES)
  {
    log->states[log->count] = state;
    log->reasons[log->count] = reason;
    log->count++;
  }
}

typedef struct az_iot_test_conn
{
  az_iot_connection_client client_storage;
  az_iot_connection_client* client;
  az_iot_mqtt_factory* factory;
  bool factory_registered;
  az_iot_test_state_log log;
} az_iot_test_conn;

/* True when `state` appears anywhere in the observed transition log. */
static inline bool az_iot_test_saw_state(
    const az_iot_test_state_log* log,
    az_iot_connection_state state)
{
  for (size_t i = 0; i < log->count; ++i)
  {
    if (log->states[i] == state)
    {
      return true;
    }
  }
  return false;
}

/* Number of times `state` was entered. */
static inline size_t az_iot_test_count_state(
    const az_iot_test_state_log* log,
    az_iot_connection_state state)
{
  size_t n = 0;
  for (size_t i = 0; i < log->count; ++i)
  {
    if (log->states[i] == state)
    {
      n++;
    }
  }
  return n;
}

/* Reason recorded with the first entry into `state`; AZ_IOT_OK if never seen. */
static inline az_iot_result az_iot_test_reason_for(
    const az_iot_test_state_log* log,
    az_iot_connection_state state)
{
  for (size_t i = 0; i < log->count; ++i)
  {
    if (log->states[i] == state)
    {
      return log->reasons[i];
    }
  }
  return AZ_IOT_OK;
}

static inline az_iot_connection_state az_iot_test_last_state(const az_iot_test_state_log* log)
{
  return log->count ? log->states[log->count - 1] : AZ_IOT_CONN_STATE_IDLE;
}

/* Options for a direct Classic hub connect with reconnection disabled. */
static inline az_iot_connection_client_options az_iot_test_classic_options(void)
{
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  return opts;
}

/* Spin until the monotonic clock advances by `ms`. The reconnect deadline is
 * compared against the same clock, so sleeping is not required (and would drag
 * a suite that runs under valgrind). */
static inline void az_iot_test_wait_ms(unsigned ms)
{
  uint64_t deadline = az_iot_time_mono_ms() + ms;
  while (az_iot_time_mono_ms() < deadline)
  { /* spin */
  }
}

#endif /* AZ_IOT_CONNECTION_TEST_HARNESS_H */
