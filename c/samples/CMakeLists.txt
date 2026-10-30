# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Every sample that connects is built once per MQTT adapter in the build: Paho as
# az_iot_sample_<...> in <group>/, az_mqtt as az_iot_sample_<...>_az_mqtt in
# <group>/az_mqtt/, with the same binary name. main.c selects its adapter with
# AZ_IOT_SAMPLE_ADAPTER_PAHO or AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT.
# See README.md for the layout and the environment variables each sample reads.

set(AZ_IOT_SAMPLES_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/common)

set(AZ_IOT_SAMPLE_ADAPTERS)
foreach(_adapter paho az_mqtt)
    if(TARGET az_iot_adapter_${_adapter})
        list(APPEND AZ_IOT_SAMPLE_ADAPTERS ${_adapter})
    endif()
endforeach()
unset(_adapter)
if(NOT AZ_IOT_SAMPLE_ADAPTERS)
    message(FATAL_ERROR "AZ_IOT_BUILD_SAMPLES=ON needs an MQTT adapter the samples can connect with: "
        "AZ_IOT_WITH_PAHO=ON or AZ_IOT_WITH_AZ_MQTT=ON (or AZ_IOT_BUILD_SAMPLES=OFF).")
endif()

# The crypto backend for samples that sign or verify (SAS tokens, software updates): OpenSSL, else mbedTLS.
set(AZ_IOT_SAMPLE_CRYPTO "")
if(TARGET az_iot_crypto_openssl)
    set(AZ_IOT_SAMPLE_CRYPTO openssl)
elseif(TARGET az_iot_crypto_mbedtls)
    set(AZ_IOT_SAMPLE_CRYPTO mbedtls)
endif()

# az_iot_add_adapter_sample(<target> SOURCES <src...> [LIBS <lib...>] [OUTPUT_NAME <name>]
#                           [INCLUDES <dir...>] [DEFINES <def...>] [ADAPTERS <adapter...>]
#                           [CRYPTO])
# One executable per adapter in AZ_IOT_SAMPLE_ADAPTERS (ADAPTERS narrows it), linked to that
# adapter and, with CRYPTO, to the AZ_IOT_SAMPLE_CRYPTO backend. Sets AZ_IOT_SAMPLE_TARGETS.
function(az_iot_add_adapter_sample target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "CRYPTO" "OUTPUT_NAME"
        "SOURCES;LIBS;INCLUDES;DEFINES;ADAPTERS")
    set(adapters ${AZ_IOT_SAMPLE_ADAPTERS})
    if("ADAPTERS" IN_LIST arg_KEYWORDS_MISSING_VALUES)
        return() # Narrowed to no adapter.
    elseif(arg_ADAPTERS)
        string(REPLACE ";" "|" re "${arg_ADAPTERS}")
        list(FILTER adapters INCLUDE REGEX "^(${re})$")
    endif()
    if(NOT arg_OUTPUT_NAME)
        set(arg_OUTPUT_NAME ${target})
    endif()
    set(targets)
    foreach(adapter IN LISTS adapters)
        string(TOUPPER "${adapter}" ADAPTER)
        if(adapter STREQUAL "paho")
            set(t ${target})
        else()
            set(t ${target}_${adapter})
        endif()
        add_executable(${t} ${arg_SOURCES})
        set_target_properties(${t} PROPERTIES OUTPUT_NAME ${arg_OUTPUT_NAME})
        if(NOT adapter STREQUAL "paho")
            set_target_properties(${t} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${adapter})
        endif()
        target_include_directories(${t} PRIVATE ${AZ_IOT_SAMPLES_COMMON_DIR} ${arg_INCLUDES})
        target_compile_definitions(${t} PRIVATE AZ_IOT_SAMPLE_ADAPTER_${ADAPTER} ${arg_DEFINES})
        target_link_libraries(${t} PRIVATE ${arg_LIBS} az_iot_adapter_${adapter})
        # Windows finds DLLs next to the executable: the SDK's stay in the shared bin/.
        if(WIN32 AND BUILD_SHARED_LIBS AND NOT adapter STREQUAL "paho")
            add_custom_command(TARGET ${t} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${t}> $<TARGET_FILE_DIR:${t}>
                COMMAND_EXPAND_LISTS)
        endif()
        if(arg_CRYPTO)
            string(TOUPPER "${AZ_IOT_SAMPLE_CRYPTO}" CRYPTO)
            target_compile_definitions(${t} PRIVATE AZ_IOT_SAMPLE_CRYPTO_${CRYPTO})
            target_link_libraries(${t} PRIVATE az_iot_crypto_${AZ_IOT_SAMPLE_CRYPTO})
        endif()
        az_iot_apply_warnings(${t})
        list(APPEND targets ${t})
    endforeach()
    set(AZ_IOT_SAMPLE_TARGETS ${targets} PARENT_SCOPE)
endfunction()

# az_iot_add_sample(<group> <name> <libs...>): <name>/main.c of the calling folder as
# az_iot_sample_<group>_<name>, binary az_iot_sample_<name> in every group, like the .NET projects.
macro(az_iot_add_sample group name)
    az_iot_add_adapter_sample(az_iot_sample_${group}_${name}
        OUTPUT_NAME az_iot_sample_${name}
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.c ${AZ_IOT_SAMPLES_COMMON_DIR}/sample_utils.c
        LIBS ${ARGN})
endmacro()

# MQTTv3 or MQTTv5, picked from the DPS assignment.
add_subdirectory(unified)
# MQTTv5 hubs only.
add_subdirectory(mqttv5)
# Software Update PC samples; they need a crypto backend.
add_subdirectory(software_update)
# Authentication / certificate-management samples. The subdirectory decides
# per-sample which other dependencies (managed provider, OpenSSL, libcurl) are required.
add_subdirectory(authentication)
