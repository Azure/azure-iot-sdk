// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file az_iot_deadline.h
 * @brief Monotonic deadline embedded in SDK client structs.
 */
#ifndef AZ_IOT_DEADLINE_H
#define AZ_IOT_DEADLINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief A monotonic time by which something must happen, kept by the SDK client that waits.
   *
   * Zero-initialized means no deadline. Fields are internal and not for application use.
   */
  typedef struct az_iot_deadline
  {
    struct
    {
      uint64_t at_ms; /**< Monotonic ms; meaningful only when state is armed. */
      uint8_t state; /**< None (0), armed or expired. */
    } _internal;
  } az_iot_deadline;

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DEADLINE_H */
