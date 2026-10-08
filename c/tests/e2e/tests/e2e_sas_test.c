// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* SAS end-to-end: a device in a DPS symmetric-key enrollment group registers
 * and connects to the assigned mqttv3 hub with SAS tokens the SDK signs from
 * the group key, then sends telemetry; renews the hub token; and does both
 * with tokens the application signs (on_sas_token_required).
 *
 * Environment: AZ_IOT_DPS_ID_SCOPE,
 * AZ_IOT_DPS_SAS_GROUP_KEY, AZ_IOT_DPS_SAS_REGISTRATION_ID, AZ_IOT_TRUSTED_CA;
 * optional AZ_IOT_DPS_GLOBAL_ENDPOINT. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "e2e_log.h"
#include "e2e_sas_device.h"

static void test_dps_and_hub_with_a_group_sas_key(void** state)
{
  (void)state;
  e2e_sas_config cfg;
  e2e_sas_config_load(&cfg);

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  e2e_sas_apply_dps(&cfg, &copts);
  /* The hub identity DPS creates has the keys derived from the group key. */
  copts.hub_auth = copts.dps_auth;

  e2e_sas_run run;
  e2e_sas_connect_and_send(&run, &copts, "e2e_sas");
  assert_int_equal(run.dps_source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
  assert_int_equal(run.hub_source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);

  e2e_sas_config_free(&cfg);
}

/* A 20 s hub token renewed at 50%: the session reconnects with a new token,
 * then sends telemetry. */
static void test_the_hub_sas_token_is_renewed(void** state)
{
  (void)state;
  e2e_sas_config cfg;
  e2e_sas_config_load(&cfg);

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  e2e_sas_apply_dps(&cfg, &copts);
  copts.hub_auth = copts.dps_auth;
  copts.hub_auth.sas.token_lifetime_seconds = 20;
  copts.hub_auth.sas.renewal_percent = 50;

  e2e_sas_run run;
  e2e_sas_connect_renew_and_send(&run, &copts, "e2e_sas_renewal", 1);
  assert_int_equal(run.hub_source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);

  e2e_sas_config_free(&cfg);
}

/** @brief Validity of each token the application signs, in seconds. */
#define E2E_USER_TOKEN_VALID_S 20u

/** @brief The device key, derived from the group key, held by the application,
 * and the run whose client takes the tokens. */
typedef struct
{
  uint8_t key[32];
  e2e_sas_run* run;
} e2e_user_key;

/** @brief Appends @p n bytes of @p src; false when @p cap is exceeded. */
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

/** @brief az_iot_sas_token_required_callback: signs the token and supplies it at once. */
static void sign_user_token(const az_iot_sas_token_request* request, void* user_ctx)
{
  const e2e_user_key* key = (const e2e_user_key*)user_ctx;
  char token[1024];
  size_t token_size = sizeof(token);
  char expiry[24];
  int expiry_len = snprintf(
      expiry, sizeof(expiry), "%llu", (unsigned long long)time(NULL) + E2E_USER_TOKEN_VALID_S);
  char to_sign[1024];
  int to_sign_len = snprintf(to_sign, sizeof(to_sign), "%s\n%s", request->resource_uri, expiry);
  uint8_t mac[32];
  unsigned int mac_len = 0;
  char sig[64];
  int32_t sig_len = 0;
  if (expiry_len <= 0 || to_sign_len <= 0 || (size_t)to_sign_len >= sizeof(to_sign)
      || HMAC(
             EVP_sha256(),
             key->key,
             (int)sizeof(key->key),
             (const uint8_t*)to_sign,
             (size_t)to_sign_len,
             mac,
             &mac_len)
          == NULL
      || az_result_failed(az_base64_encode(
          az_span_create((uint8_t*)sig, (int32_t)sizeof(sig)),
          az_span_create(mac, (int32_t)sizeof(mac)),
          &sig_len)))
  {
    return;
  }
  static const char k_prefix[] = "SharedAccessSignature sr=";
  size_t pos = 0;
  bool ok = put(token, token_size, &pos, k_prefix, sizeof(k_prefix) - 1)
      && put(token, token_size, &pos, request->resource_uri, strlen(request->resource_uri))
      && put(token, token_size, &pos, "&sig=", 5);
  for (int32_t i = 0; ok && i < sig_len; ++i)
  {
    const char* enc = sig[i] == '+' ? "%2B" : sig[i] == '/' ? "%2F" : sig[i] == '=' ? "%3D" : NULL;
    ok = enc != NULL ? put(token, token_size, &pos, enc, 3)
                     : put(token, token_size, &pos, &sig[i], 1);
  }
  ok = ok && put(token, token_size, &pos, "&se=", 4)
      && put(token, token_size, &pos, expiry, (size_t)expiry_len);
  if (ok && request->key_name[0] != '\0')
  {
    ok = put(token, token_size, &pos, "&skn=", 5)
        && put(token, token_size, &pos, request->key_name, strlen(request->key_name));
  }
  OPENSSL_cleanse(mac, sizeof(mac));
  OPENSSL_cleanse(sig, sizeof(sig));
  /* With no token, the attempt times out and is retried. */
  if (ok)
  {
    assert_int_equal(
        az_iot_connection_client_update_sas_token(
            key->run->client, request->scope, token, pos, E2E_USER_TOKEN_VALID_S),
        AZ_IOT_OK);
  }
  OPENSSL_cleanse(token, sizeof(token));
}

/* The same flow with tokens the application signs: DPS, the hub, and one hub
 * token renewal, then telemetry. */
static void test_dps_and_hub_with_user_provided_tokens(void** state)
{
  (void)state;
  e2e_sas_config cfg;
  e2e_sas_config_load(&cfg);

  uint8_t group_key[64];
  int32_t group_key_len = 0;
  assert_true(az_result_succeeded(az_base64_decode(
      az_span_create(group_key, (int32_t)sizeof(group_key)),
      az_span_create_from_str(cfg.group_key),
      &group_key_len)));
  e2e_user_key key;
  unsigned int key_len = 0;
  assert_non_null(HMAC(
      EVP_sha256(),
      group_key,
      group_key_len,
      (const uint8_t*)cfg.registration_id,
      strlen(cfg.registration_id),
      key.key,
      &key_len));
  OPENSSL_cleanse(group_key, sizeof(group_key));

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  e2e_sas_apply_dps(&cfg, &copts);
  e2e_sas_run run;
  key.run = &run;
  az_iot_auth tokens = { 0 };
  tokens.sas.on_sas_token_required = sign_user_token;
  tokens.sas.user_ctx = &key;
  tokens.sas.renewal_percent = 50;
  copts.dps_auth = tokens;
  copts.hub_auth = tokens;

  e2e_sas_connect_renew_and_send(&run, &copts, "e2e_sas_user_token", 1);
  assert_int_equal(run.dps_source, AZ_IOT_AUTH_SOURCE_USER_PROVIDED);
  assert_int_equal(run.hub_source, AZ_IOT_AUTH_SOURCE_USER_PROVIDED);

  OPENSSL_cleanse(&key, sizeof(key));
  e2e_sas_config_free(&cfg);
}

int main(void)
{
  e2e_install_log_sink();
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_dps_and_hub_with_a_group_sas_key),
    cmocka_unit_test(test_the_hub_sas_token_is_renewed),
    cmocka_unit_test(test_dps_and_hub_with_user_provided_tokens),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
