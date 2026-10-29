# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ESP-IDF component for the certificate provider over the esp_secure_cert partition,
# adapters/cert_esp_secure_cert.

# c/, four levels up from platforms/esp32/components/azure-iot-sdk-certificate-provider-esp-secure-cert/.
get_filename_component(AZ_IOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
    SRCS "${AZ_IOT_ROOT}/adapters/cert_esp_secure_cert/az_iot_certificate_provider_esp_secure_cert.c"
    INCLUDE_DIRS "${AZ_IOT_ROOT}/adapters/cert_esp_secure_cert"
    PRIV_INCLUDE_DIRS "${AZ_IOT_ROOT}/src/core"
    REQUIRES azure-iot-sdk
    PRIV_REQUIRES mbedtls  # psa_import_key() for the DS key
)
