// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* CSR-based DPS enrollment end-to-end (increment 8).
 *
 * Proves the device-side CSR path against real Azure DPS + IoT Hub:
 *   1. The device authenticates to DPS with an X.509 BOOTSTRAP identity.
 *   2. request_operational_certificate=true sends a CSR (from the OpenSSL
 *      managed provider) in the DPS register body.
 *   3. DPS returns the issued operational certificate chain (D4 callback fires),
 *      which the managed provider persists to disk.
 *   4. The client connects to the assigned hub using the issued OPERATIONAL
 *      identity - reaching CONNECTED is the hub's proof that it authenticated
 *      the DPS-issued certificate.
 *
 * This requires a DPS enrollment (group) linked to a signing CA so DPS issues an
 * operational cert - a setup only the ci-c-e2e-csr workflow provisions. It is
 * therefore built only when AZ_IOT_BUILD_E2E_CSR is set, which that workflow
 * passes. There is no runtime gate: if this binary exists, it runs, and if it
 * cannot run it should not have been built.
 *
 * Environment:
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID,
 *   AZ_IOT_CLIENT_CERT, AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA  (bootstrap X.509),
 *   AZ_IOT_DPS_GLOBAL_ENDPOINT (optional),
 *   AZ_IOT_OPERATIONAL_KEY  (optional; default e2e_operational_key.pem),
 *   AZ_IOT_OPERATIONAL_CERT (optional; default e2e_operational_cert.pem)
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "az_iot_certificate_provider_managed.h"

#include "e2e_log.h"

#define E2E_CONNECT_TIMEOUT_S 120

#ifndef _WIN32
static char* dup_cstr(const char* s)
{
  if (!s)
  {
    return NULL;
  }
  size_t n = strlen(s) + 1;
  char* out = malloc(n);
  if (out)
  {
    memcpy(out, s, n);
  }
  return out;
}
#endif

static char* env_dup(const char* name)
{
#ifdef _WIN32
  char* value = NULL;
  size_t len = 0;
  if (_dupenv_s(&value, &len, name) != 0 || value == NULL || value[0] == '\0')
  {
    free(value);
    return NULL;
  }
  return value;
#else
  const char* v = getenv(name);
  return (v && v[0]) ? dup_cstr(v) : NULL;
#endif
}

/* ---- device callbacks ----------------------------------------------------- */

typedef struct
{
  az_iot_connection_state conn_state;
  az_iot_result last_reason;
  int issued;
  size_t issued_count;
} csr_ctx;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_connection_state s = event->state;
  az_iot_result reason = event->reason;
  csr_ctx* c = (csr_ctx*)user_ctx;
  c->conn_state = s;
  c->last_reason = reason;
  fprintf(stderr, "[e2e-csr] conn state -> 0x%x (reason 0x%x)\n", (unsigned)s, (unsigned)reason);
}

static void on_operational_cert(const az_iot_issued_certificate* issued, void* user_ctx)
{
  csr_ctx* c = (csr_ctx*)user_ctx;
  c->issued = 1;
  c->issued_count = issued ? issued->count : 0;
}

/* ---- scenario ------------------------------------------------------------- */

static void run_csr_enrollment(az_iot_certificate_managed_key_type key_type, const char* label)
{
  char* id_scope = env_dup("AZ_IOT_DPS_ID_SCOPE");
  char* reg_id = env_dup("AZ_IOT_DPS_REGISTRATION_ID");
  char* cert = env_dup("AZ_IOT_CLIENT_CERT");
  char* key = env_dup("AZ_IOT_CLIENT_KEY");
  char* ca = env_dup("AZ_IOT_TRUSTED_CA");
  char* global = env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT"); /* optional */

  /* Per-key-type operational files so the EC and RSA legs never share state. */
  char op_key[128];
  char op_cert[128];
  (void)snprintf(op_key, sizeof(op_key), "e2e_operational_key_%s.pem", label);
  (void)snprintf(op_cert, sizeof(op_cert), "e2e_operational_cert_%s.pem", label);

  assert_non_null(id_scope);
  assert_non_null(reg_id);
  assert_non_null(cert);
  assert_non_null(key);
  assert_non_null(ca);

  /* Force a fresh issuance: drop anything left by a prior run. */
  remove(op_cert);
  remove(op_key);

  csr_ctx ctx = { 0 };
  az_iot_certificate_provider_managed provider = { 0 };
  az_iot_connection_client conn = { 0 };

  az_iot_certificate_provider_managed_options mopts = {
    .bootstrap_cert_pem_path = cert,
    .bootstrap_key_pem_path = key,
    .trusted_ca_pem_path = ca,
    .operational_key_pem_path = op_key,
    .operational_cert_pem_path = op_cert,
    .key_type = key_type,
  };
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&provider, &mopts));

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = id_scope;
  copts.dps.registration_id = reg_id;
  copts.certificate_provider = &provider.base;
  AZ_IOT_CSR_PAYLOAD_STORAGE(csr_buf);
  copts.csr_payload_buffer = az_span_create(csr_buf, sizeof(csr_buf));
  copts.dps.request_operational_certificate = true;
  if (global != NULL)
  {
    copts.dps.global_endpoint = global;
  }

  assert_int_equal(AZ_IOT_OK, az_iot_connection_client_init(&conn, &copts));
  az_iot_connection_client_set_state_callback(&conn, on_conn_state, &ctx);
  az_iot_connection_client_set_operational_cert_callback(&conn, on_operational_cert, &ctx);
  assert_int_equal(
      AZ_IOT_OK,
      az_iot_connection_client_register_mqtt_factory(&conn, az_iot_paho_factory_create_v3_1_1()));

  assert_int_equal(AZ_IOT_OK, az_iot_connection_client_open(&conn));

  time_t start = time(NULL);
  while (ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED
         && (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
    if (ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  /* Connected with the DPS-issued operational identity: */
  assert_int_equal(ctx.conn_state, AZ_IOT_CONN_STATE_CONNECTED);
  /* the operational-cert callback (D4) fired with a non-empty chain, and */
  assert_int_equal(ctx.issued, 1);
  assert_true(ctx.issued_count >= 1);
  /* the managed provider persisted the issued cert for the next boot. */
  assert_true(provider.has_operational);

  az_iot_connection_client_close(&conn);
  for (int i = 0; i < 100 && ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
  }

  az_iot_connection_client_destroy(&conn);
  az_iot_certificate_provider_managed_destroy(&provider);

  remove(op_key);
  remove(op_cert);
  free(id_scope);
  free(reg_id);
  free(cert);
  free(key);
  free(ca);
  free(global);
}

static void test_dps_csr_enrollment_ec(void** state)
{
  (void)state;
  run_csr_enrollment(AZ_IOT_MANAGED_KEY_EC_P256, "ec");
}

static void test_dps_csr_enrollment_rsa(void** state)
{
  (void)state;
  run_csr_enrollment(AZ_IOT_MANAGED_KEY_RSA_2048, "rsa");
}

int main(void)
{
  /* No environment gate. Whether this test exists is decided at build time by
   * AZ_IOT_BUILD_E2E_CSR, which only the ci-c-e2e-csr workflow sets -- it is
   * the one that provisions a DPS enrollment linked to a signing CA. Building
   * the test and then having it excuse itself hid the fact that it was not
   * running anywhere else. */

  /* Log verbosity is env-controlled (AZ_IOT_E2E_LOG_LEVEL=TRACE|DEBUG|INFO|
   * WARN); default ERROR. CI raises it to surface the connect/DPS failure. */
  e2e_install_log_sink();

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_dps_csr_enrollment_ec),
    cmocka_unit_test(test_dps_csr_enrollment_rsa),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
