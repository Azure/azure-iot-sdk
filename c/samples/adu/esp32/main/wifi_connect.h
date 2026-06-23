// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Minimal Wi-Fi station bring-up: connects using the SSID/password from
 * menuconfig (CONFIG_ADU_WIFI_SSID / CONFIG_ADU_WIFI_PASSWORD) and blocks until
 * an IP address is acquired. */
#ifndef WIFI_CONNECT_H
#define WIFI_CONNECT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up Wi-Fi in station mode and block until connected (or until the retry
 * budget is exhausted). Returns ESP_OK once an IP is assigned. */
esp_err_t wifi_connect_blocking(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONNECT_H */
