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
# Test-only: keep cmocka's install rules out of the SDK's install.
set_property(DIRECTORY "${cmocka_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)

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
    support/test_provider.c
)
target_include_directories(az_iot_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(az_iot_test_support PUBLIC az_iot_core)
az_iot_apply_warnings(az_iot_test_support)

az_iot_add_cmocka_test(az_iot_tests_smoke unit/smoke_test.c)
target_include_directories(az_iot_tests_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_mqtt_iface_contract unit/mqtt_iface_contract_test.c)
target_link_libraries(az_iot_tests_mqtt_iface_contract PRIVATE az_iot_test_support)
az_iot_add_cmocka_test(az_iot_tests_connection_client unit/connection_client_test.c)
target_link_libraries(az_iot_tests_connection_client PRIVATE az_iot_test_support)
# connection_client_test pulls internal headers (retry_policy.h, connection_client_internal.h)
# to drive RNG seeding and time helpers deterministically.
target_include_directories(az_iot_tests_connection_client PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_connection_lifecycle unit/connection_lifecycle_test.c)
target_link_libraries(az_iot_tests_connection_lifecycle PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_connection_lifecycle PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_connection_reconnect unit/connection_reconnect_test.c)
target_link_libraries(az_iot_tests_connection_reconnect PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_connection_reconnect PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
# Recovery after the hub refuses the identity (opts.identity_recovery).
az_iot_add_cmocka_test(az_iot_tests_connection_identity_recovery
    unit/connection_identity_recovery_test.c)
target_link_libraries(az_iot_tests_connection_identity_recovery PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_connection_identity_recovery PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_env unit/env_test.c)
target_include_directories(az_iot_tests_env PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
if(UNIX)
    # setenv()/unsetenv() are POSIX, not C99.
    target_compile_definitions(az_iot_tests_env PRIVATE _POSIX_C_SOURCE=200809L)
endif()
az_iot_add_cmocka_test(az_iot_tests_connection_dps unit/connection_dps_test.c)
target_link_libraries(az_iot_tests_connection_dps PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_connection_dps PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
if(UNIX)
    target_compile_definitions(az_iot_tests_connection_dps PRIVATE _POSIX_C_SOURCE=200809L)
endif()
# Custom DPS registration payload: the request body it produces (alone and
# alongside a CSR) and the payload the assignment returns.
az_iot_add_cmocka_test(az_iot_tests_connection_dps_payload unit/connection_dps_payload_test.c)
target_link_libraries(az_iot_tests_connection_dps_payload PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_connection_dps_payload PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_retry_policy unit/retry_policy_test.c)
target_include_directories(az_iot_tests_retry_policy PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_span_writer unit/span_writer_test.c)
target_include_directories(az_iot_tests_span_writer PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_json_string unit/json_string_test.c)
target_include_directories(az_iot_tests_json_string PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_log unit/log_test.c)
# Diagnostic surface: the *_to_string helpers, version string, default
# reconnection policy and the built-in stderr sink. Pulls internal headers for
# the role/flavor helpers and the CSR request-id generator, which are internal
# but still shipped code that nothing was exercising.
az_iot_add_cmocka_test(az_iot_tests_diagnostics unit/diagnostics_test.c)
target_include_directories(az_iot_tests_diagnostics PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(az_iot_tests_dispatch unit/dispatch_test.c)
target_include_directories(az_iot_tests_dispatch PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
az_iot_add_cmocka_test(
    az_iot_tests_mqttv3_telemetry_client unit/mqttv3_telemetry_client_test.c)
target_link_libraries(
    az_iot_tests_mqttv3_telemetry_client PRIVATE az_iot_test_support az_iot_mqttv3)
az_iot_add_cmocka_test(
    az_iot_tests_mqttv5_telemetry_client unit/mqttv5_telemetry_client_test.c)
target_link_libraries(
    az_iot_tests_mqttv5_telemetry_client PRIVATE az_iot_test_support az_iot_mqttv5)
az_iot_add_cmocka_test(az_iot_tests_mqttv3_c2d_client unit/mqttv3_c2d_client_test.c)
target_link_libraries(az_iot_tests_mqttv3_c2d_client PRIVATE az_iot_test_support az_iot_mqttv3)
az_iot_add_cmocka_test(az_iot_tests_mqttv3_direct_method_client unit/mqttv3_direct_method_client_test.c)
target_link_libraries(az_iot_tests_mqttv3_direct_method_client PRIVATE az_iot_test_support az_iot_mqttv3)
if(UNIX)
    # The slot-reclaim deadline is only observable once a real second has
    # passed, so the suite calls nanosleep().
    target_compile_definitions(az_iot_tests_mqttv3_direct_method_client PRIVATE
        _POSIX_C_SOURCE=200809L)
endif()
# AZ_IOT_DM_RID_MAX is a documented compile-time knob, and it sizes both a
# public struct and the response topic the client builds from it -- so the
# library and the application have to be built against the same value, hence
# compiling the mqttv3 source into this target rather than linking az_iot_mqttv3,
# which carries the default. Running the whole suite against a raised rid bound
# is what proves the knob reaches every buffer derived from it.
az_iot_add_cmocka_test(az_iot_tests_mqttv3_direct_method_client_tuned
    unit/mqttv3_direct_method_client_test.c
    ${PROJECT_SOURCE_DIR}/src/mqttv3/direct_method_client.c)
target_link_libraries(az_iot_tests_mqttv3_direct_method_client_tuned PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_mqttv3_direct_method_client_tuned PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/mqttv3)
target_compile_definitions(az_iot_tests_mqttv3_direct_method_client_tuned PRIVATE
    AZ_IOT_DM_RID_MAX=200)
if(UNIX)
    target_compile_definitions(az_iot_tests_mqttv3_direct_method_client_tuned PRIVATE
        _POSIX_C_SOURCE=200809L)
endif()
az_iot_add_cmocka_test(az_iot_tests_mqttv5_direct_method_client unit/mqttv5_direct_method_client_test.c)
target_link_libraries(az_iot_tests_mqttv5_direct_method_client PRIVATE az_iot_test_support az_iot_mqttv5)
target_include_directories(az_iot_tests_mqttv5_direct_method_client PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core)
if(UNIX)
    # Two MQTTv5 direct-method budgets are only observable once a real second has
    # passed, so the suite calls nanosleep().
    target_compile_definitions(az_iot_tests_mqttv5_direct_method_client PRIVATE
        _POSIX_C_SOURCE=200809L)
endif()
# The MQTTv5 design requires the concurrency limit to be configurable. It sizes a
# public struct, so the library and the application must be built against the
# same value -- hence the mqttv5 sources are compiled into this target rather than
# linked from az_iot_mqttv5, which carries the default. Running the whole suite
# against an overridden limit proves the knob reaches every pool, loop and
# admission check, and catches anyone reintroducing a hardcoded bound.
az_iot_add_cmocka_test(az_iot_tests_mqttv5_direct_method_client_tuned
    unit/mqttv5_direct_method_client_test.c
    ${PROJECT_SOURCE_DIR}/src/mqttv5/direct_method_client.c
    ${PROJECT_SOURCE_DIR}/src/mqttv5/direct_method_codec.c)
target_link_libraries(az_iot_tests_mqttv5_direct_method_client_tuned PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_mqttv5_direct_method_client_tuned PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/mqttv5)
target_compile_definitions(az_iot_tests_mqttv5_direct_method_client_tuned PRIVATE
    AZ_IOT_MQTTV5_DM_MAX_CONCURRENT=2)
if(UNIX)
    target_compile_definitions(az_iot_tests_mqttv5_direct_method_client_tuned PRIVATE
        _POSIX_C_SOURCE=200809L)
endif()
# The MQTTv5 direct-method protobuf codec is hand-rolled, so its wire format is
# pinned here against bytes taken from the .proto rather than from itself.
az_iot_add_cmocka_test(
    az_iot_tests_mqttv5_direct_method_codec unit/mqttv5_direct_method_codec_test.c)
target_link_libraries(az_iot_tests_mqttv5_direct_method_codec PRIVATE az_iot_test_support az_iot_mqttv5)
target_include_directories(az_iot_tests_mqttv5_direct_method_codec PRIVATE
    ${PROJECT_SOURCE_DIR}/src/mqttv5)
# The shared proto3 codec, pinned against bytes taken from the protobuf spec
# rather than from itself. The twin and presence clients only exercise the paths
# their own protocols take, so the boundaries are covered here.
az_iot_add_cmocka_test(az_iot_tests_proto3 unit/proto3_test.c)
target_link_libraries(az_iot_tests_proto3 PRIVATE az_iot_test_support az_iot_core)
target_include_directories(az_iot_tests_proto3 PRIVATE ${PROJECT_SOURCE_DIR}/src/core)

az_iot_add_cmocka_test(az_iot_tests_mqttv3_twin_client unit/mqttv3_twin_client_test.c)
target_link_libraries(az_iot_tests_mqttv3_twin_client PRIVATE az_iot_test_support az_iot_mqttv3)
az_iot_add_cmocka_test(az_iot_tests_mqttv5_twin_client unit/mqttv5_twin_client_test.c)
target_link_libraries(az_iot_tests_mqttv5_twin_client PRIVATE az_iot_test_support az_iot_mqttv5)
target_include_directories(az_iot_tests_mqttv5_twin_client PRIVATE ${PROJECT_SOURCE_DIR}/src/core)

az_iot_add_cmocka_test(az_iot_tests_su_client unit/su_client_test.c)
az_iot_add_cmocka_test(az_iot_tests_su_protocol unit/su_protocol_test.c)
target_link_libraries(az_iot_tests_su_client PRIVATE az_iot_test_support)
# For az_iot_time_mono_ms: the request deadline is a monotonic instant, so a
# test that exercises it has to speak the same clock.
target_include_directories(az_iot_tests_su_client PRIVATE ${PROJECT_SOURCE_DIR}/src/core)

# The channel over a real connection client: what reaches the broker, how a
# response is correlated back, and the paths that can fault provisioning.
az_iot_add_cmocka_test(az_iot_tests_su_channel_dps unit/su_channel_dps_test.c)
target_link_libraries(az_iot_tests_su_channel_dps PRIVATE az_iot_test_support)
target_include_directories(az_iot_tests_su_channel_dps PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/features/su/internal)

# Bound precondition hangs while leaving headroom for Valgrind instrumentation.
set_tests_properties(
    az_iot_tests_su_client az_iot_tests_su_protocol
    az_iot_tests_su_channel_dps
    PROPERTIES TIMEOUT 300)

# File upload is HTTPS-only (stateless request-build/response-parse helpers), so
# these tests need neither the mqtt mock nor internal headers. mqttv5 is linked
# only so the profile-pin test can show a released pin admits the other
# generation.
az_iot_add_cmocka_test(az_iot_tests_mqttv3_file_upload unit/mqttv3_file_upload_client_test.c)
target_link_libraries(az_iot_tests_mqttv3_file_upload PRIVATE az_iot_mqttv3 az_iot_mqttv5)

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
        ${PROJECT_SOURCE_DIR}/adapters/paho)
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
            ${PROJECT_SOURCE_DIR}/adapters/paho)
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
        ${PROJECT_SOURCE_DIR}/adapters/paho/az_iot_mqtt_paho.c)
    target_include_directories(az_iot_tests_paho_connect_cleanup PRIVATE
        ${PROJECT_SOURCE_DIR}/adapters/paho
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
        ${PROJECT_SOURCE_DIR}/adapters/paho/az_iot_mqtt_paho.c)
    target_include_directories(az_iot_tests_paho_transport_proxy PRIVATE
        ${PROJECT_SOURCE_DIR}/adapters/paho
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

    # Data races in the adapter's event queue: the producer/consumer seam
    # between Paho's callback threads and the application's process_loop().
    #
    # Same construction again -- the mock lets the test BE the Paho thread, so
    # the only concurrency in the binary is the concurrency under test. It
    # runs in the ordinary suite, and CI additionally runs this one exe under
    # helgrind, which is what actually adjudicates happens-before; memcheck
    # and ASan cannot see a race at all. See the race-detector job in
    # .github/workflows/ci-c.yml.
    az_iot_add_cmocka_test(az_iot_tests_paho_event_queue_race
        unit/paho_event_queue_race_test.c
        support/mock_paho_async.c
        support/fake_paho_key_custody.c
        ${PROJECT_SOURCE_DIR}/adapters/paho/az_iot_mqtt_paho.c)
    target_include_directories(az_iot_tests_paho_event_queue_race PRIVATE
        ${PROJECT_SOURCE_DIR}/adapters/paho
        ${CMAKE_CURRENT_SOURCE_DIR}/support)
    if(AZ_IOT_PAHO_INCLUDE_DIRS)
        target_include_directories(az_iot_tests_paho_event_queue_race SYSTEM PRIVATE
            ${AZ_IOT_PAHO_INCLUDE_DIRS})
    endif()
    if(_paho_defs)
        target_compile_definitions(az_iot_tests_paho_event_queue_race PRIVATE ${_paho_defs})
    endif()
    if(WIN32)
        target_link_libraries(az_iot_tests_paho_event_queue_race PRIVATE ws2_32 crypt32)
    endif()
    if(UNIX)
        target_link_libraries(az_iot_tests_paho_event_queue_race PRIVATE pthread)
    endif()
    az_iot_apply_coverage(az_iot_tests_paho_event_queue_race)
endif()

if(AZ_IOT_WITH_RUST_MQTT)
    az_iot_add_cmocka_test(az_iot_tests_rust_mqtt_adapter unit/rust_mqtt_adapter_test.c)
    target_link_libraries(az_iot_tests_rust_mqtt_adapter PRIVATE az_iot_adapter_rust_mqtt)
endif()

# Crypto backends: the shared known-answer contract suite
# (support/crypto_contract.c), once per backend target that was built. It also
# drives the SDK's composed SHA-256 and HMAC-SHA256 (src/core/crypto.c).
foreach(_backend openssl mbedtls)
    if(TARGET az_iot_crypto_${_backend})
        az_iot_add_cmocka_test(az_iot_tests_crypto_${_backend}
            unit/crypto_${_backend}_test.c support/crypto_contract.c)
        target_link_libraries(az_iot_tests_crypto_${_backend} PRIVATE
            az_iot_crypto_${_backend} az_iot_test_support)
        target_include_directories(az_iot_tests_crypto_${_backend} PRIVATE
            ${PROJECT_SOURCE_DIR}/src/core)

        # SAS authentication over this backend.
        az_iot_add_cmocka_test(az_iot_tests_connection_sas_${_backend} unit/connection_sas_test.c)
        target_link_libraries(az_iot_tests_connection_sas_${_backend} PRIVATE
            az_iot_crypto_${_backend} az_iot_test_support)
        target_include_directories(az_iot_tests_connection_sas_${_backend} PRIVATE
            ${PROJECT_SOURCE_DIR}/src/core)
        string(TOUPPER ${_backend} _backend_upper)
        target_compile_definitions(az_iot_tests_connection_sas_${_backend} PRIVATE
            AZ_IOT_TEST_CRYPTO_${_backend_upper})
    endif()
endforeach()

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

if(AZ_IOT_WITH_AZ_MQTT)
    az_iot_add_cmocka_test(az_iot_tests_az_mqtt_adapter unit/az_mqtt_adapter_test.c)
    target_link_libraries(az_iot_tests_az_mqtt_adapter PRIVATE az_iot_adapter_az_mqtt)
    target_compile_definitions(az_iot_tests_az_mqtt_adapter PRIVATE
        AZ_IOT_AZ_MQTT_BUFFER_SIZE=${AZ_IOT_AZ_MQTT_BUFFER_SIZE})
    # Needs a broker, as the conformance suites: registered under the same option.
    az_iot_add_cmocka_test(az_iot_az_mqtt_adapter_broker
        integration/az_mqtt_adapter_broker_test.c NO_CTEST)
    target_link_libraries(az_iot_az_mqtt_adapter_broker PRIVATE az_iot_adapter_az_mqtt)
    target_compile_definitions(az_iot_az_mqtt_adapter_broker PRIVATE
        AZ_IOT_AZ_MQTT_BUFFER_SIZE=${AZ_IOT_AZ_MQTT_BUFFER_SIZE})
    if(AZ_IOT_BUILD_CONFORMANCE_TESTS)
        add_test(NAME az_iot_az_mqtt_adapter_broker COMMAND az_iot_az_mqtt_adapter_broker)
    endif()

    get_target_property(_az_mqtt_adapter_defs az_iot_adapter_az_mqtt COMPILE_DEFINITIONS)
    foreach(_v 3 5)
        set(_t az_iot_conformance_az_mqtt_v${_v})
        add_executable(${_t} conformance/az_mqtt_main.c)
        target_link_libraries(${_t} PRIVATE az_iot_conformance az_iot_adapter_az_mqtt)
        target_compile_definitions(${_t} PRIVATE AZ_IOT_CONFORMANCE_AZ_MQTT_V=${_v})
        # The key-reference capability is declared only where the adapter honours it.
        if("AZ_IOT_AZ_MQTT_OPENSSL" IN_LIST _az_mqtt_adapter_defs)
            target_compile_definitions(${_t} PRIVATE AZ_IOT_AZ_MQTT_OPENSSL)
        endif()
        az_iot_apply_warnings(${_t})
        if(AZ_IOT_BUILD_CONFORMANCE_TESTS)
            add_test(NAME ${_t} COMMAND ${_t})
        endif()
    endforeach()
    unset(_t)
    unset(_v)
    unset(_az_mqtt_adapter_defs)
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
        az_iot_mqttv3 az_iot_adapter_paho az_iot_test_proxy az_iot_test_mqtt_server az_iot_test_support)
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
