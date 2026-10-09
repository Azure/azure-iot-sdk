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
 *   5. A telemetry publish completes on that same hub connection.
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
 *   AZ_IOT_OPERATIONAL_CERT (optional; default e2e_operational_cert.pem),
 *   AZ_IOT_REJECTED_CLIENT_CERT, AZ_IOT_REJECTED_CLIENT_KEY (a self-signed
 *   certificate the services refuse; certificate fallback test)
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot.h"
#include "support/test_env.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "az_iot_certificate_provider_managed.h"

#include "e2e_log.h"

#define E2E_CONNECT_TIMEOUT_S 120

/* ---- device callbacks ----------------------------------------------------- */

typedef struct
{
  az_iot_connection_state conn_state;
  az_iot_result last_reason;
  int issued;
  size_t issued_count;
  int send_done;
  az_iot_result send_status;
  /* Per scope: X.509 index 0 refused, and the index that connected. */
  bool first_refused[AZ_IOT_CONN_SCOPE_COUNT];
  int connected_index[AZ_IOT_CONN_SCOPE_COUNT];
} csr_ctx;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  if (event->error != NULL && event->error->source == AZ_IOT_CONN_ERR_SRC_DPS)
  {
    fprintf(
        stderr,
        "[e2e-csr] DPS error %ld: %.*s\n",
        (long)event->error->code,
        (int)az_span_size(event->error->message),
        az_span_size(event->error->message) > 0 ? (const char*)az_span_ptr(event->error->message)
                                                : "");
  }

  csr_ctx* c = (csr_ctx*)user_ctx;
  if (event->auth_source == AZ_IOT_AUTH_SOURCE_X509)
  {
    if (event->reason == AZ_IOT_ERR_IDENTITY_REJECTED && event->x509_index == 0)
    {
      c->first_refused[event->scope] = true;
    }
    if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      c->connected_index[event->scope] = (int)event->x509_index;
    }
    fprintf(
        stderr,
        "[e2e-csr] %s %s reason %s x509_index %u\n",
        event->scope == AZ_IOT_CONN_SCOPE_DPS ? "dps" : "hub",
        az_iot_connection_state_to_string(event->state),
        az_iot_result_to_string(event->reason),
        (unsigned)event->x509_index);
  }

  /* Hub lifecycle only: the provisioning session reports on its own scope,
   * and storing its state here would overwrite the hub state this code acts
   * on. */
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }

  az_iot_connection_state s = event->state;
  az_iot_result reason = event->reason;
  c->conn_state = s;
  c->last_reason = reason;
  fprintf(
      stderr,
      "[e2e-csr] conn state -> %s (0x%x) reason %s (0x%x)\n",
      az_iot_connection_state_to_string(s),
      (unsigned)s,
      az_iot_result_to_string(reason),
      (unsigned)reason);
}

static void on_operational_cert(const az_iot_issued_certificate* issued, void* user_ctx)
{
  csr_ctx* c = (csr_ctx*)user_ctx;
  c->issued = 1;
  c->issued_count = issued ? issued->count : 0;
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  csr_ctx* c = (csr_ctx*)user_ctx;
  c->send_status = status;
  c->send_done = 1;
}

/* ---- certificate fallback ----------------------------------------------- */

/** @brief Offers, per role, a certificate the services refuse (index 0) before
 * the managed provider's own (index 1), once the latter exists. */
typedef struct
{
  az_iot_certificate_provider base; /* first member */
  az_iot_certificate_provider* inner;
  const char* rejected_cert;
  const char* rejected_key;
  const char* trusted_ca;
} rejected_first_provider;

static az_iot_result rejected_first_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    uint8_t index,
    az_iot_certificate_material* out)
{
  rejected_first_provider* p = (rejected_first_provider*)self;
  if (p == NULL || out == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (index > 1u)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  az_iot_result r = p->inner->vtable->load(p->inner, role, 0, out);
  if (r != AZ_IOT_OK || index == 1u)
  {
    return r;
  }
  p->inner->vtable->release(p->inner, out);
  memset(out, 0, sizeof(*out));
  out->trusted_ca_path = p->trusted_ca;
  out->client_cert_path = p->rejected_cert;
  out->client_key_path = p->rejected_key;
  return AZ_IOT_OK;
}

static void rejected_first_release(
    az_iot_certificate_provider* self,
    az_iot_certificate_material* material)
{
  rejected_first_provider* p = (rejected_first_provider*)self;
  if (material != NULL && material->client_cert_path != p->rejected_cert)
  {
    p->inner->vtable->release(p->inner, material);
  }
}

static void rejected_first_deinit(az_iot_certificate_provider* self) { (void)self; }

static az_iot_result rejected_first_get_csr(
    az_iot_certificate_provider* self,
    const char* subject_common_name,
    az_iot_certificate_signing_request* out_csr)
{
  rejected_first_provider* p = (rejected_first_provider*)self;
  return p->inner->vtable->get_csr(p->inner, subject_common_name, out_csr);
}

static void rejected_first_release_csr(
    az_iot_certificate_provider* self,
    az_iot_certificate_signing_request* csr)
{
  rejected_first_provider* p = (rejected_first_provider*)self;
  p->inner->vtable->release_csr(p->inner, csr);
}

static az_iot_result rejected_first_store(
    az_iot_certificate_provider* self,
    const az_iot_issued_certificate* issued)
{
  rejected_first_provider* p = (rejected_first_provider*)self;
  return p->inner->vtable->store_issued_certificate(p->inner, issued);
}

/* ---- scenario ------------------------------------------------------------- */

/**
 * @param rejected_first Offer a refused certificate first for each role: DPS
 * and the hub must each fall back to the managed provider's certificate.
 */
static void run_csr_enrollment(
    az_iot_certificate_managed_key_type key_type,
    const char* label,
    bool rejected_first)
{
  char* id_scope = az_iot_test_env_dup("AZ_IOT_DPS_ID_SCOPE");
  char* reg_id = az_iot_test_env_dup("AZ_IOT_DPS_REGISTRATION_ID");
  char* cert = az_iot_test_env_dup("AZ_IOT_CLIENT_CERT");
  char* key = az_iot_test_env_dup("AZ_IOT_CLIENT_KEY");
  char* ca = az_iot_test_env_dup("AZ_IOT_TRUSTED_CA");
  char* global = az_iot_test_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT"); /* optional */

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
  az_iot_mqttv3_telemetry_client telemetry = { 0 };

  az_iot_certificate_provider_managed_options mopts = {
    .bootstrap_cert_pem_path = cert,
    .bootstrap_key_pem_path = key,
    .trusted_ca_pem_path = ca,
    .operational_key_pem_path = op_key,
    .operational_cert_pem_path = op_cert,
    .key_type = key_type,
  };
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&provider, &mopts));

  char* rejected_cert = rejected_first ? az_iot_test_env_dup("AZ_IOT_REJECTED_CLIENT_CERT") : NULL;
  char* rejected_key = rejected_first ? az_iot_test_env_dup("AZ_IOT_REJECTED_CLIENT_KEY") : NULL;
  static const az_iot_certificate_provider_vtable k_rejected_first_vtable = {
    .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
    .load = rejected_first_load,
    .release = rejected_first_release,
    .deinit = rejected_first_deinit,
    .get_csr = rejected_first_get_csr,
    .release_csr = rejected_first_release_csr,
    .store_issued_certificate = rejected_first_store,
  };
  rejected_first_provider wrapper = {
    .base = { .vtable = &k_rejected_first_vtable },
    .inner = &provider.base,
    .rejected_cert = rejected_cert,
    .rejected_key = rejected_key,
    .trusted_ca = ca,
  };
  if (rejected_first)
  {
    assert_non_null(rejected_cert);
    assert_non_null(rejected_key);
  }

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = id_scope;
  copts.dps.registration_id = reg_id;
  copts.certificate_provider = rejected_first ? &wrapper.base : &provider.base;
  AZ_IOT_CSR_PAYLOAD_STORAGE(csr_buf);
  copts.csr_payload_buffer = az_span_create(csr_buf, sizeof(csr_buf));
  copts.dps.request_operational_certificate = true;
  if (global != NULL)
  {
    copts.dps.global_endpoint = global;
  }

  assert_int_equal(AZ_IOT_OK, az_iot_connection_client_init(&conn, &copts));
  az_iot_connection_client_add_state_observer(&conn, on_conn_state, &ctx);
  az_iot_connection_client_set_operational_cert_callback(&conn, on_operational_cert, &ctx);
  assert_int_equal(
      AZ_IOT_OK,
      az_iot_connection_client_register_mqtt_factory(&conn, az_iot_paho_factory_create_v3_1_1()));
  assert_int_equal(AZ_IOT_OK, az_iot_mqttv3_telemetry_client_init(&telemetry, &conn));

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
  if (rejected_first)
  {
    /* Each role was refused its index 0 and connected with index 1. */
    assert_true(ctx.first_refused[AZ_IOT_CONN_SCOPE_DPS]);
    assert_int_equal(ctx.connected_index[AZ_IOT_CONN_SCOPE_DPS], 1);
    assert_true(ctx.first_refused[AZ_IOT_CONN_SCOPE_HUB]);
    assert_int_equal(ctx.connected_index[AZ_IOT_CONN_SCOPE_HUB], 1);
  }

  static const uint8_t payload[] = "{\"source\":\"e2e_csr\"}";
  az_iot_telemetry_message message = { 0 };
  message.payload = payload;
  message.payload_len = sizeof(payload) - 1;
  assert_int_equal(
      AZ_IOT_OK, az_iot_mqttv3_telemetry_client_send(&telemetry, &message, on_send_done, &ctx));
  start = time(NULL);
  while (!ctx.send_done && ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED
         && (time(NULL) - start) < 30)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
  }
  assert_int_equal(ctx.send_done, 1);
  assert_int_equal(ctx.send_status, AZ_IOT_OK);

  az_iot_connection_client_close(&conn);
  for (int i = 0; i < 100 && ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
  }

  az_iot_mqttv3_telemetry_client_deinit(&telemetry);
  az_iot_connection_client_deinit(&conn);
  az_iot_certificate_provider_managed_deinit(&provider);

  remove(op_key);
  remove(op_cert);
  free(id_scope);
  free(reg_id);
  free(cert);
  free(key);
  free(ca);
  free(global);
  free(rejected_cert);
  free(rejected_key);
}

static void test_dps_csr_enrollment_ec(void** state)
{
  (void)state;
  run_csr_enrollment(AZ_IOT_MANAGED_KEY_EC_P256, "ec", false);
}

static void test_dps_csr_enrollment_rsa(void** state)
{
  (void)state;
  run_csr_enrollment(AZ_IOT_MANAGED_KEY_RSA_2048, "rsa", false);
}

/* A refused certificate first for DPS and for the hub: each falls back to the
 * next one at once, and connects with it (x509_index 1). */
static void test_each_role_falls_back_to_its_next_certificate(void** state)
{
  (void)state;
  run_csr_enrollment(AZ_IOT_MANAGED_KEY_EC_P256, "fallback", true);
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
    cmocka_unit_test(test_each_role_falls_back_to_its_next_certificate),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
