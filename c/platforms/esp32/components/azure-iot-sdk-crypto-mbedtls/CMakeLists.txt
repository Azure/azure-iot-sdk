# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ESP-IDF component for the mbedTLS (PSA Crypto) crypto backend, adapters/crypto_mbedtls.

# c/, four levels up from platforms/esp32/components/azure-iot-sdk-crypto-mbedtls/.
get_filename_component(AZ_IOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
    SRCS "${AZ_IOT_ROOT}/adapters/crypto_mbedtls/az_iot_crypto_mbedtls.c"
    INCLUDE_DIRS "${AZ_IOT_ROOT}/adapters/crypto_mbedtls"
    REQUIRES azure-iot-sdk mbedtls
)
