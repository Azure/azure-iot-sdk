<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# ESP-IDF components

ESP-IDF (v6.0 or later) components that compile the SDK from this repository. Add
`components/` to the project's `EXTRA_COMPONENT_DIRS` and `REQUIRES` what the application uses.
The `azure-sdk-for-c` component needs the `c/deps/azure-sdk-for-c` submodule.

| Component | Builds | Header |
|---|---|---|
| `azure-iot-sdk` | `src/` (core, mqttv3, mqttv5, software updates), from [src/sources.cmake](../../src/sources.cmake) | `inc/azure/iot/` |
| `azure-sdk-for-c` | az_core and az_iot from `deps/azure-sdk-for-c` | |
| `azure-iot-sdk-crypto-mbedtls` | [adapters/crypto_mbedtls](../../adapters/crypto_mbedtls) | `az_iot_crypto_mbedtls.h` |
| `azure-iot-sdk-software-update-ota` | [adapters/su/esp32](../../adapters/su/esp32): software updates platform hooks over the ESP-IDF OTA stack | `az_iot_su_esp32_ota.h` |

Used by the [ESP32 software updates sample](../../samples/software_update/esp32/README.md), which
CI builds for esp32, esp32c3, esp32s3 and esp32c6.
