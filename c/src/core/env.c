// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/env.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

az_iot_result az_iot_env_read(const char* name, char* buf, size_t cap)
{
  buf[0] = '\0';
#if defined(_WIN32)
  DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)cap);
  if (n >= cap)
  {
    buf[0] = '\0';
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
#else
  const char* value = getenv(name);
  if (value != NULL)
  {
    size_t len = strlen(value);
    if (len >= cap)
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    memcpy(buf, value, len + 1u);
  }
#endif
  return AZ_IOT_OK;
}
