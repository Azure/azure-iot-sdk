// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* MQTT iface conformance suite for azure-iot-sdk.
 *
 * This header is the contract a customer-supplied MQTT adapter must satisfy
 * to be usable as an azure-iot-sdk MQTT client. The suite operates strictly
 * through the public `az_iot_mqtt_iface` vtable - it does NOT depend on
 * any specific adapter implementation. To validate a new adapter, the
 * customer:
 *
 *   1. Builds their adapter as a static or shared library that exposes a
 *      function returning an `az_iot_mqtt_factory*` for one or both
 *      MQTT versions.
 *   2. Links the conformance library (`az_iot_conformance`) and produces
 *      one harness exe per (version, role) tuple that calls
 *      `az_iot_conformance_run()` with the factory and the suite to run.
 *
 * The suite is split by MQTT version so adapters that only implement v3.1.1
 * or only v5 can still pass the relevant suite.
 *
 * Tests in the suite are designed to be self-contained: each test creates a
 * fresh client from the factory, connects, exercises the iface, and tears
 * down. State leakage between tests is therefore disallowed at the iface
 * level.
 *
 * Broker discovery: the suite reads the broker address from the environment
 *   AZ_IOT_MQTT_BROKER_HOST (default: "localhost")
 *   AZ_IOT_MQTT_BROKER_PORT (default: "1883")
 * If `AZ_IOT_MQTT_BROKER_SKIP=1` (or `AZ_IOT_MQTT_BROKER_HOST` is the
 * empty string), the suite is reported as skipped (CTest exit code 77) so
 * developers without a broker configured don't see false failures locally.
 */
#ifndef AZ_IOT_CONFORMANCE_H
#define AZ_IOT_CONFORMANCE_H

#include "azure/iot/az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which suite to run. Each suite is a curated set of cmocka tests appropriate
 * for the given MQTT version. */
typedef enum az_iot_conformance_suite
{
    AZ_IOT_CONFORMANCE_SUITE_V3_1_1 = 0,
    AZ_IOT_CONFORMANCE_SUITE_V5     = 1
} az_iot_conformance_suite;

/* Run the conformance suite for `suite_kind` against the given factory.
 * Returns:
 *   0  on success (all tests passed)
 *   77 if the suite was skipped (no broker configured)
 *   1  on failure (one or more tests failed)
 *
 * Suitable to use directly as the return value of main() in a harness exe. */
int az_iot_conformance_run(
    az_iot_conformance_suite suite_kind,
    az_iot_mqtt_factory* factory);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CONFORMANCE_H */
