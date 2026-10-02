// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief SAS tokens supplied by the application, to DPS and to the hub.
 *
 * The SDK asks for a token before each connect that needs one and again
 * before it expires; it never sees the key. Here the callback signs with
 * OpenSSL to stay runnable; replace sign_token() with your key store (TPM,
 * HSM, secure element) or a call to a token service. Needs no crypto backend
 * and no clock in the SDK.
 *
 * Proposed API: not built yet.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <azure/core/az_base64.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot.h"

#include "sample_utils.h"

/** @brief Lifetime of each token this sample issues, in seconds. */
#define SAMPLE_TOKEN_LIFETIME_S 3600u
/** @brief Longest wait for provisioning plus the hub connection. */
#define SAMPLE_CONNECT_TIMEOUT_MS 60000u
/** @brief Duration of one do_work() tick. */
#define SAMPLE_TICK_MS 50u

/** @brief Stand-in key store: holds the decoded device key. */
typedef struct
{
  uint8_t key[64]; /**< Device key. */
  size_t key_len; /**< Bytes used in key. */
} key_store;

/** @brief State shared with the state callback. */
typedef struct
{
  az_iot_connection_state hub_state; /**< Latest hub-scope state. */
  bool failed; /**< A failure retrying cannot fix. */
  az_iot_result failed_reason; /**< Reason of that failure. */
} sample_context;

/** @brief Appends @p n bytes of @p src at @p *pos in @p dst (capacity @p cap). */
static bool put(char* dst, size_t cap, size_t* pos, const char* src, size_t n)
{
  if (n > cap - *pos)
  {
    return false;
  }
  memcpy(dst + *pos, src, n);
  *pos += n;
  return true;
}

/** @brief URL-encodes the base64 characters that need it. */
static bool put_url_encoded(char* dst, size_t cap, size_t* pos, const char* src, size_t n)
{
  for (size_t i = 0; i < n; ++i)
  {
    const char* enc = src[i] == '+' ? "%2B" : src[i] == '/' ? "%2F" : src[i] == '=' ? "%3D" : NULL;
    if (!(enc != NULL ? put(dst, cap, pos, enc, 3) : put(dst, cap, pos, &src[i], 1)))
    {
      return false;
    }
  }
  return true;
}

/**
 * @brief Signs `<resource_uri>\n<expiry>` and formats the token. Replace with
 * your key store: only this function touches the key.
 */
static bool sign_token(
    const key_store* store,
    const az_iot_sas_token_request* request,
    uint64_t expiry,
    char* out,
    size_t cap,
    size_t* out_len)
{
  char expiry_text[24];
  int expiry_len = snprintf(expiry_text, sizeof(expiry_text), "%llu", (unsigned long long)expiry);
  char to_sign[256];
  size_t to_sign_len = 0;
  if (expiry_len <= 0
      || !put(
          to_sign,
          sizeof(to_sign),
          &to_sign_len,
          (const char*)az_span_ptr(request->resource_uri),
          (size_t)az_span_size(request->resource_uri))
      || !put(to_sign, sizeof(to_sign), &to_sign_len, "\n", 1)
      || !put(to_sign, sizeof(to_sign), &to_sign_len, expiry_text, (size_t)expiry_len))
  {
    return false;
  }

  uint8_t mac[32];
  unsigned int mac_len = 0;
  if (HMAC(
          EVP_sha256(),
          store->key,
          (int)store->key_len,
          (const uint8_t*)to_sign,
          to_sign_len,
          mac,
          &mac_len)
          == NULL
      || mac_len != sizeof(mac))
  {
    return false;
  }
  char sig_b64[64];
  int32_t sig_len = 0;
  if (az_result_failed(az_base64_encode(
          az_span_create((uint8_t*)sig_b64, (int32_t)sizeof(sig_b64)),
          az_span_create(mac, (int32_t)sizeof(mac)),
          &sig_len)))
  {
    return false;
  }

  static const char k_prefix[] = "SharedAccessSignature sr=";
  size_t pos = 0;
  bool ok = put(out, cap, &pos, k_prefix, sizeof(k_prefix) - 1)
      && put(out,
             cap,
             &pos,
             (const char*)az_span_ptr(request->resource_uri),
             (size_t)az_span_size(request->resource_uri))
      && put(out, cap, &pos, "&sig=", 5)
      && put_url_encoded(out, cap, &pos, sig_b64, (size_t)sig_len) && put(out, cap, &pos, "&se=", 4)
      && put(out, cap, &pos, expiry_text, (size_t)expiry_len);
  if (ok && az_span_size(request->key_name) > 0)
  {
    ok = put(out, cap, &pos, "&skn=", 5)
        && put(
             out,
             cap,
             &pos,
             (const char*)az_span_ptr(request->key_name),
             (size_t)az_span_size(request->key_name));
  }
  *out_len = pos;
  return ok;
}

/** @brief az_iot_sas_token_callback: issues a token for DPS or the hub. */
static az_iot_result get_token(
    const az_iot_sas_token_request* request,
    az_span token_buffer,
    int32_t* out_token_len,
    uint32_t* out_valid_seconds,
    void* user_ctx)
{
  const key_store* store = (const key_store*)user_ctx;
  uint64_t expiry = (uint64_t)time(NULL) + SAMPLE_TOKEN_LIFETIME_S;
  size_t len = 0;
  if (!sign_token(
          store,
          request,
          expiry,
          (char*)az_span_ptr(token_buffer),
          (size_t)az_span_size(token_buffer),
          &len))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  fprintf(
      stderr,
      "[sas_token_callback] issued a %s token\n",
      request->scope == AZ_IOT_CONN_SCOPE_DPS ? "DPS" : "hub");
  *out_token_len = (int32_t)len;
  *out_valid_seconds = SAMPLE_TOKEN_LIFETIME_S;
  return AZ_IOT_OK;
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  fprintf(
      stderr,
      "[sas_token_callback] %s: %s (%s)\n",
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

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  char* id_scope = sample_env_dup("AZ_IOT_DPS_ID_SCOPE", NULL);
  char* reg_id = sample_env_dup("AZ_IOT_DPS_REGISTRATION_ID", NULL);
  char* key_b64 = sample_env_dup("AZ_IOT_DPS_SYMMETRIC_KEY", NULL);
  char* ca = sample_env_dup("AZ_IOT_TRUSTED_CA", NULL);
  int rc = 1;
  key_store store = { 0 };
  sample_context ctx = { 0 };
  az_iot_connection_client client = { 0 };

  int32_t key_len = 0;
  if (id_scope == NULL || reg_id == NULL || key_b64 == NULL
      || az_result_failed(az_base64_decode(
          az_span_create(store.key, (int32_t)sizeof(store.key)),
          az_span_create_from_str(key_b64),
          &key_len)))
  {
    fprintf(
        stderr,
        "[sas_token_callback] set AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID and "
        "AZ_IOT_DPS_SYMMETRIC_KEY (base64)\n");
    goto cleanup;
  }
  store.key_len = (size_t)key_len;

  az_iot_auth tokens = { 0 };
  tokens.kind = AZ_IOT_AUTH_SAS_TOKEN_CALLBACK;
  tokens.sas_token.get_token = get_token;
  tokens.sas_token.user_ctx = &store;

  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.client_id = reg_id;
  opts.dps.id_scope = id_scope;
  opts.dps.registration_id = reg_id;
  opts.dps_auth = tokens;
  opts.hub_auth = tokens;
  opts.trusted_ca.path = ca;

  if (az_iot_connection_client_init(&client, &opts) != AZ_IOT_OK
      || az_iot_connection_client_add_state_observer(&client, on_conn_state, &ctx) != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(
             &client, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&client, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK
      || az_iot_connection_client_open(&client) != AZ_IOT_OK)
  {
    fprintf(stderr, "[sas_token_callback] connection client setup failed\n");
    goto cleanup;
  }

  uint64_t deadline = sample_now_ms() + SAMPLE_CONNECT_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED && !ctx.failed && sample_now_ms() < deadline)
  {
    (void)az_iot_connection_client_do_work(&client, SAMPLE_TICK_MS);
  }
  rc = ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED ? 0 : 1;
  fprintf(
      stderr,
      "[sas_token_callback] %s\n",
      rc == 0          ? "connected to the hub with an application-supplied token"
          : ctx.failed ? az_iot_result_to_string(ctx.failed_reason)
                       : "timeout");

  az_iot_connection_client_close(&client);
  deadline = sample_now_ms() + 5000u;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_IDLE && sample_now_ms() < deadline)
  {
    (void)az_iot_connection_client_do_work(&client, SAMPLE_TICK_MS);
  }

cleanup:
  az_iot_connection_client_deinit(&client);
  memset(&store, 0, sizeof(store));
  free(id_scope);
  free(reg_id);
  free(key_b64);
  free(ca);
  return rc;
}
