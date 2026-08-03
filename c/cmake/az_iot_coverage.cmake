# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Code-coverage instrumentation for first-party targets.
#
# Coverage flags are applied PER TARGET, mirroring az_iot_apply_warnings(), so
# that FetchContent'd dependencies (azure-sdk-for-c, cmocka, Paho) are never
# instrumented and never appear in the report. Setting --coverage globally via
# CMAKE_C_FLAGS would leak into those trees and pollute the denominator.
#
# Windows/MSVC has no gcov equivalent. Coverage there is collected externally by
# OpenCppCoverage against an ordinary PDB build, so this module is a no-op on
# MSVC and requires no build changes for that path.

if(AZ_IOT_ENABLE_COVERAGE AND MSVC)
    message(WARNING
        "AZ_IOT_ENABLE_COVERAGE is ignored on MSVC. Windows coverage is collected "
        "externally with OpenCppCoverage against a normal build.")
endif()

# Apply gcov instrumentation to a first-party target.
#
# Call this for shipped library targets only (az_iot_core, the adapters). Do NOT
# call it for test-support libraries or test executables: instrumenting test code
# would place it in the coverage denominator.
function(az_iot_apply_coverage target)
    if(NOT AZ_IOT_ENABLE_COVERAGE OR MSVC)
        return()
    endif()

    target_compile_options(${target} PRIVATE --coverage)

    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE
            # Absolute paths in the .gcno so gcovr resolves sources regardless
            # of the working directory it is invoked from.
            -fprofile-abs-path
            # Atomic counter updates. cmocka can fork per test and the
            # conformance suite links pthread; non-atomic counters can be lost.
            -fprofile-update=atomic
        )
    endif()

    # PUBLIC (not PRIVATE): consumers of this target -- the cmocka test
    # executables -- need -lgcov on their own link line.
    target_link_options(${target} PUBLIC --coverage)
endfunction()

# Create the `coverage` convenience target, which regenerates the same report CI
# produces. Requires that the test suite has already been run so .gcda files
# exist:
#
#   cmake --preset linux-gcc-coverage
#   cmake --build --preset linux-gcc-coverage
#   ctest --preset linux-gcc-coverage --output-on-failure
#   cmake --build --preset linux-gcc-coverage --target coverage
function(az_iot_add_coverage_target)
    if(NOT AZ_IOT_ENABLE_COVERAGE OR MSVC)
        return()
    endif()

    find_program(GCOVR_EXECUTABLE gcovr)
    if(NOT GCOVR_EXECUTABLE)
        message(STATUS
            "gcovr not found; the 'coverage' target was not created. "
            "Install it with: pip install gcovr")
        return()
    endif()

    set(_cov_dir "${CMAKE_BINARY_DIR}/coverage")

    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_cov_dir}/html"
        COMMAND ${GCOVR_EXECUTABLE}
            --root "${CMAKE_SOURCE_DIR}"
            # Explicit search path. gcovr defaults to searching --root, but the
            # .gcda files live in the build tree, which may be outside it.
            "${CMAKE_BINARY_DIR}"
            --filter "${CMAKE_SOURCE_DIR}/src/"
            --filter "${CMAKE_SOURCE_DIR}/adapters/"
            --exclude ".*/_deps/.*"
            --exclude ".*/tests/.*"
            --exclude ".*/samples/.*"
            --cobertura "${_cov_dir}/cobertura.xml" --cobertura-pretty
            --lcov "${_cov_dir}/lcov.info"
            --json-summary "${_cov_dir}/summary.json" --json-summary-pretty
            --html-details "${_cov_dir}/html/index.html"
            --txt --print-summary
        COMMAND ${CMAKE_COMMAND} -E echo "--- per-component summary ---"
        COMMAND ${CMAKE_COMMAND} -E env python3
            "${CMAKE_SOURCE_DIR}/tests/coverage_report.py"
            --summary "${_cov_dir}/summary.json"
            --components "${CMAKE_SOURCE_DIR}/tests/coverage-components.json"
            --source-root "${CMAKE_SOURCE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E echo
            "Coverage report: ${_cov_dir}/html/index.html"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Generating coverage report with gcovr"
        VERBATIM
    )
endfunction()
