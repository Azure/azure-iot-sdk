// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* DPS TPM attestation end-to-end, with a TPM simulated in software
 * (samples/common/sample_tpm_sim.c): an individual enrollment for a new EK,
 * registration over HTTPS with az_iot_dps_tpm_client, then the assigned mqttv3
 * hub with SAS tokens the simulated TPM signs, and one telemetry message.
 * The enrollment, registration and device are deleted afterwards.
 *
 * Environment: AZ_IOT_DPS_ID_SCOPE, IOT_DPS_CONNECTION_STRING,
 * IOTHUB_CONNECTION_STRING, IOTHUB_EVENTHUB_CONNECTION_STRING; optional
 * AZ_IOT_DPS_GLOBAL_ENDPOINT, AZ_IOT_TRUSTED_CA. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>

#include <azure/core/az_base64.h>

/* The MQTT adapter this build of the suite uses (see tests/e2e/CMakeLists.txt). */
#if defined(AZ_IOT_E2E_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_E2E_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_E2E_ADAPTER_PAHO or AZ_IOT_E2E_ADAPTER_AZ_MQTT"
#endif
#include "azure/iot/az_iot.h"
#include "az_iot_crypto_openssl.h"
#include "az_iot_e2e_service.h"
#include "e2e_log.h"
#include "e2e_dps_tpm.h"
#include "sample_tpm_sim.h"
#include "sample_utils.h"
#include "support/test_env.h"

#define E2E_TPM_DEFAULT_ENDPOINT "global.azure-devices-provisioning.net"
#define E2E_TPM_TIMEOUT_MS 90000u
#define E2E_TPM_TOKEN_LIFETIME_S 3600u
#define E2E_TPM_TICK_MS 50u

typedef struct
{
  sample_tpm_sim* tpm;
  bool token_pending;
  char resource_uri[AZ_IOT_SAS_TOKEN_SIZE(256)];
  char key_name[32];
  int tokens_issued;
  bool token_failed; /**< Signing or handing over a token failed. */
  az_iot_connection_state hub_state;
  az_iot_result hub_reason;
  bool sent;
  az_iot_result send_status;
} e2e_tpm_ctx;

/** @brief e2e_https_send over the harness's TLS transport. */
static bool https_send(
    void* ctx,
    const char* host,
    const az_iot_dps_tpm_request* request,
    e2e_https_response* response)
{
  (void)ctx;
  static char body[E2E_DPS_TPM_RESPONSE_MAX + 1];
  int status = 0;
  size_t len = 0;
  unsigned retry_after = 0;
  if (!az_iot_e2e_https_request_auth(
          host,
          request->method,
          request->path,
          request->authorization,
          request->body != NULL ? AZ_IOT_DPS_TPM_CONTENT_TYPE : NULL,
          request->body,
          request->body_len,
          &status,
          body,
          sizeof(body),
          &len,
          &retry_after))
  {
    return false;
  }
  response->status = (uint16_t)status;
  response->retry_after_seconds = retry_after;
  memcpy(response->body, body, len);
  response->body_len = len;
  return true;
}

static void on_token_required(const az_iot_sas_token_request* request, void* user_ctx)
{
  e2e_tpm_ctx* ctx = (e2e_tpm_ctx*)user_ctx;
  if (sample_string_copy(ctx->resource_uri, sizeof(ctx->resource_uri), request->resource_uri)
      && sample_string_copy(ctx->key_name, sizeof(ctx->key_name), request->key_name))
  {
    ctx->token_pending = true;
  }
}

static void issue_token(az_iot_connection_client* client, e2e_tpm_ctx* ctx)
{
  if (!ctx->token_pending)
  {
    return;
  }
  ctx->token_pending = false;
  char token[AZ_IOT_SAS_TOKEN_SIZE(256)];
  size_t len = 0;
  if (e2e_dps_tpm_sign_token(
          ctx->tpm,
          ctx->resource_uri,
          ctx->key_name,
          (uint64_t)time(NULL) + E2E_TPM_TOKEN_LIFETIME_S,
          token,
          sizeof(token),
          &len)
          == AZ_IOT_OK
      && az_iot_connection_client_update_sas_token(
             client, AZ_IOT_CONN_SCOPE_HUB, token, len, E2E_TPM_TOKEN_LIFETIME_S)
          == AZ_IOT_OK)
  {
    ctx->tokens_issued++;
  }
  else
  {
    ctx->token_failed = true;
  }
  OPENSSL_cleanse(token, sizeof(token));
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  e2e_tpm_ctx* ctx = (e2e_tpm_ctx*)user_ctx;
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    ctx->hub_state = event->state;
    ctx->hub_reason = event->reason;
    fprintf(
        stderr,
        "[e2e_tpm] hub %s (%s)\n",
        az_iot_connection_state_to_string(event->state),
        az_iot_result_to_string(event->reason));
  }
}

static void on_sent(az_iot_result status, void* user_ctx)
{
  e2e_tpm_ctx* ctx = (e2e_tpm_ctx*)user_ctx;
  ctx->sent = true;
  ctx->send_status = status;
}

static void test_dps_tpm_registration_and_hub_sas(void** state)
{
  (void)state;
  char* id_scope = az_iot_test_env_dup("AZ_IOT_DPS_ID_SCOPE");
  char* endpoint = az_iot_test_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT");
  char* ca = az_iot_test_env_dup("AZ_IOT_TRUSTED_CA");
  assert_non_null(id_scope);
  const char* svc_error = NULL;
  az_iot_e2e_service* svc = az_iot_e2e_service_create(&svc_error);
  if (svc == NULL)
  {
    fail_msg("service: %s", svc_error);
  }

  /* A new simulated TPM: its EK is enrolled for this run only. */
  e2e_tpm_ctx ctx = { 0 };
  ctx.tpm = sample_tpm_sim_create(NULL);
  assert_non_null(ctx.tpm);
  const uint8_t *ek, *srk;
  size_t ek_len, srk_len;
  sample_tpm_sim_ek_public(ctx.tpm, &ek, &ek_len);
  sample_tpm_sim_srk_public(ctx.tpm, &srk, &srk_len);
  char reg_id[AZ_IOT_DPS_TPM_REGISTRATION_ID_SIZE];
  assert_int_equal(
      az_iot_dps_tpm_registration_id_from_ek(az_iot_crypto_openssl(), ek, ek_len, reg_id),
      AZ_IOT_OK);
  char ek_b64[1024];
  int32_t written = 0;
  assert_true(az_result_succeeded(az_base64_encode(
      az_span_create((uint8_t*)ek_b64, (int32_t)sizeof(ek_b64) - 1),
      az_span_create((uint8_t*)(uintptr_t)ek, (int32_t)ek_len),
      &written)));
  ek_b64[written] = '\0';
  fprintf(stderr, "[e2e_tpm] registration ID %s\n", reg_id);
  if (!az_iot_e2e_dps_tpm_enrollment_create(reg_id, ek_b64))
  {
    (void)az_iot_e2e_dps_enrollment_delete(reg_id); /* in case it was created */
    fail_msg("creating the TPM enrollment for %s failed", reg_id);
  }

  /* Registration over HTTPS. */
  az_iot_dps_tpm_client_options dps_opts = az_iot_dps_tpm_client_options_default();
  dps_opts.id_scope = id_scope;
  dps_opts.registration_id = reg_id;
  dps_opts.endorsement_key = ek;
  dps_opts.endorsement_key_len = ek_len;
  dps_opts.storage_root_key = srk;
  dps_opts.storage_root_key_len = srk_len;
  dps_opts.tpm = sample_tpm_sim_ops(ctx.tpm);
  /* From here on nothing asserts until cleanup has run. */
  az_iot_dps_tpm_client dps;
  static e2e_https_response response;
  az_iot_result reg = az_iot_dps_tpm_client_init(&dps, &dps_opts);
  if (reg == AZ_IOT_OK)
  {
    reg = e2e_dps_tpm_register(
        &dps,
        endpoint != NULL ? endpoint : E2E_TPM_DEFAULT_ENDPOINT,
        https_send,
        NULL,
        E2E_TPM_TIMEOUT_MS,
        &response);
  }
  const az_iot_dps_tpm_assignment* a
      = reg == AZ_IOT_OK ? az_iot_dps_tpm_client_get_assignment(&dps) : NULL;
  bool assigned = reg == AZ_IOT_OK && a != NULL;
  bool sent_ok = false;
  bool client_ok = false;
  char device_id[AZ_IOT_DPS_TPM_ID_MAX + 1] = { 0 };
  if (assigned)
  {
    fprintf(stderr, "[e2e_tpm] assigned to %s as %s\n", a->assigned_hub, a->device_id);
    (void)sample_string_copy(device_id, sizeof(device_id), a->device_id);
  }

  /* The assigned hub with TPM-signed SAS tokens. */
  if (assigned && a->connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V3 && a->hub_key_imported)
  {
    az_iot_connection_client_options opts = az_iot_connection_client_options_default();
    opts.iothub_hostname = a->assigned_hub;
    opts.device_id = a->device_id;
    opts.connection_profile = a->connection_profile;
    opts.hub_auth.sas.on_sas_token_required = on_token_required;
    opts.hub_auth.sas.user_ctx = &ctx;
    static uint8_t sas_buffer[AZ_IOT_SAS_BUFFER_SIZE(0, AZ_IOT_SAS_TOKEN_SIZE(256))];
    opts.sas_buffer.buffer = sas_buffer;
    opts.sas_buffer.size = sizeof(sas_buffer);
    opts.trusted_ca.path = ca;
    az_iot_connection_client client = { 0 };
    az_iot_mqttv3_telemetry_client telemetry = { 0 };
#if defined(AZ_IOT_E2E_ADAPTER_PAHO)
    az_iot_mqtt_factory* factory = az_iot_paho_factory_create_v3_1_1();
#elif defined(AZ_IOT_E2E_ADAPTER_AZ_MQTT)
    az_iot_mqtt_factory* factory = az_iot_az_mqtt_factory_create_v3_1_1();
#endif
    client_ok = factory != NULL && az_iot_connection_client_init(&client, &opts) == AZ_IOT_OK
        && az_iot_connection_client_add_state_observer(&client, on_conn_state, &ctx) == AZ_IOT_OK
        && az_iot_connection_client_register_mqtt_factory(&client, factory) == AZ_IOT_OK
        && az_iot_mqttv3_telemetry_client_init(&telemetry, &client) == AZ_IOT_OK
        && az_iot_connection_client_open(&client) == AZ_IOT_OK;

    uint64_t deadline = sample_now_ms() + E2E_TPM_TIMEOUT_MS;
    while (client_ok && !ctx.token_failed && ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED
           && ctx.hub_state != AZ_IOT_CONN_STATE_FAULTED && sample_now_ms() < deadline)
    {
      (void)az_iot_connection_client_do_work(&client, E2E_TPM_TICK_MS);
      issue_token(&client, &ctx);
    }
    if (ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      static const uint8_t payload[] = "{\"e2e\":\"dps_tpm\"}";
      az_iot_telemetry_message msg = { 0 };
      msg.payload = payload;
      msg.payload_len = sizeof(payload) - 1;
      if (az_iot_mqttv3_telemetry_client_send(&telemetry, &msg, on_sent, &ctx) == AZ_IOT_OK)
      {
        deadline = sample_now_ms() + E2E_TPM_TIMEOUT_MS;
        while (!ctx.sent && sample_now_ms() < deadline)
        {
          (void)az_iot_connection_client_do_work(&client, E2E_TPM_TICK_MS);
        }
      }
      sent_ok = ctx.sent && ctx.send_status == AZ_IOT_OK;
    }
    az_iot_connection_client_close(&client);
    deadline = sample_now_ms() + 5000u;
    while (ctx.hub_state != AZ_IOT_CONN_STATE_IDLE && sample_now_ms() < deadline)
    {
      (void)az_iot_connection_client_do_work(&client, E2E_TPM_TICK_MS);
    }
    az_iot_mqttv3_telemetry_client_deinit(&telemetry);
    az_iot_connection_client_deinit(&client);
#if defined(AZ_IOT_E2E_ADAPTER_PAHO)
    az_iot_paho_factory_destroy(factory);
#elif defined(AZ_IOT_E2E_ADAPTER_AZ_MQTT)
    az_iot_az_mqtt_factory_destroy(factory);
#endif
  }

  /* Clean up before asserting, so a failure leaves nothing behind. */
  bool enrollment_deleted = az_iot_e2e_dps_enrollment_delete(reg_id);
  /* The enrollment sets no deviceId, so DPS names the device after the registration ID;
   * delete it even when registration failed after DPS created it (404 counts as deleted). */
  bool device_deleted
      = az_iot_e2e_service_device_delete(svc, device_id[0] != '\0' ? device_id : reg_id);
  bool cleaned = enrollment_deleted && device_deleted;
  bool mqttv3 = assigned && a->connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
  bool hub_key = assigned && a->hub_key_imported;
  az_iot_e2e_service_destroy(svc);
  sample_tpm_sim_destroy(ctx.tpm);
  free(id_scope);
  free(endpoint);
  free(ca);

  assert_int_equal(reg, AZ_IOT_OK);
  assert_true(client_ok);
  assert_false(ctx.token_failed);
  assert_true(mqttv3);
  assert_true(hub_key);
  assert_true(ctx.tokens_issued > 0);
  assert_true(sent_ok);
  assert_true(cleaned);
}

int main(void)
{
  e2e_install_log_sink();
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_dps_tpm_registration_and_hub_sas),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
