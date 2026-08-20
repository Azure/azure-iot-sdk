# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# cmocka unit-test framework. Same framework azure-sdk-for-c uses. Fetched via
# FetchContent for the same reason as azure-sdk-for-c (no vcpkg dependency).
include(FetchContent)

set(CMOCKA_TAG "cmocka-1.1.7" CACHE STRING "cmocka git tag")
# cmocka is developed on GitLab; git.cryptomilk.org is the maintainer's older
# host and is the less reliable of the two. On 2026-08-05 it began answering
# clones with "info/refs not valid: could not determine hash algorithm", which
# broke every configuration that builds the unit tests -- main included --
# because this is the only dependency we fetch at configure time.
#
# Both serve the same code: the cmocka-1.1.7 tag resolves to commit
# a01cc69ee9536f90e57c61a198f2d1944d3d4313 on either host. (On GitLab the tag
# is annotated, so refs/tags/cmocka-1.1.7 is a tag object; peeling it with ^{}
# gives that commit.)
#
# Still overridable, so a mirror or a local clone can be substituted:
#   cmake -DCMOCKA_REPO=<url> ...
set(CMOCKA_REPO "https://gitlab.com/cmocka/cmocka.git" CACHE STRING "cmocka git repository")

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
#
# Pass NO_CTEST anywhere in the argument list to build the executable without
# registering it with CTest. That is how a suite opts out of a platform it
# cannot run on: the file still compiles, so the platform's compiler keeps
# checking it, but CTest has no test to run. The alternative -- registering it
# and having it report a skip at run time -- hides the fact that it stopped
# running.
function(az_iot_add_cmocka_test name)
    set(sources "${ARGN}")
    set(register TRUE)
    if("NO_CTEST" IN_LIST sources)
        list(REMOVE_ITEM sources NO_CTEST)
        set(register FALSE)
    endif()
    add_executable(${name} ${sources})
    target_link_libraries(${name} PRIVATE az_iot_core cmocka)
    az_iot_apply_warnings(${name})
    if(register)
        add_test(NAME ${name} COMMAND ${name})
    endif()
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
az_iot_add_cmocka_test(az_iot_tests_connection_lifecycle unit/connection_lifecycle_test.c)
target_link_libraries(az_iot_tests_connection_lifecycle PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_connection_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_connection_reconnect unit/connection_reconnect_test.c)
target_link_libraries(az_iot_tests_connection_reconnect PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_connection_reconnect PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_connection_dps unit/connection_dps_test.c)
target_link_libraries(az_iot_tests_connection_dps PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_connection_dps PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_reconnect_policy unit/reconnect_policy_test.c)
target_include_directories(az_iot_tests_reconnect_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_span_writer unit/span_writer_test.c)
target_include_directories(az_iot_tests_span_writer PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_log unit/log_test.c)
# Diagnostic surface: the *_to_string helpers, version string, default
# reconnection policy and the built-in stderr sink. Pulls internal headers for
# the role/flavor helpers and the CSR request-id generator, which are internal
# but still shipped code that nothing was exercising.
az_iot_add_cmocka_test(az_iot_tests_diagnostics unit/diagnostics_test.c)
target_include_directories(az_iot_tests_diagnostics PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_protocol_profile_dispatch unit/protocol_profile_dispatch_test.c)
target_include_directories(az_iot_tests_protocol_profile_dispatch PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_telemetry_client unit/telemetry_client_test.c)
target_link_libraries(az_iot_tests_telemetry_client PRIVATE az_iot_test_support)
az_iot_add_cmocka_test(az_iot_tests_c2d_client unit/c2d_client_test.c)
target_link_libraries(az_iot_tests_c2d_client PRIVATE az_iot_test_support)
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

# File upload is HTTPS-only (stateless request-build/response-parse helpers), so
# these tests need neither the mqtt mock nor internal headers.
az_iot_add_cmocka_test(az_iot_tests_file_upload unit/file_upload_client_test.c)

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

# Managed OpenSSL certificate provider (only when the adapter target was built,
# i.e. OpenSSL 3.0+ was found). Links OpenSSL to decode/verify the CSR.
if(TARGET az_iot_certificate_provider_managed)
    find_package(OpenSSL 3.0 QUIET)
    az_iot_add_cmocka_test(az_iot_tests_certificate_provider_managed
        unit/certificate_provider_managed_test.c)
    target_link_libraries(az_iot_tests_certificate_provider_managed PRIVATE
        az_iot_certificate_provider_managed OpenSSL::Crypto)
endif()

# ---------------------------------------------------------------------------
# MQTT iface conformance suite.
#
# Reusable test library that operates strictly through az_iot_mqtt_iface.
# Customers can link this library to validate their own MQTT client+adapter
# implementations. Two harness exes are shipped that wire the bundled Paho
# adapter (one per MQTT version) into the suite.
#
# The exes always build; they are registered as CTest tests only when
# AZ_IOT_BUILD_CONFORMANCE_TESTS is on, because they need a reachable broker.
# The address comes from AZ_IOT_MQTT_BROKER_HOST / _PORT, and a missing host is
# now a hard failure rather than a self-skip: a suite that excuses itself at run
# time is one nobody notices has stopped running.
# ---------------------------------------------------------------------------
add_library(az_iot_conformance STATIC
    conformance/az_iot_conformance.c
)
target_include_directories(az_iot_conformance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/conformance
)
target_link_libraries(az_iot_conformance PUBLIC az_iot_core cmocka az_iot_test_proxy
    az_iot_test_mqtt_server)
# The certificate-validation tests drive TLS through the in-process test proxy,
# which generates and presents the certificates itself, so they need OpenSSL
# rather than a separately provisioned TLS broker. Refuse to configure when the
# option is on but OpenSSL is missing: dropping a mandatory security check is
# exactly the outcome this option exists to make visible.
if(AZ_IOT_BUILD_CONFORMANCE_TESTS_TLS)
    find_package(OpenSSL 3.0 QUIET)
    if(NOT OPENSSL_FOUND)
        message(FATAL_ERROR
            "AZ_IOT_BUILD_CONFORMANCE_TESTS_TLS=ON but OpenSSL 3.0+ was not found. The "
            "certificate-validation tests terminate TLS in the in-process test proxy, which "
            "needs OpenSSL to generate the CA and leaf it presents. Install OpenSSL 3.0+, or "
            "configure with -DAZ_IOT_BUILD_CONFORMANCE_TESTS_TLS=OFF to leave those tests out.")
    endif()
    target_compile_definitions(az_iot_conformance PRIVATE AZ_IOT_CONFORMANCE_WITH_TLS)
endif()
if(UNIX)
    # Enable POSIX (clock_gettime, nanosleep, CLOCK_MONOTONIC).
    target_compile_definitions(az_iot_conformance PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE)
    target_link_libraries(az_iot_conformance PUBLIC pthread)
endif()
az_iot_apply_warnings(az_iot_conformance)

# ---------------------------------------------------------------------------
# For-testing TCP passthrough proxy.
#
# In-process proxy that interposes a raw TCP passthrough between an MQTT client
# and a real upstream broker, so connect / reconnect / network-error paths on
# the genuine client + TLS stack become deterministically drivable. Standalone
# (sockets + libc only, no MQTT/core dependency) so it builds on every CI leg.
#
# With OpenSSL it also terminates TLS using a CA and leaf generated at run time,
# which is what lets the certificate-validation tests above run without a
# separately provisioned TLS broker.
# ---------------------------------------------------------------------------
add_library(az_iot_test_proxy STATIC
    conformance/az_iot_test_proxy.c
)
target_include_directories(az_iot_test_proxy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/conformance
)
# TLS termination needs OpenSSL. Without it the proxy is plaintext-only, and the
# conformance library refuses to configure with
# AZ_IOT_BUILD_CONFORMANCE_TESTS_TLS=ON rather than silently dropping the
# certificate tests.
find_package(OpenSSL 3.0 QUIET)
if(OPENSSL_FOUND)
    target_compile_definitions(az_iot_test_proxy PRIVATE AZ_IOT_TEST_PROXY_TLS)
    target_link_libraries(az_iot_test_proxy PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
if(UNIX)
    # Enable POSIX (getaddrinfo, select, nanosleep, CLOCK_MONOTONIC) under -std=c99.
    target_compile_definitions(az_iot_test_proxy PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE)
    target_link_libraries(az_iot_test_proxy PUBLIC pthread)
endif()
if(WIN32)
    target_link_libraries(az_iot_test_proxy PUBLIC ws2_32)
endif()
az_iot_apply_warnings(az_iot_test_proxy)

# ---------------------------------------------------------------------------
# Builders for the MQTT packets a broker sends.
#
# The proxy schedules opaque bytes and must stay that way; this is what turns
# "refuse the subscription" into the bytes for the version under test, so no
# test hand-assembles a fixed header. Deliberately a separate target from
# az_iot_test_proxy: linking it there would put protocol knowledge back into
# the layer that exists to be free of it.
#
# Standalone (libc only) -- it encodes MQTT, not anything specific to this SDK.
# ---------------------------------------------------------------------------
add_library(az_iot_test_mqtt_server STATIC
    conformance/az_iot_test_mqtt_server.c
)
target_include_directories(az_iot_test_mqtt_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/conformance
)
az_iot_apply_warnings(az_iot_test_mqtt_server)

# Pins the wire format the builders emit. The conformance cases assert client
# behaviour and would still pass if a reason code changed to another refusal,
# so the exact bytes are asserted here instead.
az_iot_add_cmocka_test(az_iot_tests_test_mqtt_server unit/test_mqtt_server_test.c)
target_link_libraries(az_iot_tests_test_mqtt_server PRIVATE az_iot_test_mqtt_server)

if(AZ_IOT_WITH_PAHO)
    add_executable(az_iot_conformance_paho_v3 conformance/paho_v3_main.c)
    target_link_libraries(az_iot_conformance_paho_v3 PRIVATE
        az_iot_conformance az_iot_adapter_paho)
    az_iot_apply_warnings(az_iot_conformance_paho_v3)

    add_executable(az_iot_conformance_paho_v5 conformance/paho_v5_main.c)
    target_link_libraries(az_iot_conformance_paho_v5 PRIVATE
        az_iot_conformance az_iot_adapter_paho)
    az_iot_apply_warnings(az_iot_conformance_paho_v5)

    if(AZ_IOT_BUILD_CONFORMANCE_TESTS)
        add_test(NAME az_iot_conformance_paho_v3 COMMAND az_iot_conformance_paho_v3)
        add_test(NAME az_iot_conformance_paho_v5 COMMAND az_iot_conformance_paho_v5)
    endif()
endif()

# ---------------------------------------------------------------------------
# Real-stack reconnect integration test.
#
# Drives the core connection client's reconnect state machine over the GENUINE
# Paho adapter, using the in-process test proxy to induce a real network drop.
# This complements (does not replace) the fast, deterministic mock-based
# connection_client_test.
#
# It needs the same reachable broker the conformance suites need, so it is
# registered under the same option rather than a second knob a job could forget
# to set. Built everywhere regardless, so every compiler keeps checking it.
# ---------------------------------------------------------------------------
if(AZ_IOT_WITH_PAHO)
    az_iot_add_cmocka_test(az_iot_reconnect_integration
        integration/reconnect_real_stack_test.c NO_CTEST)
    target_include_directories(az_iot_reconnect_integration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/conformance)
    target_link_libraries(az_iot_reconnect_integration PRIVATE
        az_iot_adapter_paho az_iot_test_proxy)
    if(UNIX)
        target_compile_definitions(az_iot_reconnect_integration PRIVATE
            _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    endif()
    if(AZ_IOT_BUILD_CONFORMANCE_TESTS)
        add_test(NAME az_iot_reconnect_integration COMMAND az_iot_reconnect_integration)
    endif()
endif()

# ---------------------------------------------------------------------------
# End-to-end (e2e) test suite.
#
# Built only when AZ_IOT_BUILD_E2E is ON (the e2e CI workflow sets it). The
# in-process cmocka suite (az_iot_tests_e2e) drives real-cloud scenarios against
# an Azure IoT Hub/DPS instance, playing both the device (shipping SDK) and the
# cloud (az_iot_e2e_service facade) halves. It needs a real MQTT adapter, so it
# also requires AZ_IOT_WITH_PAHO.
# ---------------------------------------------------------------------------
if(AZ_IOT_BUILD_E2E)
    if(AZ_IOT_WITH_PAHO)
        add_subdirectory(e2e)
    else()
        message(WARNING
            "AZ_IOT_BUILD_E2E=ON but AZ_IOT_WITH_PAHO=OFF; skipping e2e tests "
            "(they require the Paho MQTT adapter).")
    endif()
endif()
