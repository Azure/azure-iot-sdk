// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop stand-in for ESP-IDF's esp_partition.h. Behaviour: fake_esp_ota.h. */
#ifndef FAKE_ESP_PARTITION_H
#define FAKE_ESP_PARTITION_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief A flash partition; only the fields the OTA hooks read. */
typedef struct esp_partition_t
{
  uint32_t address;
  uint32_t size;
  char label[17];
} esp_partition_t;

/** @brief Read @p size bytes at @p src_offset of @p partition. */
esp_err_t esp_partition_read(
    const esp_partition_t* partition,
    size_t src_offset,
    void* dst,
    size_t size);

#endif /* FAKE_ESP_PARTITION_H */
