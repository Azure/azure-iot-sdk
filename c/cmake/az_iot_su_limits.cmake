# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Software updates manifest limits. They size the manifest structures
# az_iot_su_client embeds and the azure-sdk-for-c request and install-result
# structures it shares with az_iot_core, so
# azure-sdk-for-c, az_iot_core and the application must be built with the same
# values. The defaults accept every manifest Device Update accepts (10 files and
# 10 steps per update); smaller values save memory on constrained devices.
#
# Sets AZ_IOT_SU_LIMIT_DEFINITIONS: the compile definitions carrying them, both
# as the AZ_IOT_SU_MAX_* macros of az_iot_su.h and as the azure-sdk-for-c
# _az_IOT_ADU_CLIENT_MAX_* macros they map to.

set(AZ_IOT_SU_MAX_STEP_COUNT 10 CACHE STRING
    "Most steps a software update manifest may have (Device Update allows 10)")
set(AZ_IOT_SU_MAX_FILE_COUNT 10 CACHE STRING
    "Most files a software update may have (Device Update allows 10)")
set(AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP "" CACHE STRING
    "Most files one software update step may reference (empty: AZ_IOT_SU_MAX_FILE_COUNT)")

set(_az_iot_su_files_per_step "${AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP}")
if(_az_iot_su_files_per_step STREQUAL "")
    set(_az_iot_su_files_per_step "${AZ_IOT_SU_MAX_FILE_COUNT}")
endif()

foreach(_az_iot_su_limit IN ITEMS AZ_IOT_SU_MAX_STEP_COUNT AZ_IOT_SU_MAX_FILE_COUNT)
    if(NOT "${${_az_iot_su_limit}}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "${_az_iot_su_limit}='${${_az_iot_su_limit}}': must be a positive integer")
    endif()
endforeach()
if(NOT _az_iot_su_files_per_step MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP='${_az_iot_su_files_per_step}': "
        "must be empty or a positive integer")
endif()
if(_az_iot_su_files_per_step GREATER AZ_IOT_SU_MAX_FILE_COUNT)
    message(FATAL_ERROR "AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP (${_az_iot_su_files_per_step}) "
        "must not exceed AZ_IOT_SU_MAX_FILE_COUNT (${AZ_IOT_SU_MAX_FILE_COUNT})")
endif()

set(AZ_IOT_SU_LIMIT_DEFINITIONS
    AZ_IOT_SU_MAX_STEP_COUNT=${AZ_IOT_SU_MAX_STEP_COUNT}
    AZ_IOT_SU_MAX_FILE_COUNT=${AZ_IOT_SU_MAX_FILE_COUNT}
    AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP=${_az_iot_su_files_per_step}
    _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS=${AZ_IOT_SU_MAX_STEP_COUNT}
    _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT=${AZ_IOT_SU_MAX_FILE_COUNT}
    _az_IOT_ADU_CLIENT_MAX_FILE_COUNT_PER_STEP=${_az_iot_su_files_per_step})

unset(_az_iot_su_limit)
unset(_az_iot_su_files_per_step)

# Fails the configuration when the installed azure-sdk-for-c (az::iot::hub) was
# built with other limits: its request and install-result structures would have
# a different layout than az_iot_core uses. The check parses a manifest at the
# limits with its parser, which sizes its arrays with the same macros. Not run when cross-compiling.
function(az_iot_su_check_installed_limits)
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        message(WARNING "Cross-compiling: not checked that the installed azure-sdk-for-c was "
            "built with the software updates limits (AZ_IOT_SU_MAX_*). It must be.")
        return()
    endif()
    list(TRANSFORM AZ_IOT_SU_LIMIT_DEFINITIONS PREPEND "-D" OUTPUT_VARIABLE _definitions)
    try_run(_run_result _compile_result
        "${PROJECT_BINARY_DIR}/az_iot_su_limits_check"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/az_iot_su_limits_check.c"
        COMPILE_DEFINITIONS ${_definitions}
        LINK_LIBRARIES az::iot::hub az::core az::platform
        COMPILE_OUTPUT_VARIABLE _compile_output)
    if(NOT _compile_result)
        message(FATAL_ERROR "Could not build the azure-sdk-for-c limits check:\n${_compile_output}")
    endif()
    if(NOT _run_result EQUAL 0)
        set(_expected ${AZ_IOT_SU_LIMIT_DEFINITIONS})
        list(FILTER _expected INCLUDE REGEX "^_az_")
        list(TRANSFORM _expected PREPEND "-D")
        list(JOIN _expected " " _expected)
        message(FATAL_ERROR "The installed azure-sdk-for-c was not built with the software "
            "updates limits of this build (check exit code: ${_run_result}). Build it with "
            "these compile definitions, or set AZ_IOT_SU_MAX_* to its values:\n  ${_expected}")
    endif()
    message(STATUS "Installed azure-sdk-for-c matches the software updates limits")
endfunction()
