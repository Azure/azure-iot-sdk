// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Retry schedule of a FAILED workflow (see su_retry_internal.h). */
#include <stdbool.h>
#include <stdint.h>

#include "internal/mono_time.h"
#include "internal/su_retry_internal.h"

/** @brief Wait, in seconds, before retrying a workflow after its first failure. */
#define SU_RETRY_BASE_S 3600u
/** @brief Upper bound, in seconds, of the doubling wait (before jitter). */
#define SU_RETRY_MAX_S 86400u
/** @brief Jitter added to a wait is at most the wait divided by this. */
#define SU_RETRY_JITTER_DIV 10u
/** @brief Longest wait, in seconds: the cap plus its largest jitter. */
#define SU_RETRY_LIMIT_S (SU_RETRY_MAX_S + SU_RETRY_MAX_S / SU_RETRY_JITTER_DIV)
/** @brief Unix times before 2024-01-01T00:00:00Z are unknown (an unsynced clock). */
#define SU_UNIX_TIME_FLOOR 1704067200u

static void wr_u32le(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t rd_u32le(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t az_iot_su__unix_now(az_iot_unix_time_callback get_time, void* ctx)
{
  uint64_t now = az_iot_time_unix_s(get_time, ctx);
  return now >= SU_UNIX_TIME_FLOOR ? now : 0u;
}

void az_iot_su__workflow_retry_reset(az_iot_su_workflow_retry* retry)
{
  retry->failures = 0;
  retry->due_ms = 0;
  retry->due_unix_s = 0;
}

uint32_t az_iot_su__workflow_retry_remaining_ms(const az_iot_su_workflow_retry* retry)
{
  uint64_t now = az_iot_time_mono_ms();
  return retry->due_ms > now ? (uint32_t)(retry->due_ms - now) : 0u;
}

/**
 * @brief Hold the next attempt until @p wait_ms from now.
 *
 * The Unix deadline adds a second: the clock's fraction is unknown, so a
 * restart at the deadline must not cut the wait short.
 */
static void set_wait(
    az_iot_su_workflow_retry* retry,
    uint64_t wait_ms,
    az_iot_unix_time_callback get_time,
    void* ctx)
{
  uint64_t unix_now = az_iot_su__unix_now(get_time, ctx);
  uint64_t wait_s = (wait_ms + 999u) / 1000u + 1u;
  retry->due_ms = az_iot_time_mono_ms() + wait_ms;
  retry->due_unix_s = (unix_now == 0u || unix_now > UINT64_MAX - wait_s) ? 0u : unix_now + wait_s;
}

uint64_t az_iot_su__workflow_retry_schedule(
    az_iot_su_workflow_retry* retry,
    uint64_t* rng,
    uint64_t seed,
    az_iot_unix_time_callback get_time,
    void* ctx)
{
  if (retry->failures < UINT32_MAX)
  {
    retry->failures++;
  }
  uint64_t base_s = SU_RETRY_BASE_S;
  for (uint32_t i = 1u; i < retry->failures && base_s < SU_RETRY_MAX_S; ++i)
  {
    base_s *= 2u;
  }
  if (base_s > SU_RETRY_MAX_S)
  {
    base_s = SU_RETRY_MAX_S;
  }
  uint64_t base_ms = base_s * 1000u;

  if (*rng == 0)
  {
    /* Odd, as xorshift64 sticks at zero. */
    *rng = (az_iot_time_mono_ms() ^ seed) | 1u;
  }
  uint64_t x = *rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *rng = x;
  uint64_t wait_ms = base_ms + x % (base_ms / SU_RETRY_JITTER_DIV + 1u);
  set_wait(retry, wait_ms, get_time, ctx);
  return wait_ms;
}

void az_iot_su__workflow_retry_write(const az_iot_su_workflow_retry* retry, uint8_t* out)
{
  wr_u32le(out, retry->failures);
  wr_u32le(out + 4u, (az_iot_su__workflow_retry_remaining_ms(retry) + 999u) / 1000u);
  wr_u32le(out + 8u, (uint32_t)(retry->due_unix_s & 0xFFFFFFFFu));
  wr_u32le(out + 12u, (uint32_t)(retry->due_unix_s >> 32));
}

void az_iot_su__workflow_retry_read(
    az_iot_su_workflow_retry* retry,
    const uint8_t* in,
    az_iot_unix_time_callback get_time,
    void* ctx)
{
  uint64_t remaining_s = rd_u32le(in + 4u);
  uint64_t due_unix = (uint64_t)rd_u32le(in + 8u) | ((uint64_t)rd_u32le(in + 12u) << 32);
  uint64_t now_unix = az_iot_su__unix_now(get_time, ctx);
  if (due_unix < SU_UNIX_TIME_FLOOR)
  {
    due_unix = 0;
  }
  bool keep_deadline = due_unix != 0;
  if (due_unix != 0 && now_unix != 0)
  {
    remaining_s = due_unix > now_unix ? due_unix - now_unix : 0u;
  }
  if (remaining_s > SU_RETRY_LIMIT_S)
  {
    remaining_s = SU_RETRY_LIMIT_S;
    keep_deadline = keep_deadline && now_unix == 0;
  }
  retry->failures = rd_u32le(in);
  set_wait(retry, remaining_s * 1000u, get_time, ctx);
  /* Kept as stored, so restarts do not move it. */
  if (keep_deadline)
  {
    retry->due_unix_s = due_unix;
  }
}
