// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop stand-in for espressif/esp_secure_cert_mgr's esp_secure_cert_read.h, with the parts of
 * the ESP-IDF RSA DS PSA driver header it includes. Behaviour: fake_esp_secure_cert.h. */
#ifndef FAKE_ESP_SECURE_CERT_READ_H
#define FAKE_ESP_SECURE_CERT_READ_H

#include <stdint.h>

#include "psa/crypto.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

/** @brief The ESP-IDF DS context; only the field the provider reads. */
typedef struct esp_ds_data_ctx_t
{
  void* esp_ds_data;
  uint8_t efuse_key_id;
  uint16_t rsa_length_bits;
} esp_ds_data_ctx_t;

/** Volatile persistence (0), location 0x800003, as in ESP-IDF v6.0. */
#define PSA_KEY_LIFETIME_ESP_RSA_DS ((psa_key_lifetime_t)0x80000300u)

esp_err_t esp_secure_cert_get_device_cert(char** buffer, uint32_t* len);
esp_err_t esp_secure_cert_free_device_cert(char* buffer);
esp_err_t esp_secure_cert_get_priv_key(char** buffer, uint32_t* len);
esp_err_t esp_secure_cert_free_priv_key(char* buffer);
esp_ds_data_ctx_t* esp_secure_cert_get_ds_ctx(void);
void esp_secure_cert_free_ds_ctx(esp_ds_data_ctx_t* ds_ctx);

#endif /* FAKE_ESP_SECURE_CERT_READ_H */
