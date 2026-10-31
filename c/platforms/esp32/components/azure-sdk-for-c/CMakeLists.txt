# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# ESP-IDF component that builds the Azure SDK for Embedded C (azure-sdk-for-c)
# straight from the in-repo git submodule at deps/azure-sdk-for-c. The
# upstream project ships its own (host-oriented) CMake, so rather than invoke
# it we register the platform-independent az_core + az_iot translation units
# directly as an IDF component. Only the MQTT-stack-agnostic pieces our SDK
# uses are compiled (no az_http transport, no curl).

# c/, four levels up from platforms/esp32/components/azure-sdk-for-c/.
get_filename_component(AZ_IOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(AZ_SDK_C_ROOT "${AZ_IOT_ROOT}/deps/azure-sdk-for-c")

set(_az_src "${AZ_SDK_C_ROOT}/sdk/src/azure")

set(srcs
    # az_core (spans, JSON, base64, logging, contexts, result codes, precond).
    "${_az_src}/core/az_base64.c"
    "${_az_src}/core/az_context.c"
    "${_az_src}/core/az_json_reader.c"
    "${_az_src}/core/az_json_token.c"
    "${_az_src}/core/az_json_writer.c"
    "${_az_src}/core/az_log.c"
    "${_az_src}/core/az_precondition.c"
    "${_az_src}/core/az_span.c"

    # az_iot (MQTT topic build/parse for Hub, DPS, and software updates manifest parsing).
    "${_az_src}/iot/az_iot_common.c"
    "${_az_src}/iot/az_iot_hub_client.c"
    "${_az_src}/iot/az_iot_hub_client_c2d.c"
    "${_az_src}/iot/az_iot_hub_client_commands.c"
    "${_az_src}/iot/az_iot_hub_client_methods.c"
    "${_az_src}/iot/az_iot_hub_client_properties.c"
    "${_az_src}/iot/az_iot_hub_client_sas.c"
    "${_az_src}/iot/az_iot_hub_client_telemetry.c"
    "${_az_src}/iot/az_iot_hub_client_twin.c"
    "${_az_src}/iot/az_iot_provisioning_client.c"
    "${_az_src}/iot/az_iot_provisioning_client_sas.c"
    "${_az_src}/iot/az_iot_adu_client.c"

    # az_core's az_platform_* hooks (clock, sleep) on ESP-IDF. The az_mqtt
    # adapter sleeps through them while no connection is open.
    "az_platform_esp_idf.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "${AZ_SDK_C_ROOT}/sdk/inc"
    PRIV_REQUIRES esp_timer freertos
)

# Upstream uses legacy "func()" declarations and other constructs our strict
# warnings would reject; relax just this component.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)

# Software updates manifest limits. PUBLIC: the SDK and the application must
# compile az_iot_su.h with the values this parser is built with.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include("${AZ_IOT_ROOT}/cmake/az_iot_su_limits.cmake")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ${AZ_IOT_SU_LIMIT_DEFINITIONS})
endif()
