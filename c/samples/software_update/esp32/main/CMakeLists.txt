# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

idf_component_register(
    SRCS
        "app_main.c"
        "wifi_connect.c"
        "az_iot_mqtt_esp.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../adapters/crypto_mbedtls/az_iot_crypto_mbedtls.c"
        "az_iot_cert_embedded.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../adapters/su/esp32/su_esp32_ota.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_LIST_DIR}/../../../../adapters/crypto_mbedtls"
        "${CMAKE_CURRENT_LIST_DIR}/../../../../adapters/su/esp32"
    EMBED_TXTFILES
        "certs/device_cert.pem"
        "certs/device_key.pem"
        "certs/trusted_ca.pem"
    REQUIRES
        azure-iot-sdk      # the in-repo SDK component (core + software updates)
        mqtt               # esp-mqtt
        esp_http_client    # OTA payload download
        esp-tls            # crt bundle attach
        app_update         # esp_ota_* / partition switching
        spi_flash          # esp_partition_read
        mbedtls            # crypto backend
        nvs_flash          # Wi-Fi + software updates resume blob
        esp_wifi
        esp_netif
        esp_event
)
