# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

add_library(az_iot_core
    core/log.c
    core/result.c
    core/version.c
    core/mqtt_iface.c
    core/connection_client.c
    core/cert_util.c
    core/reconnect.c
    core/dispatch.c
    core/proto3.c
    core/span_writer.c
    core/json_string.c
    core/message.c
    core/certificate_provider_pem.c
    features/su/su_client.c
    features/su/su_device_properties.c
    features/su/su_root_keys_microsoft.c
    features/su/su_report.c
    features/su/su_channel_dps.c
    features/su/su_protocol.c
)

add_library(azure::iot::core ALIAS az_iot_core)

target_include_directories(az_iot_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/inc>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # Lets internal headers refer to siblings as "internal/foo.h" regardless
        # of where in src/ the includer lives. Required for GCC's strict
        # quoted-include resolution.
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        # Software updates' internal header lives under features/su/internal/.
        ${CMAKE_CURRENT_SOURCE_DIR}/features/su
)

# reconnect.c uses clock_gettime(CLOCK_MONOTONIC) on POSIX.
if(UNIX)
    target_compile_definitions(az_iot_core PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE)
endif()

# azure-sdk-for-c is required by the public struct definition
# (az_iot_hub_client / az_iot_provisioning_client are embedded).
target_link_libraries(az_iot_core
    PUBLIC
        az::core
        az::iot::hub
        az::iot::provisioning
)

az_iot_apply_warnings(az_iot_core)
az_iot_apply_coverage(az_iot_core)

add_library(az_iot_mqttv3
    mqttv3/telemetry_client.c
    mqttv3/c2d_client.c
    mqttv3/direct_method_client.c
    mqttv3/twin_client.c
    mqttv3/file_upload_client.c
)
add_library(azure::iot::mqttv3 ALIAS az_iot_mqttv3)
target_include_directories(az_iot_mqttv3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(az_iot_mqttv3 PUBLIC az_iot_core)
az_iot_apply_warnings(az_iot_mqttv3)
az_iot_apply_coverage(az_iot_mqttv3)

add_library(az_iot_mqttv5
    mqttv5/telemetry_client.c
    mqttv5/c2d_client.c
    mqttv5/direct_method_client.c
    mqttv5/direct_method_codec.c
    mqttv5/twin_client.c
)
add_library(azure::iot::mqttv5 ALIAS az_iot_mqttv5)
# mqttv5's own internal headers live under mqttv5/internal/; core's are reached
# through the core entry, exactly as mqttv3 does.
target_include_directories(az_iot_mqttv5 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/mqttv5
)
target_link_libraries(az_iot_mqttv5 PUBLIC az_iot_core)
az_iot_apply_warnings(az_iot_mqttv5)
az_iot_apply_coverage(az_iot_mqttv5)
