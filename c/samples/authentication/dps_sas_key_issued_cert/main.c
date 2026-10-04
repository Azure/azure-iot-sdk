// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief SAS from a symmetric key to DPS; a DPS-issued X.509 certificate to
 * the assigned hub.
 *
 * The device registers with a SAS token and a CSR over an operational key the
 * managed provider owns, then connects to the assigned hub, on either
 * generation, with the issued certificate. The managed provider has no
 * bootstrap certificate: DPS uses SAS (dps_auth), the hub X.509 (hub_auth,
 * the default). Renewal: az_iot_connection_client_send_csr() on an mqttv3
 * hub, or re-provisioning with the key.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot.h"
#include "az_iot_certificate_provider_managed.h"
#include "az_iot_crypto_openssl.h"

#include "sample_utils.h"

/** @brief Longest wait for provisioning plus the hub connection. */
#define SAMPLE_CONNECT_TIMEOUT_MS 60000u
/** @brief Longest wait for telemetry send completion. */
#define SAMPLE_SEND_TIMEOUT_MS 30000u
/** @brief Duration of one do_work() tick. */
#define SAMPLE_TICK_MS 50u

/** @brief Configuration, read from the environment. */
typedef struct
{
  char* id_scope; /**< AZ_IOT_DPS_ID_SCOPE. */
  char* reg_id; /**< AZ_IOT_DPS_REGISTRATION_ID; also the device ID. */
  char* key; /**< AZ_IOT_DPS_SYMMETRIC_KEY, or NULL. */
  char* group_key; /**< AZ_IOT_DPS_ENROLLMENT_GROUP_KEY, or NULL. */
  char* ca; /**< AZ_IOT_TRUSTED_CA, or NULL for the default trust store. */
  char* endpoint; /**< AZ_IOT_DPS_GLOBAL_ENDPOINT, or NULL. */
  char* op_key; /**< AZ_IOT_OPERATIONAL_KEY. */
  char* op_cert; /**< AZ_IOT_OPERATIONAL_CERT. */
} sas_config;

/** @brief State shared with the callbacks. */
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

static void config_release(sas_config* c)
{
  free(c->id_scope);
  free(c->reg_id);
  free(c->key);
  free(c->group_key);
  free(c->ca);
  free(c->endpoint);
  free(c->op_key);
  free(c->op_cert);
  memset(c, 0, sizeof(*c));
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  fprintf(
      stderr,
      "[dps_sas_key_issued_cert] %s: %s (%s)\n",
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

static void on_operational_cert(const az_iot_issued_certificate* issued, void* user_ctx)
{
  ((sample_context*)user_ctx)->issued = true;
  fprintf(
      stderr,
      "[dps_sas_key_issued_cert] certificate issued (%zu in chain)\n",
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

/** @brief Sends one message on the generation DPS assigned. */
static az_iot_result send_telemetry(az_iot_connection_client* client, sample_context* ctx)
{
  az_iot_connection_profile profile = AZ_IOT_CONNECTION_PROFILE_UNKNOWN;
  az_iot_result r = sample_get_hub_profile(client, &profile);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  static const uint8_t payload[] = "{\"auth\":\"sas-key,x509-issued\"}";
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
  uint64_t deadline = sample_now_ms() + SAMPLE_SEND_TIMEOUT_MS;
  while (r == AZ_IOT_OK && !ctx->send_done && !ctx->failed && sample_now_ms() < deadline)
  {
    pump(client);
  }
  if (r == AZ_IOT_OK)
  {
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

  sas_config config = {
    .id_scope = sample_env_dup("AZ_IOT_DPS_ID_SCOPE", NULL),
    .reg_id = sample_env_dup("AZ_IOT_DPS_REGISTRATION_ID", NULL),
    .key = sample_env_dup("AZ_IOT_DPS_SYMMETRIC_KEY", NULL),
    .group_key = sample_env_dup("AZ_IOT_DPS_ENROLLMENT_GROUP_KEY", NULL),
    .ca = sample_env_dup("AZ_IOT_TRUSTED_CA", NULL),
    .endpoint = sample_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT", NULL),
    .op_key = sample_env_dup("AZ_IOT_OPERATIONAL_KEY", "operational_key.pem"),
    .op_cert = sample_env_dup("AZ_IOT_OPERATIONAL_CERT", "operational_cert.pem"),
  };
  if (!is_set(config.id_scope) || !is_set(config.reg_id)
      || is_set(config.key) == is_set(config.group_key))
  {
    fprintf(
        stderr,
        "[dps_sas_key_issued_cert] set AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID and exactly "
        "one of "
        "AZ_IOT_DPS_SYMMETRIC_KEY / AZ_IOT_DPS_ENROLLMENT_GROUP_KEY\n");
    config_release(&config);
    return 1;
  }

  int rc = 1;
  sample_context ctx = { 0 };
  az_iot_certificate_provider_managed operational = { 0 };
  az_iot_connection_client client = { 0 };

  /* CSR key and issued chain only; no bootstrap certificate. */
  az_iot_certificate_provider_managed_options op_opts = {
    .operational_key_pem_path = config.op_key,
    .operational_cert_pem_path = config.op_cert,
    .key_type = AZ_IOT_MANAGED_KEY_EC_P256,
  };
  if (az_iot_certificate_provider_managed_init(&operational, &op_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_sas_key_issued_cert] managed provider init failed\n");
    goto cleanup;
  }

  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.client_id = config.reg_id;
  opts.dps.id_scope = config.id_scope;
  opts.dps.registration_id = config.reg_id;
  opts.dps.global_endpoint = config.endpoint;
  opts.dps_auth.sas.primary_key_base64 = is_set(config.key) ? config.key : config.group_key;
  opts.dps_auth.sas.is_enrollment_group_key = is_set(config.group_key);
  opts.crypto = az_iot_crypto_openssl(); /* HMAC-SHA256 for the DPS tokens */
  /* One DPS key; a token for ID scope + registration ID up to 256 characters. */
  static uint8_t sas_buffer[AZ_IOT_SAS_BUFFER_SIZE(1, AZ_IOT_SAS_TOKEN_SIZE(256))];
  opts.sas_buffer.buffer = sas_buffer;
  opts.sas_buffer.size = sizeof(sas_buffer);
  opts.trusted_ca.path = config.ca;
  /* hub_auth zeroed: the hub uses only the certificate DPS issues. */
  opts.certificate_provider = &operational.base;
  opts.dps.request_operational_certificate = true;
  AZ_IOT_CSR_PAYLOAD_STORAGE(csr_payload_buf);
  opts.csr_payload_buffer = az_span_create(csr_payload_buf, sizeof(csr_payload_buf));

  if (az_iot_connection_client_init(&client, &opts) != AZ_IOT_OK
      || az_iot_connection_client_add_state_observer(&client, on_conn_state, &ctx) != AZ_IOT_OK
      || az_iot_connection_client_set_operational_cert_callback(&client, on_operational_cert, &ctx)
          != AZ_IOT_OK
      /* v3.1.1 serves DPS and an mqttv3 hub; v5 serves an mqttv5 hub. */
      || az_iot_connection_client_register_mqtt_factory(
             &client, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&client, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK
      || az_iot_connection_client_open(&client) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_sas_key_issued_cert] connection client setup failed\n");
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
        "[dps_sas_key_issued_cert] connected with the %s certificate\n",
        ctx.issued ? "newly issued" : "persisted");
    az_iot_result sent = send_telemetry(&client, &ctx);
    fprintf(stderr, "[dps_sas_key_issued_cert] telemetry: %s\n", az_iot_result_to_string(sent));
    rc = sent == AZ_IOT_OK ? 0 : 1;
  }
  else
  {
    fprintf(
        stderr,
        "[dps_sas_key_issued_cert] not connected: %s\n",
        ctx.failed ? az_iot_result_to_string(ctx.failed_reason) : "timeout");
  }

  az_iot_connection_client_close(&client);
  deadline = sample_now_ms() + 5000u;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_IDLE && sample_now_ms() < deadline)
  {
    pump(&client);
  }

cleanup:
  az_iot_connection_client_deinit(&client);
  az_iot_certificate_provider_managed_deinit(&operational);
  config_release(&config);
  return rc;
}
