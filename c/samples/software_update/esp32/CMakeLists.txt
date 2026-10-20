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
# The azure-iot-sdk SDK is pulled in as a local ESP-IDF component that lives in
# components/azure-iot-sdk and compiles the SDK sources straight out of this repo.

cmake_minimum_required(VERSION 3.16)

# Make the in-repo SDK component visible to the IDF build system.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Applies to the SDK component and the app alike, as their struct layouts must
# match. Halves the 16 KiB default: a one-file offer is about 5 KiB, and NVS
# (partitions*.csv) must hold two generations of the largest checkpoint
# (AZ_IOT_SU_STATE_BLOB_MAX_SIZE, about 8.6 KiB) while one replaces the other.
idf_build_set_property(COMPILE_DEFINITIONS "AZ_IOT_SU_REQUEST_BUFFER_SIZE=8192" APPEND)

project(su_esp32)
