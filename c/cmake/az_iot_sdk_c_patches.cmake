# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# The patches this repo carries against azure-sdk-for-c.
#
# Upstream is ARCHIVED, so fixes land here or nowhere. There is more than one
# consumer of that source -- the FetchContent build in c/CMakeLists.txt and the
# ESP-IDF component under c/samples/adu/esp32 -- and a patch that reaches only
# one of them is worse than no patch at all: the two builds would silently speak
# different api-versions. Both include this file so the list stays single-sourced.

get_filename_component(AZ_SDK_C_PATCH_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../patches/azure-sdk-for-c" ABSOLUTE)

# 0001 raises the DPS api-version to the preview that carries connectionProfile
# -- see docs/eng/client-separation.md section 2.
set(AZ_SDK_C_PATCHES
    "${AZ_SDK_C_PATCH_DIR}/0001-dps-api-version.patch")

set(AZ_SDK_C_APPLY_PATCH_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/az_iot_apply_patch.cmake")

# Apply every patch to an already-present source tree (the ESP-IDF component's
# submodule). The FetchContent build uses PATCH_COMMAND instead, since the tree
# does not exist yet at configure time.
function(az_iot_patch_sdk_c_in_place source_dir)
    find_package(Git REQUIRED)
    foreach(_patch IN LISTS AZ_SDK_C_PATCHES)
        execute_process(
            COMMAND ${CMAKE_COMMAND}
                -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
                -DSOURCE_DIR=${source_dir}
                -DPATCH_FILE=${_patch}
                -P "${AZ_SDK_C_APPLY_PATCH_SCRIPT}"
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR
                "Failed to apply ${_patch} to ${source_dir}:\n${_out}${_err}")
        endif()
    endforeach()
endfunction()
