// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

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
 * Whether the suite runs at all is a build-time decision: it is registered as
 * a CTest test only when AZ_IOT_BUILD_CONFORMANCE_TESTS is on. Once registered
 * it never excuses itself -- an unset or empty AZ_IOT_MQTT_BROKER_HOST is a
 * failure, because a suite that skips itself is one nobody notices has stopped
 * running.
 */
#ifndef AZ_IOT_CONFORMANCE_H
#define AZ_IOT_CONFORMANCE_H

#include "azure/iot/az_iot_mqtt_iface.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /* Which suite to run. Each suite is a curated set of cmocka tests appropriate
   * for the given MQTT version. */
  typedef enum az_iot_conformance_suite
  {
    AZ_IOT_CONFORMANCE_SUITE_V3_1_1 = 0,
    AZ_IOT_CONFORMANCE_SUITE_V5 = 1
  } az_iot_conformance_suite;

  /* Optional adapter capabilities (D8 and later).
   *
   * The suite's baseline applies to EVERY adapter and is not negotiable. A
   * capability declares that the adapter implements an OPTIONAL feature, which
   * makes the suite hold it to that feature's contract as well.
   *
   * Declaring nothing is therefore never a way to be tested less on safety:
   * the baseline includes what an adapter must do when asked for a feature it
   * does NOT implement -- refuse the connect rather than proceed without the
   * credential it was asked to use. */
  typedef enum az_iot_conformance_capability
  {
    AZ_IOT_CONFORMANCE_CAP_NONE = 0,

    /* The adapter honours a non-extractable private key: az_iot_mqtt_tls_options
     * client_key_uri + crypto_engine_id, and/or the sign() hook. An adapter
     * that declares this must complete a TLS handshake using a key it cannot
     * read; see az_iot_conformance_options::key_uri for the end-to-end case.
     *
     * Declaring it without supplying that key, or in a build without TLS
     * support, fails the run rather than warning. */
    AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY = 1u << 0
  } az_iot_conformance_capability;

  typedef struct az_iot_conformance_options
  {
    /* Bitwise OR of az_iot_conformance_capability. */
    uint32_t capabilities;

    /* End-to-end key custody material, used only when
     * AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY is declared. Supply all three to have
     * the suite prove the adapter can actually sign a TLS handshake with a key
     * it cannot read:
     *
     *   key_uri          a key reference the adapter can resolve
     *                    ("pkcs11:object=...;type=private", "tpm2:...")
     *   crypto_engine_id the provider/engine that owns it ("pkcs11", "tpm2")
     *   client_cert_path a certificate whose PUBLIC key is that key's
     *
     * A declared capability that is never exercised FAILS the run: a pass has
     * to mean the claim was checked. When no token is available, either do not
     * declare the capability or set AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN=1 in the
     * environment, which downgrades it to a notice -- such a run proves
     * nothing about custody and must not be reported as conformant for it. */
    const char* key_uri;
    const char* crypto_engine_id;
    const char* client_cert_path;
  } az_iot_conformance_options;

  /* Run the conformance suite for `suite_kind` against the given factory.
   * Returns:
   *   0  on success: all tests passed and every declared capability was
   *      exercised -- UNLESS AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN=1 was set in
   *      the environment, which lets an unexercised capability through as a
   *      notice on stderr. A 0 from such a run says nothing about that
   *      capability and must not be reported as conformant for it.
   *   1  on failure: a test failed, a declared capability was never exercised
   *      (without that opt-out), or no broker was configured. There is no skip
   *      code -- whether the suite runs is decided at build time by
   *      AZ_IOT_BUILD_CONFORMANCE_TESTS, so a run that cannot test what it was
   *      asked to test is a failure.
   *
   * Suitable to use directly as the return value of main() in a harness exe.
   *
   * Equivalent to az_iot_conformance_run_with_options() with no capabilities
   * declared. An adapter that implements an optional feature should call that
   * instead, or the suite cannot hold it to the feature's contract. */
  int az_iot_conformance_run(az_iot_conformance_suite suite_kind, az_iot_mqtt_factory* factory);

  /* Internal, exposed for the suite's own tests.
   *
   * Reports a declared capability whose contract was never exercised and
   * returns what it contributes to the run's failure count: 1, unless
   * AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN is set to exactly "1", in which case it
   * reports a notice and returns 0. This is the whole of the opt-out policy,
   * kept callable so a regression in it cannot pass unnoticed. */
  int az_iot_conformance_report_unproven_capability(const char* capability, const char* why);

  /* As above, plus the adapter's declared capabilities. `options` may be NULL,
   * which means the same as declaring nothing. */
  int az_iot_conformance_run_with_options(
      az_iot_conformance_suite suite_kind,
      az_iot_mqtt_factory* factory,
      const az_iot_conformance_options* options);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CONFORMANCE_H */
