// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop stand-in for ESP-IDF's esp_ota_ops.h. Behaviour: fake_esp_ota.h. */
#ifndef FAKE_ESP_OTA_OPS_H
#define FAKE_ESP_OTA_OPS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503
#define OTA_SIZE_UNKNOWN 0xffffffff

typedef uint32_t esp_ota_handle_t;

typedef enum
{
  ESP_OTA_IMG_NEW = 0x0U,
  ESP_OTA_IMG_PENDING_VERIFY = 0x1U,
  ESP_OTA_IMG_VALID = 0x2U,
  ESP_OTA_IMG_INVALID = 0x3U,
  ESP_OTA_IMG_ABORTED = 0x4U
} esp_ota_img_states_t;

/** @brief The update partition, or NULL if fake_esp_ota_state::no_update_partition. */
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* start_from);
/** @brief The running partition. */
const esp_partition_t* esp_ota_get_running_partition(void);
/** @brief Erase the update partition and open a write session. */
esp_err_t esp_ota_begin(
    const esp_partition_t* partition,
    size_t image_size,
    esp_ota_handle_t* out_handle);
/** @brief Append to the session; the first byte must be 0xE9. */
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* data, size_t size);
/** @brief Close the session; fails if nothing was written. */
esp_err_t esp_ota_end(esp_ota_handle_t handle);
/** @brief Close the session without validating it. */
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
/** @brief Record the next boot partition. */
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition);
/** @brief Always ESP_OTA_IMG_VALID. */
esp_err_t esp_ota_get_state_partition(
    const esp_partition_t* partition,
    esp_ota_img_states_t* ota_state);
/** @brief No effect. */
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);

#endif /* FAKE_ESP_OTA_OPS_H */
