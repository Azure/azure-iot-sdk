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
az_iot_add_cmocka_test(az_iot_tests_e2e tests/e2e_scenarios_test.c)
target_link_libraries(az_iot_tests_e2e PRIVATE az_iot_e2e_service az_iot_adapter_paho)

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_adu
#
# Device Update (ADU) slow-lane placeholder. Provisioning the ADU account/
# instance takes ~25 min, so this runs in a SEPARATE workflow (ci-c-e2e-adu.yml)
# on a nightly / dispatch / ADU-paths cadence, NOT in the fast PR job. It carries
# the ADU pipeline until a real device-side ADU update scenario is written: it is
# standalone (no device/service link), gates on the provisioned config, and
# reports a CTest skip (exit 77).
#
# Filtered separately from the fast suite so neither picks up the other or the
# adu_crypto_openssl unit test: the fast job runs `ctest -R e2e -E adu`; the ADU
# job runs `ctest -R e2e_adu`.
# ---------------------------------------------------------------------------
add_executable(az_iot_tests_e2e_adu tests/e2e_adu_test.c)
az_iot_apply_warnings(az_iot_tests_e2e_adu)
add_test(NAME az_iot_tests_e2e_adu COMMAND az_iot_tests_e2e_adu)
set_tests_properties(az_iot_tests_e2e_adu PROPERTIES SKIP_RETURN_CODE 77)
