// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief SAS tokens supplied by the application, to DPS and to the hub.
 *
 * The SDK asks for a token before each connect that needs one and again at
 * az_iot_auth::sas::renewal_percent of its validity; it never sees the key. The callback
 * must not block, so it records the request and answers PENDING; the token is
 * produced outside it and handed over with
 * az_iot_connection_client_complete_sas_token(). With no clock yet it answers
 * UNAVAILABLE with a retry-after. Here the token is signed with OpenSSL to
 * stay runnable; replace sign_token() with your key store (TPM, HSM, secure
 * element) or a call to a token service. Needs no crypto backend and no clock
 * in the SDK.
 *
 * Proposed API: not built yet.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>
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

/** @brief Seconds to wait for a clock before asking again. */
#define SAMPLE_NO_CLOCK_RETRY_S 5u

/** @brief A token request answered PENDING; copied, since the request is not. */
typedef struct
{
  bool in_use; /**< A request awaits completion. */
  uint32_t request_id; /**< az_iot_sas_token_request::request_id. */
  az_iot_connection_scope scope; /**< Role the token is for. */
  char resource_uri[AZ_IOT_SAS_TOKEN_SIZE(256)]; /**< Copy of the `sr` value. */
  char key_name[32]; /**< Copy of the `skn` value. */
} pending_request;

/** @brief State shared with the callbacks. */
typedef struct
{
  key_store store; /**< Stand-in key store. */
  pending_request pending; /**< Request awaiting a token. */
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
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE when the token does not fit
 * @p out; AZ_IOT_ERR_INVALID_ARG for a resource URI longer than this sample
 * signs; AZ_IOT_ERR_INTERNAL when signing or encoding fails.
 */
static az_iot_result sign_token(
    const key_store* store,
    const char* resource_uri,
    const char* key_name,
    uint64_t expiry,
    char* out,
    size_t cap,
    size_t* out_len)
{
  char expiry_text[24];
  int expiry_len = snprintf(expiry_text, sizeof(expiry_text), "%llu", (unsigned long long)expiry);
  if (expiry_len <= 0 || (size_t)expiry_len >= sizeof(expiry_text))
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  char to_sign[AZ_IOT_SAS_TOKEN_SIZE(256)];
  size_t to_sign_len = 0;
  if (!put(to_sign, sizeof(to_sign), &to_sign_len, resource_uri, strlen(resource_uri))
      || !put(to_sign, sizeof(to_sign), &to_sign_len, "\n", 1)
      || !put(to_sign, sizeof(to_sign), &to_sign_len, expiry_text, (size_t)expiry_len))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* The HMAC and its base64 are the token's signature: wiped on every exit. */
  uint8_t mac[32];
  char sig_b64[64];
  unsigned int mac_len = 0;
  int32_t sig_len = 0;
  az_iot_result r = AZ_IOT_ERR_INTERNAL;
  if (HMAC(
          EVP_sha256(),
          store->key,
          (int)store->key_len,
          (const uint8_t*)to_sign,
          to_sign_len,
          mac,
          &mac_len)
          != NULL
      && mac_len == sizeof(mac)
      && !az_result_failed(az_base64_encode(
          az_span_create((uint8_t*)sig_b64, (int32_t)sizeof(sig_b64)),
          az_span_create(mac, (int32_t)sizeof(mac)),
          &sig_len)))
  {
    static const char k_prefix[] = "SharedAccessSignature sr=";
    size_t pos = 0;
    bool ok = put(out, cap, &pos, k_prefix, sizeof(k_prefix) - 1)
        && put(out, cap, &pos, resource_uri, strlen(resource_uri))
        && put(out, cap, &pos, "&sig=", 5)
        && put_url_encoded(out, cap, &pos, sig_b64, (size_t)sig_len)
        && put(out, cap, &pos, "&se=", 4) && put(out, cap, &pos, expiry_text, (size_t)expiry_len);
    if (ok && key_name[0] != '\0')
    {
      ok = put(out, cap, &pos, "&skn=", 5) && put(out, cap, &pos, key_name, strlen(key_name));
    }
    *out_len = pos;
    r = ok ? AZ_IOT_OK : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  OPENSSL_cleanse(mac, sizeof(mac));
  OPENSSL_cleanse(sig_b64, sizeof(sig_b64));
  return r;
}

/** @brief Current Unix time in @p now; false if the clock fails ((time_t)-1,
 * even if time_t is unsigned) or is not set (<= 0). */
static bool clock_now(time_t* now)
{
  *now = time(NULL);
  if (*now == (time_t)-1)
  {
    return false;
  }
  return *now > 0;
}

/**
 * @brief az_iot_sas_token_callback: records the request and answers PENDING.
 * Must return promptly; the token is produced by issue_pending_token().
 */
static void request_token(
    const az_iot_sas_token_request* request,
    char* token_buffer,
    size_t token_buffer_size,
    az_iot_sas_token_response* response,
    void* user_ctx)
{
  (void)token_buffer;
  (void)token_buffer_size;
  sample_context* ctx = (sample_context*)user_ctx;
  pending_request* p = &ctx->pending;
  time_t now;
  if (!clock_now(&now))
  {
    response->status = AZ_IOT_SAS_TOKEN_UNAVAILABLE; /* no clock yet */
    response->retry_after_seconds = SAMPLE_NO_CLOCK_RETRY_S;
    return;
  }
  if (strlen(request->resource_uri) >= sizeof(p->resource_uri)
      || strlen(request->key_name) >= sizeof(p->key_name))
  {
    response->status = AZ_IOT_SAS_TOKEN_UNAVAILABLE; /* 0: reconnection policy */
    return;
  }
  p->request_id = request->request_id;
  p->scope = request->scope;
  memcpy(p->resource_uri, request->resource_uri, strlen(request->resource_uri) + 1);
  memcpy(p->key_name, request->key_name, strlen(request->key_name) + 1);
  p->in_use = true;
  response->status = AZ_IOT_SAS_TOKEN_PENDING;
}

/**
 * @brief Produces the pending token and hands it to the client. Runs on the
 * do_work() thread; a real application would fetch or sign on a worker and
 * call this when the result arrives.
 */
static void issue_pending_token(az_iot_connection_client* client, sample_context* ctx)
{
  pending_request* p = &ctx->pending;
  if (!p->in_use)
  {
    return;
  }
  p->in_use = false;

  char token[AZ_IOT_SAS_TOKEN_SIZE(256)];
  size_t len = 0;
  az_iot_sas_token_response response = { 0 };
  time_t now;
  az_iot_result r = !clock_now(&now) ? AZ_IOT_ERR_BUSY /* UNAVAILABLE below */
                                     : sign_token(
                                           &ctx->store,
                                           p->resource_uri,
                                           p->key_name,
                                           (uint64_t)now + SAMPLE_TOKEN_LIFETIME_S,
                                           token,
                                           sizeof(token),
                                           &len);
  if (r == AZ_IOT_OK)
  {
    response.status = AZ_IOT_SAS_TOKEN_READY;
    response.token_len = len;
    response.valid_seconds = SAMPLE_TOKEN_LIFETIME_S;
  }
  else
  {
    response.status = AZ_IOT_SAS_TOKEN_UNAVAILABLE;
    /* No clock: retry soon. Other failures: 0, the reconnection policy decides. */
    response.retry_after_seconds = r == AZ_IOT_ERR_BUSY ? SAMPLE_NO_CLOCK_RETRY_S : 0u;
    len = 0;
  }
  r = az_iot_connection_client_complete_sas_token(
      client, p->request_id, response.status == AZ_IOT_SAS_TOKEN_READY ? token : NULL, &response);
  fprintf(
      stderr,
      "[user_provided_sas_token] %s token: %s\n",
      p->scope == AZ_IOT_CONN_SCOPE_DPS ? "DPS" : "hub",
      az_iot_result_to_string(r)); /* AZ_IOT_ERR_NOT_FOUND: request timed out */
  OPENSSL_cleanse(token, sizeof(token)); /* memset may be elided */
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  fprintf(
      stderr,
      "[user_provided_sas_token] %s: %s (%s)\n",
      event->scope == AZ_IOT_CONN_SCOPE_DPS ? "dps" : "hub",
      sample_connection_state_name(event->state),
      az_iot_result_to_string(event->reason));
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    ctx->hub_state = event->state;
  }
  /* Terminal: FAULTED, or hub IDLE with an error (no retry follows). A DPS
   * failure passes DISCONNECTING and IDLE before RETRY_PENDING or FAULTED. A
   * non-retriable RETRY_PENDING stops the sample unless it is the immediate
   * retry with the next credential (no delay). */
  bool falling_back = event->state == AZ_IOT_CONN_STATE_RETRY_PENDING && event->recovery != NULL
      && event->recovery->next_attempt_delay_ms == 0;
  if (event->reason != AZ_IOT_OK
      && (event->state == AZ_IOT_CONN_STATE_FAULTED
          || (event->scope == AZ_IOT_CONN_SCOPE_HUB && event->state == AZ_IOT_CONN_STATE_IDLE)
          || (event->state == AZ_IOT_CONN_STATE_RETRY_PENDING && !event->is_retriable
              && !falling_back)))
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
  sample_context ctx = { 0 };
  az_iot_connection_client client = { 0 };

  int32_t key_len = 0;
  if (id_scope == NULL || reg_id == NULL || key_b64 == NULL
      || az_result_failed(az_base64_decode(
          az_span_create(ctx.store.key, (int32_t)sizeof(ctx.store.key)),
          az_span_create_from_str(key_b64),
          &key_len)))
  {
    fprintf(
        stderr,
        "[user_provided_sas_token] set AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID and "
        "AZ_IOT_DPS_SYMMETRIC_KEY (base64)\n");
    goto cleanup;
  }
  ctx.store.key_len = (size_t)key_len;

  /* No keys: every token comes from the application. */
  az_iot_auth tokens = { 0 };
  tokens.sas.user_provided_token = request_token;
  tokens.sas.user_ctx = &ctx;

  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.client_id = reg_id;
  opts.dps.id_scope = id_scope;
  opts.dps.registration_id = reg_id;
  opts.dps_auth = tokens;
  opts.hub_auth = tokens;
  static uint8_t sas_buffer[AZ_IOT_SAS_BUFFER_SIZE(0, AZ_IOT_SAS_TOKEN_SIZE(256))]; /* no keys */
  opts.sas_buffer.buffer = sas_buffer;
  opts.sas_buffer.size = sizeof(sas_buffer);
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
    fprintf(stderr, "[user_provided_sas_token] connection client setup failed\n");
    goto cleanup;
  }

  uint64_t deadline = sample_now_ms() + SAMPLE_CONNECT_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED && !ctx.failed && sample_now_ms() < deadline)
  {
    (void)az_iot_connection_client_do_work(&client, SAMPLE_TICK_MS);
    issue_pending_token(&client, &ctx);
  }
  rc = ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED ? 0 : 1;
  fprintf(
      stderr,
      "[user_provided_sas_token] %s\n",
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
  OPENSSL_cleanse(&ctx.store, sizeof(ctx.store));
  free(id_scope);
  free(reg_id);
  free(key_b64);
  free(ca);
  return rc;
}
