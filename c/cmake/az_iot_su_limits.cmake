# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Software updates manifest limits. They size the manifest and request structures
# az_iot_su_client embeds, so az_iot_core and the application must be built with
# the same values. The defaults accept every manifest Device Update accepts (10 files and
# 10 steps per update); smaller values save memory on constrained devices.
#
# Sets AZ_IOT_SU_LIMIT_DEFINITIONS: the compile definitions carrying them as the
# AZ_IOT_SU_MAX_* macros of az_iot_su.h.

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
    AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP=${_az_iot_su_files_per_step})

unset(_az_iot_su_limit)
unset(_az_iot_su_files_per_step)
