# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# cmocka unit-test framework. Same framework azure-sdk-for-c uses. Fetched via
# FetchContent for the same reason as azure-sdk-for-c (no vcpkg dependency).
include(FetchContent)

set(CMOCKA_TAG "cmocka-1.1.7" CACHE STRING "cmocka git tag")
set(CMOCKA_REPO "https://git.cryptomilk.org/projects/cmocka.git" CACHE STRING "cmocka git repository")

# cmocka's single `cmocka` target follows BUILD_SHARED_LIBS. Force static here so
# downstream test exes don't need a runtime DLL on Windows. Restore afterward
# so we don't impose this on the rest of the project.
set(_sdk_save_bsl ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(WITH_CMOCKERY_SUPPORT OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES         OFF CACHE BOOL "" FORCE)
set(UNIT_TESTING          OFF CACHE BOOL "" FORCE)
set(PICKY_DEVELOPER       OFF CACHE BOOL "" FORCE)

# Suppress deprecation warnings from cmocka's old cmake_minimum_required().
set(_sdk_save_warn_deprecated ${CMAKE_WARN_DEPRECATED})
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    cmocka
    GIT_REPOSITORY ${CMOCKA_REPO}
    GIT_TAG        ${CMOCKA_TAG}
    GIT_SHALLOW    TRUE
    PATCH_COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/patch_cmocka_symlink.cmake
)
FetchContent_MakeAvailable(cmocka)

# Restore settings.
set(CMAKE_WARN_DEPRECATED ${_sdk_save_warn_deprecated} CACHE BOOL "" FORCE)
unset(_sdk_save_warn_deprecated)

set(BUILD_SHARED_LIBS ${_sdk_save_bsl} CACHE BOOL "Build shared libraries" FORCE)
unset(_sdk_save_bsl)

if(TARGET cmocka)
    if(MSVC)
        set_property(TARGET cmocka PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        # SYSTEM-include only on non-MSVC so our -Werror doesn't flag cmocka.h.
        get_target_property(_cmocka_inc cmocka INTERFACE_INCLUDE_DIRECTORIES)
        if(_cmocka_inc)
            set_target_properties(cmocka PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
            target_include_directories(cmocka SYSTEM INTERFACE ${_cmocka_inc})
        endif()
    endif()
endif()

# Helper to register a cmocka-based test executable.
function(az_iot_add_cmocka_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE az_iot_core cmocka)
    az_iot_apply_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    # If cmocka built as a shared lib (e.g. on Windows when something forced
    # BUILD_SHARED_LIBS=ON), copy the DLL next to the test exe.
    if(WIN32 AND TARGET cmocka)
        get_target_property(_cmocka_type cmocka TYPE)
        if(_cmocka_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET ${name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:cmocka> $<TARGET_FILE_DIR:${name}>
                VERBATIM)
        endif()
    endif()
endfunction()

# Test-support library: in-memory mqtt_iface mock used by multiple test exes.
add_library(az_iot_test_support STATIC
    support/mock_mqtt_iface.c
)
target_include_directories(az_iot_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(az_iot_test_support PUBLIC az_iot_core)
az_iot_apply_warnings(az_iot_test_support)

az_iot_add_cmocka_test(az_iot_tests_smoke unit/smoke_test.c)
target_include_directories(az_iot_tests_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_mqtt_iface_contract unit/mqtt_iface_contract_test.c)
target_link_libraries(az_iot_tests_mqtt_iface_contract PRIVATE az_iot_test_support)
az_iot_add_cmocka_test(az_iot_tests_connection_client unit/connection_client_test.c)
target_link_libraries(az_iot_tests_connection_client PRIVATE az_iot_test_support)
# connection_client_test pulls internal headers (reconnect.h, connection_client_internal.h)
# to drive RNG seeding and time helpers deterministically.
target_include_directories(az_iot_tests_connection_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_reconnect_policy unit/reconnect_policy_test.c)
target_include_directories(az_iot_tests_reconnect_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_protocol_profile_dispatch unit/protocol_profile_dispatch_test.c)
target_include_directories(az_iot_tests_protocol_profile_dispatch PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_telemetry_client unit/telemetry_client_test.c)
target_link_libraries(az_iot_tests_telemetry_client PRIVATE az_iot_test_support)
az_iot_add_cmocka_test(az_iot_tests_direct_method_client unit/direct_method_client_test.c)
target_link_libraries(az_iot_tests_direct_method_client PRIVATE az_iot_test_support)
az_iot_add_cmocka_test(az_iot_tests_twin_client unit/twin_client_test.c)
target_link_libraries(az_iot_tests_twin_client PRIVATE az_iot_test_support)
# twin_client_test pulls the internal header to exercise feature-client
# desired-subscriber registration (dispatched before application subscribers).
target_include_directories(az_iot_tests_twin_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)

az_iot_add_cmocka_test(az_iot_tests_adu_client unit/adu_client_test.c)
target_link_libraries(az_iot_tests_adu_client PRIVATE az_iot_test_support)

az_iot_add_cmocka_test(az_iot_tests_certificate_provider_pem unit/certificate_provider_pem_test.c)



if(AZ_IOT_WITH_PAHO)
    az_iot_add_cmocka_test(az_iot_tests_paho_adapter unit/paho_adapter_smoke_test.c)
    target_link_libraries(az_iot_tests_paho_adapter PRIVATE az_iot_adapter_paho)
endif()

if(AZ_IOT_WITH_RUST_MQTT)
    az_iot_add_cmocka_test(az_iot_tests_rust_mqtt_adapter unit/rust_mqtt_adapter_test.c)
    target_link_libraries(az_iot_tests_rust_mqtt_adapter PRIVATE az_iot_adapter_rust_mqtt)
endif()

# ADU OpenSSL crypto adapter (only when the adapter target was built, i.e.
# OpenSSL 3.0+ was found). Links OpenSSL only to generate test vectors.
if(TARGET az_iot_adu_crypto_openssl)
    find_package(OpenSSL 3.0 QUIET)
    az_iot_add_cmocka_test(az_iot_tests_adu_crypto_openssl unit/adu_crypto_openssl_test.c)
    target_link_libraries(az_iot_tests_adu_crypto_openssl PRIVATE
        az_iot_adu_crypto_openssl OpenSSL::Crypto)
endif()

# ---------------------------------------------------------------------------
# MQTT iface conformance suite.
#
# Reusable test library that operates strictly through az_iot_mqtt_iface.
# Customers can link this library to validate their own MQTT client+adapter
# implementations. Two harness exes are shipped that wire the bundled Paho
# adapter (one per MQTT version) into the suite. The suite skips itself
# (CTest exit 77) unless AZ_IOT_MQTT_BROKER_HOST is set.
# ---------------------------------------------------------------------------
add_library(az_iot_conformance STATIC
    conformance/az_iot_conformance.c
)
target_include_directories(az_iot_conformance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/conformance
)
target_link_libraries(az_iot_conformance PUBLIC az_iot_core cmocka)
if(UNIX)
    # Enable POSIX (clock_gettime, nanosleep, CLOCK_MONOTONIC).
    target_compile_definitions(az_iot_conformance PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE)
    target_link_libraries(az_iot_conformance PUBLIC pthread)
endif()
az_iot_apply_warnings(az_iot_conformance)

if(AZ_IOT_WITH_PAHO)
    add_executable(az_iot_conformance_paho_v3 conformance/paho_v3_main.c)
    target_link_libraries(az_iot_conformance_paho_v3 PRIVATE
        az_iot_conformance az_iot_adapter_paho)
    az_iot_apply_warnings(az_iot_conformance_paho_v3)
    add_test(NAME az_iot_conformance_paho_v3 COMMAND az_iot_conformance_paho_v3)
    set_tests_properties(az_iot_conformance_paho_v3 PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(az_iot_conformance_paho_v5 conformance/paho_v5_main.c)
    target_link_libraries(az_iot_conformance_paho_v5 PRIVATE
        az_iot_conformance az_iot_adapter_paho)
    az_iot_apply_warnings(az_iot_conformance_paho_v5)
    add_test(NAME az_iot_conformance_paho_v5 COMMAND az_iot_conformance_paho_v5)
    set_tests_properties(az_iot_conformance_paho_v5 PROPERTIES SKIP_RETURN_CODE 77)
endif()
