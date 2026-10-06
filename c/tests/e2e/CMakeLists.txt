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

# Everything here needs the Paho adapter, and the legacy suites additionally
# need the AMQP service facade. A target left outside this guard fails to build
# with AZ_IOT_WITH_PAHO=OFF.
if(AZ_IOT_WITH_PAHO)

# The software updates e2e suites. Both use the shipping Paho adapter and X.509
# against the real service; see docs/eng/end-to-end-tests.md.
#
#   az_iot_tests_e2e_su        the DPS channel, with no update offered
#   az_iot_tests_e2e_su_offer  offered updates through the whole client:
#                              real crypto, real download (libcurl)
#
# Requires AZ_IOT_BUILD_E2E and AZ_IOT_WITH_PAHO as well; this option only
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
    az_iot_add_cmocka_test(az_iot_tests_e2e_su tests/e2e_su_test.c tests/e2e_su_env.c)
    target_link_libraries(az_iot_tests_e2e_su PRIVATE az_iot_core az_iot_adapter_paho)
    # The suite asserts on the channel and the codecs, which are internal.
    target_include_directories(az_iot_tests_e2e_su PRIVATE
        ${PROJECT_SOURCE_DIR}/src/core
        ${PROJECT_SOURCE_DIR}/src/features/su/internal)

    if(NOT TARGET az_iot_crypto_openssl)
        message(FATAL_ERROR
            "AZ_IOT_BUILD_E2E_SU needs the OpenSSL crypto backend "
            "(AZ_IOT_WITH_CRYPTO_OPENSSL=ON).")
    endif()
    find_package(CURL REQUIRED)
    az_iot_add_cmocka_test(az_iot_tests_e2e_su_offer
        tests/e2e_su_offer_test.c tests/e2e_su_env.c tests/e2e_su_test_roots.c)
    target_link_libraries(az_iot_tests_e2e_su_offer PRIVATE
        az_iot_core az_iot_adapter_paho az_iot_crypto_openssl CURL::libcurl)
    # The spy wraps the internal channel to observe reports and verdicts.
    target_include_directories(az_iot_tests_e2e_su_offer PRIVATE
        ${PROJECT_SOURCE_DIR}/src/core
        ${PROJECT_SOURCE_DIR}/src/features/su/internal)
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

# ---------------------------------------------------------------------------
# Service-side facade (hides az_amqp behind a plain-C API used by the tests).
# ---------------------------------------------------------------------------
add_subdirectory(service)

# ---------------------------------------------------------------------------
# az_iot_tests_e2e
#
# The in-process end-to-end suite: ONE cmocka executable that plays both the
# device (shipping SDK over the Paho MQTT adapter) and the cloud (the
# az_iot_e2e_service facade) halves of every scenario against a real Azure IoT
# Hub / DPS instance. Registered with CTest so the e2e job can run it with
# `ctest -R e2e`. The test includes ONLY the service facade's public header, so
# no AMQP type ever leaks into the test translation unit.
#
# It requires provisioning + secrets in the environment (device X.509 material
# and hub/eventhub connection strings) supplied by the e2e CI job; it is only
# built when AZ_IOT_BUILD_E2E and AZ_IOT_WITH_PAHO are enabled.
# ---------------------------------------------------------------------------
az_iot_add_cmocka_test(az_iot_tests_e2e tests/e2e_scenarios_test.c tests/e2e_device.c)
target_link_libraries(az_iot_tests_e2e PRIVATE az_iot_e2e_service az_iot_mqttv3 az_iot_adapter_paho)

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
    az_iot_add_cmocka_test(az_iot_tests_e2e_csr tests/e2e_csr_test.c)
    target_link_libraries(az_iot_tests_e2e_csr PRIVATE
        az_iot_mqttv3 az_iot_adapter_paho az_iot_certificate_provider_managed)
endif()

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
az_iot_add_cmocka_test(az_iot_tests_e2e_fileupload ${_e2e_fileupload_ctest}
    tests/e2e_fileupload_test.c tests/e2e_device.c)
target_link_libraries(az_iot_tests_e2e_fileupload PRIVATE az_iot_e2e_service az_iot_mqttv3 az_iot_adapter_paho)

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
az_iot_add_cmocka_test(az_iot_tests_e2e_transport
    tests/e2e_transport_test.c tests/e2e_device.c)
target_link_libraries(az_iot_tests_e2e_transport PRIVATE
    az_iot_e2e_service az_iot_mqttv3 az_iot_adapter_paho)
if(UNIX)
    # setenv()/unsetenv() are POSIX, not C99, and these targets build -std=c99:
    # without this the calls are implicit declarations and the build fails rather
    # than warning. Same reason as az_iot_tests_paho_transport_proxy.
    target_compile_definitions(az_iot_tests_e2e_transport PRIVATE _POSIX_C_SOURCE=200809L)
endif()

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
    az_iot_add_cmocka_test(az_iot_tests_e2e_custody tests/e2e_custody_test.c)
    target_link_libraries(az_iot_tests_e2e_custody PRIVATE
        az_iot_e2e_service az_iot_mqttv3 az_iot_adapter_paho)
endif()


endif() # AZ_IOT_WITH_PAHO
