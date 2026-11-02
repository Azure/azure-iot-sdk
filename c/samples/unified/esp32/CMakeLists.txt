# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ESP32 IoT Hub sample: DPS with X.509, then telemetry, twin and a direct method on the assigned
# hub (mqttv3 or mqttv5). ESP-IDF v6.0 or later. See README.md.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../platforms/esp32/components"
    "${CMAKE_CURRENT_LIST_DIR}/../../../deps/az_mqtt"
    # ESP-IDF's example network bring-up: Wi-Fi, or OpenCores Ethernet in QEMU.
    "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(hub_esp32)
