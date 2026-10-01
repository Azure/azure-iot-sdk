// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief Symmetric key (SAS) to DPS, then a DPS-issued X.509 certificate to
 * the assigned hub.
 *
 * 1. Registers with DPS using a SAS token, sending a CSR over an operational
 *    key the managed provider owns.
 * 2. The managed provider persists the issued chain.
 * 3. Connects to the assigned hub with that certificate, on whichever
 *    generation DPS assigned, and sends one telemetry message.
 *
 * Two providers compose: the symmetric-key provider answers the onboarding
 * (DPS) role and forwards the operational role, CSR and certificate storage to
 * the managed provider, which has no bootstrap identity of its own.
 *
 * Renewal: on an MQTTv3 hub, az_iot_connection_client_send_csr() (see
 * hub_renew); on either generation, re-provisioning, which uses the symmetric
 * key again.
 *
 * Environment: as dps_symmetric_key, plus
 *   AZ_IOT_OPERATIONAL_KEY   operational key PEM (default operational_key.pem)
 *   AZ_IOT_OPERATIONAL_CERT  issued chain PEM (default operational_cert.pem)
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot.h"
#include "az_iot_certificate_provider_managed.h"
#include "az_iot_certificate_provider_symmetric_key.h"

#include "sample_utils.h"

/** @brief Longest wait for provisioning plus the hub connection. */
#define SAMPLE_CONNECT_TIMEOUT_MS 60000u
/** @brief Longest wait for telemetry send completion. */
#define SAMPLE_SEND_TIMEOUT_MS 30000u
/** @brief Longest wait for a graceful disconnect. */
#define SAMPLE_CLOSE_TIMEOUT_MS 5000u
/** @brief Duration of one do_work() tick. */
#define SAMPLE_TICK_MS 50u

/** @brief Sample configuration, read from the environment. */
typedef struct
{
  char* id_scope; /**< DPS ID scope. */
  char* reg_id; /**< Registration ID; also the device ID and the CSR CN. */
  char* key; /**< Individual enrollment key, or NULL. */
  char* group_key; /**< Enrollment-group key, or NULL. */
  char* ca; /**< Trusted CA path, or NULL. */
  char* dps_global_endpoint; /**< Provisioning endpoint, or NULL. */
  char* op_key; /**< Operational key path. */
  char* op_cert; /**< Issued chain path. */
} sas_csr_config;

/** @brief State shared with the client callbacks. */
typedef struct
{
  az_iot_connection_state hub_state; /**< Latest hub-scope state. */
  bool failed; /**< A failure retrying cannot fix. */
  az_iot_result failed_reason; /**< Reason of that failure. */
  bool issued; /**< DPS issued an operational certificate. */
  bool send_done; /**< Telemetry callback was invoked. */
  az_iot_result send_status; /**< Telemetry callback result. */
} sample_context;

static bool is_set(const char* s) { return s != NULL && s[0] != '\0'; }

/** @brief Loads @p c; returns true when the required values are present. */
static bool config_load(sas_csr_config* c)
{
  c->id_scope = sample_env_dup("AZ_IOT_DPS_ID_SCOPE", NULL);
  c->reg_id = sample_env_dup("AZ_IOT_DPS_REGISTRATION_ID", NULL);
  c->key = sample_env_dup("AZ_IOT_DPS_SYMMETRIC_KEY", NULL);
  c->group_key = sample_env_dup("AZ_IOT_DPS_ENROLLMENT_GROUP_KEY", NULL);
  c->ca = sample_env_dup("AZ_IOT_TRUSTED_CA", NULL);
  c->dps_global_endpoint = sample_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT", NULL);
  c->op_key = sample_env_dup("AZ_IOT_OPERATIONAL_KEY", "operational_key.pem");
  c->op_cert = sample_env_dup("AZ_IOT_OPERATIONAL_CERT", "operational_cert.pem");
  return is_set(c->id_scope) && is_set(c->reg_id) && (is_set(c->key) != is_set(c->group_key))
      && c->op_key != NULL && c->op_cert != NULL;
}

static void config_release(sas_csr_config* c)
{
  free(c->id_scope);
  free(c->reg_id);
  free(c->key);
  free(c->group_key);
  free(c->ca);
  free(c->dps_global_endpoint);
  free(c->op_key);
  free(c->op_cert);
  memset(c, 0, sizeof(*c));
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  fprintf(
      stderr,
      "[dps_sas_csr] %s: %s (%s)\n",
      event->scope == AZ_IOT_CONN_SCOPE_DPS ? "dps" : "hub",
      sample_connection_state_name(event->state),
      az_iot_result_to_string(event->reason));

  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    ctx->hub_state = event->state;
  }
  if (event->reason != AZ_IOT_OK
      && (event->state == AZ_IOT_CONN_STATE_FAULTED || !event->is_retriable))
  {
    ctx->failed = true;
    ctx->failed_reason = event->reason;
  }
}

/** @brief Called after the managed provider has persisted the issued chain. */
static void on_operational_cert(const az_iot_issued_certificate* issued, void* user_ctx)
{
  ((sample_context*)user_ctx)->issued = true;
  fprintf(
      stderr,
      "[dps_sas_csr] operational certificate issued (%zu cert(s))\n",
      issued != NULL ? issued->count : (size_t)0);
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  ctx->send_status = status;
  ctx->send_done = true;
}

/** @brief One do_work() tick of at least SAMPLE_TICK_MS. */
static void pump(az_iot_connection_client* client)
{
  uint64_t start = sample_now_ms();
  (void)az_iot_connection_client_do_work(client, SAMPLE_TICK_MS);
  uint64_t spent = sample_now_ms() - start;
  if (spent < SAMPLE_TICK_MS)
  {
    sample_sleep_ms((long)(SAMPLE_TICK_MS - spent));
  }
}

/** @brief Sends one message on the generation DPS assigned; returns the result. */
static az_iot_result send_telemetry(az_iot_connection_client* client, sample_context* ctx)
{
  az_iot_connection_profile profile = AZ_IOT_CONNECTION_PROFILE_UNKNOWN;
  az_iot_result r = sample_get_hub_profile(client, &profile);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  static const uint8_t payload[] = "{\"auth\":\"sas-onboarding,x509-operational\"}";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;

  az_iot_mqttv3_telemetry_client mqttv3 = { 0 };
  az_iot_mqttv5_telemetry_client mqttv5 = { 0 };
  bool v5 = profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  r = v5 ? az_iot_mqttv5_telemetry_client_init(&mqttv5, client)
         : az_iot_mqttv3_telemetry_client_init(&mqttv3, client);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  r = v5 ? az_iot_mqttv5_telemetry_client_send(&mqttv5, &msg, on_send_done, ctx)
         : az_iot_mqttv3_telemetry_client_send(&mqttv3, &msg, on_send_done, ctx);
  if (r == AZ_IOT_OK)
  {
    uint64_t deadline = sample_now_ms() + SAMPLE_SEND_TIMEOUT_MS;
    while (!ctx->send_done && !ctx->failed && sample_now_ms() < deadline)
    {
      pump(client);
    }
    r = ctx->send_done ? ctx->send_status : AZ_IOT_ERR_TIMEOUT;
  }

  if (v5)
  {
    az_iot_mqttv5_telemetry_client_deinit(&mqttv5);
  }
  else
  {
    az_iot_mqttv3_telemetry_client_deinit(&mqttv3);
  }
  return r;
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sas_csr_config config = { 0 };
  if (!config_load(&config))
  {
    fprintf(
        stderr,
        "[dps_sas_csr] set AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID and exactly one of "
        "AZ_IOT_DPS_SYMMETRIC_KEY / AZ_IOT_DPS_ENROLLMENT_GROUP_KEY\n");
    config_release(&config);
    return 1;
  }

  int rc = 1;
  sample_context ctx = { 0 };
  az_iot_certificate_provider_managed operational = { 0 };
  az_iot_certificate_provider_symmetric_key provider = { 0 };
  az_iot_connection_client client = { 0 };

  /* Operational role only: no bootstrap certificate. */
  az_iot_certificate_provider_managed_options op_opts = {
    .trusted_ca_pem_path = config.ca,
    .operational_key_pem_path = config.op_key,
    .operational_cert_pem_path = config.op_cert,
    .key_type = AZ_IOT_MANAGED_KEY_EC_P256,
  };
  if (az_iot_certificate_provider_managed_init(&operational, &op_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_sas_csr] managed provider init failed\n");
    goto cleanup;
  }

  /* Onboarding role: SAS. Everything operational forwards to `operational`. */
  az_iot_certificate_provider_symmetric_key_options key_opts
      = az_iot_certificate_provider_symmetric_key_options_default();
  key_opts.symmetric_key_base64 = is_set(config.key) ? config.key : config.group_key;
  key_opts.is_enrollment_group_key = is_set(config.group_key);
  key_opts.registration_id = config.reg_id;
  key_opts.trusted_ca_pem_path = config.ca;
  key_opts.operational = &operational.base;
  if (az_iot_certificate_provider_symmetric_key_init(&provider, &key_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_sas_csr] invalid symmetric key\n");
    goto cleanup;
  }

  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.client_id = config.reg_id;
  opts.dps.id_scope = config.id_scope;
  opts.dps.registration_id = config.reg_id;
  opts.dps.global_endpoint = config.dps_global_endpoint;
  opts.certificate_provider = &provider.base;
  opts.dps.request_operational_certificate = true;
  AZ_IOT_CSR_PAYLOAD_STORAGE(csr_payload_buf);
  opts.csr_payload_buffer = az_span_create(csr_payload_buf, sizeof(csr_payload_buf));

  if (az_iot_connection_client_init(&client, &opts) != AZ_IOT_OK
      || az_iot_connection_client_add_state_observer(&client, on_conn_state, &ctx) != AZ_IOT_OK
      || az_iot_connection_client_set_operational_cert_callback(&client, on_operational_cert, &ctx)
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(
             &client, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&client, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK
      || az_iot_connection_client_open(&client) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_sas_csr] connection client setup failed\n");
    goto cleanup;
  }

  uint64_t deadline = sample_now_ms() + SAMPLE_CONNECT_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED && !ctx.failed && sample_now_ms() < deadline)
  {
    pump(&client);
  }

  if (ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    /* Without `issued`, the hub took a certificate persisted by a previous run. */
    fprintf(
        stderr,
        "[dps_sas_csr] connected with the %s operational certificate\n",
        ctx.issued ? "newly issued" : "persisted");
    az_iot_result sent = send_telemetry(&client, &ctx);
    fprintf(stderr, "[dps_sas_csr] telemetry: %s\n", az_iot_result_to_string(sent));
    rc = sent == AZ_IOT_OK ? 0 : 1;
  }
  else
  {
    fprintf(
        stderr,
        "[dps_sas_csr] not connected: %s\n",
        ctx.failed ? az_iot_result_to_string(ctx.failed_reason) : "timeout");
  }

  az_iot_connection_client_close(&client);
  deadline = sample_now_ms() + SAMPLE_CLOSE_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_IDLE && sample_now_ms() < deadline)
  {
    pump(&client);
  }

cleanup:
  az_iot_connection_client_deinit(&client);
  az_iot_certificate_provider_symmetric_key_deinit(&provider);
  az_iot_certificate_provider_managed_deinit(&operational);
  config_release(&config);
  return rc;
}
