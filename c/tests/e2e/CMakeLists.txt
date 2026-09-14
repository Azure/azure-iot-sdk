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
include_directories(${CMAKE_SOURCE_DIR}/tests)

# Everything here needs the Paho adapter, and the legacy suites additionally
# need the AMQP service facade. A target left outside this guard fails to build
# with AZ_IOT_WITH_PAHO=OFF.
if(AZ_IOT_WITH_PAHO)

# The ADUv2 device-update suite. Uses the shipping Paho adapter and X.509, so it
# needs no transport or credential of its own -- it exercises the SDK's real
# auth and transport path rather than a test substitute.
#
# Requires AZ_IOT_BUILD_E2E and AZ_IOT_WITH_PAHO as well; this option only
# selects the suite once the e2e directory is being built at all.
#
# The PEM certificate provider it uses is part of az_iot_core, so the managed
# (OpenSSL) provider is deliberately NOT linked: doing so would make the suite
# fail to build wherever that provider is off.
if(AZ_IOT_BUILD_E2E_ADU_V2)
    az_iot_add_cmocka_test(az_iot_tests_e2e_adu tests/e2e_adu_v2_test.c)
    target_link_libraries(az_iot_tests_e2e_adu PRIVATE az_iot_core az_iot_adapter_paho)
    # The suite asserts on the channel and the codecs, which are internal.
    target_include_directories(az_iot_tests_e2e_adu PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/features/adu/internal)
else()
    message(STATUS "ADUv2 e2e suite disabled (set AZ_IOT_BUILD_E2E_ADU_V2=ON).")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/amqp
                 ${CMAKE_CURRENT_BINARY_DIR}/deps/amqp)

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
target_link_libraries(az_iot_tests_e2e PRIVATE az_iot_e2e_service az_iot_gen1 az_iot_adapter_paho)

# ---------------------------------------------------------------------------
# (historical description of the retired twin-driven suite)
#
# Device Update (ADU) e2e suite. It drives a signed deployment end-to-end
# through the device twin: the cloud PATCHes a signed manifest into desired
# properties and the device runs the download -> install -> apply workflow
# (crypto + platform ops mocked, as in the ADU unit test) and reports the
# outcome back. Because a twin desired-property PATCH is the entire trigger, it
# needs NO Device Update account, so it is fast. Like the fast suite it plays
# both halves in one process, so it links the same device (Paho) + service
# facade libraries.
#
# It runs in the fast PR workflow (ci-c-e2e.yml, `ctest -R e2e`, which matches
# BOTH e2e executables) AND, as an interim placeholder, in the nightly ADU
# workflow (ci-c-e2e-adu.yml, `ctest -R e2e_adu`, which selects only this one)
# until the real-ADU suite -- one that provisions an actual Device Update
# account and triggers updates through the ADU service -- lands. The `e2e` name
# filters never match the adu_crypto_openssl unit test.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# az_iot_tests_e2e_adu -- RETIRED WITH THE ADUv1 CUT, AWAITING THE ADUv2 REWRITE
#
# All five scenarios in tests/e2e_adu_test.c drove a deployment through a twin
# desired-property PATCH. That channel no longer exists: ADU is now driven
# through an az_iot_adu_channel, so the suite cannot compile and its trigger has
# no equivalent until the ADUv2 channel lands.
#
# The file is deliberately KEPT rather than deleted: its device fixture and its
# mocked crypto/payload hooks carry over to the rewritten suite unchanged, and
# they are the expensive part. Only the trigger and the assertions change.
#
# The ci-c-e2e-adu workflow is likewise kept rather than removed, so that the
# coverage gap between the cut and the rewrite stays visible instead of quietly
# disappearing from the pipeline.
#
# Re-enable this target as part of the ADUv2 channel work.
# ---------------------------------------------------------------------------

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
        az_iot_adapter_paho az_iot_certificate_provider_managed)
endif()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_fileupload
#
# Classic file-upload device flow against a real hub that has an Azure Storage
# account associated for file upload. It plays both halves like the other
# suites: the device requests a SAS URI, PUTs the blob, reads it back and reports
# completion, while the service side watches the hub's file-upload NOTIFICATION
# endpoint -- the only cloud-side proof that a round trip completed. It also
# covers the failure report (accepted, but never notified), a completion carrying
# an unknown correlation id, and local argument rejection.
#
# The Classic HTTP hook and the Storage PUT/GET go through the e2e service lib's
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
target_link_libraries(az_iot_tests_e2e_fileupload PRIVATE az_iot_e2e_service az_iot_gen1 az_iot_adapter_paho)

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
        az_iot_e2e_service az_iot_gen1 az_iot_adapter_paho)
endif()


endif() # AZ_IOT_WITH_PAHO