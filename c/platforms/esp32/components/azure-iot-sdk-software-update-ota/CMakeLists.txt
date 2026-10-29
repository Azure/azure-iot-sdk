# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ESP-IDF component for the software updates platform hooks over the ESP-IDF OTA
# stack, adapters/su/esp32.

# c/, four levels up from platforms/esp32/components/azure-iot-sdk-software-update-ota/.
get_filename_component(AZ_IOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
    SRCS "${AZ_IOT_ROOT}/adapters/su/esp32/az_iot_su_esp32_ota.c"
    INCLUDE_DIRS "${AZ_IOT_ROOT}/adapters/su/esp32"
    REQUIRES azure-iot-sdk app_update esp_http_client
    PRIV_REQUIRES esp-tls nvs_flash spi_flash
)
