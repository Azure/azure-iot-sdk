# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

idf_component_register(
    SRCS
        "app_main.c"
        "wifi_connect.c"
        "az_iot_cert_embedded.c"
    INCLUDE_DIRS
        "."
    EMBED_TXTFILES
        "certs/device_cert.pem"
        "certs/device_key.pem"
        "certs/trusted_ca.pem"
    REQUIRES
        azure-iot-sdk                      # SDK (core, mqttv3, mqttv5, software updates)
        azure-iot-sdk-crypto-mbedtls       # crypto backend
        azure-iot-sdk-software-update-ota  # OTA platform hooks
        azure-iot-sdk-az-mqtt              # MQTT adapter
        nvs_flash                          # Wi-Fi, update check route
        esp_wifi
        esp_netif
        esp_event
)
