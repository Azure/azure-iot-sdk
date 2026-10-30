// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_certificate_provider_esp_secure_cert.h
 * @brief Certificate provider over the ESP32 esp_secure_cert partition
 * (espressif/esp_secure_cert_mgr).
 *
 * The device certificate comes from the partition. With the Digital Signature peripheral
 * (CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL) the private key never leaves it: init() imports the
 * partition's DS context into PSA and load() returns its id as client_key_psa_id, for the az_mqtt
 * adapter. Otherwise load() reads the PEM key and the last release() wipes it. One certificate
 * (index 0), the same for every role; no CSR enrollment. The DS peripheral with
 * CONFIG_ESP_SECURE_CERT_SUPPORT_LEGACY_FORMATS does not build (the dependency frees the DS context
 * twice).
 *
 * ESP-IDF component: platforms/esp32/components/azure-iot-sdk-certificate-provider-esp-secure-cert.
 */
#ifndef AZ_IOT_CERTIFICATE_PROVIDER_ESP_SECURE_CERT_H
#define AZ_IOT_CERTIFICATE_PROVIDER_ESP_SECURE_CERT_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief Caller-owned provider. Fields other than @c base are internal. */
  typedef struct az_iot_certificate_provider_esp_secure_cert
  {
    az_iot_certificate_provider base; /**< MUST be first. */
    const char* trusted_ca_pem;
    char* client_cert_pem;
    char* client_key_pem; /* only while loaned by load() */
    size_t client_key_size;
    uint32_t client_key_refs; /* load() calls not yet released */
    void* ds_context; /* esp_ds_data_ctx_t*, referenced by the PSA key until deinit */
    uint32_t client_key_psa_id;
  } az_iot_certificate_provider_esp_secure_cert;

  /**
   * @brief Reads the device certificate and, with the DS peripheral, makes its key a PSA key.
   *
   * @param[out] provider Provider to initialize.
   * @param[in] trusted_ca_pem Server trust anchors (PEM), borrowed until deinit; NULL: the
   * adapter's default.
   * @retval AZ_IOT_OK Success.
   * @retval AZ_IOT_ERR_INVALID_ARG @p provider is NULL.
   * @retval AZ_IOT_ERR_NOT_FOUND The partition has no device certificate or DS context.
   * @retval AZ_IOT_ERR_OUT_OF_MEMORY Out of memory.
   * @retval AZ_IOT_ERR_INTERNAL PSA refused the DS key.
   */
  az_iot_result az_iot_certificate_provider_esp_secure_cert_init(
      az_iot_certificate_provider_esp_secure_cert* provider,
      const char* trusted_ca_pem);

  /**
   * @brief Frees the certificate, wipes a loaned key and destroys the PSA key.
   *
   * Call after the connection client using it is deinitialized. NULL is ignored.
   */
  void az_iot_certificate_provider_esp_secure_cert_deinit(
      az_iot_certificate_provider_esp_secure_cert* provider);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CERTIFICATE_PROVIDER_ESP_SECURE_CERT_H */
