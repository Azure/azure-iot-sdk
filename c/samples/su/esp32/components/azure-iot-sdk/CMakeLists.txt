# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ESP-IDF component that compiles the azure-iot-sdk SDK (core + features) straight
# out of this repository, so the ESP32 sample links the very same sources the
# desktop build uses. The azure-sdk-for-c dependency it needs (az_core, az_iot
# hub/provisioning/su parsing) is built by the sibling `azure-sdk-for-c`
# component from the in-repo git submodule (deps/azure-sdk-for-c).

# Repo root, five levels up from samples/su/esp32/components/azure-iot-sdk/.
get_filename_component(AZ_IOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)

idf_component_register(
    SRCS
        # Mirrors the az_iot_core/mqttv3/mqttv5 source list in src/CMakeLists.txt.
        # ESP-IDF cannot include that file, so this list is maintained by hand:
        # when you add or remove a source there, change it here too. Nothing in
        # CI builds this component, so a mismatch surfaces only on-device.
        "${AZ_IOT_ROOT}/src/core/log.c"
        "${AZ_IOT_ROOT}/src/core/result.c"
        "${AZ_IOT_ROOT}/src/core/version.c"
        "${AZ_IOT_ROOT}/src/core/mqtt_iface.c"
        "${AZ_IOT_ROOT}/src/core/connection_client.c"
        "${AZ_IOT_ROOT}/src/core/cert_util.c"
        "${AZ_IOT_ROOT}/src/core/reconnect.c"
        "${AZ_IOT_ROOT}/src/core/dispatch.c"
        "${AZ_IOT_ROOT}/src/core/proto3.c"
        "${AZ_IOT_ROOT}/src/core/span_writer.c"
        "${AZ_IOT_ROOT}/src/core/json_string.c"
        "${AZ_IOT_ROOT}/src/core/message.c"
        "${AZ_IOT_ROOT}/src/core/certificate_provider_pem.c"
        "${AZ_IOT_ROOT}/src/features/su/su_client.c"
        "${AZ_IOT_ROOT}/src/features/su/su_device_properties.c"
        "${AZ_IOT_ROOT}/src/features/su/su_root_keys_microsoft.c"
        "${AZ_IOT_ROOT}/src/features/su/su_report.c"
        "${AZ_IOT_ROOT}/src/features/su/su_channel_dps.c"
        "${AZ_IOT_ROOT}/src/features/su/su_protocol.c"
        "${AZ_IOT_ROOT}/src/mqttv3/telemetry_client.c"
        "${AZ_IOT_ROOT}/src/mqttv3/c2d_client.c"
        "${AZ_IOT_ROOT}/src/mqttv3/direct_method_client.c"
        "${AZ_IOT_ROOT}/src/mqttv3/twin_client.c"
        "${AZ_IOT_ROOT}/src/mqttv3/file_upload_client.c"
        "${AZ_IOT_ROOT}/src/mqttv5/telemetry_client.c"
        "${AZ_IOT_ROOT}/src/mqttv5/c2d_client.c"
        "${AZ_IOT_ROOT}/src/mqttv5/direct_method_client.c"
        "${AZ_IOT_ROOT}/src/mqttv5/direct_method_codec.c"
        "${AZ_IOT_ROOT}/src/mqttv5/twin_client.c"
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
