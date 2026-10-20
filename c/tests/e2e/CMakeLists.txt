# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ---------------------------------------------------------------------------
# Vendored, test-only AMQP 1.0 client (az_amqp + reference transport). Used by
# the in-process service client below to receive device telemetry (Event
# Hub-compatible endpoint) and send C2D over the IoT Hub AMQP service endpoint.
# ---------------------------------------------------------------------------
# Shared test helpers (support/) are included as "support/<name>.h" from every
# suite here, so the tests root goes on the include path once for the directory
# rather than per target.
include_directories(${PROJECT_SOURCE_DIR}/tests)

# Each suite that connects is built once per MQTT adapter in the build: <suite>
# over Paho, <suite>_az_mqtt over az_mqtt. Its sources select the adapter with
# AZ_IOT_E2E_ADAPTER_PAHO or AZ_IOT_E2E_ADAPTER_AZ_MQTT. Every name contains
# `e2e`, so the workflows' `ctest -R` filters run both.
set(AZ_IOT_E2E_ADAPTERS)
foreach(_adapter paho az_mqtt)
    if(TARGET az_iot_adapter_${_adapter})
        list(APPEND AZ_IOT_E2E_ADAPTERS ${_adapter})
    endif()
endforeach()
unset(_adapter)
# Key references (the custody suites): az_mqtt honours them only over OpenSSL TLS.
set(AZ_IOT_E2E_KEY_REF_ADAPTERS ${AZ_IOT_E2E_ADAPTERS})
if(TARGET az_iot_adapter_az_mqtt)
    get_target_property(_defs az_iot_adapter_az_mqtt COMPILE_DEFINITIONS)
    if(NOT "AZ_IOT_AZ_MQTT_OPENSSL" IN_LIST _defs)
        list(REMOVE_ITEM AZ_IOT_E2E_KEY_REF_ADAPTERS az_mqtt)
    endif()
    unset(_defs)
endif()

# az_iot_add_e2e_suite(<name> [NO_CTEST] [ADAPTERS <adapter...>] SOURCES <src...> LIBS <lib...>)
# One cmocka suite per adapter in AZ_IOT_E2E_ADAPTERS (ADAPTERS narrows it), linked to it.
function(az_iot_add_e2e_suite name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NO_CTEST" "" "ADAPTERS;SOURCES;LIBS")
    set(adapters ${AZ_IOT_E2E_ADAPTERS})
    if("ADAPTERS" IN_LIST arg_KEYWORDS_MISSING_VALUES)
        return() # Narrowed to no adapter.
    elseif(arg_ADAPTERS)
        string(REPLACE ";" "|" re "${arg_ADAPTERS}")
        list(FILTER adapters INCLUDE REGEX "^(${re})$")
    endif()
    set(no_ctest "")
    if(arg_NO_CTEST)
        set(no_ctest NO_CTEST)
    endif()
    foreach(adapter IN LISTS adapters)
        string(TOUPPER "${adapter}" ADAPTER)
        set(t ${name})
        if(NOT adapter STREQUAL "paho")
            set(t ${name}_${adapter})
        endif()
        az_iot_add_cmocka_test(${t} ${no_ctest} ${arg_SOURCES})
        target_compile_definitions(${t} PRIVATE AZ_IOT_E2E_ADAPTER_${ADAPTER})
        target_link_libraries(${t} PRIVATE ${arg_LIBS} az_iot_adapter_${adapter})
    endforeach()
endfunction()

# The software updates e2e suites. Both use the shipping MQTT adapters and X.509
# against the real service; see docs/eng/end-to-end-tests.md.
#
#   az_iot_tests_e2e_su        the DPS channel, with no update offered
#   az_iot_tests_e2e_su_offer  offered updates through the whole client:
#                              real crypto, real download (libcurl)
#
# Requires AZ_IOT_BUILD_E2E as well; this option only
# selects the suites once the e2e directory is being built at all. Every
# dependency is REQUIRED: a suite that cannot be built fails the configure
# rather than dropping out of the run.
#
# Not built on Windows: the suites read their configuration with the borrowing
# az_iot_test_env(), which cannot exist under MSVC (getenv is deprecated there
# and these targets build with /WX), and nothing runs them on Windows.
if(AZ_IOT_BUILD_E2E_SU AND WIN32)
    message(STATUS "Software updates e2e suites skipped on Windows.")
endif()

if(AZ_IOT_BUILD_E2E_SU AND NOT WIN32)
    az_iot_add_e2e_suite(az_iot_tests_e2e_su
        SOURCES tests/e2e_su_test.c tests/e2e_su_env.c
        LIBS az_iot_core)

    if(NOT TARGET az_iot_crypto_openssl)
        message(FATAL_ERROR
            "AZ_IOT_BUILD_E2E_SU needs the OpenSSL crypto backend "
            "(AZ_IOT_WITH_CRYPTO_OPENSSL=ON).")
    endif()
    find_package(CURL REQUIRED)
    az_iot_add_e2e_suite(az_iot_tests_e2e_su_offer
        SOURCES tests/e2e_su_offer_test.c tests/e2e_su_env.c tests/e2e_su_test_roots.c
        LIBS az_iot_core az_iot_crypto_openssl CURL::libcurl)
    # The suites assert on the channel and the codecs, and the offer suite's spy
    # wraps the channel: all internal.
    foreach(_t az_iot_tests_e2e_su az_iot_tests_e2e_su_az_mqtt
               az_iot_tests_e2e_su_offer az_iot_tests_e2e_su_offer_az_mqtt)
        if(TARGET ${_t})
            target_include_directories(${_t} PRIVATE
                ${PROJECT_SOURCE_DIR}/src/core ${PROJECT_SOURCE_DIR}/src/features/su/internal)
        endif()
    endforeach()
else()
    message(STATUS "Software updates e2e suites disabled (set AZ_IOT_BUILD_E2E_SU=ON).")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/amqp
                 ${CMAKE_CURRENT_BINARY_DIR}/deps/amqp)

# Receiver credit accounting of the vendored AMQP client, over an in-memory
# peer. Needs no Azure resources; named `e2e` so the e2e job's filter runs it.
az_iot_add_cmocka_test(az_iot_tests_e2e_amqp_link tests/e2e_amqp_link_test.c)
target_link_libraries(az_iot_tests_e2e_amqp_link PRIVATE az_amqp)

# Notification enqueuedTimeUtc parsing (the watcher deletes what it parses as
# stale). Pure parsing; named `e2e` so the e2e job's filter runs it.
az_iot_add_cmocka_test(az_iot_tests_e2e_notify_time
    tests/e2e_notify_time_test.c service/e2e_notify_time.c)
target_include_directories(az_iot_tests_e2e_notify_time PRIVATE service)

# Response header parsing of the e2e HTTPS client (status, Content-Length,
# Retry-After). Pure parsing; named `e2e` so the e2e job's filter runs it.
az_iot_add_cmocka_test(az_iot_tests_e2e_http_headers tests/e2e_http_headers_test.c)
target_include_directories(az_iot_tests_e2e_http_headers PRIVATE service)
target_link_libraries(az_iot_tests_e2e_http_headers PRIVATE
    az_iot_e2e_service az_amqp az_amqp_transport)

# ---------------------------------------------------------------------------
# Service-side facade (hides az_amqp behind a plain-C API used by the tests).
# ---------------------------------------------------------------------------
add_subdirectory(service)

# ---------------------------------------------------------------------------
# az_iot_tests_e2e
#
# The in-process end-to-end suite: ONE cmocka executable that plays both the
# device (shipping SDK over an MQTT adapter) and the cloud (the
# az_iot_e2e_service facade) halves of every scenario against a real Azure IoT
# Hub / DPS instance. Registered with CTest so the e2e job can run it with
# `ctest -R e2e`. The test includes ONLY the service facade's public header, so
# no AMQP type ever leaks into the test translation unit.
#
# It requires provisioning + secrets in the environment (device X.509 material
# and hub/eventhub connection strings) supplied by the e2e CI job.
# ---------------------------------------------------------------------------
az_iot_add_e2e_suite(az_iot_tests_e2e
    SOURCES tests/e2e_scenarios_test.c tests/e2e_device.c
    LIBS az_iot_e2e_service az_iot_mqttv3)

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_csr
#
# CSR-based DPS enrollment (issued operational cert). Requires a DPS enrollment
# linked to a signing CA, which only the ci-c-e2e-csr workflow provisions, so it
# is built only when AZ_IOT_BUILD_E2E_CSR is set -- and when the OpenSSL
# "managed" certificate provider is available.
#
# It used to be built everywhere and skip itself at run time. That meant the
# main e2e leg paid to compile and launch a test that could never do anything,
# and a CTest "Skipped" line was the only evidence it was not running.
# ---------------------------------------------------------------------------
if(AZ_IOT_BUILD_E2E_CSR)
    if(NOT TARGET az_iot_certificate_provider_managed)
        message(FATAL_ERROR
            "AZ_IOT_BUILD_E2E_CSR=ON requires the OpenSSL managed certificate provider "
            "(AZ_IOT_WITH_CERT_PROVIDER_MANAGED=ON).")
    endif()
    az_iot_add_e2e_suite(az_iot_tests_e2e_csr
        SOURCES tests/e2e_csr_test.c
        LIBS az_iot_mqttv3 az_iot_certificate_provider_managed)
endif()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_sas, az_iot_tests_e2e_csr_sas
#
# SAS from a DPS symmetric-key enrollment group: DPS and hub over SAS; and,
# with AZ_IOT_BUILD_E2E_CSR (group linked to a signing CA), SAS onboarding with
# a DPS-issued certificate for the hub. Every e2e environment provides the
# group, so both are built with the suites they sit beside.
# ---------------------------------------------------------------------------
if(NOT TARGET az_iot_crypto_openssl)
    message(FATAL_ERROR
        "AZ_IOT_BUILD_E2E=ON requires the OpenSSL crypto backend (az_iot_crypto_openssl) "
        "for the SAS e2e suites.")
endif()
# OpenSSL::Crypto: the user-provided token test signs its own tokens.
az_iot_add_e2e_suite(az_iot_tests_e2e_sas
    SOURCES tests/e2e_sas_test.c tests/e2e_sas_device.c
    LIBS az_iot_mqttv3 az_iot_crypto_openssl OpenSSL::Crypto)
if(AZ_IOT_BUILD_E2E_CSR)
    az_iot_add_e2e_suite(az_iot_tests_e2e_csr_sas
        SOURCES tests/e2e_csr_sas_test.c tests/e2e_sas_device.c
        LIBS az_iot_mqttv3 az_iot_crypto_openssl az_iot_certificate_provider_managed)
endif()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_tpm
#
# DPS TPM attestation with a TPM simulated in software (samples/common): an
# individual enrollment for a new EK (DPS service API), registration over
# HTTPS, then the assigned mqttv3 hub with TPM-signed SAS tokens. Needs
# IOT_DPS_CONNECTION_STRING to create the enrollment.
# ---------------------------------------------------------------------------
set(_e2e_sample_common ${PROJECT_SOURCE_DIR}/samples/common)
az_iot_add_e2e_suite(az_iot_tests_e2e_tpm
    SOURCES tests/e2e_tpm_test.c
            ${_e2e_sample_common}/sample_tpm_sim.c
            ${_e2e_sample_common}/sample_dps_tpm.c
            ${_e2e_sample_common}/sample_utils.c
    LIBS az_iot_e2e_service az_iot_mqttv3 az_iot_crypto_openssl OpenSSL::Crypto)
foreach(_t az_iot_tests_e2e_tpm az_iot_tests_e2e_tpm_az_mqtt)
    if(TARGET ${_t})
        target_include_directories(${_t} PRIVATE ${_e2e_sample_common} service)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_fileupload
#
# MQTTv3 file-upload device flow against a real hub that has an Azure Storage
# account associated for file upload. It plays both halves like the other
# suites: the device requests a SAS URI, PUTs the blob, reads it back and reports
# completion, while the service side watches the hub's file-upload NOTIFICATION
# endpoint -- the only cloud-side proof that a round trip completed. It also
# covers the failure report (accepted, but never notified), a completion carrying
# an unknown correlation id, and local argument rejection.
#
# The MQTTv3 HTTP hook and the Storage PUT/GET go through the e2e service lib's
# HTTPS helper (the harness az_amqp TLS transport), so there is no external HTTP
# dependency. It requires the standard e2e device env, the service connection
# strings, and a file-upload-capable hub. The `e2e` name matches the fast job's
# `ctest -R e2e` filter.
#
# Runs on the Linux e2e legs. On Windows the harness's Schannel transport does
# not present the self-signed device certificate on the CI runners, so it is not
# registered as a test there -- but the executable is still built, so MSVC keeps
# compiling it. The file-upload sample also builds and runs natively on Windows.
# ---------------------------------------------------------------------------
set(_e2e_fileupload_ctest "")
if(WIN32)
    set(_e2e_fileupload_ctest NO_CTEST)
endif()
az_iot_add_e2e_suite(az_iot_tests_e2e_fileupload ${_e2e_fileupload_ctest}
    SOURCES tests/e2e_fileupload_test.c tests/e2e_device.c
    LIBS az_iot_e2e_service az_iot_mqttv3)

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_transport
#
# The egress transports: MQTT over WebSockets, and MQTT tunnelled through an
# HTTP CONNECT proxy. Each scenario connects a device with one transport
# configuration and round-trips a uniquely marked telemetry message to the
# hub's Event Hub-compatible endpoint, so a tunnel that connects but carries
# nothing fails. It also asserts that an unreachable proxy leaves the device
# disconnected instead of silently connecting around it.
#
# The proxy scenarios skip themselves when AZ_IOT_PROXY_HOST is unset, so the
# suite is useful on a CI leg with no proxy (the WebSocket scenario still runs)
# and complete on one that provisions a proxy. The `e2e` name matches the fast
# job's `ctest -R e2e` filter.
# ---------------------------------------------------------------------------
# setenv()/unsetenv() are POSIX, not C99, and these targets build -std=c99:
# without _POSIX_C_SOURCE the calls are implicit declarations and the build fails
# rather than warning. Same reason as az_iot_tests_paho_transport_proxy.
set(_e2e_posix "")
if(UNIX)
    set(_e2e_posix _POSIX_C_SOURCE=200809L)
endif()
az_iot_add_e2e_suite(az_iot_tests_e2e_transport
    SOURCES tests/e2e_transport_test.c tests/e2e_device.c
    LIBS az_iot_e2e_service az_iot_mqttv3)
foreach(_t az_iot_tests_e2e_transport az_iot_tests_e2e_transport_az_mqtt)
    if(TARGET ${_t} AND _e2e_posix)
        target_compile_definitions(${_t} PRIVATE ${_e2e_posix})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_custody
#
# The "Storage / custody" group of docs/eng/certificate-management.md: DPS
# issuance and hub traffic performed by a device whose private key is inside a
# PKCS#11 token and cannot be read by the process. Nothing is stubbed -- if the
# token were not signing the TLS handshake the hub would refuse the connection,
# so reaching CONNECTED is itself the assertion.
#
# The token is provisioned by eng/setup-softhsm.sh (SoftHSM2 in CI) and named
# through AZ_IOT_CLIENT_KEY_URI. Built only when that is going to be supplied;
# it fails rather than skipping when it is not.
# ---------------------------------------------------------------------------
if(AZ_IOT_BUILD_E2E_PKCS11)
    az_iot_add_e2e_suite(az_iot_tests_e2e_custody ADAPTERS ${AZ_IOT_E2E_KEY_REF_ADAPTERS}
        SOURCES tests/e2e_custody_test.c
        LIBS az_iot_e2e_service az_iot_mqttv3)
endif()



# ---------------------------------------------------------------------------
# az_iot_tests_e2e_mqttv5_custom_topic
#
# mqttv5 custom topics. Needs an mqttv5 hub with the custom topic templates
# e2e/{deviceId}/# and e2e/shared/#, which only the ci-c-e2e-mqttv5 workflow's
# environment has, so it is built only when AZ_IOT_BUILD_E2E_MQTTV5 is set.
# ---------------------------------------------------------------------------
if(AZ_IOT_BUILD_E2E_MQTTV5)
    az_iot_add_e2e_suite(az_iot_tests_e2e_mqttv5_custom_topic
        SOURCES tests/e2e_mqttv5_custom_topic_test.c tests/e2e_device.c
        LIBS az_iot_e2e_service az_iot_mqttv5)

    # mqttv5 twin: desired delivery (push and pull), reported patches, GET options, push_reported.
    az_iot_add_e2e_suite(az_iot_tests_e2e_mqttv5_twin
        SOURCES tests/e2e_mqttv5_twin_test.c tests/e2e_device.c
        LIBS az_iot_e2e_service az_iot_mqttv5)

    # mqttv5 connection lifecycle: DPS profile, saved hub with DPS fallback, reopen, eviction.
    az_iot_add_e2e_suite(az_iot_tests_e2e_mqttv5_connection
        SOURCES tests/e2e_mqttv5_connection_test.c tests/e2e_device.c
        LIBS az_iot_e2e_service az_iot_mqttv5)

    # mqttv5 telemetry: delivery, properties as the service sees them, order and size limits.
    az_iot_add_e2e_suite(az_iot_tests_e2e_mqttv5_telemetry
        SOURCES tests/e2e_mqttv5_telemetry_test.c tests/e2e_device.c
        LIBS az_iot_e2e_service az_iot_mqttv5)

    # mqttv5 direct methods: probe rejections, timeouts, result size, concurrency.
    # Not run on Windows: concurrent REST calls would share the transport's one global TLS session.
    set(_e2e_direct_method_ctest "")
    if(WIN32)
        set(_e2e_direct_method_ctest NO_CTEST)
    endif()
    az_iot_add_e2e_suite(az_iot_tests_e2e_mqttv5_direct_method ${_e2e_direct_method_ctest}
        SOURCES tests/e2e_mqttv5_direct_method_test.c tests/e2e_device.c
        LIBS az_iot_e2e_service az_iot_mqttv5)

    # mqttv5 over WebSockets and through an HTTP CONNECT proxy. Needs a proxy and
    # its access log (AZ_IOT_PROXY_HOST, AZ_IOT_PROXY_PORT, AZ_IOT_E2E_PROXY_LOG); fails
    # without them.
    az_iot_add_e2e_suite(az_iot_tests_e2e_mqttv5_transport
        SOURCES tests/e2e_mqttv5_transport_test.c tests/e2e_device.c
        LIBS az_iot_e2e_service az_iot_mqttv5)
    # setenv()/unsetenv(), as for az_iot_tests_e2e_transport.
    foreach(_t az_iot_tests_e2e_mqttv5_transport az_iot_tests_e2e_mqttv5_transport_az_mqtt)
        if(TARGET ${_t} AND _e2e_posix)
            target_compile_definitions(${_t} PRIVATE ${_e2e_posix})
        endif()
    endforeach()

    # mqttv5 key custody: the device key inside a PKCS#11 token (eng/setup-softhsm.sh), as
    # az_iot_tests_e2e_custody. Needs AZ_IOT_CLIENT_KEY_URI; fails without it.
    if(AZ_IOT_BUILD_E2E_PKCS11)
        az_iot_add_e2e_suite(az_iot_tests_e2e_mqttv5_custody ADAPTERS ${AZ_IOT_E2E_KEY_REF_ADAPTERS}
            SOURCES tests/e2e_mqttv5_custody_test.c
            LIBS az_iot_e2e_service az_iot_mqttv5)
    endif()
endif()

