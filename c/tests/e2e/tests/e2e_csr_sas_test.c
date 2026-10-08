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
 * Built with AZ_IOT_BUILD_E2E_CSR (ci-c-e2e-csr). Environment as
 * e2e_sas_test.c. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>

#include <cmocka.h>

#include "az_iot_certificate_provider_managed.h"
#include "e2e_log.h"
#include "e2e_sas_device.h"

/* Per-run operational files, named after the registration ID; removed by the
 * teardown, which runs even when an assertion fails. */
static char g_op_key[192];
static char g_op_cert[192];

static int remove_operational_files(void** state)
{
  (void)state;
  if (g_op_key[0] != '\0')
  {
    (void)remove(g_op_key);
  }
  if (g_op_cert[0] != '\0')
  {
    (void)remove(g_op_cert);
  }
  return 0;
}

static void test_sas_onboarding_then_an_issued_certificate(void** state)
{
  (void)state;
  e2e_sas_config cfg;
  e2e_sas_config_load(&cfg);
  int n = snprintf(g_op_key, sizeof(g_op_key), "e2e_op_key_%s.pem", cfg.registration_id);
  assert_true(n > 0 && (size_t)n < sizeof(g_op_key));
  n = snprintf(g_op_cert, sizeof(g_op_cert), "e2e_op_cert_%s.pem", cfg.registration_id);
  assert_true(n > 0 && (size_t)n < sizeof(g_op_cert));
  (void)remove_operational_files(NULL);

  az_iot_certificate_provider_managed provider = { 0 };
  az_iot_certificate_provider_managed_options mopts = {
    .trusted_ca_pem_path = cfg.trusted_ca,
    .operational_key_pem_path = g_op_key,
    .operational_cert_pem_path = g_op_cert,
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
  e2e_sas_config_free(&cfg);
}

int main(void)
{
  e2e_install_log_sink();
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(
        test_sas_onboarding_then_an_issued_certificate, remove_operational_files),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
