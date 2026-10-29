// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief DPS registration with TPM attestation, then telemetry to the assigned
 * mqttv3 hub with SAS tokens signed by the TPM.
 *
 * DPS takes TPM attestation over HTTPS or AMQP, not MQTT. This sample runs
 * az_iot_dps_tpm_client over libcurl (HTTPS); the hub connection is then a direct
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

/* The MQTT adapter this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_SAMPLE_ADAPTER_PAHO or AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT"
#endif
#include "azure/iot/az_iot.h"
#include "az_iot_crypto_openssl.h"

#include "sample_tpm_sim.h"
#include "sample_utils.h"

/** @brief Lifetime of each hub token, in seconds. */
#define SAMPLE_TOKEN_LIFETIME_S 3600u
#define SAMPLE_REGISTER_TIMEOUT_MS 120000u
#define SAMPLE_CONNECT_TIMEOUT_MS 60000u
#define SAMPLE_TICK_MS 50u
#define SAMPLE_DEFAULT_ENDPOINT "global.azure-devices-provisioning.net"
/** @brief Response bytes kept per DPS request. */
#define SAMPLE_RESPONSE_MAX 8192
/** @brief Longest resource URI signed here: a hub host, `%2Fdevices%2F` and a
 * fully percent-encoded device ID (the DPS registration URI is shorter). */
#define SAMPLE_URI_MAX (AZ_IOT_DPS_TPM_HOST_MAX + 13u + 3u * AZ_IOT_DPS_TPM_ID_MAX)

/** @brief A response to one DPS request. */
typedef struct
{
  uint16_t status; /**< HTTP status. */
  char body[SAMPLE_RESPONSE_MAX]; /**< Body; not terminated. */
  size_t body_len; /**< Bytes in body. */
  uint32_t retry_after_seconds; /**< Retry-After; 0 when absent. */
} https_response;

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
  https_response* r = (https_response*)user;
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

/**
 * @brief Parses a delay-seconds Retry-After value of @p len characters (not NUL-terminated).
 * @return False for an empty, non-numeric (e.g. HTTP-date) or out-of-range value.
 */
static bool parse_retry_after(const char* p, size_t len, uint32_t* out)
{
  size_t i = 0;
  while (i < len && (p[i] == ' ' || p[i] == '\t'))
  {
    ++i;
  }
  uint64_t v = 0;
  size_t digits = 0;
  for (; i < len && p[i] >= '0' && p[i] <= '9'; ++i, ++digits)
  {
    v = v * 10u + (uint64_t)(p[i] - '0');
    if (v > UINT32_MAX)
    {
      return false;
    }
  }
  while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n'))
  {
    ++i;
  }
  if (digits == 0 || i != len)
  {
    return false;
  }
  *out = (uint32_t)v;
  return true;
}

static size_t on_header(char* data, size_t size, size_t count, void* user)
{
  https_response* r = (https_response*)user;
  size_t n = size * count;
  static const char name[] = "retry-after:";
  uint32_t seconds = 0;
  if (header_is(data, n, name)
      && parse_retry_after(data + sizeof(name) - 1, n - (sizeof(name) - 1), &seconds))
  {
    r->retry_after_seconds = seconds;
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

/** @brief Sends @p request to `https://<host><request->path>` with the headers
 * it needs. @return false when no response was received. */
static bool https_send(
    const char* ca,
    const char* host,
    const az_iot_dps_tpm_request* request,
    https_response* response)
{
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

/* ---- DPS TPM registration -------------------------------------------------- */

/**
 * @brief Registers until assigned or failed, waiting as DPS asks between
 * requests: az_iot_dps_tpm_client_get_request() builds each request,
 * az_iot_dps_tpm_client_handle_response() consumes its response.
 *
 * @return AZ_IOT_OK when assigned; AZ_IOT_ERR_TIMEOUT after @p timeout_ms;
 * AZ_IOT_ERR_NOT_CONNECTED when HTTPS fails; the client's error otherwise.
 */
static az_iot_result register_with_dps(
    az_iot_dps_tpm_client* dps,
    const char* host,
    const char* ca,
    uint64_t timeout_ms,
    https_response* response)
{
  static char buffer[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE + 4096]; /* room for a CSR and payload */
  uint64_t deadline = sample_now_ms() + timeout_ms;
  while (sample_now_ms() < deadline)
  {
    az_iot_dps_tpm_request request;
    az_iot_result r = az_iot_dps_tpm_client_get_request(
        dps, (uint64_t)time(NULL), buffer, sizeof(buffer), &request);
    if (r != AZ_IOT_OK)
    {
      return r;
    }
    memset(response, 0, sizeof(*response));
    if (!https_send(ca, host, &request, response))
    {
      return AZ_IOT_ERR_NOT_CONNECTED;
    }
    fprintf(stderr, "[dps_tpm] %s %s -> %u\n", request.method, request.path, response->status);

    az_iot_dps_tpm_next next;
    r = az_iot_dps_tpm_client_handle_response(
        dps,
        response->status,
        az_span_create((uint8_t*)response->body, (int32_t)response->body_len),
        response->retry_after_seconds,
        &next);
    if (r != AZ_IOT_OK)
    {
      uint16_t status = 0;
      int32_t code = 0;
      az_iot_dps_tpm_client_get_error(dps, &status, &code);
      fprintf(
          stderr,
          "[dps_tpm] registration failed: %s (HTTP %u, errorCode %ld)\n",
          az_iot_result_to_string(r),
          status,
          (long)code);
      return r;
    }
    if (next.assigned)
    {
      return AZ_IOT_OK;
    }
    /* Wait as asked, but not past the deadline. */
    uint64_t now = sample_now_ms();
    uint64_t wait_ms = (uint64_t)next.retry_after_seconds * 1000u;
    uint64_t left_ms = deadline > now ? deadline - now : 0;
    sample_sleep_ms((long)(wait_ms < left_ms ? wait_ms : left_ms));
  }
  return AZ_IOT_ERR_TIMEOUT;
}

/**
 * @brief A SAS token for @p resource_uri and @p key_name, signed by the TPM's
 * imported key: az_iot_sas_token_string_to_sign(), TPM HMAC, then
 * az_iot_sas_token_from_signature().
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE; AZ_IOT_ERR_INTERNAL when the TPM fails.
 */
static az_iot_result sign_token_with_tpm(
    sample_tpm_sim* tpm,
    const char* resource_uri,
    const char* key_name,
    uint64_t expiry_unix_seconds,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  char to_sign[AZ_IOT_SAS_STRING_TO_SIGN_SIZE(SAMPLE_URI_MAX)];
  size_t to_sign_len = 0;
  az_iot_result r = az_iot_sas_token_string_to_sign(
      resource_uri, expiry_unix_seconds, to_sign, sizeof(to_sign), &to_sign_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  r = sample_tpm_sim_hmac(tpm, (const uint8_t*)to_sign, to_sign_len, mac)
      ? az_iot_sas_token_from_signature(
            resource_uri, key_name, expiry_unix_seconds, mac, out, out_size, out_len)
      : AZ_IOT_ERR_INTERNAL;
  OPENSSL_cleanse(mac, sizeof(mac));
  return r;
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
  az_iot_result r = sign_token_with_tpm(
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
      az_iot_connection_state_to_string(event->state),
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
  static https_response response;

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
  char reg_id[AZ_IOT_DPS_TPM_ID_MAX + 1]; /* also fits AZ_IOT_DPS_TPM_REGISTRATION_ID_SIZE */
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
    r = register_with_dps(&dps, endpoint, ca, SAMPLE_REGISTER_TIMEOUT_MS, &response);
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
  opts.iothub_hostname = a->assigned_hub;
  opts.device_id = a->device_id;
  opts.connection_profile = a->connection_profile;
  opts.hub_auth = hub_auth;
  static uint8_t sas_buffer[AZ_IOT_SAS_BUFFER_SIZE(0, AZ_IOT_SAS_TOKEN_SIZE(256))];
  opts.sas_buffer.buffer = sas_buffer;
  opts.sas_buffer.size = sizeof(sas_buffer);
  opts.trusted_ca.path = ca;

#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  factory = az_iot_paho_factory_create_v3_1_1();
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  factory = az_iot_az_mqtt_factory_create_v3_1_1();
#endif
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
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  az_iot_paho_factory_destroy(factory);
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  az_iot_az_mqtt_factory_destroy(factory);
#endif
  sample_tpm_sim_destroy(ctx.tpm);
  free(id_scope);
  free(state_dir);
  free(endpoint);
  free(reg_id_env);
  free(ca);
  return rc;
}
