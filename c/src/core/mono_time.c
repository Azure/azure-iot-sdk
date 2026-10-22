// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/mono_time.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
uint64_t az_iot_time_mono_ms(void) { return (uint64_t)GetTickCount64(); }
#else
#include <time.h>
uint64_t az_iot_time_mono_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}
#endif

#include <time.h>

uint64_t az_iot_time_unix_s(uint64_t (*get_time)(void* ctx), void* ctx)
{
  if (get_time != NULL)
  {
    return get_time(ctx);
  }
  time_t t = time(NULL);
  /* (time_t)-1 is failure. Compared explicitly: time_t may be unsigned, where
   * it would pass a `> 0` test as a far-future time. */
  if (t == (time_t)-1)
  {
    return 0u;
  }
  return t > 0 ? (uint64_t)t : 0u;
}
