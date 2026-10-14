// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief DPS registration with TPM attestation, then telemetry to the assigned
 * mqttv3 hub with SAS tokens signed by the TPM.
 *
 * DPS takes TPM attestation over HTTPS only, so registration runs
 * az_iot_dps_tpm_client over libcurl; the hub connection is then a direct
 * az_iot_connection_client connection. The TPM is simulated in software
 * (samples/common/sample_tpm_sim.c) so the sample runs anywhere: replace it
 * with your TSS for a real TPM.
 *
 * Run with `--enrollment` first to print the registration ID and endorsement
 * key for a DPS individual enrollment with TPM attestation.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include <openssl/crypto.h>

#include <azure/core/az_base64.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot.h"
#include "az_iot_crypto_openssl.h"

#include "sample_dps_tpm.h"
#include "sample_tpm_sim.h"
#include "sample_utils.h"

/** @brief Lifetime of each hub token, in seconds. */
#define SAMPLE_TOKEN_LIFETIME_S 3600u
#define SAMPLE_REGISTER_TIMEOUT_MS 120000u
#define SAMPLE_CONNECT_TIMEOUT_MS 60000u
#define SAMPLE_TICK_MS 50u
#define SAMPLE_DEFAULT_ENDPOINT "global.azure-devices-provisioning.net"

/** @brief A hub token request, copied out of the callback. */
typedef struct
{
  bool pending;
  char resource_uri[AZ_IOT_SAS_TOKEN_SIZE(256)];
  char key_name[32];
} token_request;

typedef struct
{
  sample_tpm_sim* tpm;
  token_request request;
  az_iot_connection_state hub_state;
  bool failed;
  az_iot_result failed_reason;
  bool sent;
  az_iot_result send_result;
} sample_context;

/* ---- HTTPS over libcurl ---------------------------------------------------- */

static size_t on_body(char* data, size_t size, size_t count, void* user)
{
  sample_https_response* r = (sample_https_response*)user;
  size_t n = size * count;
  if (n > sizeof(r->body) - r->body_len)
  {
    return 0; /* too large: fails the transfer */
  }
  memcpy(r->body + r->body_len, data, n);
  r->body_len += n;
  return n;
}

/** @brief Whether @p data starts with lowercase @p name, ignoring case. */
static bool header_is(const char* data, size_t len, const char* name)
{
  size_t n = strlen(name);
  if (len < n)
  {
    return false;
  }
  for (size_t i = 0; i < n; ++i)
  {
    char c = data[i];
    if ((c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c) != name[i])
    {
      return false;
    }
  }
  return true;
}

static size_t on_header(char* data, size_t size, size_t count, void* user)
{
  sample_https_response* r = (sample_https_response*)user;
  size_t n = size * count;
  static const char name[] = "retry-after:";
  if (n > sizeof(name) && header_is(data, n, name))
  {
    r->retry_after_seconds = (uint32_t)strtoul(data + sizeof(name) - 1, NULL, 10);
  }
  return n;
}

/** @brief Appends @p line to @p list, which is kept as it was on failure. */
static bool add_header(struct curl_slist** list, const char* line)
{
  struct curl_slist* appended = curl_slist_append(*list, line);
  if (appended == NULL)
  {
    return false;
  }
  *list = appended;
  return true;
}

static bool https_send(
    void* ctx,
    const char* host,
    const az_iot_dps_tpm_request* request,
    sample_https_response* response)
{
  const char* ca = (const char*)ctx;
  char url[1024];
  int n = snprintf(url, sizeof(url), "https://%s%s", host, request->path);
  if (n < 0 || (size_t)n >= sizeof(url))
  {
    return false;
  }
  char auth[AZ_IOT_SAS_TOKEN_SIZE(256) + 32];
  struct curl_slist* headers = NULL;
  bool ok = add_header(&headers, "Accept: application/json")
      && (request->body == NULL
          || add_header(&headers, "Content-Type: " AZ_IOT_DPS_TPM_CONTENT_TYPE));
  if (ok && request->authorization != NULL)
  {
    n = snprintf(auth, sizeof(auth), "Authorization: %s", request->authorization);
    ok = n > 0 && (size_t)n < sizeof(auth) && add_header(&headers, auth);
  }
  CURL* curl = ok ? curl_easy_init() : NULL;
  if (curl == NULL)
  {
    fprintf(stderr, "[dps_tpm] cannot prepare the HTTPS request\n");
    OPENSSL_cleanse(auth, sizeof(auth));
    curl_slist_free_all(headers);
    return false;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request->method);
  if (request->body != NULL)
  {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request->body_len);
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  if (ca != NULL)
  {
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
  }
  long status = 0;
  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK)
  {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response->status = (uint16_t)status;
  }
  else
  {
    fprintf(stderr, "[dps_tpm] HTTPS failed: %s\n", curl_easy_strerror(rc));
  }
  OPENSSL_cleanse(auth, sizeof(auth));
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK;
}

/* ---- Hub connection -------------------------------------------------------- */

static void on_token_required(const az_iot_sas_token_request* request, void* user_ctx)
{
  token_request* p = &((sample_context*)user_ctx)->request;
  if (!sample_copy_str(p->resource_uri, sizeof(p->resource_uri), request->resource_uri)
      || !sample_copy_str(p->key_name, sizeof(p->key_name), request->key_name))
  {
    fprintf(stderr, "[dps_tpm] token request too long for this sample\n");
    return;
  }
  p->pending = true;
}

/** @brief Signs a pending hub token with the TPM and hands it to the client. */
static void issue_token(az_iot_connection_client* client, sample_context* ctx)
{
  if (!ctx->request.pending)
  {
    return;
  }
  ctx->request.pending = false;
  char token[AZ_IOT_SAS_TOKEN_SIZE(256)];
  size_t len = 0;
  az_iot_result r = sample_dps_tpm_sign_token(
      ctx->tpm,
      ctx->request.resource_uri,
      ctx->request.key_name,
      (uint64_t)time(NULL) + SAMPLE_TOKEN_LIFETIME_S,
      token,
      sizeof(token),
      &len);
  if (r == AZ_IOT_OK)
  {
    r = az_iot_connection_client_update_sas_token(
        client, AZ_IOT_CONN_SCOPE_HUB, token, len, SAMPLE_TOKEN_LIFETIME_S);
  }
  fprintf(stderr, "[dps_tpm] hub token: %s\n", az_iot_result_to_string(r));
  OPENSSL_cleanse(token, sizeof(token));
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }
  fprintf(
      stderr,
      "[dps_tpm] hub: %s (%s)\n",
      sample_connection_state_name(event->state),
      az_iot_result_to_string(event->reason));
  ctx->hub_state = event->state;
  if (event->reason != AZ_IOT_OK
      && (event->state == AZ_IOT_CONN_STATE_FAULTED || event->state == AZ_IOT_CONN_STATE_IDLE
          || (event->state == AZ_IOT_CONN_STATE_RETRY_PENDING && !event->is_retriable)))
  {
    ctx->failed = true;
    ctx->failed_reason = event->reason;
  }
}

static void on_sent(az_iot_result status, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  ctx->sent = true;
  ctx->send_result = status;
}

/** @brief Prints what a DPS individual enrollment with TPM attestation needs. */
static int print_enrollment(sample_tpm_sim* tpm, const char* reg_id)
{
  const uint8_t* ek = NULL;
  size_t ek_len = 0;
  sample_tpm_sim_ek_public(tpm, &ek, &ek_len);
  char ek_b64[1024];
  int32_t written = 0;
  if (az_result_failed(az_base64_encode(
          AZ_SPAN_FROM_BUFFER(ek_b64),
          az_span_create((uint8_t*)(uintptr_t)ek, (int32_t)ek_len),
          &written))
      || (size_t)written >= sizeof(ek_b64))
  {
    return 1;
  }
  ek_b64[written] = '\0';
  printf("registration ID: %s\nendorsement key: %s\n", reg_id, ek_b64);
  printf(
      "\naz iot dps enrollment create --dps-name <dps> --resource-group <group> "
      "--enrollment-id %s --attestation-type tpm --endorsement-key %s\n",
      reg_id,
      ek_b64);
  return 0;
}

int main(int argc, char** argv)
{
  az_iot_log_sink log = sample_log_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  char* id_scope = sample_env_dup("AZ_IOT_DPS_ID_SCOPE", NULL);
  char* state_dir = sample_env_dup("AZ_IOT_TPM_SIM_STATE", ".");
  char* endpoint = sample_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT", SAMPLE_DEFAULT_ENDPOINT);
  char* reg_id_env = sample_env_dup("AZ_IOT_DPS_REGISTRATION_ID", NULL);
  char* ca = sample_env_dup("AZ_IOT_TRUSTED_CA", NULL);
  bool enrollment_only = argc > 1 && strcmp(argv[1], "--enrollment") == 0;
  int rc = 1;
  sample_context ctx = { 0 };
  az_iot_connection_client client = { 0 };
  az_iot_mqttv3_telemetry_client telemetry = { 0 };
  az_iot_mqtt_factory* factory = NULL;
  static sample_https_response response;

  ctx.tpm = sample_tpm_sim_create(state_dir);
  if (ctx.tpm == NULL)
  {
    fprintf(stderr, "[dps_tpm] could not create the simulated TPM in %s\n", state_dir);
    goto cleanup;
  }
  const uint8_t *ek, *srk;
  size_t ek_len, srk_len;
  sample_tpm_sim_ek_public(ctx.tpm, &ek, &ek_len);
  sample_tpm_sim_srk_public(ctx.tpm, &srk, &srk_len);
  char reg_id[AZ_IOT_DPS_TPM_REGISTRATION_ID_SIZE];
  if (reg_id_env != NULL)
  {
    if (!sample_copy_str(reg_id, sizeof(reg_id), reg_id_env))
    {
      goto cleanup;
    }
  }
  else if (
      az_iot_dps_tpm_registration_id_from_ek(az_iot_crypto_openssl(), ek, ek_len, reg_id)
      != AZ_IOT_OK)
  {
    goto cleanup;
  }
  if (enrollment_only)
  {
    rc = print_enrollment(ctx.tpm, reg_id);
    goto cleanup;
  }
  if (id_scope == NULL)
  {
    fprintf(stderr, "[dps_tpm] set AZ_IOT_DPS_ID_SCOPE (run with --enrollment first)\n");
    goto cleanup;
  }

  /* 1. Register with DPS over HTTPS. */
  az_iot_dps_tpm_client_options dps_opts = az_iot_dps_tpm_client_options_default();
  dps_opts.id_scope = id_scope;
  dps_opts.registration_id = reg_id;
  dps_opts.endorsement_key = ek;
  dps_opts.endorsement_key_len = ek_len;
  dps_opts.storage_root_key = srk;
  dps_opts.storage_root_key_len = srk_len;
  dps_opts.tpm = sample_tpm_sim_ops(ctx.tpm);
  az_iot_dps_tpm_client dps;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  az_iot_result r = az_iot_dps_tpm_client_init(&dps, &dps_opts);
  if (r == AZ_IOT_OK)
  {
    r = sample_dps_tpm_register(
        &dps, endpoint, https_send, ca, SAMPLE_REGISTER_TIMEOUT_MS, &response);
  }
  curl_global_cleanup();
  const az_iot_dps_tpm_assignment* a = az_iot_dps_tpm_client_get_assignment(&dps);
  if (r != AZ_IOT_OK || a == NULL)
  {
    goto cleanup;
  }
  fprintf(stderr, "[dps_tpm] assigned to %s as %s\n", a->assigned_hub, a->device_id);
  if (a->connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V3 || !a->hub_key_imported)
  {
    fprintf(
        stderr,
        "[dps_tpm] connectionProfile \"%s\": this sample signs SAS tokens for mqttv3 hubs only\n",
        a->connection_profile_raw);
    goto cleanup;
  }

  /* 2. Connect to the assigned hub; the TPM signs each token. */
  az_iot_auth hub_auth = { 0 };
  hub_auth.sas.on_sas_token_required = on_token_required;
  hub_auth.sas.user_ctx = &ctx;
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.host = a->assigned_hub;
  opts.client_id = a->device_id;
  opts.connection_profile = a->connection_profile;
  opts.hub_auth = hub_auth;
  static uint8_t sas_buffer[AZ_IOT_SAS_BUFFER_SIZE(0, AZ_IOT_SAS_TOKEN_SIZE(256))];
  opts.sas_buffer.buffer = sas_buffer;
  opts.sas_buffer.size = sizeof(sas_buffer);
  opts.trusted_ca.path = ca;

  factory = az_iot_paho_factory_create_v3_1_1();
  if (az_iot_connection_client_init(&client, &opts) != AZ_IOT_OK
      || az_iot_connection_client_add_state_observer(&client, on_conn_state, &ctx) != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&client, factory) != AZ_IOT_OK
      || az_iot_mqttv3_telemetry_client_init(&telemetry, &client) != AZ_IOT_OK
      || az_iot_connection_client_open(&client) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_tpm] connection client setup failed\n");
    goto cleanup;
  }
  uint64_t deadline = sample_now_ms() + SAMPLE_CONNECT_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED && !ctx.failed && sample_now_ms() < deadline)
  {
    (void)az_iot_connection_client_do_work(&client, SAMPLE_TICK_MS);
    issue_token(&client, &ctx);
  }

  /* 3. One telemetry message. */
  if (ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    static const uint8_t payload[] = "{\"attestation\":\"tpm\"}";
    az_iot_telemetry_message msg = { 0 };
    msg.payload = payload;
    msg.payload_len = sizeof(payload) - 1;
    if (az_iot_mqttv3_telemetry_client_send(&telemetry, &msg, on_sent, &ctx) == AZ_IOT_OK)
    {
      deadline = sample_now_ms() + SAMPLE_CONNECT_TIMEOUT_MS;
      while (!ctx.sent && sample_now_ms() < deadline)
      {
        (void)az_iot_connection_client_do_work(&client, SAMPLE_TICK_MS);
      }
    }
  }
  rc = ctx.sent && ctx.send_result == AZ_IOT_OK ? 0 : 1;
  fprintf(
      stderr,
      "[dps_tpm] %s\n",
      rc == 0          ? "telemetry sent with a TPM-signed SAS token"
          : ctx.failed ? az_iot_result_to_string(ctx.failed_reason)
                       : "failed");

  az_iot_connection_client_close(&client);
  deadline = sample_now_ms() + 5000u;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_IDLE && sample_now_ms() < deadline)
  {
    (void)az_iot_connection_client_do_work(&client, SAMPLE_TICK_MS);
  }

cleanup:
  az_iot_mqttv3_telemetry_client_deinit(&telemetry);
  az_iot_connection_client_deinit(&client);
  az_iot_paho_factory_destroy(factory);
  sample_tpm_sim_destroy(ctx.tpm);
  free(id_scope);
  free(state_dir);
  free(endpoint);
  free(reg_id_env);
  free(ca);
  return rc;
}
