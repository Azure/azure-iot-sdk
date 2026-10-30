<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# ESP-IDF components

ESP-IDF (v6.0 or later) components that compile the SDK from this repository. Add
`components/` and `c/deps/az_mqtt` to the project's `EXTRA_COMPONENT_DIRS`, `REQUIRES` what the
application uses, and set `CONFIG_AZ_MQTT_BUILD_AZ_CORE=n` and
`CONFIG_AZ_MQTT_AZ_CORE_COMPONENT="azure-sdk-for-c"` (azure-sdk-for-c is compiled once).
The `azure-sdk-for-c` component needs the `c/deps/azure-sdk-for-c` submodule.

| Component | Builds | Header |
|---|---|---|
| `azure-iot-sdk` | `src/` (core, mqttv3, mqttv5, software updates), from [src/sources.cmake](../../src/sources.cmake) | `inc/azure/iot/` |
| `azure-sdk-for-c` | az_core and az_iot from `deps/azure-sdk-for-c` | |
| `azure-iot-sdk-crypto-mbedtls` | [adapters/crypto_mbedtls](../../adapters/crypto_mbedtls) | `az_iot_crypto_mbedtls.h` |
| `azure-iot-sdk-az-mqtt` | [adapters/az_mqtt](../../adapters/az_mqtt): the MQTT adapter, over `c/deps/az_mqtt` and ESP-IDF's mbedTLS. With no CA, the server is verified against the ESP-IDF certificate bundle | `azure/iot/adapters/az_iot_adapter_az_mqtt.h` |
| `azure-iot-sdk-software-update-ota` | [adapters/su/esp32](../../adapters/su/esp32): software updates platform hooks over the ESP-IDF OTA stack | `az_iot_su_esp32_ota.h` |
| `azure-iot-sdk-certificate-provider-esp-secure-cert` | [adapters/cert_esp_secure_cert](../../adapters/cert_esp_secure_cert): certificate provider over the `esp_secure_cert` partition (`espressif/esp_secure_cert_mgr` 2.9.3). With the Digital Signature peripheral, the key is a PSA key (`client_key_psa_id`). Refuses `CONFIG_ESP_SECURE_CERT_SUPPORT_LEGACY_FORMATS`, whose 2.9.3 code frees a DS context twice and leaks HMAC-decrypted keys | `az_iot_certificate_provider_esp_secure_cert.h` |

Used by the [ESP32 software updates sample](../../samples/software_update/esp32/README.md), which
CI builds for esp32, esp32c3, esp32s3 and esp32c6.
