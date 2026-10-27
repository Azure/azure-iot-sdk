# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Applies c/adapters/paho/patches/*.patch, in name order, to the fetched Paho.
# Runs as PATCH_COMMAND inside FetchContent_Declare, so the working directory is
# the Paho source directory.
#
#   -DGIT_EXECUTABLE=<git> -DPATCH_DIR=<dir> -DPATCHED_COMMIT=<sha>
#
# Patches only the commit they were made against: any other Paho is left as
# fetched. A patch already applied (a re-run of this step) is skipped. The
# adapter's CMakeLists.txt warns when the result is not the patched source.

foreach(_var GIT_EXECUTABLE PATCH_DIR PATCHED_COMMIT)
    if(NOT ${_var})
        message(FATAL_ERROR "patch_paho.cmake: ${_var} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    OUTPUT_VARIABLE _head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _rc
    ERROR_QUIET)
if(NOT _rc EQUAL 0 OR NOT _head STREQUAL PATCHED_COMMIT)
    return()
endif()

file(GLOB _patches "${PATCH_DIR}/*.patch")
list(SORT _patches)
foreach(_patch IN LISTS _patches)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_patch}"
        RESULT_VARIABLE _rc
        OUTPUT_QUIET ERROR_QUIET)
    if(_rc EQUAL 0)
        continue()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "patch_paho.cmake: ${_patch} does not apply to Paho ${_head}")
    endif()
endforeach()
