// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* SAS onboarding with certificate-based operations: a device in a DPS
 * symmetric-key enrollment group linked to a signing CA registers with SAS,
 * sends a CSR, receives a DPS-issued operational certificate, and connects to
 * the assigned hub with it (X.509), then sends telemetry. The managed provider
 * runs without a bootstrap identity.
 *
 * Built only with AZ_IOT_BUILD_E2E_SAS and AZ_IOT_BUILD_E2E_CSR, which only the
 * ci-c-e2e-csr workflow sets. Environment as e2e_sas_test.c. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>

#include <cmocka.h>

#include "az_iot_certificate_provider_managed.h"
#include "e2e_log.h"
#include "e2e_sas_device.h"

#define OP_KEY "e2e_csr_sas_operational_key.pem"
#define OP_CERT "e2e_csr_sas_operational_cert.pem"

static void test_sas_onboarding_then_an_issued_certificate(void** state)
{
  (void)state;
  e2e_sas_config cfg;
  e2e_sas_config_load(&cfg);
  (void)remove(OP_KEY);
  (void)remove(OP_CERT);

  az_iot_certificate_provider_managed provider = { 0 };
  az_iot_certificate_provider_managed_options mopts = {
    .trusted_ca_pem_path = cfg.trusted_ca,
    .operational_key_pem_path = OP_KEY,
    .operational_cert_pem_path = OP_CERT,
    .key_type = AZ_IOT_MANAGED_KEY_EC_P256,
  };
  assert_int_equal(az_iot_certificate_provider_managed_init(&provider, &mopts), AZ_IOT_OK);

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  e2e_sas_apply_dps(&cfg, &copts);
  copts.certificate_provider = &provider.base;
  copts.dps.request_operational_certificate = true;
  AZ_IOT_CSR_PAYLOAD_STORAGE(csr_buf);
  copts.csr_payload_buffer = az_span_create(csr_buf, sizeof(csr_buf));

  e2e_sas_run run;
  e2e_sas_connect_and_send(&run, &copts, "e2e_csr_sas");
  assert_int_equal(run.dps_source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
  assert_int_equal(run.hub_source, AZ_IOT_AUTH_SOURCE_X509);
  assert_true(provider.has_operational);

  az_iot_certificate_provider_managed_deinit(&provider);
  (void)remove(OP_KEY);
  (void)remove(OP_CERT);
  e2e_sas_config_free(&cfg);
}

int main(void)
{
  e2e_install_log_sink();
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_sas_onboarding_then_an_issued_certificate),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
