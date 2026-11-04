// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop stand-in for ESP-IDF's esp_err.h: the codes the OTA hooks and the fakes use. */
#ifndef FAKE_ESP_ERR_H
#define FAKE_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105

/** @brief Name of @p code. */
const char* esp_err_to_name(esp_err_t code);

#endif /* FAKE_ESP_ERR_H */
