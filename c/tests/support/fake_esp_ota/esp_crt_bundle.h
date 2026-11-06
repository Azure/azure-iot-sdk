// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop stand-in for ESP-IDF's esp_crt_bundle.h. */
#ifndef FAKE_ESP_CRT_BUNDLE_H
#define FAKE_ESP_CRT_BUNDLE_H

#include "esp_err.h"

/** @brief Attach the certificate bundle; always succeeds. */
esp_err_t esp_crt_bundle_attach(void* conf);

#endif /* FAKE_ESP_CRT_BUNDLE_H */
