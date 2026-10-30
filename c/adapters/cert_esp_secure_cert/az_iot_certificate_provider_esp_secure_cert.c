// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "az_iot_certificate_provider_esp_secure_cert.h"

#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_log_components.h"
#include "esp_secure_cert_read.h"
#include "internal/crypto.h" // az_iot_crypto__wipe()

#if defined(CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL)
#include <psa/crypto.h>
#elif defined(ESP_PLATFORM)
#include "esp_memory_utils.h"
#endif

/** @brief A NUL-terminated copy of @p length bytes at @p data; NULL when out of memory. */
static char* copy_text(const char* data, uint32_t length, size_t* out_size)
{
  char* text = (char*)malloc((size_t)length + 1);
  if (text != NULL)
  {
    memcpy(text, data, length);
    text[length] = '\0';
    *out_size = (size_t)length + 1;
  }
  return text;
}

/** @brief Copies the partition's device certificate into @p p. */
static az_iot_result read_device_certificate(az_iot_certificate_provider_esp_secure_cert* p)
{
  char* data = NULL;
  uint32_t length = 0;
  if (esp_secure_cert_get_device_cert(&data, &length) != ESP_OK || data == NULL || length == 0)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CERT, "esp_secure_cert: no device certificate");
    return AZ_IOT_ERR_NOT_FOUND;
  }
  size_t size = 0;
  p->client_cert_pem = copy_text(data, length, &size);
  (void)esp_secure_cert_free_device_cert(data);
  return p->client_cert_pem != NULL ? AZ_IOT_OK : AZ_IOT_ERR_OUT_OF_MEMORY;
}

#if defined(CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL)
/** @brief Imports the partition's DS context into PSA, signing only; @p p keeps the context. */
static az_iot_result import_ds_key(az_iot_certificate_provider_esp_secure_cert* p)
{
  esp_ds_data_ctx_t* ds = esp_secure_cert_get_ds_ctx();
  if (ds == NULL)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CERT, "esp_secure_cert: no DS context");
    return AZ_IOT_ERR_NOT_FOUND;
  }
  // As esp-tls does: the PSA key references the context, which must outlive it.
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
  psa_set_key_bits(&attributes, ds->rsa_length_bits);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
  psa_set_key_algorithm(&attributes, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_ANY_HASH));
#if defined(CONFIG_MBEDTLS_SSL_PROTO_TLS1_3)
  psa_set_key_enrollment_algorithm(&attributes, PSA_ALG_RSA_PSS(PSA_ALG_ANY_HASH));
#endif
  psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_ESP_RSA_DS);
  psa_key_id_t id = 0;
  psa_status_t status = psa_crypto_init();
  if (status == PSA_SUCCESS)
  {
    status = psa_import_key(&attributes, (const uint8_t*)ds, sizeof(*ds), &id);
  }
  psa_reset_key_attributes(&attributes);
  if (status != PSA_SUCCESS || id == 0)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CERT, "esp_secure_cert: PSA refused the DS key (%d)", (int)status);
    esp_secure_cert_free_ds_ctx(ds);
    return AZ_IOT_ERR_INTERNAL;
  }
  p->ds_context = ds;
  p->client_key_psa_id = (uint32_t)id;
  return AZ_IOT_OK;
}
#else
/** @brief Wipes @p data if it is in RAM (NVS or HMAC-decrypted keys), not mapped flash. */
static void wipe_key_source(char* data, uint32_t length)
{
#if defined(ESP_PLATFORM)
  if (!esp_ptr_in_dram(data) && !esp_ptr_external_ram(data))
  {
    return;
  }
#endif
  az_iot_crypto__wipe(data, length);
}

/** @brief Copies the partition's PEM private key into @p p. */
static az_iot_result read_client_key(az_iot_certificate_provider_esp_secure_cert* p)
{
  char* data = NULL;
  uint32_t length = 0;
  if (esp_secure_cert_get_priv_key(&data, &length) != ESP_OK || data == NULL || length == 0)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CERT, "esp_secure_cert: no private key");
    return AZ_IOT_ERR_NOT_FOUND;
  }
  p->client_key_pem = copy_text(data, length, &p->client_key_size);
  wipe_key_source(data, length); // esp_secure_cert_free_priv_key() frees without wiping.
  (void)esp_secure_cert_free_priv_key(data);
  return p->client_key_pem != NULL ? AZ_IOT_OK : AZ_IOT_ERR_OUT_OF_MEMORY;
}
#endif

/** @brief Wipes and frees the loaned key. */
static void drop_client_key(az_iot_certificate_provider_esp_secure_cert* p)
{
  if (p->client_key_pem != NULL)
  {
    az_iot_crypto__wipe(p->client_key_pem, p->client_key_size);
    free(p->client_key_pem);
  }
  p->client_key_pem = NULL;
  p->client_key_size = 0;
  p->client_key_refs = 0;
}

static az_iot_result esp_secure_cert_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    uint8_t index,
    az_iot_certificate_material* out)
{
  az_iot_certificate_provider_esp_secure_cert* p
      = (az_iot_certificate_provider_esp_secure_cert*)self;
  (void)role; // One device identity for every role.
  if (p == NULL || out == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (index != 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  if (p->client_cert_pem == NULL)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
#if !defined(CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL)
  // The key is in memory only between load() and the last release().
  if (p->client_key_refs == UINT32_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  if (p->client_key_refs == 0)
  {
    az_iot_result const r = read_client_key(p);
    if (r != AZ_IOT_OK)
    {
      return r;
    }
  }
  p->client_key_refs++;
#endif
  memset(out, 0, sizeof(*out));
  out->trusted_ca_pem = p->trusted_ca_pem;
  out->client_cert_pem = p->client_cert_pem;
  out->client_key_pem = p->client_key_pem;
  out->client_key_psa_id = p->client_key_psa_id;
  return AZ_IOT_OK;
}

static void esp_secure_cert_release(
    az_iot_certificate_provider* self,
    az_iot_certificate_material* material)
{
  az_iot_certificate_provider_esp_secure_cert* p
      = (az_iot_certificate_provider_esp_secure_cert*)self;
  if (p == NULL || material == NULL)
  {
    return;
  }
  if (material->client_key_pem != NULL && material->client_key_pem == p->client_key_pem
      && p->client_key_refs > 0 && --p->client_key_refs == 0)
  {
    drop_client_key(p);
  }
  memset(material, 0, sizeof(*material));
}

static void esp_secure_cert_deinit_vtable(az_iot_certificate_provider* self)
{
  az_iot_certificate_provider_esp_secure_cert_deinit(
      (az_iot_certificate_provider_esp_secure_cert*)self);
}

static const az_iot_certificate_provider_vtable s_esp_secure_cert_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = esp_secure_cert_load,
  .release = esp_secure_cert_release,
  .deinit = esp_secure_cert_deinit_vtable,
};

az_iot_result az_iot_certificate_provider_esp_secure_cert_init(
    az_iot_certificate_provider_esp_secure_cert* provider,
    const char* trusted_ca_pem)
{
  if (provider == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(provider, 0, sizeof(*provider));
  az_iot_result r = read_device_certificate(provider);
#if defined(CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL)
  r = r == AZ_IOT_OK ? import_ds_key(provider) : r;
#endif
  if (r != AZ_IOT_OK)
  {
    az_iot_certificate_provider_esp_secure_cert_deinit(provider);
    return r;
  }
  provider->trusted_ca_pem = trusted_ca_pem;
  provider->base.vtable = &s_esp_secure_cert_vtable;
  return AZ_IOT_OK;
}

void az_iot_certificate_provider_esp_secure_cert_deinit(
    az_iot_certificate_provider_esp_secure_cert* provider)
{
  if (provider == NULL)
  {
    return;
  }
  drop_client_key(provider);
  free(provider->client_cert_pem);
#if defined(CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL)
  if (provider->client_key_psa_id != 0)
  {
    (void)psa_destroy_key((psa_key_id_t)provider->client_key_psa_id);
  }
  if (provider->ds_context != NULL)
  {
    esp_secure_cert_free_ds_ctx((esp_ds_data_ctx_t*)provider->ds_context);
  }
#endif
  memset(provider, 0, sizeof(*provider));
}
