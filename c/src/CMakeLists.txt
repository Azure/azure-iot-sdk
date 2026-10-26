# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

add_library(az_iot_core ${AZ_IOT_CORE_SOURCES})

add_library(azure::iot::core ALIAS az_iot_core)

target_include_directories(az_iot_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # Lets internal headers refer to siblings as "internal/foo.h" regardless
        # of where in src/ the includer lives. Required for GCC's strict
        # quoted-include resolution.
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        # Software updates' internal header lives under features/su/internal/.
        ${CMAKE_CURRENT_SOURCE_DIR}/features/su
)

# mono_time.c uses clock_gettime(CLOCK_MONOTONIC) on POSIX.
if(UNIX)
    target_compile_definitions(az_iot_core PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE)
endif()

# Synchronization helgrind and DRD cannot see on their own (internal/race_annotations.h).
if(AZ_IOT_RACE_ANNOTATIONS)
    include(CheckIncludeFile)
    check_include_file(valgrind/helgrind.h AZ_IOT_HAVE_HELGRIND_H)
    if(NOT AZ_IOT_HAVE_HELGRIND_H)
        message(FATAL_ERROR "AZ_IOT_RACE_ANNOTATIONS=ON needs <valgrind/helgrind.h> (install valgrind).")
    endif()
    target_compile_definitions(az_iot_core PRIVATE AZ_IOT_RACE_ANNOTATIONS=1)
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

add_library(az_iot_mqttv3 ${AZ_IOT_MQTTV3_SOURCES})
add_library(azure::iot::mqttv3 ALIAS az_iot_mqttv3)
target_include_directories(az_iot_mqttv3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_link_libraries(az_iot_mqttv3 PUBLIC az_iot_core)
az_iot_apply_warnings(az_iot_mqttv3)
az_iot_apply_coverage(az_iot_mqttv3)

add_library(az_iot_mqttv5 ${AZ_IOT_MQTTV5_SOURCES})
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

# Shared builds carry no ABI guarantee (docs/struct_versioning.md). The full
# version in the soname (Linux) and DLL name (Windows) makes the loader refuse
# a library from any other release instead of letting it misread our structs.
# The pre-release label is included: previews may break the ABI between them.
foreach(_tgt IN ITEMS az_iot_core az_iot_mqttv3 az_iot_mqttv5)
    set_target_properties(${_tgt} PROPERTIES
        VERSION ${AZ_IOT_VERSION_FULL}
        SOVERSION ${AZ_IOT_VERSION_FULL}
        WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(WIN32)
        set_target_properties(${_tgt} PROPERTIES RUNTIME_OUTPUT_NAME ${_tgt}-${AZ_IOT_VERSION_FULL})
    endif()
endforeach()
