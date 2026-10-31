// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file mono_time.h
 * @brief Internal clocks: monotonic milliseconds and Unix seconds. */
#ifndef AZ_IOT_MONO_TIME_INTERNAL_H
#define AZ_IOT_MONO_TIME_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief Monotonic milliseconds since an unspecified start. */
  uint64_t az_iot_time_mono_ms(void);

  /**
   * @brief Current Unix time in seconds.
   *
   * @param get_time Time source; NULL uses the C library's time().
   * @param ctx Passed to @p get_time.
   * @return Seconds since the Unix epoch; 0 when unknown.
   */
  uint64_t az_iot_time_unix_s(uint64_t (*get_time)(void* ctx), void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MONO_TIME_INTERNAL_H */
