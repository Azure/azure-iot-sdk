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
if(UNIX)
    target_compile_definitions(az_iot_tests_connection_dps PRIVATE _POSIX_C_SOURCE=200809L)
endif()
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
az_iot_add_cmocka_test(az_iot_tests_dispatch unit/dispatch_test.c)
target_include_directories(az_iot_tests_dispatch PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(
    az_iot_tests_gen1_telemetry_client unit/gen1_telemetry_client_test.c)
target_link_libraries(
    az_iot_tests_gen1_telemetry_client PRIVATE az_iot_test_support az_iot_gen1)
az_iot_add_cmocka_test(
    az_iot_tests_gen2_telemetry_client unit/gen2_telemetry_client_test.c)
target_link_libraries(
    az_iot_tests_gen2_telemetry_client PRIVATE az_iot_test_support az_iot_gen2)
az_iot_add_cmocka_test(az_iot_tests_gen1_c2d_client unit/gen1_c2d_client_test.c)
target_link_libraries(az_iot_tests_gen1_c2d_client PRIVATE az_iot_test_support az_iot_gen1)
az_iot_add_cmocka_test(az_iot_tests_gen2_c2d_client unit/gen2_c2d_client_test.c)
target_link_libraries(az_iot_tests_gen2_c2d_client PRIVATE az_iot_test_support az_iot_gen2)
az_iot_add_cmocka_test(az_iot_tests_gen1_direct_method_client unit/gen1_direct_method_client_test.c)
target_link_libraries(az_iot_tests_gen1_direct_method_client PRIVATE az_iot_test_support az_iot_gen1)
if(UNIX)
    # The slot-reclaim deadline is only observable once a real second has
    # passed, so the suite calls nanosleep().
    target_compile_definitions(az_iot_tests_gen1_direct_method_client PRIVATE
        _POSIX_C_SOURCE=200809L)
endif()
# AZ_IOT_DM_RID_MAX is a documented compile-time knob, and it sizes both a
# public struct and the response topic the client builds from it -- so the
# library and the application have to be built against the same value, hence
# compiling the gen1 source into this target rather than linking az_iot_gen1,
# which carries the default. Running the whole suite against a raised rid bound
# is what proves the knob reaches every buffer derived from it.
az_iot_add_cmocka_test(az_iot_tests_gen1_direct_method_client_tuned
    unit/gen1_direct_method_client_test.c
    ${CMAKE_SOURCE_DIR}/src/gen1/direct_method_client.c)
target_link_libraries(az_iot_tests_gen1_direct_method_client_tuned PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_gen1_direct_method_client_tuned PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/gen1)
target_compile_definitions(az_iot_tests_gen1_direct_method_client_tuned PRIVATE
    AZ_IOT_DM_RID_MAX=200)
if(UNIX)
    target_compile_definitions(az_iot_tests_gen1_direct_method_client_tuned PRIVATE
        _POSIX_C_SOURCE=200809L)
endif()
az_iot_add_cmocka_test(az_iot_tests_gen2_direct_method_client unit/gen2_direct_method_client_test.c)
target_link_libraries(az_iot_tests_gen2_direct_method_client PRIVATE az_iot_test_support az_iot_gen2)
if(UNIX)
    # Two AEG direct-method budgets are only observable once a real second has
    # passed, so the suite calls nanosleep().
    target_compile_definitions(az_iot_tests_gen2_direct_method_client PRIVATE
        _POSIX_C_SOURCE=200809L)
endif()
# The AEG design requires the concurrency limit to be configurable. It sizes a
# public struct, so the library and the application must be built against the
# same value -- hence the gen2 sources are compiled into this target rather than
# linked from az_iot_gen2, which carries the default. Running the whole suite
# against an overridden limit proves the knob reaches every pool, loop and
# admission check, and catches anyone reintroducing a hardcoded bound.
az_iot_add_cmocka_test(az_iot_tests_gen2_direct_method_client_tuned
    unit/gen2_direct_method_client_test.c
    ${CMAKE_SOURCE_DIR}/src/gen2/direct_method_client.c
    ${CMAKE_SOURCE_DIR}/src/gen2/direct_method_codec.c)
target_link_libraries(az_iot_tests_gen2_direct_method_client_tuned PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_gen2_direct_method_client_tuned PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/gen2)
target_compile_definitions(az_iot_tests_gen2_direct_method_client_tuned PRIVATE
    AZ_IOT_GEN2_DM_MAX_CONCURRENT=2)
if(UNIX)
    target_compile_definitions(az_iot_tests_gen2_direct_method_client_tuned PRIVATE
        _POSIX_C_SOURCE=200809L)
endif()
# The AEG direct-method protobuf codec is hand-rolled, so its wire format is
# pinned here against bytes taken from the .proto rather than from itself.
az_iot_add_cmocka_test(
    az_iot_tests_gen2_direct_method_codec unit/gen2_direct_method_codec_test.c)
target_link_libraries(az_iot_tests_gen2_direct_method_codec PRIVATE az_iot_test_support az_iot_gen2)
target_include_directories(az_iot_tests_gen2_direct_method_codec PRIVATE
    ${CMAKE_SOURCE_DIR}/src/gen2)
az_iot_add_cmocka_test(az_iot_tests_gen1_twin_client unit/gen1_twin_client_test.c)
target_link_libraries(az_iot_tests_gen1_twin_client PRIVATE az_iot_test_support az_iot_gen1)
az_iot_add_cmocka_test(az_iot_tests_gen2_twin_client unit/gen2_twin_client_test.c)
target_link_libraries(az_iot_tests_gen2_twin_client PRIVATE az_iot_test_support az_iot_gen2)
# The defensive timeouts run on a multi-minute schedule, so the tests reach the
# internal seam that expires them rather than waiting one out.
target_include_directories(az_iot_tests_gen2_twin_client PRIVATE ${CMAKE_SOURCE_DIR}/src)

az_iot_add_cmocka_test(az_iot_tests_adu_client unit/adu_client_test.c)
az_iot_add_cmocka_test(az_iot_tests_adu_protocol unit/adu_protocol_test.c)
target_link_libraries(az_iot_tests_adu_client PRIVATE az_iot_test_support)

# The channel over a real connection client: what reaches the broker, how a
# response is correlated back, and the paths that can fault provisioning.
az_iot_add_cmocka_test(az_iot_tests_adu_channel_dps unit/adu_channel_dps_test.c)
target_link_libraries(az_iot_tests_adu_channel_dps PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_adu_channel_dps PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/features/adu/internal)

# File upload is HTTPS-only (stateless request-build/response-parse helpers), so
# these tests need neither the mqtt mock nor internal headers. gen2 is linked
# only so the profile-pin test can show a released pin admits the other
# generation.
az_iot_add_cmocka_test(az_iot_tests_gen1_file_upload unit/gen1_file_upload_client_test.c)
target_link_libraries(az_iot_tests_gen1_file_upload PRIVATE az_iot_gen1 az_iot_gen2)

az_iot_add_cmocka_test(az_iot_tests_certificate_provider_pem unit/certificate_provider_pem_test.c)

# Non-extractable key custody (D8) through the connection client: the
# key-reference fields and the sign() hook reaching the adapter on BOTH connect
# paths, the v2 gating, and the open-time credential validation.
az_iot_add_cmocka_test(az_iot_tests_connection_key_custody unit/connection_key_custody_test.c)
target_link_libraries(az_iot_tests_connection_key_custody PRIVATE az_iot_test_support)



if(AZ_IOT_WITH_PAHO)
    az_iot_add_cmocka_test(az_iot_tests_paho_adapter unit/paho_adapter_smoke_test.c)
    target_link_libraries(az_iot_tests_paho_adapter PRIVATE az_iot_adapter_paho)

    # The Paho adapter's non-extractable key custody (D8). It reaches into the
    # adapter's private header, so it needs the adapter's source directory on
    # the include path, and it needs the same AZ_IOT_PAHO_KEY_CUSTODY definition
    # the adapter was compiled with so the cases match the built behaviour.
    az_iot_add_cmocka_test(az_iot_tests_paho_key_custody unit/paho_key_custody_test.c)
    target_link_libraries(az_iot_tests_paho_key_custody PRIVATE az_iot_adapter_paho)
    target_include_directories(az_iot_tests_paho_key_custody PRIVATE
        ${CMAKE_SOURCE_DIR}/adapters/paho)
    get_target_property(_paho_defs az_iot_adapter_paho COMPILE_DEFINITIONS)
    if(_paho_defs)
        target_compile_definitions(az_iot_tests_paho_key_custody PRIVATE ${_paho_defs})
    endif()

    # The same custody code against a REAL PKCS#11 token. Built only when a
    # token is going to be supplied (AZ_IOT_TEST_PKCS11_KEY_URI); it fails
    # loudly rather than skipping when the environment is missing.
    if(AZ_IOT_BUILD_PKCS11_TESTS)
        find_package(OpenSSL 3.0 REQUIRED)
        az_iot_add_cmocka_test(az_iot_tests_paho_key_custody_pkcs11
            unit/paho_key_custody_pkcs11_test.c)
        target_link_libraries(az_iot_tests_paho_key_custody_pkcs11 PRIVATE
            az_iot_adapter_paho OpenSSL::SSL OpenSSL::Crypto)
        target_include_directories(az_iot_tests_paho_key_custody_pkcs11 PRIVATE
            ${CMAKE_SOURCE_DIR}/adapters/paho)
        if(_paho_defs)
            target_compile_definitions(az_iot_tests_paho_key_custody_pkcs11 PRIVATE ${_paho_defs})
        endif()
    endif()

    # What connect() does with a key it has already taken custody of when a
    # later step fails. Unlike every other Paho suite this one does NOT link
    # az_iot_adapter_paho: it compiles az_iot_mqtt_paho.c against a mock
    # MQTTAsync and a fake custody module, because the failures it asserts on
    # (a Paho call rejecting the request, an allocation failing) cannot be
    # produced through the real library, and a successful custody prepare()
    # needs a PKCS#11 token. See tests/support/mock_paho_async.h.
    az_iot_add_cmocka_test(az_iot_tests_paho_connect_cleanup
        unit/paho_connect_cleanup_test.c
        support/mock_paho_async.c
        support/fake_paho_key_custody.c
        ${CMAKE_SOURCE_DIR}/adapters/paho/az_iot_mqtt_paho.c)
    target_include_directories(az_iot_tests_paho_connect_cleanup PRIVATE
        ${CMAKE_SOURCE_DIR}/adapters/paho
        ${CMAKE_CURRENT_SOURCE_DIR}/support)
    if(AZ_IOT_PAHO_INCLUDE_DIRS)
        target_include_directories(az_iot_tests_paho_connect_cleanup SYSTEM PRIVATE
            ${AZ_IOT_PAHO_INCLUDE_DIRS})
    endif()
    if(_paho_defs)
        target_compile_definitions(az_iot_tests_paho_connect_cleanup PRIVATE ${_paho_defs})
    endif()
    if(WIN32)
        target_link_libraries(az_iot_tests_paho_connect_cleanup PRIVATE ws2_32 crypt32)
    endif()
    if(UNIX)
        target_link_libraries(az_iot_tests_paho_connect_cleanup PRIVATE pthread)
    endif()
    # --wrap is a GNU ld / lld feature. Without it the allocation-failure case
    # is simply not registered; every other case still runs.
    if(UNIX AND NOT APPLE)
        target_link_options(az_iot_tests_paho_connect_cleanup PRIVATE -Wl,--wrap=malloc)
        target_compile_definitions(az_iot_tests_paho_connect_cleanup PRIVATE
            AZ_IOT_TEST_WRAP_MALLOC=1)
    endif()
    # Deliberately instrumented, unlike every other test target: the source
    # being measured here is a SHIPPED one (adapters/paho/az_iot_mqtt_paho.c),
    # and these are the only tests that reach its cleanup paths. gcovr excludes
    # tests/, so the doubles compiled alongside it stay out of the report, and
    # gcov merges this object's counters with the library's.
    az_iot_apply_coverage(az_iot_tests_paho_connect_cleanup)

    # Transport selection (TCP vs WebSockets) and the HTTP proxy, asserted on
    # the MQTTAsync options the adapter builds. Same construction as the
    # cleanup suite above and for the same reason: the real library would only
    # answer these questions by opening a socket to the broker.
    az_iot_add_cmocka_test(az_iot_tests_paho_transport_proxy
        unit/paho_transport_proxy_test.c
        support/mock_paho_async.c
        support/fake_paho_key_custody.c
        ${CMAKE_SOURCE_DIR}/adapters/paho/az_iot_mqtt_paho.c)
    target_include_directories(az_iot_tests_paho_transport_proxy PRIVATE
        ${CMAKE_SOURCE_DIR}/adapters/paho
        ${CMAKE_CURRENT_SOURCE_DIR}/support)
    if(AZ_IOT_PAHO_INCLUDE_DIRS)
        target_include_directories(az_iot_tests_paho_transport_proxy SYSTEM PRIVATE
            ${AZ_IOT_PAHO_INCLUDE_DIRS})
    endif()
    if(_paho_defs)
        target_compile_definitions(az_iot_tests_paho_transport_proxy PRIVATE ${_paho_defs})
    endif()
    if(WIN32)
        target_link_libraries(az_iot_tests_paho_transport_proxy PRIVATE ws2_32 crypt32)
    endif()
    if(UNIX)
        target_link_libraries(az_iot_tests_paho_transport_proxy PRIVATE pthread)
        # setenv() is POSIX, not C99, and these targets build with -std=c99
        # -Werror -- so without this the call is an implicit declaration and the
        # build fails rather than warning. Same reason as az_iot_tests_connection_dps.
        target_compile_definitions(az_iot_tests_paho_transport_proxy PRIVATE
            _POSIX_C_SOURCE=200809L)
    endif()
    az_iot_apply_coverage(az_iot_tests_paho_transport_proxy)
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

# The suite's own exit-code contract: a declared capability that was never
# exercised must fail the run. No pipeline can catch a regression in it, since
# every job either supplies a token or sets the opt-out, so it is tested here.
az_iot_add_cmocka_test(
    az_iot_tests_conformance_capability unit/conformance_capability_test.c)
target_link_libraries(az_iot_tests_conformance_capability PRIVATE az_iot_conformance)

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
    # The harness declares AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI only in a build
    # where the adapter actually honours a key reference, so it needs the same
    # AZ_IOT_PAHO_KEY_CUSTODY definition the adapter was compiled with.
    if(_paho_defs)
        target_compile_definitions(az_iot_conformance_paho_v3 PRIVATE ${_paho_defs})
    endif()

    add_executable(az_iot_conformance_paho_v5 conformance/paho_v5_main.c)
    target_link_libraries(az_iot_conformance_paho_v5 PRIVATE
        az_iot_conformance az_iot_adapter_paho)
    az_iot_apply_warnings(az_iot_conformance_paho_v5)
    if(_paho_defs)
        target_compile_definitions(az_iot_conformance_paho_v5 PRIVATE ${_paho_defs})
    endif()

    if(AZ_IOT_BUILD_CONFORMANCE_TESTS)
        add_test(NAME az_iot_conformance_paho_v3 COMMAND az_iot_conformance_paho_v3)
        add_test(NAME az_iot_conformance_paho_v5 COMMAND az_iot_conformance_paho_v5)
    endif()

    # Negative control for the sign-route custody case. No adapter here
    # implements that route, so the positive case never runs in CI and could be
    # neutered without any pipeline noticing. This declares the capability
    # against an adapter that must refuse it and requires the suite to fail on
    # that case and no other. Needs the TLS cases (the custody case lives there)
    # and OpenSSL, for the throwaway certificate it generates.
    if(AZ_IOT_BUILD_CONFORMANCE_TESTS_TLS)
        find_package(OpenSSL 3.0 REQUIRED)
        add_executable(az_iot_conformance_paho_sign_negative
            conformance/paho_sign_negative_main.c)
        target_link_libraries(az_iot_conformance_paho_sign_negative PRIVATE
            az_iot_conformance az_iot_adapter_paho OpenSSL::SSL OpenSSL::Crypto)
        az_iot_apply_warnings(az_iot_conformance_paho_sign_negative)
        if(UNIX)
            # dup/dup2/fileno/getpid are POSIX: under -std=c99 they are not
            # declared without this, and an implicit declaration is an error
            # here. Same macros az_iot_conformance itself is built with.
            target_compile_definitions(az_iot_conformance_paho_sign_negative PRIVATE
                _POSIX_C_SOURCE=200809L
                _DEFAULT_SOURCE)
        endif()
        if(_paho_defs)
            target_compile_definitions(az_iot_conformance_paho_sign_negative PRIVATE ${_paho_defs})
        endif()
        if(AZ_IOT_BUILD_CONFORMANCE_TESTS)
            add_test(NAME az_iot_conformance_paho_sign_negative
                COMMAND az_iot_conformance_paho_sign_negative)
        endif()
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
        az_iot_gen1 az_iot_adapter_paho az_iot_test_proxy az_iot_test_mqtt_server)
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
