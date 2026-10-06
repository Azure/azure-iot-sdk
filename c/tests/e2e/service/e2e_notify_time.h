// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef E2E_NOTIFY_TIME_H
#define E2E_NOTIFY_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Reads the `enqueuedTimeUtc` field of a file-upload notification body.
   *
   * Accepts `"YYYY-MM-DDTHH:MM:SS[.fraction](Z|+00:00)"` with valid calendar and
   * time ranges. @p body must be NUL-terminated.
   *
   * @return true with seconds since the Unix epoch in @p out_epoch_s; false if the
   * field is missing or malformed.
   */
  bool e2e_notify_enqueued_time(const char* body, int64_t* out_epoch_s);

#ifdef __cplusplus
}
#endif

#endif /* E2E_NOTIFY_TIME_H */
