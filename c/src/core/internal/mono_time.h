// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file mono_time.h
 * @brief Internal monotonic millisecond clock. */
#ifndef AZ_IOT_MONO_TIME_INTERNAL_H
#define AZ_IOT_MONO_TIME_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief Monotonic milliseconds since an unspecified start. */
  uint64_t az_iot_time_mono_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MONO_TIME_INTERNAL_H */
