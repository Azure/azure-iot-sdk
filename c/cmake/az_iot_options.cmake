# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

option(AZ_IOT_WITH_PAHO        "Build the Paho-C MQTT adapter (default)"      ON)
option(AZ_IOT_WITH_RUST_MQTT   "Build the Rust MQTT adapter shell"             OFF)
option(AZ_IOT_WITH_ADU_CRYPTO_OPENSSL "Build the OpenSSL ADU crypto adapter"   ON)
option(AZ_IOT_WITH_EASY        "Build API B (easy / convenience layer)"        ON)
option(AZ_IOT_BUILD_SAMPLES    "Build sample apps"                              ON)
option(AZ_IOT_BUILD_TESTS      "Build unit tests"                               OFF)
# End-to-end tests talk to a REAL Azure IoT Hub/DPS instance and are off by
# default (they require provisioned cloud resources + the Paho adapter). The
# e2e GitHub Actions workflow enables this alongside AZ_IOT_BUILD_TESTS.
option(AZ_IOT_BUILD_E2E        "Build the end-to-end test device agent"         OFF)
option(AZ_IOT_USE_SYSTEM_DEPS  "Prefer find_package() over fetched deps"       OFF)
option(AZ_IOT_USE_CPM          "Use CPM.cmake to fetch deps from source"       OFF)
option(AZ_IOT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"           ON)
# azure-sdk-for-c (az::core + az::iot::hub + az::iot::provisioning) is a
# MANDATORY dependency. It is how we talk to DPS and IoTHub-Classic. There is
# intentionally no option to disable it.
