// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop stand-in for ESP-IDF's nvs.h: NVS is never available (nvs_open() fails). */
#ifndef FAKE_NVS_H
#define FAKE_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ESP_ERR_NVS_NOT_FOUND 0x1102

typedef uint32_t nvs_handle_t;

typedef enum
{
  NVS_READONLY,
  NVS_READWRITE
} nvs_open_mode_t;

/** @brief Always ESP_ERR_NVS_NOT_FOUND. */
esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t open_mode, nvs_handle_t* out_handle);
/** @brief Always ESP_FAIL. */
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length);
/** @brief Always ESP_FAIL. */
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length);
/** @brief Always ESP_FAIL. */
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
/** @brief Always ESP_FAIL. */
esp_err_t nvs_commit(nvs_handle_t handle);
/** @brief No effect. */
void nvs_close(nvs_handle_t handle);

#endif /* FAKE_NVS_H */
