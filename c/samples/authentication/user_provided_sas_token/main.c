// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief SAS tokens supplied by the application, to DPS and to the hub.
 *
 * The SDK notifies when a token is needed: before each connect that needs one
 * and at az_iot_auth::sas::renewal_percent of its lifetime; it never sees the
 * key. The callback must not block, so it records the request; the token is
 * produced outside it and handed over with
 * az_iot_connection_client_update_sas_token(). With no clock yet the request
 * waits. The SDK formats the token (az_iot_sas_token.h); only its HMAC comes
 * from the key store, here OpenSSL to stay runnable. Replace key_store_hmac()
 * with your TPM, HSM or secure element, or sign_token() with a call to a token
 * service. Needs no crypto backend and no clock in the SDK.
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

/** @brief A token request; copied, since the request is valid only during the callback. */
typedef struct
{
  bool in_use; /**< A token is to be supplied. */
  char resource_uri[AZ_IOT_SAS_TOKEN_SIZE(256)]; /**< Copy of the `sr` value. */
  char key_name[32]; /**< Copy of the `skn` value. */
} pending_request;

/** @brief State shared with the callbacks. */
typedef struct
{
  key_store store; /**< Stand-in key store. */
  pending_request pending[AZ_IOT_CONN_SCOPE_COUNT]; /**< Requests by scope. */
  az_iot_connection_state hub_state; /**< Latest hub-scope state. */
  bool failed; /**< A failure retrying cannot fix. */
  az_iot_result failed_reason; /**< Reason of that failure. */
} sample_context;

/**
 * @brief Stand-in key store: HMAC-SHA256 of @p data with the device key.
 * Replace with your TPM, HSM or secure element; only this function touches
 * the key.
 */
static bool key_store_hmac(
    const key_store* store,
    const char* data,
    size_t data_len,
    uint8_t mac[AZ_IOT_SHA256_SIZE])
{
  unsigned int mac_len = 0;
  return HMAC(
             EVP_sha256(),
             store->key,
             (int)store->key_len,
             (const uint8_t*)data,
             data_len,
             mac,
             &mac_len)
      != NULL
      && mac_len == AZ_IOT_SHA256_SIZE;
}

/**
 * @brief Has the key store sign the request's string to sign, then formats
 * the token with the SDK.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE when the token does not fit
 * @p out; AZ_IOT_ERR_INTERNAL when signing fails.
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
  char to_sign[AZ_IOT_SAS_STRING_TO_SIGN_SIZE(AZ_IOT_SAS_TOKEN_SIZE(256))];
  size_t to_sign_len = 0;
  az_iot_result r = az_iot_sas_token_string_to_sign(
      resource_uri, expiry, to_sign, sizeof(to_sign), &to_sign_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  /* The MAC is the token's signature: wiped on every exit. */
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  r = key_store_hmac(store, to_sign, to_sign_len, mac)
      ? az_iot_sas_token_from_signature(resource_uri, key_name, expiry, mac, out, cap, out_len)
      : AZ_IOT_ERR_INTERNAL;
  OPENSSL_cleanse(mac, sizeof(mac));
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
 * @brief az_iot_sas_token_required_callback: records the request. Must return
 * promptly; the token is produced by issue_pending_tokens().
 */
static void request_token(const az_iot_sas_token_request* request, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  pending_request* p = &ctx->pending[request->scope];
  if (strlen(request->resource_uri) >= sizeof(p->resource_uri)
      || strlen(request->key_name) >= sizeof(p->key_name))
  {
    /* Not recorded: the attempt times out and is retried. */
    fprintf(stderr, "[user_provided_sas_token] resource URI too long for this sample\n");
    return;
  }
  memcpy(p->resource_uri, request->resource_uri, strlen(request->resource_uri) + 1);
  memcpy(p->key_name, request->key_name, strlen(request->key_name) + 1);
  p->in_use = true;
}

/**
 * @brief Produces the pending tokens and hands them to the client. Runs on the
 * do_work() thread; a real application would fetch or sign on a worker and
 * call update_sas_token() on this thread when the result arrives.
 */
static void issue_pending_tokens(az_iot_connection_client* client, sample_context* ctx)
{
  time_t now;
  if (!clock_now(&now))
  {
    return; /* no clock yet: the requests wait */
  }
  for (int i = 0; i < (int)AZ_IOT_CONN_SCOPE_COUNT; ++i)
  {
    pending_request* p = &ctx->pending[i];
    if (!p->in_use)
    {
      continue;
    }
    p->in_use = false;
    char token[AZ_IOT_SAS_TOKEN_SIZE(256)];
    size_t len = 0;
    az_iot_result r = sign_token(
        &ctx->store,
        p->resource_uri,
        p->key_name,
        (uint64_t)now + SAMPLE_TOKEN_LIFETIME_S,
        token,
        sizeof(token),
        &len);
    if (r == AZ_IOT_OK)
    {
      r = az_iot_connection_client_update_sas_token(
          client, (az_iot_connection_scope)i, token, len, SAMPLE_TOKEN_LIFETIME_S);
    }
    fprintf(
        stderr,
        "[user_provided_sas_token] %s token: %s\n",
        i == (int)AZ_IOT_CONN_SCOPE_DPS ? "DPS" : "hub",
        az_iot_result_to_string(r));
    OPENSSL_cleanse(token, sizeof(token)); /* memset may be elided */
  }
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
  char* endpoint = sample_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT", NULL);
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
  tokens.sas.on_sas_token_required = request_token;
  tokens.sas.user_ctx = &ctx;

  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.client_id = reg_id;
  opts.dps.id_scope = id_scope;
  opts.dps.registration_id = reg_id;
  opts.dps.global_endpoint = endpoint;
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
    issue_pending_tokens(&client, &ctx);
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
  free(endpoint);
  return rc;
}
