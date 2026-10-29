// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "fake_esp_secure_cert.h"

#include <stdlib.h>
#include <string.h>

fake_esp_secure_cert_state g_fake_esp_secure_cert;

/** @brief The key id the fake PSA hands out. */
#define FAKE_PSA_KEY_ID ((psa_key_id_t)0x7FFF0042u)

void fake_esp_secure_cert_reset(const char* device_cert, const char* private_key)
{
  memset(&g_fake_esp_secure_cert, 0, sizeof(g_fake_esp_secure_cert));
  g_fake_esp_secure_cert.device_cert = device_cert;
  g_fake_esp_secure_cert.private_key = private_key;
  g_fake_esp_secure_cert.has_ds_context = true;
  g_fake_esp_secure_cert.import_status = PSA_SUCCESS;
}

/** @brief A heap copy of @p text without its NUL. */
static esp_err_t hand_out(const char* text, char** buffer, uint32_t* len)
{
  if (text == NULL)
  {
    return ESP_FAIL;
  }
  size_t const n = strlen(text);
  *buffer = (char*)malloc(n);
  if (*buffer == NULL)
  {
    return ESP_FAIL;
  }
  memcpy(*buffer, text, n);
  *len = (uint32_t)n;
  return ESP_OK;
}

esp_err_t esp_secure_cert_get_device_cert(char** buffer, uint32_t* len)
{
  g_fake_esp_secure_cert.device_cert_reads++;
  return hand_out(g_fake_esp_secure_cert.device_cert, buffer, len);
}

esp_err_t esp_secure_cert_free_device_cert(char* buffer)
{
  g_fake_esp_secure_cert.device_cert_frees++;
  free(buffer);
  return ESP_OK;
}

esp_err_t esp_secure_cert_get_priv_key(char** buffer, uint32_t* len)
{
  g_fake_esp_secure_cert.private_key_reads++;
  return hand_out(g_fake_esp_secure_cert.private_key, buffer, len);
}

esp_err_t esp_secure_cert_free_priv_key(char* buffer)
{
  g_fake_esp_secure_cert.private_key_frees++;
  free(buffer);
  return ESP_OK;
}

esp_ds_data_ctx_t* esp_secure_cert_get_ds_ctx(void)
{
  g_fake_esp_secure_cert.ds_context_reads++;
  if (!g_fake_esp_secure_cert.has_ds_context)
  {
    return NULL;
  }
  esp_ds_data_ctx_t* ds = (esp_ds_data_ctx_t*)calloc(1, sizeof(*ds));
  if (ds != NULL)
  {
    ds->rsa_length_bits = 2048;
  }
  return ds;
}

void esp_secure_cert_free_ds_ctx(esp_ds_data_ctx_t* ds_ctx)
{
  g_fake_esp_secure_cert.ds_context_frees++;
  free(ds_ctx);
}

psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }

psa_status_t psa_import_key(
    const psa_key_attributes_t* attributes,
    const uint8_t* data,
    size_t data_length,
    psa_key_id_t* key)
{
  g_fake_esp_secure_cert.imports++;
  g_fake_esp_secure_cert.imported_attributes = *attributes;
  g_fake_esp_secure_cert.imported_ds_context
      = data_length == sizeof(esp_ds_data_ctx_t) ? (const esp_ds_data_ctx_t*)data : NULL;
  *key = g_fake_esp_secure_cert.import_status == PSA_SUCCESS ? FAKE_PSA_KEY_ID : 0;
  return g_fake_esp_secure_cert.import_status;
}

psa_status_t psa_destroy_key(psa_key_id_t key)
{
  g_fake_esp_secure_cert.destroys++;
  g_fake_esp_secure_cert.destroyed_key = key;
  return PSA_SUCCESS;
}

void psa_reset_key_attributes(psa_key_attributes_t* attributes)
{
  memset(attributes, 0, sizeof(*attributes));
}
