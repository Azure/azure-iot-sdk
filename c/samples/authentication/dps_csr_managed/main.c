// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* authentication/dps_csr_managed
 *
 * DPS enrollment that obtains an OPERATIONAL certificate via CSR, using the
 * OpenSSL-backed "managed" certificate provider (increments 3, 5, 6).
 *
 * Flow:
 *   1. The device authenticates to DPS with an X.509 BOOTSTRAP identity.
 *   2. request_operational_certificate=true makes the connection client ask the
 *      provider for a CSR and send it in the DPS register body.
 *   3. DPS returns the issued operational certificate chain; the managed
 *      provider persists it to disk and the connection proceeds to the assigned
 *      hub using the OPERATIONAL identity.
 *   4. The optional operational-cert callback (D4) is notified of the new chain.
 *
 * It also shows the optional custom registration payload sharing that one
 * register body with the CSR: set AZ_IOT_DPS_REGISTRATION_PAYLOAD to a JSON
 * object (for example {"modelId":"dtmi:com:example:Thermostat;1"}) and the
 * device sends {"csr":"...","payload":{...}}. Whatever the allocation policy
 * returns comes back through the registration-payload callback.
 *
 * Requires the managed provider (OpenSSL 3.0+).
 *
 * Environment:
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID
 *   AZ_IOT_CLIENT_CERT, AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA  (bootstrap X.509)
 *   AZ_IOT_OPERATIONAL_KEY   (optional; default operational_key.pem)
 *   AZ_IOT_OPERATIONAL_CERT  (optional; default operational_cert.pem)
 *   AZ_IOT_DPS_REGISTRATION_PAYLOAD (optional; a JSON object sent with the
 *                                    registration request)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "az_iot_certificate_provider_managed.h"

#include "sample_utils.h"

typedef struct
{
  az_iot_connection_state conn_state;
  int issued;
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  /* Hub lifecycle only: the provisioning session reports on its own scope,
   * and storing its state here would overwrite the hub state this code acts
   * on. */
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }

  az_iot_connection_state s = event->state;
  az_iot_result reason = event->reason;
  (void)reason;
  ((user_context*)user_ctx)->conn_state = s;
}

/* The assignment's custom payload. The span points into the inbound message and
 * dies with this call, so anything worth keeping must be copied here. */
static void on_registration_payload(az_span payload, void* user_ctx)
{
  (void)user_ctx;
  fprintf(
      stderr,
      "[dps_csr] registration payload from the service: %.*s\n",
      (int)az_span_size(payload),
      (const char*)az_span_ptr(payload));
}

static void on_operational_cert(const az_iot_issued_certificate* issued, void* user_ctx)
{
  ((user_context*)user_ctx)->issued = 1;
  fprintf(
      stderr,
      "[dps_csr] operational certificate issued: %zu cert(s) in chain\n",
      issued ? issued->count : (size_t)0);
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sample_config config = { 0 };
  if (sample_config_load(&config) != 0)
  {
    return 1;
  }

  char* op_key = sample_env_dup("AZ_IOT_OPERATIONAL_KEY", "operational_key.pem");
  char* op_cert = sample_env_dup("AZ_IOT_OPERATIONAL_CERT", "operational_cert.pem");
  /* Declared with the other environment-derived strings, before the first
   * `goto cleanup`, so cleanup never frees an uninitialized pointer. */
  char* registration_payload = sample_env_dup("AZ_IOT_DPS_REGISTRATION_PAYLOAD", NULL);

  int rc = 1;
  user_context user_ctx = { 0 };
  az_iot_certificate_provider_managed provider = { 0 };
  az_iot_connection_client connection_client = { 0 };

  az_iot_certificate_provider_managed_options mopts = {
    .bootstrap_cert_pem_path = config.cert,
    .bootstrap_key_pem_path = config.key,
    .trusted_ca_pem_path = config.ca,
    .operational_key_pem_path = op_key,
    .operational_cert_pem_path = op_cert,
    .key_type = AZ_IOT_MANAGED_KEY_EC_P256,
  };
  if (az_iot_certificate_provider_managed_init(&provider, &mopts) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_csr] managed provider init failed\n");
    goto cleanup;
  }

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &config);
  copts.certificate_provider = &provider.base;
  AZ_IOT_CSR_PAYLOAD_STORAGE(csr_payload_buf);
  copts.csr_payload_buffer = az_span_create(csr_payload_buf, sizeof(csr_payload_buf));
  copts.dps.request_operational_certificate = true; /* opt in to CSR enrollment (D2) */

  /* Optional custom registration payload. It shares the register body with the
   * CSR, so the build buffer has to hold both -- which is what
   * AZ_IOT_DPS_REGISTRATION_BODY_STORAGE() sizes. */
  AZ_IOT_DPS_REGISTRATION_BODY_STORAGE(registration_body_buf);
  if (registration_payload != NULL && registration_payload[0] != '\0')
  {
    copts.dps.registration_payload
        = az_span_create((uint8_t*)registration_payload, (int32_t)strlen(registration_payload));
    copts.dps.registration_body_buffer
        = az_span_create(registration_body_buf, sizeof(registration_body_buf));
  }

  if (az_iot_connection_client_init(&connection_client, &copts) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  az_iot_connection_client_add_state_observer(&connection_client, on_conn_state, &user_ctx);
  az_iot_connection_client_set_operational_cert_callback(
      &connection_client, on_operational_cert, &user_ctx);
  az_iot_connection_client_set_registration_payload_callback(
      &connection_client, on_registration_payload, &user_ctx);

  if (az_iot_connection_client_register_mqtt_factory(
          &connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    goto cleanup;
  }

  if (az_iot_connection_client_open(&connection_client) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&connection_client, 50);
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(
        stderr,
        "[dps_csr] connected with %s identity\n",
        user_ctx.issued ? "operational" : "bootstrap");
    rc = 0;
  }

  az_iot_connection_client_close(&connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&connection_client, 50);
  }

cleanup:
  az_iot_connection_client_destroy(&connection_client);
  az_iot_certificate_provider_managed_destroy(&provider);
  free(op_key);
  free(op_cert);
  free(registration_payload);
  sample_config_release(&config);
  return rc;
}
