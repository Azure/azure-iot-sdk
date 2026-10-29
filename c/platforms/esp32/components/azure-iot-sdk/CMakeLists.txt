# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ESP-IDF component that compiles the azure-iot-sdk SDK (core + features) straight
# out of this repository, so ESP32 applications link the very same sources the
# desktop build uses. The azure-sdk-for-c dependency it needs (az_core, az_iot
# hub/provisioning/su parsing) is built by the sibling `azure-sdk-for-c`
# component from the in-repo git submodule (deps/azure-sdk-for-c).

# c/, four levels up from platforms/esp32/components/azure-iot-sdk/.
get_filename_component(AZ_IOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

# Same source lists as src/CMakeLists.txt.
include("${AZ_IOT_ROOT}/src/sources.cmake")
set(srcs)
foreach(_src IN LISTS AZ_IOT_CORE_SOURCES AZ_IOT_MQTTV3_SOURCES AZ_IOT_MQTTV5_SOURCES)
    list(APPEND srcs "${AZ_IOT_ROOT}/src/${_src}")
endforeach()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "${AZ_IOT_ROOT}/inc"
    PRIV_INCLUDE_DIRS
        # Internal headers resolve siblings as "internal/foo.h" regardless of
        # which src/ subtree the includer lives in (matches src/CMakeLists.txt).
        "${AZ_IOT_ROOT}/src/core"
        "${AZ_IOT_ROOT}/src/features/su"
    REQUIRES
        azure-sdk-for-c
)
