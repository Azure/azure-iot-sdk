# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ---------------------------------------------------------------------------
# Vendored, test-only AMQP 1.0 client (az_amqp + reference transport). Used by
# the in-process service client below to receive device telemetry (Event
# Hub-compatible endpoint) and send C2D over the IoT Hub AMQP service endpoint.
# ---------------------------------------------------------------------------
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
target_link_libraries(az_iot_tests_e2e PRIVATE az_iot_e2e_service az_iot_adapter_paho)

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_adu
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
az_iot_add_cmocka_test(az_iot_tests_e2e_adu tests/e2e_adu_test.c tests/e2e_device.c)
target_link_libraries(az_iot_tests_e2e_adu PRIVATE az_iot_e2e_service az_iot_adapter_paho)

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_csr
#
# CSR-based DPS enrollment (issued operational cert). Requires a DPS enrollment
# linked to a signing CA - which the fast job does not provision - so it
# self-skips (CTest exit 77) unless AZ_IOT_DPS_CSR_ENABLED is set. Built only
# when the OpenSSL "managed" certificate provider is available.
# ---------------------------------------------------------------------------
if(TARGET az_iot_certificate_provider_managed)
    az_iot_add_cmocka_test(az_iot_tests_e2e_csr tests/e2e_csr_test.c)
    target_link_libraries(az_iot_tests_e2e_csr PRIVATE
        az_iot_adapter_paho az_iot_certificate_provider_managed)
    set_tests_properties(az_iot_tests_e2e_csr PROPERTIES SKIP_RETURN_CODE 77)
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
# Runs on BOTH e2e legs. Only the notification scenario self-skips on Windows
# (the harness TLS transport keeps a single process-wide Schannel session, so it
# cannot hold the watcher's AMQP connection open across the HTTPS calls); the
# whole upload round trip runs there. SKIP_RETURN_CODE is kept so the suite can
# still report a wholesale skip if its environment is unavailable.
# ---------------------------------------------------------------------------
az_iot_add_cmocka_test(az_iot_tests_e2e_fileupload tests/e2e_fileupload_test.c tests/e2e_device.c)
target_link_libraries(az_iot_tests_e2e_fileupload PRIVATE az_iot_e2e_service az_iot_adapter_paho)
set_tests_properties(az_iot_tests_e2e_fileupload PROPERTIES SKIP_RETURN_CODE 77)
