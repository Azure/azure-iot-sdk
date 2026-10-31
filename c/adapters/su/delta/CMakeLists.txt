# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Reference microsoft/delta:1 download handler for software updates (POSIX).
# The delta processor (libadudiffapi) is loaded with dlopen() at run time: no
# build-time dependency beyond the dynamic loader.

if(NOT UNIX)
    message(FATAL_ERROR "AZ_IOT_WITH_SU_DELTA needs a POSIX platform.")
endif()

add_library(az_iot_su_delta STATIC
    az_iot_su_delta.c
)
add_library(azure::iot::su_delta ALIAS az_iot_su_delta)

target_include_directories(az_iot_su_delta
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(az_iot_su_delta
    PUBLIC
        az_iot_core
    PRIVATE
        ${CMAKE_DL_LIBS}
)
az_iot_apply_warnings(az_iot_su_delta)
az_iot_apply_coverage(az_iot_su_delta)
