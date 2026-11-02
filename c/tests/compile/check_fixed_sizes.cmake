# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Builds each target in TARGETS (comma-separated) in BUILD_DIR and fails unless
# every one fails to compile on a "fixed" size guard. CONFIG: the build
# configuration, for multi-config generators.
#
#   cmake -DBUILD_DIR=<dir> -DCONFIG=<cfg> -DTARGETS=<t1,t2,...> -P check_fixed_sizes.cmake

string(REPLACE "," ";" _targets "${TARGETS}")
set(_config_args "")
if(NOT CONFIG STREQUAL "")
    set(_config_args --config "${CONFIG}")
endif()
set(_failures 0)
foreach(_target IN LISTS _targets)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${_target}" ${_config_args}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _out)
    if(_rc EQUAL 0)
        message(SEND_ERROR "${_target} compiled: the fixed size it defines is not guarded")
        math(EXPR _failures "${_failures} + 1")
    elseif(NOT _out MATCHES "(is|are) fixed")
        message(SEND_ERROR "${_target} failed, but not on a fixed size guard:\n${_out}")
        math(EXPR _failures "${_failures} + 1")
    else()
        message(STATUS "${_target}: refused as expected")
    endif()
endforeach()
if(_failures GREATER 0)
    message(FATAL_ERROR "${_failures} fixed size override(s) not refused")
endif()
