# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ESP-IDF component that compiles the azure-iot-sdk SDK (core + features) straight
# out of this repository, so the ESP32 sample links the very same sources the
# desktop build uses. The azure-sdk-for-c dependency it needs (az_core, az_iot
# hub/provisioning/adu parsing) is built by the sibling `azure-sdk-for-c`
# component from the in-repo git submodule (deps/azure-sdk-for-c).

# Repo root, five levels up from samples/adu/esp32/components/azure-iot-sdk/.
get_filename_component(AZ_IOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)

idf_component_register(
    SRCS
        "${AZ_IOT_ROOT}/src/core/log.c"
        "${AZ_IOT_ROOT}/src/core/result.c"
        "${AZ_IOT_ROOT}/src/core/version.c"
        "${AZ_IOT_ROOT}/src/core/mqtt_iface.c"
        "${AZ_IOT_ROOT}/src/core/connection_client.c"
        "${AZ_IOT_ROOT}/src/core/reconnect.c"
        "${AZ_IOT_ROOT}/src/core/dispatch.c"
        "${AZ_IOT_ROOT}/src/core/protocol_profile.c"
        "${AZ_IOT_ROOT}/src/core/certificate_provider_pem.c"
        "${AZ_IOT_ROOT}/src/features/twin/twin_client.c"
        "${AZ_IOT_ROOT}/src/features/direct_method/direct_method_client.c"
        "${AZ_IOT_ROOT}/src/gen1/telemetry_client.c"
        "${AZ_IOT_ROOT}/src/gen2/telemetry_client.c"
        "${AZ_IOT_ROOT}/src/features/c2d/c2d_client.c"
        "${AZ_IOT_ROOT}/src/features/file_upload/file_upload_client.c"
        "${AZ_IOT_ROOT}/src/features/adu/adu_client.c"
        "${AZ_IOT_ROOT}/src/features/adu/adu_root_keys_microsoft.c"
        "${AZ_IOT_ROOT}/src/features/adu/adu_state_reporter.c"
    INCLUDE_DIRS
        "${AZ_IOT_ROOT}/inc"
    PRIV_INCLUDE_DIRS
        # Internal headers resolve siblings as "internal/foo.h" regardless of
        # which src/ subtree the includer lives in (matches src/CMakeLists.txt).
        "${AZ_IOT_ROOT}/src/core"
        "${AZ_IOT_ROOT}/src/features/adu"
    REQUIRES
        azure-sdk-for-c
)
