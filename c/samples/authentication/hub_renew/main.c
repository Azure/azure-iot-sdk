// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* authentication/hub_renew
 *
 * Runtime operational-certificate renewal against a connected MQTTv3 hub
 * (D7). Connects to whichever hub DPS assigns; renewal through send_csr() is
 * MQTTv3-only, so on an MQTTv5 hub the sample reports that and exits non-zero.
 * End to end:
 *   1. Connect, then call az_iot_connection_client_send_csr() with no CSR: the
 *      client takes it from the managed provider.
 *   2. The hub responds in two phases - ACCEPTED (202) then ISSUED (200) with
 *      the new chain. Because the provider made the CSR, the client stores the
 *      chain there and reports the outcome.
 *   3. Apply the renewed cert by reconnecting: close + reopen re-establishes the
 *      session with the new operational identity (a new client certificate
 *      requires a fresh TLS handshake). Without this, the next reconnect
 *      applies it.
 *
 * Requires the managed provider (OpenSSL 3.0+).
 *
 * Environment:
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID
 *   AZ_IOT_CLIENT_CERT, AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA  (bootstrap X.509)
 *   AZ_IOT_OPERATIONAL_KEY   (optional; default operational_key.pem)
 *   AZ_IOT_OPERATIONAL_CERT  (optional; default operational_cert.pem)
 */
#include <stdio.h>
#include <stdlib.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "az_iot_certificate_provider_managed.h"

#include "sample_utils.h"

typedef struct
{
  az_iot_connection_state conn_state;
  int provisioning_faulted;
  int csr_done;
  az_iot_result csr_status;
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  /* A rejected assignment or a failed registration faults the provisioning
   * lifecycle and leaves the hub IDLE, so the wait below must watch for it. */
  if (event->scope == AZ_IOT_CONN_SCOPE_DPS)
  {
    if (event->state == AZ_IOT_CONN_STATE_FAULTED)
    {
      ((user_context*)user_ctx)->provisioning_faulted = 1;
    }
    return;
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
  (void)reason;
  ((user_context*)user_ctx)->conn_state = s;
}

static void on_csr_event(const az_iot_csr_event* evt, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  switch (evt->kind)
  {
    case AZ_IOT_CSR_ACCEPTED:
      fprintf(stderr, "[hub_renew] hub accepted CSR; signing in progress\n");
      break;
    case AZ_IOT_CSR_ISSUED:
      fprintf(
          stderr,
          "[hub_renew] renewed chain issued: %zu cert(s)\n",
          evt->issued ? evt->issued->count : (size_t)0);
      /* The provider made the CSR, so the client already stored the chain there. */
      fprintf(stderr, "[hub_renew] stored in provider: status=%d\n", (int)evt->store_status);
      ctx->csr_status = evt->store_status;
      ctx->csr_done = 1;
      break;
    case AZ_IOT_CSR_FAILED:
    default:
      fprintf(
          stderr,
          "[hub_renew] CSR failed: status=%d service_code=%d retry_after=%us\n",
          (int)evt->status,
          (int)evt->service_code,
          evt->retry_after_s);
      ctx->csr_status = evt->status;
      ctx->csr_done = 1;
      break;
  }
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
    fprintf(stderr, "[hub_renew] managed provider init failed\n");
    goto cleanup;
  }

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &config);
  copts.certificate_provider = &provider.base;
  AZ_IOT_CSR_PAYLOAD_STORAGE(csr_payload_buf);
  copts.csr_payload_buffer = az_span_create(csr_payload_buf, sizeof(csr_payload_buf));

  if (az_iot_connection_client_init(&connection_client, &copts) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  az_iot_connection_client_add_state_observer(&connection_client, on_conn_state, &user_ctx);

  /* Both adapters: v3.1.1 serves DPS and an MQTTv3 hub, v5 serves an MQTTv5 hub. */
  if (az_iot_connection_client_register_mqtt_factory(
          &connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    goto cleanup;
  }
  if (az_iot_connection_client_register_mqtt_factory(
          &connection_client, az_iot_paho_factory_create_v5())
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
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED || user_ctx.provisioning_faulted)
    {
      break;
    }
  }

  az_iot_connection_profile profile = AZ_IOT_CONNECTION_PROFILE_UNKNOWN;
  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED
      && sample_get_hub_profile(&connection_client, &profile) == AZ_IOT_OK
      && profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V3)
  {
    /* send_csr() is MQTTv3-only and returns AZ_IOT_ERR_NOT_SUPPORTED here. */
    fprintf(stderr, "[hub_renew] runtime renewal is not available on this hub generation\n");
  }
  else if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    /* No CSR: the client takes it from the provider, which keeps the key. */
    if (az_iot_connection_client_send_csr(
            &connection_client, NULL, NULL, NULL, on_csr_event, &user_ctx)
        == AZ_IOT_OK)
    {
      for (int i = 0; i < 1200 && !user_ctx.csr_done; ++i)
      {
        (void)az_iot_connection_client_do_work(&connection_client, 50);
      }
    }
  }

  /* Apply the renewed certificate now. The client stored it in the provider,
   * which serves it as the OPERATIONAL identity; a new client certificate needs
   * a fresh TLS handshake, so close and reopen. The reopen reconnects to the
   * same hub (no re-provisioning) with the renewed cert. */
  if (user_ctx.csr_done && user_ctx.csr_status == AZ_IOT_OK)
  {
    fprintf(stderr, "[hub_renew] renewal complete; reconnecting to apply the new certificate\n");

    az_iot_connection_client_close(&connection_client);
    for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
    {
      (void)az_iot_connection_client_do_work(&connection_client, 50);
    }

    if (az_iot_connection_client_open(&connection_client) == AZ_IOT_OK)
    {
      for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
      {
        (void)az_iot_connection_client_do_work(&connection_client, 50);
        if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED || user_ctx.provisioning_faulted)
        {
          break;
        }
      }
      if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
      {
        fprintf(stderr, "[hub_renew] reconnected with the renewed operational certificate\n");
        rc = 0;
      }
    }
  }

  az_iot_connection_client_close(&connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&connection_client, 50);
  }

cleanup:
  az_iot_connection_client_deinit(&connection_client);
  az_iot_certificate_provider_managed_deinit(&provider);
  free(op_key);
  free(op_cert);
  sample_config_release(&config);
  return rc;
}
