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
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "internal/connection_client_internal.h"
#include "internal/mono_time.h"
#include "support/connection_test_harness.h"
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
/* Bytes 32..63: a second key. */
#define KEY2_B64 "ICEiIyQlJicoKSorLC0uLzAxMjM0NTY3ODk6Ozw9Pj8="
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

#define SECONDARY_HUB_TOKEN                                                               \
  "SharedAccessSignature sr=broker.example%2Fdevices%2Fut-device&sig=JwI0ymoXeU8O0yUvGj3" \
  "xw7NZqlr%2BwLYi%2FzyXWeQOgVA%3D&se=1700003600"
#define SECONDARY_DPS_TOKEN                                                              \
  "SharedAccessSignature sr=0ne00000001%2fregistrations%2fut-device&sig=XELqJ87dYfM7n59" \
  "twiOa9F9Zp0bkI7SwXa5fCHoFMAQ%3D&se=1700003600&skn=registration"
#define ENCODED_ID_HUB_TOKEN                                                               \
  "SharedAccessSignature sr=broker.example%2Fdevices%2Fd%40v%201%2Fx&sig=tcKOOmzVO8ju8u3X" \
  "MjhaYBAcaJFUnYwOhPtV5soYaiY%3D&se=1700003600"
#define GROUP_HUB_TOKEN                                                                    \
  "SharedAccessSignature sr=broker.example%2Fdevices%2Fut-device&sig=BRrnGeCm9r2C4ThgRfkk" \
  "iZDw7C0H8EGZaA%2BwJ03WJx8%3D&se=1700003600"

/* A provider with trust anchors but no certificate for any role, as the managed
 * provider is without a bootstrap identity before issuance. */
static az_iot_result ca_only_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  (void)self;
  (void)role;
  memset(out, 0, sizeof(*out));
  out->trusted_ca_path = "provider-ca.pem";
  return AZ_IOT_ERR_NOT_FOUND;
}

/* A provider that cannot load right now, as opposed to having no certificate. */
static az_iot_result not_ready_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  (void)self;
  (void)role;
  memset(out, 0, sizeof(*out));
  return AZ_IOT_ERR_NOT_INITIALIZED;
}

static void ca_only_release(az_iot_certificate_provider* self, az_iot_certificate_material* m)
{
  (void)self;
  (void)m;
}

static void ca_only_deinit(az_iot_certificate_provider* self) { (void)self; }

/* A provider whose certificate appears once g_late_cert_ready is set. */
static bool g_late_cert_ready;

static az_iot_result late_cert_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  (void)self;
  (void)role;
  memset(out, 0, sizeof(*out));
  return g_late_cert_ready ? AZ_IOT_OK : AZ_IOT_ERR_NOT_FOUND;
}

static const az_iot_certificate_provider_vtable k_late_cert_vtable = {
  .version = 1u,
  .load = late_cert_load,
  .release = ca_only_release,
  .deinit = ca_only_deinit,
};

static const az_iot_certificate_provider_vtable k_ca_only_vtable = {
  .version = 1u,
  .load = ca_only_load,
  .release = ca_only_release,
  .deinit = ca_only_deinit,
};

/* OPERATIONAL not ready; no BOOTSTRAP certificate. */
static az_iot_result operational_not_ready_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  (void)self;
  memset(out, 0, sizeof(*out));
  return role == AZ_IOT_CRED_OPERATIONAL ? AZ_IOT_ERR_NOT_INITIALIZED : AZ_IOT_ERR_NOT_FOUND;
}

static const az_iot_certificate_provider_vtable k_operational_not_ready_vtable = {
  .version = 1u,
  .load = operational_not_ready_load,
  .release = ca_only_release,
  .deinit = ca_only_deinit,
};

static const az_iot_certificate_provider_vtable k_not_ready_vtable = {
  .version = 1u,
  .load = not_ready_load,
  .release = ca_only_release,
  .deinit = ca_only_deinit,
};

typedef struct
{
  az_iot_connection_scope scope;
  az_iot_connection_state state;
  az_iot_auth_source source;
  az_iot_result reason;
  az_iot_connection_failure_class classification;
  uint32_t attempt;
  uint32_t delay_ms;
  bool renewal;
} recorded_event;

typedef struct
{
  az_iot_connection_client client;
  az_iot_mqtt_factory* factory;
  bool initialized;
  size_t events;
  az_iot_connection_state last_state;
  az_iot_auth_source last_source;
  recorded_event log[64];
  size_t log_count;
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
  if (fx->log_count < sizeof(fx->log) / sizeof(fx->log[0]))
  {
    recorded_event* e = &fx->log[fx->log_count++];
    e->scope = event->scope;
    e->state = event->state;
    e->source = event->auth_source;
    e->reason = event->reason;
    e->classification
        = event->recovery != NULL ? event->recovery->classification : AZ_IOT_CONN_FAILURE_NONE;
    e->attempt = event->recovery != NULL ? event->recovery->attempt : 0u;
    e->delay_ms = event->recovery != NULL ? event->recovery->next_attempt_delay_ms : 0u;
    e->renewal = event->is_credential_renewal;
  }
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
/* Room for two distinct keys and a token for IDs up to 256 characters. */
static uint8_t g_sas_buffer[AZ_IOT_SAS_BUFFER_SIZE(2, AZ_IOT_SAS_TOKEN_SIZE(256))];

static bool all_zero(const void* p, size_t n)
{
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i)
  {
    if (b[i] != 0)
    {
      return false;
    }
  }
  return true;
}

static az_iot_connection_client_options hub_sas_options(void)
{
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "ut-device";
  opts.crypto = TEST_CRYPTO();
  opts.hub_auth.sas.primary_key_base64 = KEY_B64;
  opts.unix_time.get_time = fixed_time;
  opts.sas_buffer.buffer = g_sas_buffer;
  opts.sas_buffer.size = sizeof(g_sas_buffer);
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
  opts.sas_buffer.buffer = g_sas_buffer;
  opts.sas_buffer.size = sizeof(g_sas_buffer);
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
  /* 96 bytes: over AZ_IOT_SAS_KEY_MAX. */
  opts.hub_auth.sas.primary_key_base64
      = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v"
        "MDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5f";
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);

  opts = hub_sas_options();
  opts.client_id = NULL;
  opts.hub_auth.sas.is_enrollment_group_key = true;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_INVALID_ARG);
}

/* A token callback alone needs no key, crypto or clock, but needs sas_buffer. */
static void init_accepts_a_token_callback_without_keys(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "ut-device";
  opts.hub_auth.sas.user_provided_token = unused_token_callback;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  opts.sas_buffer.buffer = g_sas_buffer;
  opts.sas_buffer.size = sizeof(g_sas_buffer);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
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

/* DPS re-provisioning replaces the hub host and device ID. The next CONNECT
 * must name the new ones, in both the username and the token, even when the
 * new strings have different lengths than the old. */
static void a_changed_hub_identity_is_used_on_the_next_connect(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  init_and_open(fx, &opts);
  assert_non_null(strstr(last_connect(fx)->password, "sr=broker.example%2Fdevices%2Fut-device&"));

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->client, 0), AZ_IOT_OK);
  az_iot_connection_client_close(&fx->client);
  for (int i = 0; i < 10 && fx->last_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    if (m != NULL)
    {
      (void)az_iot_mock_mqtt_client_inject_disconnected(m);
    }
    (void)az_iot_connection_client_do_work(&fx->client, 0);
  }
  assert_int_equal(fx->last_state, AZ_IOT_CONN_STATE_IDLE);
  assert_int_equal(
      az_iot_connection_client__set_host(&fx->client, "other-hub.example.net"), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__set_client_id(&fx->client, "dev-2"), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  const az_iot_mock_call* call = last_connect(fx);
  assert_non_null(strstr(call->username, "other-hub.example.net/dev-2/?api-version="));
  assert_non_null(strstr(call->password, "sr=other-hub.example.net%2Fdevices%2Fdev-2&"));
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

static void a_provider_that_cannot_load_does_not_fall_back_to_sas(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_certificate_provider provider = { .vtable = &k_not_ready_vtable };
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_NOT_INITIALIZED);
}

static void an_operational_error_is_not_hidden_by_a_missing_bootstrap(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_certificate_provider provider = { .vtable = &k_operational_not_ready_vtable };
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_NOT_INITIALIZED);
}

static void a_provider_ca_is_kept_when_the_role_falls_back_to_sas(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_certificate_provider provider = { .vtable = &k_ca_only_vtable };
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider;
  init_and_open(fx, &opts);

  const az_iot_mock_call* call = last_connect(fx);
  assert_string_equal(call->password, HUB_TOKEN);
  assert_string_equal(call->connect.trusted_ca_path, "provider-ca.pem");
  assert_string_equal(call->connect.client_cert_path, "");
}

static void hub_group_key_derives_from_client_id(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.primary_key_base64 = GROUP_KEY_B64;
  opts.hub_auth.sas.is_enrollment_group_key = true;
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, GROUP_HUB_TOKEN);
}

static void the_device_id_is_url_encoded_in_the_token(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.client_id = "d@v 1/x";
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, ENCODED_ID_HUB_TOKEN);
}

static uint64_t max_time(void* user_ctx)
{
  (void)user_ctx;
  return UINT64_MAX - (uint64_t)AZ_IOT_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS; /* 20-digit expiry */
}

/* Worst case for AZ_IOT_SAS_TOKEN_SIZE(): every ID character URL-encoded, a
 * 20-digit expiry, and the role with the longer infix and `skn`. A token area
 * of that size must be enough for both roles, and one byte less is refused
 * before signing. For DPS the bound must be exact except for the signature,
 * which the bound assumes is fully URL-encoded. */
static void the_token_size_macro_is_enough_for_the_worst_case(void** state)
{
  (void)state;
  /* 10 + 20 = 30 ID characters, all '/' (each encodes to 3 bytes). */
  static const char k_first[] = "//////////";
  static const char k_second[] = "////////////////////";
  enum
  {
    ids = sizeof(k_first) - 1 + sizeof(k_second) - 1
  };
  for (int dps = 0; dps <= 1; ++dps)
  {
    for (int shrink = 0; shrink <= 1; ++shrink)
    {
      static uint8_t buf[AZ_IOT_SAS_BUFFER_SIZE(1, AZ_IOT_SAS_TOKEN_SIZE(ids))];
      az_iot_connection_client client;
      az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
      az_iot_connection_client_options opts = { 0 };
      opts.crypto = TEST_CRYPTO();
      opts.unix_time.get_time = max_time;
      opts.sas_buffer.buffer = buf;
      opts.sas_buffer.size = sizeof(buf) - (size_t)shrink;
      if (dps)
      {
        opts.client_id = k_second;
        opts.dps.id_scope = k_first;
        opts.dps.registration_id = k_second;
        opts.dps_auth.sas.primary_key_base64 = KEY_B64;
        opts.hub_auth.sas.primary_key_base64 = KEY_B64;
      }
      else
      {
        opts.host = k_first;
        opts.client_id = k_second;
        opts.hub_auth.sas.primary_key_base64 = KEY_B64;
      }
      assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_OK);
      assert_int_equal(az_iot_connection_client_register_mqtt_factory(&client, factory), AZ_IOT_OK);
      az_iot_result r = az_iot_connection_client_open(&client);
      if (shrink)
      {
        assert_int_equal(r, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
      }
      else
      {
        assert_int_equal(r, AZ_IOT_OK);
        const az_iot_mock_call* call = az_iot_mock_mqtt_client_last_of(
            az_iot_mock_mqtt_factory_last_client(factory), AZ_IOT_MOCK_CALL_CONNECT);
        assert_non_null(call);
        size_t used = strlen(call->password) + 1u;
        assert_true(used <= AZ_IOT_SAS_TOKEN_SIZE(ids));
        if (dps)
        {
          /* DPS is the longer role: the only slack left is the signature's
           * unencoded characters, 2 bytes each. The bound assumes all 44
           * encode; a random HMAC rarely does, so only this is asserted. */
          const char* sig = strstr(call->password, "&sig=") + 5;
          size_t sig_len = (size_t)(strstr(sig, "&se=") - sig);
          size_t escapes = 0;
          for (size_t i = 0; i < sig_len; ++i)
          {
            escapes += sig[i] == '%' ? 1u : 0u;
          }
          size_t plain = sig_len - 3u * escapes;
          assert_int_equal(escapes + plain, 44);
          assert_int_equal(used + 2u * plain, AZ_IOT_SAS_TOKEN_SIZE(ids));
        }
      }
      az_iot_connection_client_deinit(&client);
    }
  }
}

static void a_token_that_does_not_fit_fails_and_is_wiped(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.sas_buffer.size = AZ_IOT_SAS_BUFFER_SIZE(1, AZ_IOT_SAS_KEY_MAX); /* smallest token area */
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_true(all_zero(fx->client.sas_token, fx->client.sas_token_size));
}

static void a_username_that_does_not_fit_fails_the_attempt(void** state)
{
  fixture* fx = (fixture*)*state;
  static char long_id[AZ_IOT_MQTT_USERNAME_BUF];
  memset(long_id, 'a', sizeof(long_id) - 1u);
  long_id[sizeof(long_id) - 1u] = '\0';
  az_iot_test_provider provider;
  az_iot_test_provider_init(&provider, "provider-ca.pem");
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.primary_key_base64 = NULL;
  opts.certificate_provider = &provider.base;
  opts.client_id = long_id;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void the_token_is_wiped_once_the_adapter_has_the_connect(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);
  assert_true(all_zero(fx->client.sas_token, fx->client.sas_token_size));
}

static void sas_keys_need_a_large_enough_buffer(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.sas_buffer.buffer = NULL;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  opts = hub_sas_options();
  opts.sas_buffer.size = AZ_IOT_SAS_BUFFER_SIZE(1, AZ_IOT_SAS_KEY_MAX) - 1u;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  /* Two distinct keys need two slots. */
  opts = hub_sas_options();
  opts.hub_auth.sas.secondary_key_base64 = GROUP_KEY_B64;
  opts.sas_buffer.size = AZ_IOT_SAS_BUFFER_SIZE(2, AZ_IOT_SAS_KEY_MAX) - 1u;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  opts.sas_buffer.size = AZ_IOT_SAS_BUFFER_SIZE(2, AZ_IOT_SAS_KEY_MAX);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  assert_int_equal(fx->client.sas_token_size, AZ_IOT_SAS_KEY_MAX);
  az_iot_connection_client_deinit(&fx->client);
}

/* az_span sizes are int32_t: a token area past INT32_MAX must be capped, or
 * az_span_create() would get a negative size and hit a precondition. */
static void the_token_area_is_capped_at_int32_max(void** state)
{
  (void)state;
  size_t fixed = AZ_IOT_SAS_BUFFER_SIZE(1, 0);
  assert_int_equal(
      az_iot_connection_client__sas_token_area(fixed + AZ_IOT_SAS_KEY_MAX, 1), AZ_IOT_SAS_KEY_MAX);
  assert_int_equal(
      az_iot_connection_client__sas_token_area(fixed + (size_t)INT32_MAX, 1), (size_t)INT32_MAX);
  assert_int_equal(az_iot_connection_client__sas_token_area(SIZE_MAX, 1), (size_t)INT32_MAX);
}

static void no_buffer_is_needed_without_sas(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_test_provider provider;
  az_iot_test_provider_init(&provider, NULL);
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.primary_key_base64 = NULL;
  opts.certificate_provider = &provider.base;
  opts.sas_buffer.buffer = NULL;
  opts.sas_buffer.size = 0;
  init_and_open(fx, &opts);
  assert_int_equal(fx->last_source, AZ_IOT_AUTH_SOURCE_X509);
}

static void identical_dps_and_hub_keys_share_one_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  static uint8_t one_key[AZ_IOT_SAS_BUFFER_SIZE(1, AZ_IOT_SAS_TOKEN_SIZE(256))];
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.sas_buffer.buffer = one_key;
  opts.sas_buffer.size = sizeof(one_key);
  init_and_open(fx, &opts);
  assert_ptr_equal(
      fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].primary_key,
      fx->client.auth[AZ_IOT_CONN_SCOPE_HUB].primary_key);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);
}

static uint64_t overflowing_time(void* user_ctx)
{
  (void)user_ctx;
  return UINT64_MAX - (uint64_t)AZ_IOT_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS + 1u;
}

static void a_time_that_would_overflow_the_expiry_fails_with_busy(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.unix_time.get_time = overflowing_time;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_ERR_BUSY);
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

static uint64_t switchable_time(void* user_ctx) { return *(const uint64_t*)user_ctx; }

/* Without a Unix time no token can be signed, so every retry fails while
 * setting up. Each is reported, with the step, under a retry-for-ever policy. */
static void each_retry_without_unix_time_is_reported(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.unix_time.get_time = switchable_time;
  opts.unix_time.user_ctx = &now;
  opts.reconnection_policy.initial_delay_ms = 20;
  opts.reconnection_policy.max_delay_ms = 20;
  opts.reconnection_policy.jitter_pct = 0;
  init_and_open(fx, &opts);
  az_iot_test_state_log log = { 0 };
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, az_iot_test_on_state, &log),
      AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  now = 0;
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    log.count = 0;
    fx->client.reconnect_due_ms = az_iot_time_mono_ms();
    (void)az_iot_connection_client_do_work(&fx->client, 0);
    assert_int_equal(log.count, 2);
    assert_int_equal(log.states[0], AZ_IOT_CONN_STATE_SETTING_UP);
    assert_int_equal(log.states[1], AZ_IOT_CONN_STATE_RETRY_PENDING);
    assert_int_equal(log.reasons[1], AZ_IOT_ERR_BUSY);
    assert_true(log.error_present[1]);
    assert_int_equal(log.error_sources[1], AZ_IOT_CONN_ERR_SRC_LOCAL);
    assert_string_equal(log.error_message[1], "SAS token signing failed");
  }
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

/* ---- credential fallback --------------------------------------------------- */

#define RECONNECT_MS 20u
#define IDENTITY_MS 30u

static void with_policies(az_iot_connection_client_options* opts)
{
  opts->reconnection_policy.initial_delay_ms = RECONNECT_MS;
  opts->reconnection_policy.max_delay_ms = RECONNECT_MS;
  opts->identity_recovery.policy.initial_delay_ms = IDENTITY_MS;
  opts->identity_recovery.policy.max_delay_ms = IDENTITY_MS;
  opts->identity_recovery.mode = AZ_IOT_IDENTITY_RECOVERY_RETRY_HUB;
}

static void pump(fixture* fx, int n)
{
  for (int i = 0; i < n; ++i)
  {
    (void)az_iot_connection_client_do_work(&fx->client, 0);
  }
}

static void connack(fixture* fx, az_iot_result status)
{
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, status));
  pump(fx, 3);
}

static void wait_and_fire_retry(fixture* fx)
{
  assert_int_not_equal(fx->client.reconnect_due_ms, 0);
  while (az_iot_time_mono_ms() < fx->client.reconnect_due_ms)
  {
  }
  pump(fx, 2);
}

/** @brief The newest @p state event of @p scope. */
static const recorded_event* last_event(
    const fixture* fx,
    az_iot_connection_scope scope,
    az_iot_connection_state state)
{
  const recorded_event* found = NULL;
  for (size_t i = fx->log_count; i > 0 && found == NULL; --i)
  {
    if (fx->log[i - 1].scope == scope && fx->log[i - 1].state == state)
    {
      found = &fx->log[i - 1];
    }
  }
  assert_non_null(found);
  return found;
}

/** @brief No attempt is in flight: the rejected one was torn down and none followed. */
static bool no_connect_pending(const fixture* fx)
{
  return az_iot_mock_mqtt_factory_last_client(fx->factory) == NULL;
}

static void hub_x509_rejected_falls_back_to_the_primary_key(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_test_provider provider;
  az_iot_test_provider_init(&provider, "provider-ca.pem");
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider.base;
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, "");

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->source, AZ_IOT_AUTH_SOURCE_X509);
  assert_int_equal(e->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(e->attempt, 0);
  assert_int_equal(e->delay_ms, 0);

  const az_iot_mock_call* call = last_connect(fx);
  assert_string_equal(call->password, HUB_TOKEN);
  assert_string_equal(call->connect.client_cert_path, "");
  assert_string_equal(call->connect.trusted_ca_path, "provider-ca.pem");
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->last_state, AZ_IOT_CONN_STATE_CONNECTED);
  assert_int_equal(fx->last_source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
}

/* Kept after connecting; when rejected later, the pass wraps to the sources
 * before it, then ends. */
static void a_kept_secondary_key_wraps_to_the_primary_when_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_string_equal(last_connect(fx)->password, SECONDARY_HUB_TOKEN);
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->last_source, AZ_IOT_AUTH_SOURCE_SECONDARY_KEY);

  /* A drop is not a rejection: the secondary key is kept. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(
      az_iot_mock_mqtt_factory_last_client(fx->factory)));
  pump(fx, 2);
  wait_and_fire_retry(fx);
  assert_string_equal(last_connect(fx)->password, SECONDARY_HUB_TOKEN);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_true(no_connect_pending(fx));
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
  assert_int_equal(e->delay_ms, IDENTITY_MS);
}

static void a_fully_rejected_pass_goes_to_identity_recovery_then_restarts(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_test_provider provider;
  az_iot_test_provider_init(&provider, "provider-ca.pem");
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider.base;
  opts.hub_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  init_and_open(fx, &opts);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_string_equal(last_connect(fx)->password, SECONDARY_HUB_TOKEN);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);

  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->source, AZ_IOT_AUTH_SOURCE_SECONDARY_KEY);
  assert_int_equal(e->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(e->attempt, 1);
  assert_int_equal(e->delay_ms, IDENTITY_MS);

  wait_and_fire_retry(fx);
  assert_string_equal(last_connect(fx)->password, "");
}

/* With retries disabled, open() still makes one full pass. */
static void fallback_does_not_need_a_reconnection_policy(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.secondary_key_base64 = KEY2_B64;
  init_and_open(fx, &opts);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_string_equal(last_connect(fx)->password, SECONDARY_HUB_TOKEN);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_int_equal(fx->last_state, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->last_source, AZ_IOT_AUTH_SOURCE_SECONDARY_KEY);
}

static void open_starts_again_at_the_first_source(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.secondary_key_base64 = KEY2_B64;
  init_and_open(fx, &opts);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->last_source, AZ_IOT_AUTH_SOURCE_SECONDARY_KEY);

  assert_int_equal(az_iot_connection_client_close(&fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(
      az_iot_mock_mqtt_factory_last_client(fx->factory)));
  pump(fx, 2);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);
}

static void dps_x509_rejected_falls_back_to_the_primary_key(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_test_provider provider;
  az_iot_test_provider_init(&provider, "provider-ca.pem");
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.certificate_provider = &provider.base;
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, "");

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->source, AZ_IOT_AUTH_SOURCE_X509);
  assert_int_equal(e->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(e->delay_ms, 0);

  const az_iot_mock_call* call = last_connect(fx);
  assert_string_equal(call->password, DPS_TOKEN);
  assert_string_equal(call->connect.client_cert_path, "");
  assert_string_equal(call->connect.trusted_ca_path, "provider-ca.pem");
}

/* provision_only: the pump reopens the session with the next source at once,
 * without a reconnection policy. */
static void a_provision_only_session_falls_back_to_the_secondary_key(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  opts.dps.provision_only = true;
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_string_equal(last_connect(fx)->password, SECONDARY_DPS_TOKEN);
  /* No RETRY_PENDING for a session without a registration: IDLE reports the
   * rejection, CONNECTING the next source. */
  const recorded_event* idle = last_event(fx, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE);
  assert_int_equal(idle->source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
  assert_int_equal(idle->reason, AZ_IOT_ERR_IDENTITY_REJECTED);
  for (size_t i = 0; i < fx->log_count; ++i)
  {
    assert_int_not_equal(fx->log[i].state, AZ_IOT_CONN_STATE_RETRY_PENDING);
  }
  assert_int_equal(
      last_event(fx, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_CONNECTING)->source,
      AZ_IOT_AUTH_SOURCE_SECONDARY_KEY);
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].pass_from, AZ_IOT_AUTH_SOURCE_NONE);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].first, AZ_IOT_AUTH_SOURCE_SECONDARY_KEY);
}

/* A session held by a feature client: its next ensure() reopens at once with
 * the next source; a full rejected pass is paced as before. */
static void a_feature_held_session_falls_back_to_the_secondary_key(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, on_state, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_false(fx->client.dps_user_retry_blocked);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_string_equal(last_connect(fx)->password, SECONDARY_DPS_TOKEN);

  /* Whole pass rejected: paced under the policy, not reopened at once. */
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_null(fx->client.dps_mqtt);

  az_iot_connection_client__dps_user_release(&fx->client);
}

/* The last holder releasing ends the pass: new demand starts at the first source. */
static void a_released_session_starts_a_new_pass(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].first, AZ_IOT_AUTH_SOURCE_SECONDARY_KEY);

  az_iot_connection_client__dps_user_release(&fx->client);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].first, AZ_IOT_AUTH_SOURCE_NONE);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].pass_from, AZ_IOT_AUTH_SOURCE_NONE);

  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/* Released from the IDLE callback that reports the rejection: no source is
 * advanced for demand that no longer exists. */
static void release_on_idle(const az_iot_connection_state_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope == AZ_IOT_CONN_SCOPE_DPS && event->state == AZ_IOT_CONN_STATE_IDLE
      && event->reason != AZ_IOT_OK && fx->client.dps_user_count > 0)
  {
    az_iot_connection_client__dps_user_release(&fx->client);
  }
}

static void a_release_during_the_rejection_does_not_advance(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, release_on_idle, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_int_equal(fx->client.dps_user_count, 0);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].first, AZ_IOT_AUTH_SOURCE_NONE);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].pass_from, AZ_IOT_AUTH_SOURCE_NONE);
}

/* close() from the IDLE callback that reports a DPS rejection: no fallback
 * attempt follows. */
static void close_on_dps_idle(const az_iot_connection_state_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope == AZ_IOT_CONN_SCOPE_DPS && event->state == AZ_IOT_CONN_STATE_IDLE
      && event->reason != AZ_IOT_OK)
  {
    (void)az_iot_connection_client_close(&fx->client);
  }
}

static void a_close_during_the_dps_rejection_stops_the_fallback(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, on_state, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, close_on_dps_idle, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_true(no_connect_pending(fx));
  assert_int_equal(fx->client.reconnect_due_ms, 0);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].first, AZ_IOT_AUTH_SOURCE_NONE);
  for (size_t i = 0; i < fx->log_count; ++i)
  {
    assert_int_not_equal(fx->log[i].state, AZ_IOT_CONN_STATE_RETRY_PENDING);
  }
}

/* close() and open() from the DISCONNECTING callback of a rejected
 * registration: the old attempt's finalizer leaves the new one alone. */
static int reopens;
static void reopen_on_dps_disconnecting(const az_iot_connection_state_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope == AZ_IOT_CONN_SCOPE_DPS && event->state == AZ_IOT_CONN_STATE_DISCONNECTING
      && event->reason != AZ_IOT_OK && reopens++ == 0)
  {
    (void)az_iot_connection_client_close(&fx->client);
    assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  }
}

static void a_reopen_during_the_dps_finalizer_keeps_the_new_attempt(void** state)
{
  fixture* fx = (fixture*)*state;
  reopens = 0;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, on_state, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, reopen_on_dps_disconnecting, fx),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_int_equal(reopens, 1);
  assert_non_null(fx->client.dps_mqtt);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_DPS], AZ_IOT_CONN_STATE_CONNECTING);
  assert_int_not_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_DONE);
  size_t last = fx->log_count;
  while (last > 0 && fx->log[last - 1].scope != AZ_IOT_CONN_SCOPE_DPS)
  {
    --last;
  }
  assert_true(last > 0);
  assert_int_equal(fx->log[last - 1].state, AZ_IOT_CONN_STATE_CONNECTING);
  /* A fresh open() starts at the primary key; the old attempt's fallback is not applied. */
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);
  assert_int_equal(fx->client.reconnect_due_ms, 0);
}

/* Released and re-acquired from the IDLE callback that reports the rejection:
 * the new demand starts at the first source, without the old session's pacing. */
static void release_and_reacquire_on_idle(
    const az_iot_connection_state_event* event,
    void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope == AZ_IOT_CONN_SCOPE_DPS && event->state == AZ_IOT_CONN_STATE_IDLE
      && event->reason != AZ_IOT_OK && fx->client.dps_user_count > 0)
  {
    az_iot_connection_client__dps_user_release(&fx->client);
    assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  }
}

static void replaced_demand_starts_a_fresh_pass(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  with_policies(&opts);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, release_and_reacquire_on_idle, fx),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);

  assert_int_equal(fx->client.dps_user_count, 1);
  assert_false(fx->client.dps_user_retry_blocked);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].first, AZ_IOT_AUTH_SOURCE_NONE);
  /* Not paced: the next ensure() opens at once, with the primary key. */
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/* close() from the IDLE callback of a feature-held session's rejection: no
 * fallback and no pacing; with retries disabled nothing latches. */
static void a_close_during_a_session_rejection_leaves_no_pacing(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, close_on_dps_idle, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);

  assert_false(fx->client.dps_user_retry_blocked);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].first, AZ_IOT_AUTH_SOURCE_NONE);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/* provision_only plus a feature holder that releases in the rejection's IDLE
 * callback: the standing demand remains, so the fallback still happens. */
static void a_feature_release_keeps_provision_only_fallback(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  opts.dps.provision_only = true;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, release_on_idle, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_int_equal(fx->client.dps_user_count, 0);
  assert_string_equal(last_connect(fx)->password, SECONDARY_DPS_TOKEN);
}

/* A pass that began on the primary key and failed without a rejection: when a
 * certificate has since appeared, the retry starts a new pass at X.509, so its
 * rejection still falls back to the keys. */
static void a_retry_from_the_first_source_starts_a_new_pass(void** state)
{
  fixture* fx = (fixture*)*state;
  g_late_cert_ready = false;
  az_iot_certificate_provider provider = { .vtable = &k_late_cert_vtable };
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider;
  opts.hub_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);

  connack(fx, AZ_IOT_ERR_TIMEOUT);
  g_late_cert_ready = true;
  wait_and_fire_retry(fx);
  assert_string_equal(last_connect(fx)->password, "");

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);
  g_late_cert_ready = false;
}

/* A provider whose load() returns g_cert_load_result for every role. */
static az_iot_result g_cert_load_result;

static az_iot_result scripted_cert_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  (void)self;
  (void)role;
  memset(out, 0, sizeof(*out));
  return g_cert_load_result;
}

static const az_iot_certificate_provider_vtable k_scripted_cert_vtable = {
  .version = 1u,
  .load = scripted_cert_load,
  .release = ca_only_release,
  .deinit = ca_only_deinit,
};

/* With the policy disabled, a fallback attempt that fails to start faults
 * instead of retrying with no delay. */
static void a_failed_fallback_start_without_a_policy_faults(void** state)
{
  fixture* fx = (fixture*)*state;
  g_cert_load_result = AZ_IOT_OK;
  az_iot_certificate_provider provider = { .vtable = &k_scripted_cert_vtable };
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider;
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, "");

  g_cert_load_result = AZ_IOT_ERR_NOT_INITIALIZED;
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  pump(fx, 3);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->client.reconnect_due_ms, 0);
  assert_true(no_connect_pending(fx));
  g_cert_load_result = AZ_IOT_OK;
}

/* Both keys rejected, then the certificate that made X.509 next is gone: the
 * pass ends with a paced retry, and the next pass starts at the primary key. */
static void a_vanished_fallback_certificate_ends_the_pass(void** state)
{
  fixture* fx = (fixture*)*state;
  g_late_cert_ready = false;
  az_iot_certificate_provider provider = { .vtable = &k_late_cert_vtable };
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider;
  opts.hub_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);

  g_late_cert_ready = true;
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_string_equal(last_connect(fx)->password, SECONDARY_HUB_TOKEN);

  g_late_cert_ready = false;
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_true(no_connect_pending(fx));
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_true(fx->client.reconnect_due_ms > az_iot_time_mono_ms());

  wait_and_fire_retry(fx);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);
}

/* The last holder releases while the CONNECT is in flight and a new one
 * acquires before the pump: the session and its pass are kept, so the
 * rejection still falls back at once. */
static void a_reacquired_in_flight_session_keeps_its_pass(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  az_iot_connection_client__dps_user_release(&fx->client);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);

  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_false(fx->client.dps_user_retry_blocked);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_string_equal(last_connect(fx)->password, SECONDARY_DPS_TOKEN);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/** @brief DPS options with a primary and a secondary key, CONNACK and SUBACK
 * done, so the next step is the registration response. */
static az_iot_mock_mqtt_client* dps_registering_with_two_keys(fixture* fx, bool policies)
{
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  opts.hub_auth.sas.secondary_key_base64 = KEY2_B64;
  if (policies)
  {
    with_policies(&opts);
  }
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  pump(fx, 1);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  pump(fx, 1);
  return m;
}

static void dps_registration_error_401000_falls_back_to_the_secondary_key(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = dps_registering_with_two_keys(fx, true);
  static const char k_body[] = "{\"errorCode\":401000,\"message\":\"Unauthorized\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$dps/registrations/res/401/?$rid=1",
      (const uint8_t*)k_body,
      strlen(k_body),
      AZ_IOT_MQTT_QOS_1));
  pump(fx, 4);

  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
  assert_int_equal(e->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(e->delay_ms, 0);
  assert_string_equal(last_connect(fx)->password, SECONDARY_DPS_TOKEN);
}

/* A registration on a session a feature client already holds starts a pass
 * from that session's source, so 401000 still falls back to the secondary key. */
static void a_registration_on_a_held_session_falls_back(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = dps_sas_options(KEY_B64);
  opts.dps_auth.sas.secondary_key_base64 = KEY2_B64;
  with_policies(&opts);
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);
  fx->initialized = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, on_state, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  pump(fx, 1);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  pump(fx, 1);
  assert_true(az_iot_connection_client__dps_session_ready(&fx->client));

  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  static const char k_body[] = "{\"errorCode\":401000,\"message\":\"Unauthorized\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$dps/registrations/res/401/?$rid=1",
      (const uint8_t*)k_body,
      strlen(k_body),
      AZ_IOT_MQTT_QOS_1));
  pump(fx, 4);

  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_non_null(e);
  assert_int_equal(e->source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
  assert_int_equal(e->delay_ms, 0);
  assert_string_equal(last_connect(fx)->password, SECONDARY_DPS_TOKEN);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/* Only a rejected credential falls back; another registration error does not. */
static void other_dps_registration_errors_do_not_fall_back(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = dps_registering_with_two_keys(fx, false);
  static const char k_body[] = "{\"errorCode\":404201,\"message\":\"Not found\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$dps/registrations/res/404/?$rid=1",
      (const uint8_t*)k_body,
      strlen(k_body),
      AZ_IOT_MQTT_QOS_1));
  pump(fx, 4);
  assert_true(no_connect_pending(fx));
  assert_int_equal(fx->last_state, AZ_IOT_CONN_STATE_FAULTED);
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
  assert_true(all_zero(g_sas_buffer, sizeof(g_sas_buffer)));
  assert_true(all_zero(fx->client.auth, sizeof(fx->client.auth)));
}

/* ---- SAS token renewal ---- */

/** @brief Opens a hub SAS session, CONNECTED, whose Unix time is @p now; the
 * renewal is due at 50% of a 100 s token. */
static void open_renewable_hub(fixture* fx, uint64_t* now, const char* secondary)
{
  az_iot_connection_client_options opts = hub_sas_options();
  opts.unix_time.get_time = switchable_time;
  opts.unix_time.user_ctx = now;
  opts.hub_auth.sas.secondary_key_base64 = secondary;
  opts.hub_auth.sas.token_lifetime_seconds = 100;
  opts.hub_auth.sas.renewal_percent = 50;
  uint64_t before = az_iot_time_mono_ms();
  init_and_open(fx, &opts);
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_CONNECTED);
  assert_true(fx->client.sas_token_renewal_due_ms >= before + 50000u);
  assert_true(fx->client.sas_token_renewal_due_ms <= az_iot_time_mono_ms() + 50000u);
}

/** @brief Makes the renewal due and runs the pump once: the disconnect is
 * requested, the session is still CONNECTED. Returns the old adapter. */
static az_iot_mock_mqtt_client* start_renewal(fixture* fx)
{
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms();
  pump(fx, 1);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 1);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_CONNECTED);
  assert_true(fx->client.sas_token_renewal_in_progress);
  return m;
}

/* The hub session is disconnected and reconnected with a new token, with no
 * reconnection policy: RETRY_PENDING to CONNECTED, all flagged, reason OK. */
static void the_hub_sas_token_is_renewed(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  open_renewable_hub(fx, &now, NULL);
  char old_token[sizeof(last_connect(fx)->password)];
  memcpy(old_token, last_connect(fx)->password, sizeof(old_token));
  size_t from = fx->log_count;

  now = NOW + 50u;
  az_iot_mock_mqtt_client* m = start_renewal(fx);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  pump(fx, 2);
  assert_string_not_equal(last_connect(fx)->password, old_token);
  assert_int_equal(last_connect(fx)->password[0], 'S');
  connack(fx, AZ_IOT_OK);

  static const az_iot_connection_state k_expected[] = {
    AZ_IOT_CONN_STATE_RETRY_PENDING,
    AZ_IOT_CONN_STATE_SETTING_UP,
    AZ_IOT_CONN_STATE_CONNECTING,
    AZ_IOT_CONN_STATE_CONNECTED,
  };
  assert_int_equal(fx->log_count - from, 4);
  for (size_t i = 0; i < 4; ++i)
  {
    assert_int_equal(fx->log[from + i].scope, AZ_IOT_CONN_SCOPE_HUB);
    assert_int_equal(fx->log[from + i].state, k_expected[i]);
    assert_int_equal(fx->log[from + i].reason, AZ_IOT_OK);
    assert_true(fx->log[from + i].renewal);
    assert_int_equal(fx->log[from + i].classification, AZ_IOT_CONN_FAILURE_NONE);
  }
  assert_false(fx->client.sas_token_renewal_in_progress);
  assert_int_equal(fx->client.retry_attempt[AZ_IOT_CONN_SCOPE_HUB], 0);
  assert_true(fx->client.sas_token_renewal_due_ms > az_iot_time_mono_ms());
}

/* An adapter that never reports the disconnect: the renewal reconnects after
 * the bound. */
static void a_renewal_without_a_disconnect_event_reconnects(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  open_renewable_hub(fx, &now, NULL);
  (void)start_renewal(fx);
  pump(fx, 1);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_CONNECTED);
  assert_int_equal(fx->log[fx->log_count - 1].state, AZ_IOT_CONN_STATE_CONNECTED);

  fx->client.sas_token_renewal_disconnect_deadline_ms = az_iot_time_mono_ms();
  pump(fx, 1);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_CONNECTING);
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_non_null(e);
  assert_true(e->renewal);
  connack(fx, AZ_IOT_OK);
  assert_true(fx->log[fx->log_count - 1].renewal);
}

/* A session authenticated with X.509 has no token to renew. */
static void an_x509_session_is_not_renewed(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_test_provider provider;
  az_iot_test_provider_init(&provider, "provider-ca.pem");
  az_iot_connection_client_options opts = hub_sas_options();
  opts.certificate_provider = &provider.base;
  init_and_open(fx, &opts);
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_HUB].source, AZ_IOT_AUTH_SOURCE_X509);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms();
  pump(fx, 2);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 0);
  assert_false(fx->client.sas_token_renewal_in_progress);
}

/* close() while the renewal waits for its disconnect: the client settles to
 * IDLE, with no reconnect and no renewal flag. */
static void a_close_during_a_renewal_settles_idle(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  open_renewable_hub(fx, &now, NULL);
  az_iot_mock_mqtt_client* m = start_renewal(fx);
  size_t from = fx->log_count;
  assert_int_equal(az_iot_connection_client_close(&fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  pump(fx, 2);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_IDLE);
  assert_true(no_connect_pending(fx));
  assert_false(fx->client.sas_token_renewal_in_progress);
  for (size_t i = from; i < fx->log_count; ++i)
  {
    assert_false(fx->log[i].renewal);
  }
}

/* A PUBACK callback run by the renewal's teardown closes the client: the
 * renewal does not reconnect. */
static void close_on_puback(az_iot_result status, void* user_ctx)
{
  (void)status;
  (void)az_iot_connection_client_close((az_iot_connection_client*)user_ctx);
}

static void a_close_from_a_renewal_teardown_callback_stops_it(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  open_renewable_hub(fx, &now, NULL);
  az_iot_mqtt_message msg = { 0 };
  msg.topic = "devices/ut-device/messages/events/";
  msg.payload = (const uint8_t*)"x";
  msg.payload_len = 1;
  msg.qos = AZ_IOT_MQTT_QOS_1;
  assert_int_equal(
      az_iot_connection_client__publish(&fx->client, NULL, &msg, close_on_puback, &fx->client),
      AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = start_renewal(fx);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  pump(fx, 2);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_IDLE);
  assert_true(no_connect_pending(fx));
  assert_int_equal(fx->client.reconnect_due_ms, 0);
  assert_false(fx->client.sas_token_renewal_in_progress);
}

/* The hub's process_loop() wait ends by the renewal time, then by the
 * disconnect bound. */
static void the_hub_wait_is_capped_by_the_renewal(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  open_renewable_hub(fx, &now, NULL);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms() + 1000u;
  (void)az_iot_connection_client_do_work(&fx->client, 60000u);
  assert_true(
      az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms <= 1000u);

  (void)start_renewal(fx);
  (void)az_iot_connection_client_do_work(&fx->client, 60000u);
  assert_true(
      az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms <= 5000u);
}

/* A provisioning session a feature client holds beside the hub: its wait,
 * which comes first, also ends by the renewal time. */
static void a_held_dps_wait_is_capped_by_the_hub_renewal(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.unix_time.get_time = switchable_time;
  opts.unix_time.user_ctx = &now;
  opts.dps.id_scope = "0ne00000001";
  opts.dps.registration_id = "ut-device";
  opts.dps_auth.sas.primary_key_base64 = KEY_B64;
  init_and_open(fx, &opts);
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_CONNECTED);

  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);
  assert_non_null(fx->client.dps_mqtt);

  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms() + 1000u;
  (void)az_iot_connection_client_do_work(&fx->client, 60000u);
  assert_true(
      az_iot_mock_mqtt_client_last_of(dps, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms <= 1000u);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/* A monotonic clock that stopped in suspend: the renewal starts when Unix time
 * reaches it. */
static void a_suspended_monotonic_clock_still_renews(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  open_renewable_hub(fx, &now, NULL);
  assert_int_equal(fx->client.sas_token_renewal_due_unix_seconds, NOW + 50u);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  pump(fx, 1);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 0);

  now = NOW + 50u;
  (void)az_iot_connection_client_do_work(&fx->client, 60000u);
  assert_int_equal(
      az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms, 0);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 1);
  assert_true(fx->client.sas_token_renewal_in_progress);
  assert_int_equal(fx->client.sas_token_renewal_due_unix_seconds, 0);
}

/* Defaults: 80% of one hour. */
static void the_default_renewal_time_is_80_percent_of_an_hour(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  uint64_t before = az_iot_time_mono_ms();
  init_and_open(fx, &opts);
  assert_int_equal(fx->client.sas_token_renewal_due_unix_seconds, NOW + 2880u);
  assert_true(fx->client.sas_token_renewal_due_ms >= before + 2880000u);
  assert_true(fx->client.sas_token_renewal_due_ms <= az_iot_time_mono_ms() + 2880000u);
}

/* The largest lifetime at 99% does not overflow. */
static void the_largest_renewal_time_does_not_overflow(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.hub_auth.sas.token_lifetime_seconds = UINT32_MAX;
  opts.hub_auth.sas.renewal_percent = 99;
  uint64_t before = az_iot_time_mono_ms();
  init_and_open(fx, &opts);
  uint64_t delay_ms = (uint64_t)UINT32_MAX * 990u;
  assert_int_equal(fx->client.sas_token_renewal_due_unix_seconds, NOW + (delay_ms + 999u) / 1000u);
  assert_true(fx->client.sas_token_renewal_due_ms >= before + delay_ms);
}

/* A sub-second renewal delay: the Unix deadline is rounded up, so it does not
 * renew at once. */
static void a_sub_second_renewal_delay_does_not_renew_at_once(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  az_iot_connection_client_options opts = hub_sas_options();
  opts.unix_time.get_time = switchable_time;
  opts.unix_time.user_ctx = &now;
  opts.hub_auth.sas.token_lifetime_seconds = 20;
  opts.hub_auth.sas.renewal_percent = 1;
  init_and_open(fx, &opts);
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->client.sas_token_renewal_due_unix_seconds, NOW + 1u);
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms() + 60000u;
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  pump(fx, 1);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 0);
}

/* A renewal reconnect that is refused is a failure: no renewal flag, and the
 * credential fallback applies. */
static void a_refused_renewal_falls_back(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  open_renewable_hub(fx, &now, KEY2_B64);
  az_iot_mock_mqtt_client* m = start_renewal(fx);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  pump(fx, 2);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  const recorded_event* e = &fx->log[fx->log_count - 1];
  for (size_t i = fx->log_count; i > 0; --i)
  {
    if (fx->log[i - 1].reason == AZ_IOT_ERR_IDENTITY_REJECTED)
    {
      e = &fx->log[i - 1];
      break;
    }
  }
  assert_int_equal(e->reason, AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_false(e->renewal);
  assert_false(fx->client.sas_token_renewal_in_progress);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_HUB].source, AZ_IOT_AUTH_SOURCE_SECONDARY_KEY);
  assert_non_null(strstr(last_connect(fx)->password, "SharedAccessSignature "));
}

/* ---- User-provided tokens ---- */

#define USER_TOKEN "SharedAccessSignature sr=x&sig=user&se=1"

typedef enum
{
  FAKE_TOKEN_READY,
  FAKE_TOKEN_PENDING,
  FAKE_TOKEN_UNAVAILABLE,
  FAKE_TOKEN_COMPLETE_INSIDE, /* complete_sas_token() from the callback, then PENDING */
  FAKE_TOKEN_CLOSE_INSIDE /* close() from the callback */
} fake_token_mode;

#define PARTIAL_TOKEN "SharedAccessSignature sr=partial"

static struct
{
  fake_token_mode mode;
  uint32_t valid_seconds;
  uint32_t retry_after_seconds;
  int calls;
  uint32_t request_id;
  az_iot_connection_scope scope;
  char resource_uri[128];
  char key_name[32];
  size_t buffer_size;
  az_iot_connection_client* client;
} g_fake;

static void fake_token_callback(
    const az_iot_sas_token_request* request,
    char* token_buffer,
    size_t token_buffer_size,
    az_iot_sas_token_response* response,
    void* user_ctx)
{
  (void)user_ctx;
  g_fake.calls++;
  g_fake.request_id = request->request_id;
  g_fake.scope = request->scope;
  g_fake.buffer_size = token_buffer_size;
  snprintf(g_fake.resource_uri, sizeof(g_fake.resource_uri), "%s", request->resource_uri);
  snprintf(g_fake.key_name, sizeof(g_fake.key_name), "%s", request->key_name);
  switch (g_fake.mode)
  {
    case FAKE_TOKEN_READY:
      memcpy(token_buffer, USER_TOKEN, strlen(USER_TOKEN));
      response->status = AZ_IOT_SAS_TOKEN_READY;
      response->token_len = strlen(USER_TOKEN);
      response->valid_seconds = g_fake.valid_seconds;
      break;
    case FAKE_TOKEN_COMPLETE_INSIDE:
    {
      az_iot_sas_token_response done = { 0 };
      done.status = AZ_IOT_SAS_TOKEN_READY;
      done.token_len = strlen(USER_TOKEN);
      done.valid_seconds = g_fake.valid_seconds;
      assert_int_equal(
          az_iot_connection_client_complete_sas_token(
              g_fake.client, request->request_id, USER_TOKEN, &done),
          AZ_IOT_OK);
      response->status = AZ_IOT_SAS_TOKEN_PENDING;
      break;
    }
    case FAKE_TOKEN_CLOSE_INSIDE:
      memcpy(token_buffer, PARTIAL_TOKEN, strlen(PARTIAL_TOKEN));
      (void)az_iot_connection_client_close(g_fake.client);
      response->status = AZ_IOT_SAS_TOKEN_PENDING;
      break;
    case FAKE_TOKEN_UNAVAILABLE:
      /* A partial token written before declining. */
      memcpy(token_buffer, PARTIAL_TOKEN, strlen(PARTIAL_TOKEN));
      response->status = AZ_IOT_SAS_TOKEN_UNAVAILABLE;
      response->retry_after_seconds = g_fake.retry_after_seconds;
      break;
    case FAKE_TOKEN_PENDING:
    default:
      memcpy(token_buffer, PARTIAL_TOKEN, strlen(PARTIAL_TOKEN));
      response->status = AZ_IOT_SAS_TOKEN_PENDING;
      break;
  }
}

/** @brief Whether the token area holds the fake's partial token. */
static bool partial_token_left(const fixture* fx)
{
  size_t n = strlen(PARTIAL_TOKEN);
  for (size_t i = 0; i + n <= fx->client.sas_token_size; i++)
  {
    if (memcmp(fx->client.sas_token + i, PARTIAL_TOKEN, n) == 0)
    {
      return true;
    }
  }
  return false;
}

/** @brief Hub options with the token callback only, in @p mode. */
static az_iot_connection_client_options user_token_hub_options(fixture* fx, fake_token_mode mode)
{
  memset(&g_fake, 0, sizeof(g_fake));
  g_fake.mode = mode;
  g_fake.valid_seconds = 100;
  g_fake.client = &fx->client;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "ut-device";
  opts.hub_auth.sas.user_provided_token = fake_token_callback;
  opts.sas_buffer.buffer = g_sas_buffer;
  opts.sas_buffer.size = sizeof(g_sas_buffer);
  return opts;
}

static az_iot_result complete_with_user_token(fixture* fx, uint32_t request_id)
{
  az_iot_sas_token_response done = { 0 };
  done.status = AZ_IOT_SAS_TOKEN_READY;
  done.token_len = strlen(USER_TOKEN);
  done.valid_seconds = 100;
  return az_iot_connection_client_complete_sas_token(&fx->client, request_id, USER_TOKEN, &done);
}

/* The callback is called from do_work(), not open(); a READY token connects
 * the hub, and renewal is armed at renewal_percent of its validity. */
static void a_ready_user_token_connects_the_hub(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  init_and_open(fx, &opts);
  assert_int_equal(g_fake.calls, 0);
  assert_true(no_connect_pending(fx));
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_SETTING_UP);

  uint64_t before = az_iot_time_mono_ms();
  pump(fx, 1);
  assert_int_equal(g_fake.calls, 1);
  assert_int_not_equal(g_fake.request_id, 0);
  assert_int_equal(g_fake.scope, AZ_IOT_CONN_SCOPE_HUB);
  assert_string_equal(g_fake.resource_uri, "broker.example%2Fdevices%2Fut-device");
  assert_string_equal(g_fake.key_name, "");
  assert_true(g_fake.buffer_size > strlen(USER_TOKEN));
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_CONNECTED);
  assert_int_equal(fx->last_source, AZ_IOT_AUTH_SOURCE_USER_PROVIDED);
  assert_true(fx->client.sas_token_renewal_due_ms >= before + 80000u);
  assert_true(fx->client.sas_token_renewal_due_ms <= az_iot_time_mono_ms() + 80000u);
}

/* PENDING: the attempt waits in SETTING_UP until the token is delivered, then
 * connects DPS with it. */
static void a_pending_user_token_connects_dps_once_delivered(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  opts.host = NULL;
  opts.dps.id_scope = "0ne00000001";
  opts.dps.registration_id = "ut-device";
  opts.dps_auth.sas.user_provided_token = fake_token_callback;
  init_and_open(fx, &opts);
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 1);
  assert_int_equal(g_fake.scope, AZ_IOT_CONN_SCOPE_DPS);
  assert_string_equal(g_fake.resource_uri, "0ne00000001%2fregistrations%2fut-device");
  assert_string_equal(g_fake.key_name, "registration");
  assert_true(no_connect_pending(fx));
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_DPS], AZ_IOT_CONN_STATE_SETTING_UP);

  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_ERR_NOT_FOUND);
  pump(fx, 1);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
  assert_int_equal(fx->client.auth[AZ_IOT_CONN_SCOPE_DPS].source, AZ_IOT_AUTH_SOURCE_USER_PROVIDED);
  assert_int_equal(g_fake.calls, 1);
}

/* complete_sas_token() argument and request checks. */
static void complete_sas_token_checks_its_arguments(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  init_and_open(fx, &opts);
  pump(fx, 1);
  uint32_t id = g_fake.request_id;
  az_iot_sas_token_response r = { 0 };
  assert_int_equal(complete_with_user_token(fx, id + 1u), AZ_IOT_ERR_NOT_FOUND);
  assert_int_equal(
      az_iot_connection_client_complete_sas_token(&fx->client, id, USER_TOKEN, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  r.status = AZ_IOT_SAS_TOKEN_PENDING;
  assert_int_equal(
      az_iot_connection_client_complete_sas_token(&fx->client, id, USER_TOKEN, &r),
      AZ_IOT_ERR_INVALID_ARG);
  r.status = AZ_IOT_SAS_TOKEN_READY;
  r.token_len = strlen(USER_TOKEN);
  assert_int_equal(
      az_iot_connection_client_complete_sas_token(&fx->client, id, USER_TOKEN, &r),
      AZ_IOT_ERR_INVALID_ARG); /* no validity */
  r.valid_seconds = 10;
  r.token_len = sizeof(g_sas_buffer);
  assert_int_equal(
      az_iot_connection_client_complete_sas_token(&fx->client, id, USER_TOKEN, &r),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(complete_with_user_token(fx, id), AZ_IOT_OK);
}

/* UNAVAILABLE fails the attempt, retried under the policy no sooner than
 * retry_after_seconds; the hub-unreachable count is untouched. */
static void an_unavailable_user_token_is_retried_after_its_delay(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_UNAVAILABLE);
  g_fake.retry_after_seconds = 30;
  with_policies(&opts);
  init_and_open(fx, &opts);
  pump(fx, 1);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_RETRY_PENDING);
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->reason, AZ_IOT_ERR_BUSY);
  assert_true(e->delay_ms >= 30000u);
  assert_true(fx->client.reconnect_due_ms >= az_iot_time_mono_ms() + 29000u);
  assert_int_equal(fx->client.consecutive_hub_connect_failures, 0);
}

/* No token within connect_timeout_seconds: the attempt fails with TIMEOUT. */
static void an_undelivered_user_token_times_out(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  with_policies(&opts);
  init_and_open(fx, &opts);
  pump(fx, 1);
  uint32_t id = g_fake.request_id;
  fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_HUB].deadline_ms = az_iot_time_mono_ms();
  pump(fx, 1);
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->reason, AZ_IOT_ERR_TIMEOUT);
  assert_int_equal(complete_with_user_token(fx, id), AZ_IOT_ERR_NOT_FOUND);
}

/* close() cancels a pending request. */
static void close_cancels_a_pending_user_token(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  init_and_open(fx, &opts);
  pump(fx, 1);
  assert_int_equal(az_iot_connection_client_close(&fx->client), AZ_IOT_OK);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_IDLE);
  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_ERR_NOT_FOUND);
  pump(fx, 2);
  assert_true(no_connect_pending(fx));
}

/* close() from inside the callback: nothing connects, and what the callback
 * wrote is wiped. */
static void close_from_the_token_callback_ends_the_attempt(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_CLOSE_INSIDE);
  init_and_open(fx, &opts);
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 1);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_IDLE);
  assert_true(no_connect_pending(fx));
  assert_false(partial_token_left(fx));
}

/* complete_sas_token() from inside the callback connects at once. */
static void a_token_completed_inside_the_callback_connects(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_COMPLETE_INSIDE);
  init_and_open(fx, &opts);
  pump(fx, 1);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
}

/* A rejected key falls back to the user-provided token at once. */
static void a_rejected_key_falls_back_to_the_user_token(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  opts.crypto = TEST_CRYPTO();
  opts.unix_time.get_time = fixed_time;
  opts.hub_auth.sas.primary_key_base64 = KEY_B64;
  init_and_open(fx, &opts);
  assert_string_equal(last_connect(fx)->password, HUB_TOKEN);
  connack(fx, AZ_IOT_ERR_IDENTITY_REJECTED);
  pump(fx, 1);
  assert_int_equal(g_fake.calls, 1);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
  assert_int_equal(e->delay_ms, 0);
}

/* Renewal asks while the session stays up; a PENDING token keeps it up until
 * delivered, then the renewal reconnects with it. */
static void a_user_token_renewal_waits_for_the_token_connected(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  init_and_open(fx, &opts);
  pump(fx, 1);
  connack(fx, AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);

  g_fake.mode = FAKE_TOKEN_PENDING;
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms();
  pump(fx, 3);
  assert_int_equal(g_fake.calls, 2);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 0);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_CONNECTED);

  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  pump(fx, 1);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 1);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  pump(fx, 2);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
  connack(fx, AZ_IOT_OK);
  assert_true(fx->log[fx->log_count - 1].renewal);
  assert_int_equal(g_fake.calls, 2);
}

/* A connection loss queued before the renewal token arrives is reported as a
 * loss, not as the renewal's disconnect. */
static void a_loss_queued_before_a_renewal_token_is_a_loss(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  init_and_open(fx, &opts);
  pump(fx, 1);
  connack(fx, AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);

  g_fake.mode = FAKE_TOKEN_PENDING;
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms();
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 2);
  az_iot_mqtt_event lost;
  memset(&lost, 0, sizeof(lost));
  lost.kind = AZ_IOT_MQTT_EVT_DISCONNECTED;
  lost.status = AZ_IOT_ERR_NOT_CONNECTED;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &lost));
  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  pump(fx, 1);
  /* m is freed with the lost session. */
  const recorded_event* e = &fx->log[fx->log_count - 1];
  assert_int_equal(e->state, AZ_IOT_CONN_STATE_IDLE);
  assert_false(e->renewal);
  assert_int_equal(e->reason, AZ_IOT_ERR_NOT_CONNECTED);
}

/* No renewal token now: the session stays up and the callback is asked again later. */
static void an_unavailable_renewal_token_keeps_the_session(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  init_and_open(fx, &opts);
  pump(fx, 1);
  connack(fx, AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  g_fake.mode = FAKE_TOKEN_UNAVAILABLE;
  g_fake.retry_after_seconds = 7;
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms();
  pump(fx, 3);
  assert_int_equal(g_fake.calls, 2);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 0);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_CONNECTED);
  assert_true(fx->client.sas_token_renewal_due_ms >= az_iot_time_mono_ms() + 6000u);
}

/* The resumed attempt's connect() fails: retried like any start failure. */
static void a_failed_connect_after_a_user_token_is_retried(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  with_policies(&opts);
  init_and_open(fx, &opts);
  az_iot_mock_mqtt_factory_fail_next_connect(fx->factory, AZ_IOT_ERR_MQTT);
  pump(fx, 1);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_not_equal(fx->client.reconnect_due_ms, 0);
}

static void a_failed_dps_connect_after_a_user_token_is_retried(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  opts.host = NULL;
  opts.dps.id_scope = "0ne00000001";
  opts.dps.registration_id = "ut-device";
  opts.dps_auth.sas.user_provided_token = fake_token_callback;
  with_policies(&opts);
  init_and_open(fx, &opts);
  az_iot_mock_mqtt_factory_fail_next_connect(fx->factory, AZ_IOT_ERR_MQTT);
  pump(fx, 1);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_DPS], AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_not_equal(fx->client.reconnect_due_ms, 0);
  assert_false(fx->client.dps_registration_ref);
}

/* close() + open() from DPS:CONNECTING of a resumed attempt: the new attempt
 * waits for its own token; the old one does not fail it. */
static int g_reopens;
static void reopen_on_dps_connecting(const az_iot_connection_state_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope == AZ_IOT_CONN_SCOPE_DPS && event->state == AZ_IOT_CONN_STATE_CONNECTING
      && g_reopens++ == 0)
  {
    (void)az_iot_connection_client_close(&fx->client);
    assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  }
}

static void a_reopen_from_a_resumed_dps_attempt_keeps_the_new_one(void** state)
{
  fixture* fx = (fixture*)*state;
  g_reopens = 0;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  opts.host = NULL;
  opts.dps.id_scope = "0ne00000001";
  opts.dps.registration_id = "ut-device";
  opts.dps_auth.sas.user_provided_token = fake_token_callback;
  with_policies(&opts);
  init_and_open(fx, &opts);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, reopen_on_dps_connecting, fx),
      AZ_IOT_OK);
  pump(fx, 1);
  assert_int_equal(g_reopens, 1);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_DPS], AZ_IOT_CONN_STATE_SETTING_UP);
  assert_true(fx->client.dps_registration_ref);
  assert_int_not_equal(fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_DPS].request_id, 0);
  assert_int_equal(fx->client.reconnect_due_ms, 0);
}

/* From inside a callback, the other role's request cannot be completed. */
static uint32_t g_dps_request;
static void complete_other_then_ready(
    const az_iot_sas_token_request* request,
    char* token_buffer,
    size_t token_buffer_size,
    az_iot_sas_token_response* response,
    void* user_ctx)
{
  (void)token_buffer_size;
  fixture* fx = (fixture*)user_ctx;
  if (request->scope == AZ_IOT_CONN_SCOPE_DPS)
  {
    g_dps_request = request->request_id;
    response->status = AZ_IOT_SAS_TOKEN_PENDING;
    return;
  }
  assert_int_equal(complete_with_user_token(fx, g_dps_request), AZ_IOT_ERR_BUSY);
  memcpy(token_buffer, USER_TOKEN, strlen(USER_TOKEN));
  response->status = AZ_IOT_SAS_TOKEN_READY;
  response->token_len = strlen(USER_TOKEN);
  response->valid_seconds = 100;
}

static void the_other_roles_token_waits_for_the_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  g_dps_request = 0;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  opts.dps.id_scope = "0ne00000001";
  opts.dps.registration_id = "ut-device";
  opts.dps_auth.sas.user_provided_token = complete_other_then_ready;
  opts.dps_auth.sas.user_ctx = fx;
  opts.hub_auth.sas.user_provided_token = complete_other_then_ready;
  opts.hub_auth.sas.user_ctx = fx;
  init_and_open(fx, &opts);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  pump(fx, 1);
  assert_int_not_equal(g_dps_request, 0);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
  assert_int_equal(complete_with_user_token(fx, g_dps_request), AZ_IOT_OK);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/* A hub that faults drops the renewal token it held. */
static void a_faulted_hub_drops_its_held_token(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  init_and_open(fx, &opts);
  pump(fx, 1);
  connack(fx, AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms();
  pump(fx, 2);
  assert_true(fx->client.sas_token_renewal_in_progress);
  assert_int_equal(fx->client.sas_token_holder, AZ_IOT_CONN_SCOPE_HUB + 1);
  assert_true(az_iot_mock_mqtt_client_inject_error(m, AZ_IOT_ERR_MQTT));
  pump(fx, 2);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->client.sas_token_holder, 0);
  assert_int_equal(fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_HUB].request_id, 0);
}

/* A retry_after_seconds past what 32-bit milliseconds hold is kept. */
static void a_long_retry_after_is_not_cut_short(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_UNAVAILABLE);
  g_fake.retry_after_seconds = 5000000u;
  with_policies(&opts);
  init_and_open(fx, &opts);
  uint64_t before = az_iot_time_mono_ms();
  pump(fx, 1);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_true(fx->client.reconnect_due_ms >= before + 5000000000ull);
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->delay_ms, UINT32_MAX);
}

/* The hub waits for its token beside a held provisioning session: that
 * session's process_loop() wait ends by the token deadline. */
static void a_token_deadline_caps_the_other_sessions_wait(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  opts.crypto = TEST_CRYPTO();
  opts.unix_time.get_time = fixed_time;
  opts.dps.id_scope = "0ne00000001";
  opts.dps.registration_id = "ut-device";
  opts.dps_auth.sas.primary_key_base64 = KEY_B64;
  init_and_open(fx, &opts);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_string_equal(last_connect(fx)->password, DPS_TOKEN);
  pump(fx, 1);
  assert_int_equal(g_fake.calls, 1);
  fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_HUB].deadline_ms = az_iot_time_mono_ms() + 1000u;
  (void)az_iot_connection_client_do_work(&fx->client, 60000u);
  assert_true(
      az_iot_mock_mqtt_client_last_of(dps, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms <= 1000u);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/* A result for a request past its deadline is not used: the attempt fails
 * with TIMEOUT, and a late completion is NOT_FOUND. */
static void a_token_after_the_deadline_is_not_used(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  with_policies(&opts);
  init_and_open(fx, &opts);
  fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_HUB].deadline_ms = az_iot_time_mono_ms();
  pump(fx, 1);
  assert_int_equal(g_fake.calls, 0);
  assert_true(no_connect_pending(fx));
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->reason, AZ_IOT_ERR_TIMEOUT);
}

static void a_completion_after_the_deadline_is_not_found(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  with_policies(&opts);
  init_and_open(fx, &opts);
  pump(fx, 1);
  fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_HUB].deadline_ms = az_iot_time_mono_ms();
  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_ERR_NOT_FOUND);
  pump(fx, 1);
  assert_true(no_connect_pending(fx));
  const recorded_event* e = last_event(fx, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(e->reason, AZ_IOT_ERR_TIMEOUT);
}

/* The renewal token is still pending when the current one expires: the
 * session ends, and the reconnect waits for that same request. */
static void a_pending_renewal_past_expiry_ends_the_session(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  init_and_open(fx, &opts);
  pump(fx, 1);
  connack(fx, AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(fx->client.sas_token_expiry_ms > az_iot_time_mono_ms() + 90000u);
  g_fake.mode = FAKE_TOKEN_PENDING;
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms();
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 2);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 0);

  fx->client.sas_token_expiry_ms = az_iot_time_mono_ms();
  pump(fx, 1);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 1);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  pump(fx, 2);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_SETTING_UP);
  assert_int_equal(g_fake.calls, 2);
  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  pump(fx, 1);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
}

/* valid_seconds counts from delivery: a token used late renews earlier, and
 * one that expired before use is asked for again. */
static void a_delivered_token_ages_from_delivery(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  init_and_open(fx, &opts);
  pump(fx, 1);
  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  uint64_t now = az_iot_time_mono_ms();
  fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_HUB].delivered_ms = now - 100000u;
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 2);
  assert_true(no_connect_pending(fx));

  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  now = az_iot_time_mono_ms();
  fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_HUB].delivered_ms = now - 50000u;
  pump(fx, 1);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
  assert_true(fx->client.sas_token_renewal_due_ms <= now + 30000u);
  assert_true(fx->client.sas_token_expiry_ms <= now + 50000u);
}

/* A callback-only token area smaller than a key-signed token: the resource
 * URI fits, so the callback is asked, and a delivered token connects. */
static void a_small_token_area_serves_a_token_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  opts.sas_buffer.size = AZ_IOT_SAS_BUFFER_SIZE(0, 150);
  assert_true(150 < AZ_IOT_SAS_TOKEN_SIZE(sizeof("broker.example") + sizeof("ut-device") - 2));
  init_and_open(fx, &opts);
  pump(fx, 1);
  assert_int_equal(g_fake.calls, 1);
  assert_string_equal(g_fake.resource_uri, "broker.example%2Fdevices%2Fut-device");
  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  pump(fx, 1);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
}

/* An UNAVAILABLE renewal's retry_after_seconds outlasts the token: the
 * reconnect at expiry waits in SETTING_UP and asks only after it. */
static void a_renewal_retry_after_holds_across_expiry(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  init_and_open(fx, &opts);
  pump(fx, 1);
  connack(fx, AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  g_fake.mode = FAKE_TOKEN_UNAVAILABLE;
  g_fake.retry_after_seconds = 600;
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms();
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 2);
  uint64_t ask_after = fx->client.sas_token_ask_after_ms;
  assert_true(ask_after >= az_iot_time_mono_ms() + 590000u);

  fx->client.sas_token_expiry_ms = az_iot_time_mono_ms();
  pump(fx, 1);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  pump(fx, 3);
  assert_int_equal(fx->client.state[AZ_IOT_CONN_SCOPE_HUB], AZ_IOT_CONN_STATE_SETTING_UP);
  assert_int_equal(g_fake.calls, 2);
  assert_true(fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_HUB].deadline_ms > ask_after);

  g_fake.mode = FAKE_TOKEN_READY;
  fx->client.sas_token_ask_after_ms = az_iot_time_mono_ms();
  pump(fx, 1);
  assert_int_equal(g_fake.calls, 3);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
}

/* A CONNECTING observer completes the other role's request before the
 * CONNECT has taken this role's token: refused, and the token is intact. */
static uint32_t g_pending_dps_request;
static az_iot_result g_complete_from_connecting;
static void complete_dps_on_hub_connecting(
    const az_iot_connection_state_event* event,
    void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB && event->state == AZ_IOT_CONN_STATE_CONNECTING)
  {
    static const char k_other[] = "SharedAccessSignature sr=y&sig=other&se=1";
    az_iot_sas_token_response done = { 0 };
    done.status = AZ_IOT_SAS_TOKEN_READY;
    done.token_len = strlen(k_other);
    done.valid_seconds = 100;
    g_complete_from_connecting = az_iot_connection_client_complete_sas_token(
        &fx->client, g_pending_dps_request, k_other, &done);
  }
}

static void a_token_is_kept_until_its_connect(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  opts.dps.id_scope = "0ne00000001";
  opts.dps.registration_id = "ut-device";
  opts.dps_auth.sas.user_provided_token = fake_token_callback;
  init_and_open(fx, &opts);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  pump(fx, 1); /* both roles asked; both PENDING */
  assert_int_equal(g_fake.calls, 2);
  g_pending_dps_request = fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_DPS].request_id;
  uint32_t hub_request = fx->client.sas_token_request[AZ_IOT_CONN_SCOPE_HUB].request_id;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->client, complete_dps_on_hub_connecting, fx),
      AZ_IOT_OK);
  g_complete_from_connecting = AZ_IOT_OK;
  assert_int_equal(complete_with_user_token(fx, hub_request), AZ_IOT_OK);
  pump(fx, 1);
  assert_int_equal(g_complete_from_connecting, AZ_IOT_ERR_BUSY);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/* An UNAVAILABLE renewal without a retry-after still waits 30 s to ask. */
static void a_renewal_without_retry_after_waits_30_seconds(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  init_and_open(fx, &opts);
  pump(fx, 1);
  connack(fx, AZ_IOT_OK);
  g_fake.mode = FAKE_TOKEN_UNAVAILABLE;
  g_fake.retry_after_seconds = 0;
  fx->client.sas_token_renewal_due_ms = az_iot_time_mono_ms();
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 2);
  assert_true(fx->client.sas_token_ask_after_ms >= az_iot_time_mono_ms() + 29000u);
}

/* A suspended monotonic clock: Unix time passes the token's expiry while the
 * renewal token is pending, and the session ends. */
static void a_suspended_clock_ends_an_expired_user_token_session(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  opts.unix_time.get_time = switchable_time;
  opts.unix_time.user_ctx = &now;
  init_and_open(fx, &opts);
  pump(fx, 1);
  connack(fx, AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(fx->client.sas_token_expiry_unix_seconds >= NOW + 99u);
  assert_true(fx->client.sas_token_expiry_unix_seconds <= NOW + 100u);
  g_fake.mode = FAKE_TOKEN_PENDING;
  now = NOW + 80u;
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 2);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 0);

  now = NOW + 100u;
  pump(fx, 1);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 1);
}

/* A suspend between delivery and use: Unix time ages the delivered token
 * though the monotonic clock did not move. */
static void a_delivered_token_ages_by_unix_time(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  opts.unix_time.get_time = switchable_time;
  opts.unix_time.user_ctx = &now;
  init_and_open(fx, &opts);
  pump(fx, 1);
  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  now = NOW + 100u;
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 2);
  assert_true(no_connect_pending(fx));

  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  now += 50u;
  uint64_t mono = az_iot_time_mono_ms();
  pump(fx, 1);
  assert_string_equal(last_connect(fx)->password, USER_TOKEN);
  assert_true(fx->client.sas_token_renewal_due_ms <= mono + 31000u);
  assert_true(fx->client.sas_token_expiry_ms <= mono + 51000u);
  assert_true(fx->client.sas_token_expiry_unix_seconds <= now + 50u);
}

/* PENDING and UNAVAILABLE responses: what the callback wrote is wiped. */
static void a_declining_callback_leaves_no_token_bytes(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  init_and_open(fx, &opts);
  pump(fx, 1);
  assert_int_equal(g_fake.calls, 1);
  assert_false(partial_token_left(fx));
  assert_int_equal(complete_with_user_token(fx, g_fake.request_id), AZ_IOT_OK);
  (void)az_iot_connection_client_close(&fx->client);
  pump(fx, 1);

  g_fake.mode = FAKE_TOKEN_UNAVAILABLE;
  g_fake.calls = 0;
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  pump(fx, 1);
  assert_int_equal(g_fake.calls, 1);
  assert_false(partial_token_left(fx));
}

/* Unix time elapsed beyond the monotonic uptime (suspend early in boot): the
 * token's age is not capped at the uptime, so an expired token is re-asked. */
static void a_token_aged_beyond_the_uptime_is_re_asked(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = NOW;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_PENDING);
  opts.unix_time.get_time = switchable_time;
  opts.unix_time.user_ctx = &now;
  init_and_open(fx, &opts);
  pump(fx, 1);
  uint32_t uptime_seconds = (uint32_t)(az_iot_time_mono_ms() / 1000u);
  az_iot_sas_token_response done = { 0 };
  done.status = AZ_IOT_SAS_TOKEN_READY;
  done.token_len = strlen(USER_TOKEN);
  done.valid_seconds = uptime_seconds + 100u;
  assert_int_equal(
      az_iot_connection_client_complete_sas_token(
          &fx->client, g_fake.request_id, USER_TOKEN, &done),
      AZ_IOT_OK);
  now = NOW + uptime_seconds + 150u;
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 2);
  assert_true(no_connect_pending(fx));
}

/* A Unix time near its maximum: the Unix deadlines are left unset rather
 * than wrapped into the past, so the session is not renewed at once. */
static void a_unix_time_near_its_maximum_does_not_wrap_deadlines(void** state)
{
  fixture* fx = (fixture*)*state;
  uint64_t now = UINT64_MAX - 10u;
  az_iot_connection_client_options opts = user_token_hub_options(fx, FAKE_TOKEN_READY);
  opts.unix_time.get_time = switchable_time;
  opts.unix_time.user_ctx = &now;
  init_and_open(fx, &opts);
  pump(fx, 1);
  connack(fx, AZ_IOT_OK);
  assert_int_equal(fx->client.sas_token_renewal_due_unix_seconds, 0);
  assert_int_equal(fx->client.sas_token_expiry_unix_seconds, 0);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  pump(fx, 2);
  assert_int_equal(g_fake.calls, 1);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_rejects_invalid_sas_options, setup, teardown),
    cmocka_unit_test_setup_teardown(init_accepts_a_token_callback_without_keys, setup, teardown),
    cmocka_unit_test_setup_teardown(a_ready_user_token_connects_the_hub, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_pending_user_token_connects_dps_once_delivered, setup, teardown),
    cmocka_unit_test_setup_teardown(complete_sas_token_checks_its_arguments, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_unavailable_user_token_is_retried_after_its_delay, setup, teardown),
    cmocka_unit_test_setup_teardown(an_undelivered_user_token_times_out, setup, teardown),
    cmocka_unit_test_setup_teardown(close_cancels_a_pending_user_token, setup, teardown),
    cmocka_unit_test_setup_teardown(
        close_from_the_token_callback_ends_the_attempt, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_token_completed_inside_the_callback_connects, setup, teardown),
    cmocka_unit_test_setup_teardown(a_rejected_key_falls_back_to_the_user_token, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_user_token_renewal_waits_for_the_token_connected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_loss_queued_before_a_renewal_token_is_a_loss, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_unavailable_renewal_token_keeps_the_session, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_connect_after_a_user_token_is_retried, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_dps_connect_after_a_user_token_is_retried, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_reopen_from_a_resumed_dps_attempt_keeps_the_new_one, setup, teardown),
    cmocka_unit_test_setup_teardown(the_other_roles_token_waits_for_the_callback, setup, teardown),
    cmocka_unit_test_setup_teardown(a_faulted_hub_drops_its_held_token, setup, teardown),
    cmocka_unit_test_setup_teardown(a_long_retry_after_is_not_cut_short, setup, teardown),
    cmocka_unit_test_setup_teardown(a_token_deadline_caps_the_other_sessions_wait, setup, teardown),
    cmocka_unit_test_setup_teardown(a_token_after_the_deadline_is_not_used, setup, teardown),
    cmocka_unit_test_setup_teardown(a_completion_after_the_deadline_is_not_found, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_pending_renewal_past_expiry_ends_the_session, setup, teardown),
    cmocka_unit_test_setup_teardown(a_delivered_token_ages_from_delivery, setup, teardown),
    cmocka_unit_test_setup_teardown(a_small_token_area_serves_a_token_callback, setup, teardown),
    cmocka_unit_test_setup_teardown(a_renewal_retry_after_holds_across_expiry, setup, teardown),
    cmocka_unit_test_setup_teardown(a_token_is_kept_until_its_connect, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_renewal_without_retry_after_waits_30_seconds, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_suspended_clock_ends_an_expired_user_token_session, setup, teardown),
    cmocka_unit_test_setup_teardown(a_delivered_token_ages_by_unix_time, setup, teardown),
    cmocka_unit_test_setup_teardown(a_declining_callback_leaves_no_token_bytes, setup, teardown),
    cmocka_unit_test_setup_teardown(a_token_aged_beyond_the_uptime_is_re_asked, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_unix_time_near_its_maximum_does_not_wrap_deadlines, setup, teardown),
    cmocka_unit_test_setup_teardown(
        hub_connects_with_a_sas_token_when_only_a_key_is_set, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_changed_hub_identity_is_used_on_the_next_connect, setup, teardown),
    cmocka_unit_test_setup_teardown(x509_from_the_provider_is_tried_before_sas, setup, teardown),
    cmocka_unit_test_setup_teardown(trusted_ca_overrides_the_provider_ca, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_provider_ca_is_kept_when_the_role_falls_back_to_sas, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_provider_that_cannot_load_does_not_fall_back_to_sas, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_operational_error_is_not_hidden_by_a_missing_bootstrap, setup, teardown),
    cmocka_unit_test_setup_teardown(hub_group_key_derives_from_client_id, setup, teardown),
    cmocka_unit_test_setup_teardown(the_device_id_is_url_encoded_in_the_token, setup, teardown),
    cmocka_unit_test(the_token_size_macro_is_enough_for_the_worst_case),
    cmocka_unit_test_setup_teardown(a_token_that_does_not_fit_fails_and_is_wiped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_username_that_does_not_fit_fails_the_attempt, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_token_is_wiped_once_the_adapter_has_the_connect, setup, teardown),
    cmocka_unit_test_setup_teardown(sas_keys_need_a_large_enough_buffer, setup, teardown),
    cmocka_unit_test(the_token_area_is_capped_at_int32_max),
    cmocka_unit_test_setup_teardown(no_buffer_is_needed_without_sas, setup, teardown),
    cmocka_unit_test_setup_teardown(identical_dps_and_hub_keys_share_one_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(no_unix_time_fails_the_attempt_with_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(each_retry_without_unix_time_is_reported, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_time_that_would_overflow_the_expiry_fails_with_busy, setup, teardown),
    cmocka_unit_test_setup_teardown(dps_connects_with_a_sas_token, setup, teardown),
    cmocka_unit_test_setup_teardown(
        dps_group_key_signs_with_the_derived_device_key, setup, teardown),
    cmocka_unit_test_setup_teardown(
        no_provider_and_no_key_is_credential_incomplete, setup, teardown),
    cmocka_unit_test_setup_teardown(
        hub_x509_rejected_falls_back_to_the_primary_key, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_kept_secondary_key_wraps_to_the_primary_when_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_fully_rejected_pass_goes_to_identity_recovery_then_restarts, setup, teardown),
    cmocka_unit_test_setup_teardown(fallback_does_not_need_a_reconnection_policy, setup, teardown),
    cmocka_unit_test_setup_teardown(open_starts_again_at_the_first_source, setup, teardown),
    cmocka_unit_test_setup_teardown(
        dps_x509_rejected_falls_back_to_the_primary_key, setup, teardown),
    cmocka_unit_test_setup_teardown(
        dps_registration_error_401000_falls_back_to_the_secondary_key, setup, teardown),
    cmocka_unit_test_setup_teardown(a_registration_on_a_held_session_falls_back, setup, teardown),
    cmocka_unit_test_setup_teardown(the_hub_sas_token_is_renewed, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_renewal_without_a_disconnect_event_reconnects, setup, teardown),
    cmocka_unit_test_setup_teardown(an_x509_session_is_not_renewed, setup, teardown),
    cmocka_unit_test_setup_teardown(a_close_during_a_renewal_settles_idle, setup, teardown),
    cmocka_unit_test_setup_teardown(a_refused_renewal_falls_back, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_close_from_a_renewal_teardown_callback_stops_it, setup, teardown),
    cmocka_unit_test_setup_teardown(the_hub_wait_is_capped_by_the_renewal, setup, teardown),
    cmocka_unit_test_setup_teardown(a_held_dps_wait_is_capped_by_the_hub_renewal, setup, teardown),
    cmocka_unit_test_setup_teardown(a_suspended_monotonic_clock_still_renews, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_default_renewal_time_is_80_percent_of_an_hour, setup, teardown),
    cmocka_unit_test_setup_teardown(the_largest_renewal_time_does_not_overflow, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_sub_second_renewal_delay_does_not_renew_at_once, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_provision_only_session_falls_back_to_the_secondary_key, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_feature_held_session_falls_back_to_the_secondary_key, setup, teardown),
    cmocka_unit_test_setup_teardown(a_released_session_starts_a_new_pass, setup, teardown),
    cmocka_unit_test_setup_teardown(replaced_demand_starts_a_fresh_pass, setup, teardown),
    cmocka_unit_test_setup_teardown(a_reacquired_in_flight_session_keeps_its_pass, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_feature_release_keeps_provision_only_fallback, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_retry_from_the_first_source_starts_a_new_pass, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_close_during_the_dps_rejection_stops_the_fallback, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_reopen_during_the_dps_finalizer_keeps_the_new_attempt, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_fallback_start_without_a_policy_faults, setup, teardown),
    cmocka_unit_test_setup_teardown(a_vanished_fallback_certificate_ends_the_pass, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_close_during_a_session_rejection_leaves_no_pacing, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_release_during_the_rejection_does_not_advance, setup, teardown),
    cmocka_unit_test_setup_teardown(
        other_dps_registration_errors_do_not_fall_back, setup, teardown),
    cmocka_unit_test_setup_teardown(deinit_wipes_keys_and_tokens, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
