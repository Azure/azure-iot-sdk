# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Top-level ESP-IDF project for the software updates ESP32-WROOM sample.
#
# Build with the ESP-IDF tooling (after sourcing the IDF environment, e.g.
# C:\esp\v6.0\esp-idf\export.bat on Windows):
#
#   idf.py set-target esp32
#   idf.py build flash monitor
#
# The SDK, azure-sdk-for-c, the mbedTLS crypto backend and the OTA hooks are
# ESP-IDF components in c/platforms/esp32/components, compiled from this repo.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../platforms/esp32/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(su_esp32)
