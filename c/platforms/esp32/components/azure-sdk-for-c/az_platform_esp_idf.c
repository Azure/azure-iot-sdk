// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/**
 * @file az_platform_esp_idf.c
 * @brief az_core's az_platform_* hooks on ESP-IDF: esp_timer clock, FreeRTOS sleep.
 */

#include <azure/core/az_platform.h>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

AZ_NODISCARD az_result az_platform_clock_msec(int64_t* out_clock_msec)
{
  if (out_clock_msec == NULL)
  {
    return AZ_ERROR_ARG;
  }
  *out_clock_msec = esp_timer_get_time() / 1000;
  return AZ_OK;
}

/** @brief Blocks the calling task, at least one tick for any positive @p milliseconds. */
AZ_NODISCARD az_result az_platform_sleep_msec(int32_t milliseconds)
{
  if (milliseconds > 0)
  {
    // Widened: pdMS_TO_TICKS multiplies in TickType_t, which overflows for long sleeps.
    TickType_t const ticks
        = (TickType_t)((uint64_t)(uint32_t)milliseconds * configTICK_RATE_HZ / 1000u);
    vTaskDelay(ticks > 0 ? ticks : 1);
  }
  return AZ_OK;
}
