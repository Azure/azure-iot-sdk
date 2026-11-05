// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop stand-in for ESP-IDF's esp_timer.h. Behaviour: fake_esp_ota.h. */
#ifndef FAKE_ESP_TIMER_H
#define FAKE_ESP_TIMER_H

#include <stdint.h>

/** @brief Microseconds since boot; constant. */
int64_t esp_timer_get_time(void);

#endif /* FAKE_ESP_TIMER_H */
