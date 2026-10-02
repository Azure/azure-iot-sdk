# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Consumes an INSTALLED azure-iot-sdk package the way an application does, to
# prove the package works on its own. Not part of the SDK build.
#
#   cmake -S tests/install -B <dir> -DCMAKE_PREFIX_PATH=<install prefix>
#         [-DAZ_IOT_EXPECTED_COMPONENTS="core;mqttv3;mqttv5;adapter_paho;..."]
#   cmake --build <dir> && ctest --test-dir <dir>
#
# Every expected component is REQUIRED, so an adapter missing from the package
# fails the configure instead of silently shrinking the test set.

cmake_minimum_required(VERSION 3.21)

project(azure_iot_sdk_install_test LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# Must match the SDK's static CRT (/MT[d]).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(AZ_IOT_EXPECTED_COMPONENTS "core;mqttv3;mqttv5" CACHE STRING
    "azure-iot-sdk components the package must provide")

find_package(azure-iot-sdk CONFIG REQUIRED COMPONENTS ${AZ_IOT_EXPECTED_COMPONENTS})
message(STATUS "azure-iot-sdk ${azure-iot-sdk_VERSION} found at ${azure-iot-sdk_DIR}")

enable_testing()

# One executable per installed component, linked to that target alone, so each
# target's usage requirements (headers, transitive libraries) are checked alone.
set(_sources
    core                          core_test.c
    mqttv3                        mqttv3_test.c
    mqttv5                        mqttv5_test.c
    adapter_paho                  adapter_paho_test.c
    adapter_rust_mqtt             adapter_rust_mqtt_test.c
    crypto_openssl                crypto_test.c
    crypto_mbedtls                crypto_test.c
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
    add_executable(install_test_${_comp} ${_src})
    target_link_libraries(install_test_${_comp} PRIVATE azure::iot::${_comp})
    if(MSVC)
        target_compile_options(install_test_${_comp} PRIVATE /W4 /WX)
    else()
        target_compile_options(install_test_${_comp} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME install_test_${_comp} COMMAND install_test_${_comp})
endforeach()

if(TARGET install_test_crypto_openssl)
    target_compile_definitions(install_test_crypto_openssl PRIVATE
        AZ_IOT_TEST_CRYPTO_HEADER="az_iot_crypto_openssl.h"
        AZ_IOT_TEST_CRYPTO=az_iot_crypto_openssl)
endif()
if(TARGET install_test_crypto_mbedtls)
    target_compile_definitions(install_test_crypto_mbedtls PRIVATE
        AZ_IOT_TEST_CRYPTO_HEADER="az_iot_crypto_mbedtls.h"
        AZ_IOT_TEST_CRYPTO=az_iot_crypto_mbedtls)
endif()
