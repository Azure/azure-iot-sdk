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
#include "support/subscription_ack.h"

/* Transitions observed through the state callback. */
#define AZ_IOT_TEST_MAX_STATES 32

typedef struct az_iot_test_state_log
{
  az_iot_connection_state states[AZ_IOT_TEST_MAX_STATES];
  /* The scope each event carried. `states` is meaningless without it. */
  az_iot_connection_scope scopes[AZ_IOT_TEST_MAX_STATES];
  az_iot_result reasons[AZ_IOT_TEST_MAX_STATES];
  uint32_t event_sizes[AZ_IOT_TEST_MAX_STATES];
  bool profile_present[AZ_IOT_TEST_MAX_STATES];
  uint32_t profile_sizes[AZ_IOT_TEST_MAX_STATES];
  az_iot_connection_profile profiles[AZ_IOT_TEST_MAX_STATES];
  char profile_raw[AZ_IOT_TEST_MAX_STATES][AZ_IOT_CONNECTION_PROFILE_RAW_BUF];
  /* Diagnostic detail (item 8). `error_message` is COPIED because the event's
   * span points into the adapter's inbound buffer and dies with the callback --
   * a test that kept the span would be reading freed memory by the time it
   * asserted. */
  bool is_retriable[AZ_IOT_TEST_MAX_STATES];
  bool error_present[AZ_IOT_TEST_MAX_STATES];
  uint32_t error_sizes[AZ_IOT_TEST_MAX_STATES];
  az_iot_connection_error_source error_sources[AZ_IOT_TEST_MAX_STATES];
  int32_t error_codes[AZ_IOT_TEST_MAX_STATES];
  char error_message[AZ_IOT_TEST_MAX_STATES][128];
  size_t count;
} az_iot_test_state_log;

static inline void az_iot_test_on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_test_state_log* log = (az_iot_test_state_log*)user_ctx;
  if (log->count < AZ_IOT_TEST_MAX_STATES)
  {
    size_t index = log->count;
    log->states[index] = event->state;
    log->scopes[index] = event->scope;
    log->reasons[index] = event->reason;
    log->event_sizes[index] = event->_internal_size;
    log->profile_present[index] = event->profile != NULL;
    log->is_retriable[index] = event->is_retriable;
    log->error_present[index] = event->error != NULL;
    if (event->error)
    {
      log->error_sizes[index] = event->error->_internal_size;
      log->error_sources[index] = event->error->source;
      log->error_codes[index] = event->error->code;
      int32_t msg_len = az_span_size(event->error->message);
      if (msg_len > 0)
      {
        size_t n = (size_t)msg_len;
        if (n >= sizeof(log->error_message[index]))
        {
          n = sizeof(log->error_message[index]) - 1u;
        }
        memcpy(log->error_message[index], az_span_ptr(event->error->message), n);
        log->error_message[index][n] = '\0';
      }
    }
    if (event->profile)
    {
      log->profile_sizes[index] = event->profile->_internal_size;
      log->profiles[index] = event->profile->connection_profile;
      if (event->profile->connection_profile_raw)
      {
        size_t raw_len = strlen(event->profile->connection_profile_raw);
        if (raw_len >= sizeof(log->profile_raw[index]))
        {
          raw_len = sizeof(log->profile_raw[index]) - 1u;
        }
        memcpy(log->profile_raw[index], event->profile->connection_profile_raw, raw_len);
        log->profile_raw[index][raw_len] = '\0';
      }
    }
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

/* Scope-aware queries. The unscoped helpers above answer "what happened", which
 * is still useful; these answer "what happened to THIS lifecycle", which is the
 * only question with a well-defined answer once the two move independently. */
static inline az_iot_connection_state az_iot_test_last_state_for(
    const az_iot_test_state_log* log,
    az_iot_connection_scope scope)
{
  for (size_t i = log->count; i > 0; --i)
  {
    if (log->scopes[i - 1] == scope)
    {
      return log->states[i - 1];
    }
  }
  return AZ_IOT_CONN_STATE_IDLE;
}

static inline size_t az_iot_test_count_for(
    const az_iot_test_state_log* log,
    az_iot_connection_scope scope,
    az_iot_connection_state state)
{
  size_t n = 0;
  for (size_t i = 0; i < log->count; ++i)
  {
    if (log->scopes[i] == scope && log->states[i] == state)
    {
      ++n;
    }
  }
  return n;
}

/* Index of the first event matching (scope, state), or SIZE_MAX. Lets a test
 * assert ORDER between the two lifecycles. */
static inline size_t az_iot_test_index_of(
    const az_iot_test_state_log* log,
    az_iot_connection_scope scope,
    az_iot_connection_state state)
{
  for (size_t i = 0; i < log->count; ++i)
  {
    if (log->scopes[i] == scope && log->states[i] == state)
    {
      return i;
    }
  }
  return SIZE_MAX;
}

/* Options for a direct MQTTv3 hub connect with reconnection disabled. */
static inline az_iot_connection_client_options az_iot_test_mqtt_v3_options(void)
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

/* Spin until `deadline_ms` on the monotonic clock has passed, plus a small
 * margin.
 *
 * Takes the absolute deadline rather than a computed duration on purpose. The
 * obvious form -- az_iot_test_wait_ms(deadline - now + 5) -- underflows when
 * the deadline has ALREADY passed, which happens whenever the test process is
 * descheduled for longer than the backoff it is waiting on. Both operands are
 * uint64_t, so the difference wraps to an enormous value and the wait becomes
 * a multi-day busy spin: a CI hang rather than a test failure. */
static inline void az_iot_test_wait_until_ms(uint64_t deadline_ms)
{
  uint64_t now = az_iot_time_mono_ms();
  az_iot_test_wait_ms((now >= deadline_ms) ? 5u : (unsigned)(deadline_ms - now) + 5u);
}

#endif /* AZ_IOT_CONNECTION_TEST_HARNESS_H */
