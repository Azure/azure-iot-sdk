# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Applies a patch to a FetchContent source tree, idempotently.
#
# FetchContent re-runs PATCH_COMMAND on reconfigure, and `git apply` fails on an
# already-applied patch, so a bare `git apply` turns the second `cmake` into an
# error. Reverse-checking first makes a re-run a no-op.
#
# Invoked with -P; expects GIT_EXECUTABLE, SOURCE_DIR and PATCH_FILE.

foreach(var GIT_EXECUTABLE SOURCE_DIR PATCH_FILE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "az_iot_apply_patch: ${var} is required")
    endif()
endforeach()

if(NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "az_iot_apply_patch: no such patch: ${PATCH_FILE}")
endif()

get_filename_component(_patch_name "${PATCH_FILE}" NAME)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE _already_applied
    OUTPUT_QUIET ERROR_QUIET)

if(_already_applied EQUAL 0)
    message(STATUS "azure-sdk-for-c: ${_patch_name} already applied")
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "azure-sdk-for-c: failed to apply ${_patch_name}\n"
        "  source: ${SOURCE_DIR}\n"
        "  git says: ${_err}\n"
        "This usually means AZ_SDK_C_TAG moved and the patch no longer matches. "
        "Regenerate it against the new tag.")
endif()

message(STATUS "azure-sdk-for-c: applied ${_patch_name}")
