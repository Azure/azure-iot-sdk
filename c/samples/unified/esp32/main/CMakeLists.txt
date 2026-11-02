# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

idf_component_register(
    SRCS
        "app_main.c"
    REQUIRES
        azure-iot-sdk                      # SDK (core, mqttv3, mqttv5)
        azure-iot-sdk-crypto-mbedtls       # crypto backend
        azure-iot-sdk-az-mqtt              # MQTT adapter
        azure-iot-sdk-certificate-provider-esp-secure-cert  # device identity
        nvs_flash                          # Wi-Fi
        esp_netif
        esp_event
        protocol_examples_common           # example_connect(): Wi-Fi, or Ethernet in QEMU
)
