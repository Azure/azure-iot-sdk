# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Embeds the SDK with add_subdirectory(), as an application building it from
# source (or via FetchContent) does, to prove c/ builds when it is not the
# top-level project. Not part of the SDK build. Reuses the tests/install
# sources, linked to the build-tree targets.
#
#   cmake -S tests/subproject -B <dir> [-DAZ_IOT_BUILD_TESTS=ON ...]
#   cmake --build <dir> && ctest --test-dir <dir>

cmake_minimum_required(VERSION 3.21)

project(azure_iot_sdk_subproject_test LANGUAGES C)

# Before add_subdirectory(), so ctest in this build tree also runs the SDK's tests.
enable_testing()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. azure-iot-sdk)

set(_src_dir ${CMAKE_CURRENT_LIST_DIR}/../install)

# One executable per built component, linked to that target alone.
set(_sources
    core                          core_test.c
    mqttv3                        mqttv3_test.c
    mqttv5                        mqttv5_test.c
    adapter_paho                  adapter_paho_test.c
    adapter_rust_mqtt             adapter_rust_mqtt_test.c
    su_crypto_openssl             su_crypto_test.c
    su_crypto_mbedtls             su_crypto_test.c
    certificate_provider_managed  certificate_provider_managed_test.c
)
list(LENGTH _sources _n)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE 0 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _sources ${_i} _comp)
    list(GET _sources ${_j} _src)
    if(NOT TARGET azure::iot::${_comp})
        continue()
    endif()
    add_executable(subproject_test_${_comp} ${_src_dir}/${_src})
    target_link_libraries(subproject_test_${_comp} PRIVATE azure::iot::${_comp})
    add_test(NAME subproject_test_${_comp} COMMAND subproject_test_${_comp})
endforeach()

if(TARGET subproject_test_su_crypto_openssl)
    target_compile_definitions(subproject_test_su_crypto_openssl PRIVATE
        AZ_IOT_TEST_HOOKS_HEADER="az_iot_su_crypto_openssl.h"
        AZ_IOT_TEST_HOOKS=az_iot_su_crypto_openssl_hooks)
endif()
if(TARGET subproject_test_su_crypto_mbedtls)
    target_compile_definitions(subproject_test_su_crypto_mbedtls PRIVATE
        AZ_IOT_TEST_HOOKS_HEADER="az_iot_su_crypto_mbedtls.h"
        AZ_IOT_TEST_HOOKS=az_iot_su_crypto_mbedtls_hooks)
endif()
