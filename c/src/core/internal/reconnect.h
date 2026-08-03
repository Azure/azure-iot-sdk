// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Internal monotonic clock + reconnect-policy helper.
 *
 * Both are implementation-private; not part of the public ABI. ConnectionClient
 * uses these to drive the RECONNECTING state. The reconnect helper is also
 * unit-testable in isolation (deterministic for a given seed/attempt).
 */
#ifndef AZ_IOT_RECONNECT_INTERNAL_H
#define AZ_IOT_RECONNECT_INTERNAL_H

#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Monotonic millisecond clock. Wraps after 2^64 ms (~5.8e8 years) — fine. */
  uint64_t az_iot_time_mono_ms(void);

  /* Compute the delay before the (1-based) attempt N when reconnecting under the
   * given policy. The function is pure (no I/O, no global state apart from the
   * caller-provided rng_state for jitter). Returns 0 ms when reconnect is
   * disabled (initial_delay_ms == 0).
   *
   * Algorithm:
   *   base  = min(max_delay_ms, initial_delay_ms << min(attempt - 1, 30))
   *   jitter ~ uniform(-jitter_pct%, +jitter_pct%) of base
   *   result = clamp(base + jitter, 1, max_delay_ms)  (if reconnect enabled)
   *
   * The shift cap at 30 prevents UB on 32-bit overflow. attempt == 0 is treated
   * as attempt == 1.
   *
   * `rng_state` must be non-NULL and is updated in-place (xorshift64). Tests
   * seed it deterministically; production seeds it from time_mono.
   */
  uint32_t az_iot_reconnect_delay_ms(
      const az_iot_reconnection_policy* policy,
      uint32_t attempt,
      uint64_t* rng_state);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_RECONNECT_INTERNAL_H */
