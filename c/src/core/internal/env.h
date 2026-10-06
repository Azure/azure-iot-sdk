// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file env.h
 * @brief Process environment access.
 */
#ifndef AZ_IOT_ENV_INTERNAL_H
#define AZ_IOT_ENV_INTERNAL_H

#include <stddef.h>

#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Copies environment variable @p name into @p buf.
   *
   * Reads the process environment, not the C runtime's copy: with a static CRT,
   * each Windows DLL has its own copy, which misses values the application sets.
   *
   * @param[in] name Variable name.
   * @param[out] buf Receives the value, "" when unset or on error.
   * @param[in] cap Size of @p buf, > 0.
   * @return AZ_IOT_OK, or AZ_IOT_ERR_NOT_ENOUGH_SPACE when the value does not fit.
   */
  az_iot_result az_iot_env_read(const char* name, char* buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ENV_INTERNAL_H */
