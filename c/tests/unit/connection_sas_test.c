// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* SAS authentication: token format, key handling, init() validation, and the
 * CONNECT credentials DPS and the mqttv3 hub receive. Built once per crypto
 * backend; reference tokens were computed independently (Python hmac). */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "internal/sas.h"
#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"

#if defined(AZ_IOT_TEST_CRYPTO_MBEDTLS)
#include "az_iot_crypto_mbedtls.h"
#define TEST_CRYPTO() az_iot_crypto_mbedtls()
#else
#include "az_iot_crypto_openssl.h"
#define TEST_CRYPTO() az_iot_crypto_openssl()
#endif

/* Bytes 0..31, and 0..63 as a group key. */
#define KEY_B64 "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
#define GROUP_KEY_B64                                                                    \
  "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+" \
  "Pw=="
#define NOW 1700000000u
#define HUB_TOKEN                                                                       \
  "SharedAccessSignature sr=broker.example%2Fdevices%2Fut-device&sig=gqgacRa%2FNHRRK0%" \
  "2FCjw6CUls%2BkHtn3nhT2hrASNZQan0%3D&se=1700003600"
#define DPS_TOKEN                                                                        \
  "SharedAccessSignature sr=0ne00000001%2fregistrations%2fut-device&sig=nfgjIUfmDHBv8oC" \
  "GcLAcWu2fieJcGyogj%2F8V3iBMPHo%3D&se=1700003600&skn=registration"
#define GROUP_DPS_TOKEN                                                               \
  "SharedAccessSignature sr=0ne00000001%2fregistrations%2fut-device&sig=9nM2SzDCAOl%" \
  "2FzsYjmXT%2Fu5JwUmodICuLzA2jNfMfHAg%3D&se=1700000600&skn=registration"

typedef struct
{
  az_iot_connection_client client;
  az_iot_mqtt_factory* factory;
  bool initialized;
  size_t events;
  az_iot_connection_state last_state;
  az_iot_auth_source last_source;
} fixture;

static uint64_t fixed_time(void* user_ctx)
{
  (void)user_ctx;
  return NOW;
}

static uint64_t no_time(void* user_ctx)
{
  (void)user_ctx;
  return 0;
}

static void unused_token_callback(
    const az_iot_sas_token_request* request,
    char* token_buffer,
    size_t token_buffer_size,
    az_iot_sas_token_response* response,
    void* user_ctx)
{
  (void)request;
  (void)token_buffer;
  (void)token_buffer_size;
  (void)response;
  (void)user_ctx;
}

static void on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  fx->events++;
  fx->last_state = event->state;
  fx->last_source = event->auth_source;
}

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);
  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);
  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  bool adopted = fx->initialized && fx->client.factory_count > 0;
  if (fx->initialized)
  {
    az_iot_connection_client_deinit(&fx->client);
  }
  if (!adopted && fx->factory != NULL)
  {
    az_iot_mock_mqtt_factory_destroy(fx->factory);
  }
  free(fx);
  return 0;
}

/** @brief Direct mqttv3 hub options authenticating with KEY_B64 only. */
static az_iot_connection_client_options hub_sas_options(void)
{
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "ut-device";
  opts.crypto = TEST_CRYPTO();
  opts.hub_auth.sas.primary_key_base64 = KEY_B64;
  opts.unix_time.get_time = fixed_time;
  return opts;
}

/** @brief DPS options authenticating with @p key only. */
static az_iot_connection_client_options dps_sas_options(const char* key)
{
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000001";
  opts.dps.registration_id = "ut-device";
  opts.crypto = TEST_CRYPTO();
  opts.dps_auth.sas.primary_key_base64 = key;
  opts.hub_auth.sas.primary_key_base64 = key;
  opts.unix_time.get_time = fixed_time;
  return opts;
}

static void init_and_open(fixture* fx, const az_iot_connection_client_options* opts)
{
  assert_int_equal(az_iot_connection_client_init(&fx->client, opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, on_state, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
}

static const az_iot_mock_call* last_connect(const fixture* fx)
{
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  const az_iot_mock_call* call = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(call);
  return call;
}

/* ---- token construction ------------------------------------------------- */

static void hub_token_matches_reference(void** state)
{
  (void)state;
  uint8_t key[AZ_IOT_SAS_KEY_MAX];
  size_t key_len = 0;
  assert_int_equal(az_iot_sas__decode_key(KEY_B64, key, sizeof(key), &key_len), AZ_IOT_OK);
  assert_int_equal(key_len, 32);
  char uri[256];
  assert_int_equal(
      az_iot_sas__resource_uri(false, "broker.example", "ut-device", uri, sizeof(uri)), AZ_IOT_OK);
  assert_string_equal(uri, "broker.example%2Fdevices%2Fut-device");
  char token[AZ_IOT_SAS_TOKEN_BUF];
  assert_int_equal(
      az_iot_sas__build_token(
          TEST_CRYPTO(), key, key_len, uri, "", NOW + 3600u, token, sizeof(token)),
      AZ_IOT_OK);
  assert_string_equal(token, HUB_TOKEN);
}

static void dps_token_matches_reference(void** state)
{
  (void)state;
  uint8_t key[AZ_IOT_SAS_KEY_MAX];
  size_t key_len = 0;
  assert_int_equal(az_iot_sas__decode_key(KEY_B64, key, sizeof(key), &key_len), AZ_IOT_OK);
  char uri[256];
  assert_int_equal(
      az_iot_sas__resource_uri(true, "0ne00000001", "ut-device", uri, sizeof(uri)), AZ_IOT_OK);
  char token[AZ_IOT_SAS_TOKEN_BUF];
  assert_int_equal(
      az_iot_sas__build_token(
          TEST_CRYPTO(), key, key_len, uri, "registration", NOW + 3600u, token, sizeof(token)),
      AZ_IOT_OK);
  assert_string_equal(token, DPS_TOKEN);
}

static void resource_uri_url_encodes_ids(void** state)
{
  (void)state;
  char uri[64];
  assert_int_equal(az_iot_sas__resource_uri(false, "h", "d@v 1/x", uri, sizeof(uri)), AZ_IOT_OK);
  assert_string_equal(uri, "h%2Fdevices%2Fd%40v%201%2Fx");
  assert_int_equal(
      az_iot_sas__resource_uri(false, "h", "", uri, sizeof(uri)), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas__resource_uri(false, "h", "d@v 1/x", uri, 10), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void token_too_large_is_rejected_and_wiped(void** state)
{
  (void)state;
  uint8_t key[AZ_IOT_SAS_KEY_MAX];
  size_t key_len = 0;
  assert_int_equal(az_iot_sas__decode_key(KEY_B64, key, sizeof(key), &key_len), AZ_IOT_OK);
  char token[100];
  memset(token, 'x', sizeof(token));
  assert_int_equal(
      az_iot_sas__build_token(
          TEST_CRYPTO(),
          key,
          key_len,
          "broker.example%2Fdevices%2Fut-device",
          "",
          NOW,
          token,
          sizeof(token)),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  for (size_t i = 0; i < sizeof(token); ++i)
  {
    assert_int_equal(token[i], 0);
  }
}

static void decode_key_rejects_bad_input(void** state)
{
  (void)state;
  uint8_t key[AZ_IOT_SAS_KEY_MAX];
  size_t key_len = 7;
  assert_int_equal(az_iot_sas__decode_key("", key, sizeof(key), &key_len), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(key_len, 0);
  assert_int_equal(
      az_iot_sas__decode_key("not base64!", key, sizeof(key), &key_len), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_sas__decode_key(KEY_B64, key, 16, &key_len), AZ_IOT_ERR_INVALID_ARG);
}

/* ---- init() validation --------------------------------------------------- */

static void init_rejects_invalid_sas_options(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.primary_key_base64 = NULL;
  opts.hub_auth.sas.secondary_key_base64 = KEY_B64;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);

  opts = hub_sas_options();
  opts.crypto = NULL;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);

  opts = hub_sas_options();
  opts.hub_auth.sas.renewal_percent = 100;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);

  opts = hub_sas_options();
  opts.trusted_ca.pem = "-----BEGIN CERTIFICATE-----";
  opts.trusted_ca.path = "ca.pem";
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);

  opts = hub_sas_options();
  opts.hub_auth.sas.primary_key_base64 = "not base64!";
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);

  opts = hub_sas_options();
  opts.client_id = NULL;
  opts.hub_auth.sas.is_enrollment_group_key = true;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);
}

static void init_does_not_support_user_provided_token_yet(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.user_provided_token = unused_token_callback;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_NOT_SUPPORTED);
}

/* ---- CONNECT credentials ------------------------------------------------- */

static void hub_connects_with_a_sas_token_when_only_a_key_is_set(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.trusted_ca.path = "ca.pem";
  init_and_open(fx, &opts);

  const az_iot_mock_call* call = last_connect(fx);
  assert_string_equal(call->password, HUB_TOKEN);
  assert_non_null(strstr(call->username, "broker.example/ut-device/?api-version="));
  assert_true(call->connect.use_tls);
  assert_string_equal(call->connect.trusted_ca_path, "ca.pem");
  assert_string_equal(call->connect.client_cert_path, "");
  assert_false(call->connect.has_client_cert_pem);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->client, 0), AZ_IOT_OK);
  assert_int_equal(fx->last_state, AZ_IOT_CONN_STATE_CONNECTED);
  assert_int_equal(fx->last_source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
}

static void x509_from_the_provider_is_tried_before_sas(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_test_provider provider;
  az_iot_test_provider_init(&provider, "provider-ca.pem");
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider.base;
  init_and_open(fx, &opts);

  const az_iot_mock_call* call = last_connect(fx);
  assert_string_equal(call->password, "");
  assert_string_equal(call->connect.trusted_ca_path, "provider-ca.pem");
  assert_int_equal(fx->last_source, AZ_IOT_AUTH_SOURCE_X509);
}

static void trusted_ca_overrides_the_provider_ca(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_test_provider provider;
  az_iot_test_provider_init(&provider, "provider-ca.pem");
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider.base;
  opts.trusted_ca.path = "ca.pem";
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->connect.trusted_ca_path, "ca.pem");
}

static void no_unix_time_fails_the_attempt_with_busy(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.unix_time.get_time = no_time;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_BUSY);
}

static void dps_connects_with_a_sas_token(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  init_and_open(fx, &opts);

  const az_iot_mock_call* call = last_connect(fx);
  assert_string_equal(call->password, DPS_TOKEN);
  assert_non_null(strstr(call->username, "0ne00000001/registrations/ut-device/api-version="));
  assert_true(call->connect.use_tls);
  assert_string_equal(call->connect.client_cert_path, "");
}

static void dps_group_key_signs_with_the_derived_device_key(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(GROUP_KEY_B64);
  opts.dps_auth.sas.is_enrollment_group_key = true;
  opts.dps_auth.sas.token_lifetime_seconds = 600;
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, GROUP_DPS_TOKEN);
}

static void no_provider_and_no_key_is_credential_incomplete(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.primary_key_base64 = NULL;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_CREDENTIAL_INCOMPLETE);
}

static void deinit_wipes_keys_and_tokens(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.secondary_key_base64 = KEY_B64;
  init_and_open(fx, &opts);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_HUB].primary_key_len, 32);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_HUB].secondary_key_len, 32);
  az_iot_connection_client_deinit(&fx->client);
  fx->initialized = false;
  fx->factory = NULL; /* adopted and freed by deinit() */
  const uint8_t* p = (const uint8_t*)fx->client.auth;
  for (size_t i = 0; i < sizeof(fx->client.auth); ++i)
  {
    assert_int_equal(p[i], 0);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(hub_token_matches_reference),
    cmocka_unit_test(dps_token_matches_reference),
    cmocka_unit_test(resource_uri_url_encodes_ids),
    cmocka_unit_test(token_too_large_is_rejected_and_wiped),
    cmocka_unit_test(decode_key_rejects_bad_input),
    cmocka_unit_test_setup_teardown(init_rejects_invalid_sas_options, setup, teardown),
    cmocka_unit_test_setup_teardown(init_does_not_support_user_provided_token_yet, setup, teardown),
    cmocka_unit_test_setup_teardown(
        hub_connects_with_a_sas_token_when_only_a_key_is_set, setup, teardown),
    cmocka_unit_test_setup_teardown(x509_from_the_provider_is_tried_before_sas, setup, teardown),
    cmocka_unit_test_setup_teardown(trusted_ca_overrides_the_provider_ca, setup, teardown),
    cmocka_unit_test_setup_teardown(no_unix_time_fails_the_attempt_with_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(dps_connects_with_a_sas_token, setup, teardown),
    cmocka_unit_test_setup_teardown(
        dps_group_key_signs_with_the_derived_device_key, setup, teardown),
    cmocka_unit_test_setup_teardown(
        no_provider_and_no_key_is_credential_incomplete, setup, teardown),
    cmocka_unit_test_setup_teardown(deinit_wipes_keys_and_tokens, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
