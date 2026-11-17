# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Fuzz targets (docs/eng/fuzzing.md). Each target is one LLVMFuzzerTestOneInput() source with a
# seed corpus in corpus/<target>/.
# - AZ_IOT_BUILD_FUZZERS ON: libFuzzer executables (clang or MSVC), for OneFuzz or local runs.
# - Otherwise: each target links fuzz_replay_main.c and CTest replays its corpus, so every test
#   build runs the seeds and the saved crash inputs.

set(_az_iot_fuzz_targets
    fuzz_parsers
    fuzz_connection_inbound
)

foreach(_target IN LISTS _az_iot_fuzz_targets)
    if(AZ_IOT_BUILD_FUZZERS)
        # MSVC links libFuzzer through the global /fsanitize=fuzzer (cmake/az_iot_options.cmake).
        add_executable(${_target} ${_target}.c)
        if(NOT MSVC)
            target_compile_options(${_target} PRIVATE -fsanitize=fuzzer)
            target_link_options(${_target} PRIVATE -fsanitize=fuzzer)
        endif()
    else()
        add_executable(${_target} ${_target}.c fuzz_replay_main.c)
        file(GLOB _corpus CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${_target}/*")
        add_test(NAME ${_target}_corpus COMMAND ${_target} ${_corpus})
    endif()
    target_link_libraries(${_target} PRIVATE az_iot_core az_iot_test_support az_iot_mqttv3)
    target_include_directories(${_target} PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
    az_iot_apply_warnings(${_target})
endforeach()
