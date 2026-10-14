// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* DPS registration with TPM attestation: requests, responses and key parsing.
 * The fake TPM uses the imported key as an HMAC key, so a token it signs can be
 * checked against az_iot_sas_token_sign(). */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <string.h>

#include "azure/iot/az_iot_dps_tpm_client.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_sas_token.h"
#include "internal/crypto.h"

#if defined(AZ_IOT_TEST_CRYPTO_MBEDTLS)
#include "az_iot_crypto_mbedtls.h"
#define TEST_CRYPTO() az_iot_crypto_mbedtls()
#else
#include "az_iot_crypto_openssl.h"
#define TEST_CRYPTO() az_iot_crypto_openssl()
#endif

#define SCOPE "0ne00000001"
#define REG_ID "ut-device"
#define NOW 1700000000u
#define DPS_URI SCOPE "%2fregistrations%2f" REG_ID
#define REGISTER_PATH \
  "/" SCOPE "/registrations/" REG_ID "/register?api-version=" AZ_IOT_DPS_TPM_DEFAULT_API_VERSION

/* Two TPM2B_PUBLIC stand-ins. */
static const uint8_t ek[] = { 0x00, 0x03, 0x01, 0x02, 0x03 };
static const uint8_t srk[] = { 0x00, 0x01, 0xFF };
#define EK_B64 "AAMBAgM="
#define SRK_B64 "AAH/"

/* Five TPM2Bs and UINT16-sized encrypted data; the base64 has '/' to escape. */
static const uint8_t challenge_key[]
    = { 0x00, 0x02, 0xAA, 0xBB, 0x00, 0x01, 0xCC, 0x00, 0x00, 0x00,
        0x01, 0xDD, 0x00, 0x03, 0xFB, 0xFF, 0xFF, 0x00, 0x01, 0xFC };
#define CHALLENGE_KEY_JSON "AAKquwABzAAAAAHdAAP7\\/\\/8AAfw="
static const uint8_t hub_key[]
    = { 0x00, 0x01, 0x42, 0x00, 0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
#define HUB_KEY_B64 "AAFCAAFDAAAAAAAA"

typedef struct fake_tpm
{
  uint8_t key[64];
  size_t key_len;
  int imports;
  az_iot_result import_result;
  az_iot_result hmac_result;
} fake_tpm;

static az_iot_result fake_import(void* ctx, const uint8_t* key, size_t key_len)
{
  fake_tpm* t = (fake_tpm*)ctx;
  ++t->imports;
  if (t->import_result != AZ_IOT_OK)
  {
    return t->import_result;
  }
  assert_true(key_len <= sizeof(t->key));
  memcpy(t->key, key, key_len);
  t->key_len = key_len;
  return AZ_IOT_OK;
}

static az_iot_result fake_hmac(
    void* ctx,
    const uint8_t* data,
    size_t data_len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  fake_tpm* t = (fake_tpm*)ctx;
  if (t->hmac_result != AZ_IOT_OK)
  {
    return t->hmac_result;
  }
  return az_iot_crypto__hmac_sha256(TEST_CRYPTO(), t->key, t->key_len, data, data_len, out);
}

static az_iot_dps_tpm_client_options options_for(fake_tpm* t)
{
  az_iot_dps_tpm_client_options o = az_iot_dps_tpm_client_options_default();
  o.id_scope = SCOPE;
  o.registration_id = REG_ID;
  o.endorsement_key = ek;
  o.endorsement_key_len = sizeof(ek);
  o.storage_root_key = srk;
  o.storage_root_key_len = sizeof(srk);
  o.tpm.import_key = fake_import;
  o.tpm.hmac_sha256 = fake_hmac;
  o.tpm.ctx = t;
  return o;
}

/** @brief Hands @p json to the client as a writable body. */
static az_iot_result respond(
    az_iot_dps_tpm_client* c,
    uint16_t status,
    const char* json,
    uint32_t retry_after,
    az_iot_dps_tpm_next* next)
{
  static uint8_t body[1024];
  size_t n = strlen(json);
  assert_true(n <= sizeof(body));
  memcpy(body, json, n);
  return az_iot_dps_tpm_client_handle_response(
      c, status, az_span_create(body, (int32_t)n), retry_after, next);
}

/** @brief Drives the client to the authenticated register request. */
static void challenge(az_iot_dps_tpm_client* c, fake_tpm* t)
{
  az_iot_dps_tpm_client_options o = options_for(t);
  assert_int_equal(az_iot_dps_tpm_client_init(c, &o), AZ_IOT_OK);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  assert_int_equal(az_iot_dps_tpm_client_get_request(c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  az_iot_dps_tpm_next next;
  assert_int_equal(
      respond(
          c,
          401,
          "{\"authenticationKey\":\"" CHALLENGE_KEY_JSON "\",\"keyName\":\"registration\"}",
          0,
          &next),
      AZ_IOT_OK);
}

static void expected_token(const fake_tpm* t, char* out, size_t out_size)
{
  size_t len = 0;
  assert_int_equal(
      az_iot_sas_token_sign(
          TEST_CRYPTO(),
          t->key,
          t->key_len,
          DPS_URI,
          "registration",
          NOW + AZ_IOT_DPS_TPM_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS,
          out,
          out_size,
          &len),
      AZ_IOT_OK);
}

static void full_flow_registers_and_imports_both_keys(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client_options o = options_for(&t);
  az_iot_dps_tpm_client c;
  assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
  assert_null(az_iot_dps_tpm_client_get_assignment(&c));

  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;

  /* 1. Unauthenticated register. */
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_string_equal(req.method, "PUT");
  assert_string_equal(req.path, REGISTER_PATH);
  assert_null(req.authorization);
  const char* body = "{\"registrationId\":\"" REG_ID "\",\"tpm\":{\"endorsementKey\":\"" EK_B64
                     "\",\"storageRootKey\":\"" SRK_B64 "\"}}";
  assert_int_equal(req.body_len, strlen(body));
  assert_memory_equal(req.body, body, req.body_len);

  /* 2. Challenge: the key is unescaped, decoded and imported. */
  assert_int_equal(
      respond(
          &c,
          401,
          "{\"authenticationKey\":\"" CHALLENGE_KEY_JSON "\",\"keyName\":\"registration\"}",
          0,
          &next),
      AZ_IOT_OK);
  assert_false(next.assigned);
  assert_int_equal(next.retry_after_seconds, 0);
  assert_int_equal(t.imports, 1);
  assert_int_equal(t.key_len, sizeof(challenge_key));
  assert_memory_equal(t.key, challenge_key, sizeof(challenge_key));

  /* 3. Authenticated register, signed by the TPM. */
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_string_equal(req.method, "PUT");
  assert_string_equal(req.path, REGISTER_PATH);
  char token[512];
  expected_token(&t, token, sizeof(token));
  assert_non_null(req.authorization);
  assert_string_equal(req.authorization, token);
  assert_int_equal(req.body_len, strlen(body));
  assert_memory_equal(req.body, body, req.body_len);

  /* 4. Assigning. */
  assert_int_equal(
      respond(&c, 202, "{\"operationId\":\"4.abc.def\",\"status\":\"assigning\"}", 2, &next),
      AZ_IOT_OK);
  assert_false(next.assigned);
  assert_int_equal(next.retry_after_seconds, 2);

  /* 5. Poll. */
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_string_equal(req.method, "GET");
  assert_string_equal(
      req.path,
      "/" SCOPE "/registrations/" REG_ID
      "/operations/4.abc.def?api-version=" AZ_IOT_DPS_TPM_DEFAULT_API_VERSION);
  assert_string_equal(req.authorization, token);
  assert_null(req.body);
  assert_int_equal(req.body_len, 0);

  /* 6. Assigned: the hub key is imported. */
  assert_int_equal(
      respond(
          &c,
          200,
          "{\"operationId\":\"4.abc.def\",\"status\":\"assigned\",\"registrationState\":{"
          "\"registrationId\":\"ut-device\",\"assignedHub\":\"hub.example\","
          "\"deviceId\":\"dev-1\",\"status\":\"assigned\",\"connectionProfile\":\"mqttV5\","
          "\"issuedCertificateChain\":[\"QQ==\",\"Qg==\"],"
          "\"tpm\":{\"authenticationKey\":\"" HUB_KEY_B64 "\"},\"payload\":{\"a\":[1]}}}",
          0,
          &next),
      AZ_IOT_OK);
  assert_true(next.assigned);
  assert_int_equal(t.imports, 2);
  assert_int_equal(t.key_len, sizeof(hub_key));
  assert_memory_equal(t.key, hub_key, sizeof(hub_key));

  const az_iot_dps_tpm_assignment* a = az_iot_dps_tpm_client_get_assignment(&c);
  assert_non_null(a);
  assert_string_equal(a->assigned_hub, "hub.example");
  assert_string_equal(a->device_id, "dev-1");
  assert_int_equal(a->connection_profile, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  assert_true(a->hub_key_imported);
  const char* chain = "[\"QQ==\",\"Qg==\"]";
  assert_int_equal(az_span_size(a->issued_certificate_chain), (int32_t)strlen(chain));
  assert_memory_equal(az_span_ptr(a->issued_certificate_chain), chain, strlen(chain));

  /* Done: nothing more to send or receive. */
  assert_int_equal(
      az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req),
      AZ_IOT_ERR_NOT_INITIALIZED);
  assert_int_equal(respond(&c, 200, "{}", 0, &next), AZ_IOT_ERR_NOT_INITIALIZED);
}

static void assigned_from_register_needs_no_poll(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client c;
  challenge(&c, &t);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_int_equal(
      respond(
          &c,
          200,
          "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h.x\","
          "\"tpm\":{\"authenticationKey\":\"" HUB_KEY_B64 "\"},\"deviceId\":\"d\\/1\"}}",
          0,
          &next),
      AZ_IOT_OK);
  assert_true(next.assigned);
  const az_iot_dps_tpm_assignment* a = az_iot_dps_tpm_client_get_assignment(&c);
  assert_string_equal(a->assigned_hub, "h.x");
  assert_string_equal(a->device_id, "d/1");
  assert_int_equal(a->connection_profile, AZ_IOT_CONNECTION_PROFILE_MQTT_V3);
  assert_true(a->hub_key_imported);
  assert_int_equal(az_span_size(a->issued_certificate_chain), 0);
  assert_int_equal(t.imports, 2);
}

static void an_mqttv3_assignment_needs_a_hub_key_or_a_certificate(void** state)
{
  (void)state;
  static const struct
  {
    const char* body;
    az_iot_result result;
  } cases[] = {
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\"}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QQ==\"]}}",
      AZ_IOT_OK },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QUJD\",\"Pz8\\/\",\"QUI=\"]}}",
      AZ_IOT_OK },
    /* A chain must hold one or more canonical base64 strings. */
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"@@@@\"]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QQ\"]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QR==\"]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QUJ=\"]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"====\"]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"Q===\"]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QQ=A\"]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QQ\\n=\"]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[1]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QQ==\",\"\"]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[[\"QQ==\"]]}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"connectionProfile\":\"mqttV5\"}}",
      AZ_IOT_OK },
    /* A profile that is neither a string nor null is malformed. */
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QQ==\"],\"connectionProfile\":1}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QQ==\"],\"connectionProfile\":true}}",
      AZ_IOT_ERR_PROTOCOL },
    { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
      "\"issuedCertificateChain\":[\"QQ==\"],\"connectionProfile\":{}}}",
      AZ_IOT_ERR_PROTOCOL },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client c;
    challenge(&c, &t);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(respond(&c, 200, cases[i].body, 0, &next), cases[i].result);
    assert_int_equal(next.assigned, cases[i].result == AZ_IOT_OK);
    if (cases[i].result == AZ_IOT_OK)
    {
      assert_false(az_iot_dps_tpm_client_get_assignment(&c)->hub_key_imported);
    }
  }
}

static void connection_profiles_map_to_the_enum(void** state)
{
  (void)state;
  static const struct
  {
    const char* member;
    az_iot_connection_profile expected;
    const char* raw;
    bool truncated;
  } cases[] = {
    { "", AZ_IOT_CONNECTION_PROFILE_MQTT_V3, "classic", false },
    { "\"connectionProfile\":\"classic\",", AZ_IOT_CONNECTION_PROFILE_MQTT_V3, "classic", false },
    { "\"connectionProfile\":null,", AZ_IOT_CONNECTION_PROFILE_MQTT_V3, "classic", false },
    { "\"connectionProfile\":\"mqttV5\",", AZ_IOT_CONNECTION_PROFILE_MQTT_V5, "mqttV5", false },
    { "\"connectionProfile\":\"other\",", AZ_IOT_CONNECTION_PROFILE_UNKNOWN, "other", false },
    /* Escapes are decoded before the profile is matched. */
    { "\"connectionProfile\":\"mqtt\\/V5\",", AZ_IOT_CONNECTION_PROFILE_UNKNOWN, "mqtt/V5", false },
    { "\"connectionProfile\":\"\\u006dqttV5\",", AZ_IOT_CONNECTION_PROFILE_UNKNOWN, NULL, false },
    /* 70 characters: kept to AZ_IOT_CONNECTION_PROFILE_RAW_BUF - 1. */
    { "\"connectionProfile\":"
      "\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\",",
      AZ_IOT_CONNECTION_PROFILE_UNKNOWN,
      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      true },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client c;
    challenge(&c, &t);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    char json[256];
    const char* parts[] = { "{\"status\":\"assigned\",\"registrationState\":{",
                            cases[i].member,
                            "\"tpm\":{\"authenticationKey\":\"" HUB_KEY_B64
                            "\"},\"assignedHub\":\"h\",\"deviceId\":\"d\"}}" };
    size_t len = 0;
    for (size_t k = 0; k < sizeof(parts) / sizeof(parts[0]); ++k)
    {
      size_t n = strlen(parts[k]);
      assert_true(len + n < sizeof(json));
      memcpy(json + len, parts[k], n);
      len += n;
    }
    json[len] = '\0';
    if (cases[i].raw == NULL)
    {
      assert_int_equal(respond(&c, 200, json, 0, &next), AZ_IOT_ERR_PROTOCOL);
      continue;
    }
    assert_int_equal(respond(&c, 200, json, 0, &next), AZ_IOT_OK);
    const az_iot_dps_tpm_assignment* a = az_iot_dps_tpm_client_get_assignment(&c);
    assert_int_equal(a->connection_profile, cases[i].expected);
    assert_int_equal(a->connection_profile_raw_truncated, cases[i].truncated);
    if (cases[i].truncated)
    {
      assert_int_equal(strlen(a->connection_profile_raw), AZ_IOT_CONNECTION_PROFILE_RAW_BUF - 1);
      assert_memory_equal(
          a->connection_profile_raw, cases[i].raw, AZ_IOT_CONNECTION_PROFILE_RAW_BUF - 1);
    }
    else
    {
      assert_string_equal(a->connection_profile_raw, cases[i].raw);
    }
  }
}

static void throttling_and_server_errors_keep_the_request(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client c;
  challenge(&c, &t);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_int_equal(respond(&c, 429, "", 7, &next), AZ_IOT_OK);
  assert_false(next.assigned);
  assert_int_equal(next.retry_after_seconds, 7);
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_string_equal(req.method, "PUT");
  assert_non_null(req.authorization);
  assert_int_equal(respond(&c, 503, "oops", 0, &next), AZ_IOT_OK);
  assert_int_equal(next.retry_after_seconds, AZ_IOT_DPS_TPM_DEFAULT_RETRY_AFTER_SECONDS);
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_string_equal(req.method, "PUT");
  assert_int_equal(t.imports, 1);
}

static void error_codes_are_numbers_and_the_registration_state_one_wins(void** state)
{
  (void)state;
  static const struct
  {
    const char* body;
    az_iot_result result;
    int32_t code;
  } cases[] = {
    { "{\"status\":\"failed\",\"errorCode\":\"403\"}", AZ_IOT_ERR_PROTOCOL, 0 },
    { "{\"status\":\"failed\",\"registrationState\":{\"errorCode\":null}}",
      AZ_IOT_ERR_PROTOCOL,
      0 },
    { "{\"status\":\"failed\",\"errorCode\":1,\"registrationState\":{\"errorCode\":2}}",
      AZ_IOT_ERR_DPS,
      2 },
    { "{\"registrationState\":{\"errorCode\":2},\"errorCode\":1,\"status\":\"failed\"}",
      AZ_IOT_ERR_DPS,
      2 },
    { "{\"status\":\"failed\",\"errorCode\":1}", AZ_IOT_ERR_DPS, 1 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client c;
    challenge(&c, &t);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(respond(&c, 200, cases[i].body, 0, &next), cases[i].result);
    int32_t code = -1;
    az_iot_dps_tpm_client_get_error(&c, NULL, &code);
    if (cases[i].result == AZ_IOT_ERR_DPS)
    {
      assert_int_equal(code, cases[i].code);
    }
  }
}

static void statuses_past_599_are_not_retried(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client c;
  challenge(&c, &t);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_int_equal(respond(&c, 599, "", 0, &next), AZ_IOT_OK);
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_int_equal(respond(&c, 600, "", 0, &next), AZ_IOT_ERR_DPS);
  assert_int_equal(
      az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req),
      AZ_IOT_ERR_NOT_INITIALIZED);
}

static void a_refused_token_is_an_auth_failure(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client c;
  challenge(&c, &t);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_int_equal(
      respond(&c, 401, "{\"errorCode\":401002,\"message\":\"no\"}", 0, &next), AZ_IOT_ERR_AUTH);
  uint16_t http = 0;
  int32_t code = 0;
  az_iot_dps_tpm_client_get_error(&c, &http, &code);
  assert_int_equal(http, 401);
  assert_int_equal(code, 401002);
  assert_int_equal(
      az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req),
      AZ_IOT_ERR_NOT_INITIALIZED);
}

static void a_challenge_without_a_key_is_an_auth_failure(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client_options o = options_for(&t);
  az_iot_dps_tpm_client c;
  assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_int_equal(respond(&c, 401, "{\"errorCode\":401002}", 0, &next), AZ_IOT_ERR_AUTH);
  assert_int_equal(t.imports, 0);
}

static void malformed_challenges_are_protocol_errors(void** state)
{
  (void)state;
  static const char* const bodies[] = {
    "not json",
    "{\"authenticationKey\":\"A$==\"}",
    "{\"authenticationKey\":\"AA\\nA\"}",
    "{\"authenticationKey\":\"AAAA\",\"keyName\":\"bad name\"}",
  };
  for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i)
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client_options o = options_for(&t);
    az_iot_dps_tpm_client c;
    assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(respond(&c, 401, bodies[i], 0, &next), AZ_IOT_ERR_PROTOCOL);
    assert_int_equal(t.imports, 0);
  }
}

static void a_2xx_before_the_challenge_is_a_protocol_error(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client_options o = options_for(&t);
  az_iot_dps_tpm_client c;
  assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_int_equal(
      respond(&c, 202, "{\"operationId\":\"x\",\"status\":\"assigning\"}", 0, &next),
      AZ_IOT_ERR_PROTOCOL);
}

static void service_failures_report_the_error_code(void** state)
{
  (void)state;
  /* A 4xx with an error body. */
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client c;
    challenge(&c, &t);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(
        respond(&c, 400, "{\"errorCode\":400004,\"trackingId\":\"t\"}", 0, &next), AZ_IOT_ERR_DPS);
    uint16_t http = 0;
    int32_t code = 0;
    az_iot_dps_tpm_client_get_error(&c, &http, &code);
    assert_int_equal(http, 400);
    assert_int_equal(code, 400004);
  }
  /* A failed registration. */
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client c;
    challenge(&c, &t);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(
        respond(
            &c,
            200,
            "{\"operationId\":\"o\",\"status\":\"failed\",\"registrationState\":{"
            "\"status\":\"failed\",\"errorCode\":400209,\"errorMessage\":\"m\"}}",
            0,
            &next),
        AZ_IOT_ERR_DPS);
    int32_t code = 0;
    az_iot_dps_tpm_client_get_error(&c, NULL, &code);
    assert_int_equal(code, 400209);
    assert_null(az_iot_dps_tpm_client_get_assignment(&c));
  }
}

static char last_log[256];

static void capture_log(
    void* user_ctx,
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* msg)
{
  (void)user_ctx;
  (void)level;
  (void)component;
  (void)file;
  (void)line;
  size_t n = strlen(msg);
  n = n < sizeof(last_log) - 1 ? n : sizeof(last_log) - 1;
  memcpy(last_log, msg, n);
  last_log[n] = '\0';
}

static void a_failed_status_is_logged(void** state)
{
  (void)state;
  az_iot_log_sink sink = { capture_log, NULL, AZ_IOT_LOG_LEVEL_TRACE };
  az_iot_log_set_global_sink(&sink);
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client c;
  challenge(&c, &t);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  last_log[0] = '\0';
  assert_int_equal(respond(&c, 200, "{\"status\":\"disabled\"}", 0, &next), AZ_IOT_ERR_DPS);
  az_iot_log_set_global_sink(NULL);
  assert_non_null(strstr(last_log, "status \"disabled\", errorCode 0"));
}

static void assignment_problems_are_protocol_errors(void** state)
{
  (void)state;
  static const char* const bodies[] = {
    "{\"status\":\"assigned\",\"registrationState\":{\"deviceId\":\"d\"}}",
    "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\"}}",
    "{\"status\":\"assigning\"}",
    "{}",
    "{\"operationId\":\"a\\nb\",\"status\":\"assigning\"}",
    "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":"
    "\"d\\tx\"}}",
    "{\"status\":\"\"}",
    "{\"status\":null}",
    "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\\u002ex\",\"deviceId\":"
    "\"d\"}}",
    "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\",\"deviceId\":\"d\","
    "\"tpm\":{\"authenticationKey\":\"@@@@\"}}}",
  };
  for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i)
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client c;
    challenge(&c, &t);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(respond(&c, 200, bodies[i], 0, &next), AZ_IOT_ERR_PROTOCOL);
    assert_false(next.assigned);
    assert_null(az_iot_dps_tpm_client_get_assignment(&c));
  }
}

static void tpm_failures_end_the_registration(void** state)
{
  (void)state;
  /* import_key. */
  {
    fake_tpm t = { 0 };
    t.import_result = AZ_IOT_ERR_INTERNAL;
    az_iot_dps_tpm_client_options o = options_for(&t);
    az_iot_dps_tpm_client c;
    assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(
        respond(&c, 401, "{\"authenticationKey\":\"" HUB_KEY_B64 "\"}", 0, &next),
        AZ_IOT_ERR_INTERNAL);
    assert_int_equal(
        az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req),
        AZ_IOT_ERR_NOT_INITIALIZED);
  }
  /* hmac_sha256: the request is not built; the buffer is wiped. */
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client c;
    challenge(&c, &t);
    t.hmac_result = AZ_IOT_ERR_TLS;
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    assert_int_equal(
        az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_ERR_TLS);
    assert_null(req.path);
    assert_int_equal(buf[0], 0);
    /* A TPM that recovers can sign the same request. */
    t.hmac_result = AZ_IOT_OK;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  }
}

static void a_short_buffer_or_zero_time_is_rejected(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client c;
  challenge(&c, &t);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  assert_int_equal(
      az_iot_dps_tpm_client_get_request(&c, 0, buf, sizeof(buf), &req), AZ_IOT_ERR_INVALID_ARG);
  /* An expiry past UINT64_MAX is rejected; the output is cleared. */
  static const uint64_t too_late[]
      = { UINT64_MAX, UINT64_MAX - AZ_IOT_DPS_TPM_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS + 1u };
  for (size_t k = 0; k < sizeof(too_late) / sizeof(too_late[0]); ++k)
  {
    memset(buf, 'x', sizeof(buf));
    memset(&req, 0xA5, sizeof(req));
    assert_int_equal(
        az_iot_dps_tpm_client_get_request(&c, too_late[k], buf, sizeof(buf), &req),
        AZ_IOT_ERR_INVALID_ARG);
    assert_null(req.path);
    assert_null(req.authorization);
    assert_int_equal(buf[0], 0);
  }
  /* The largest valid time still works. */
  assert_int_equal(
      az_iot_dps_tpm_client_get_request(
          &c,
          UINT64_MAX - AZ_IOT_DPS_TPM_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS,
          buf,
          sizeof(buf),
          &req),
      AZ_IOT_OK);
  for (size_t size = 1; size < 300; size += 7)
  {
    memset(buf, 'x', sizeof(buf));
    assert_int_equal(
        az_iot_dps_tpm_client_get_request(&c, NOW, buf, size, &req), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    for (size_t i = 0; i < size; ++i)
    {
      assert_int_equal(buf[i], 0);
    }
  }
  /* Still usable. */
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
}

static void optional_members_and_options_are_sent(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client_options o = options_for(&t);
  o.registration_id = "a:b";
  o.csr = "TUlJQw==";
  o.payload = " {\"x\":1} ";
  o.api_version = "2021-06-01";
  o.sas_token_lifetime_seconds = 60;
  az_iot_dps_tpm_client c;
  assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_string_equal(req.path, "/" SCOPE "/registrations/a%3Ab/register?api-version=2021-06-01");
  assert_string_equal(
      req.body,
      "{\"registrationId\":\"a:b\",\"tpm\":{\"endorsementKey\":\"" EK_B64
      "\",\"storageRootKey\":\"" SRK_B64 "\"},\"csr\":\"TUlJQw==\",\"payload\": {\"x\":1} }");

  /* Key name from the challenge; lifetime from the options. */
  az_iot_dps_tpm_next next;
  assert_int_equal(
      respond(&c, 401, "{\"authenticationKey\":\"" HUB_KEY_B64 "\",\"keyName\":\"k1\"}", 0, &next),
      AZ_IOT_OK);
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  char token[512];
  size_t len = 0;
  assert_int_equal(
      az_iot_sas_token_sign(
          TEST_CRYPTO(),
          hub_key,
          sizeof(hub_key),
          SCOPE "%2fregistrations%2fa%3Ab",
          "k1",
          NOW + 60u,
          token,
          sizeof(token),
          &len),
      AZ_IOT_OK);
  assert_string_equal(req.authorization, token);
}

static void init_rejects_bad_options(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client c;
  az_iot_dps_tpm_client_options o = options_for(&t);
  assert_int_equal(az_iot_dps_tpm_client_init(NULL, &o), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_dps_tpm_client_init(&c, NULL), AZ_IOT_ERR_INVALID_ARG);

  char long_id[AZ_IOT_DPS_TPM_ID_MAX + 2];
  memset(long_id, 'a', sizeof(long_id) - 1);
  long_id[sizeof(long_id) - 1] = '\0';

  for (int i = 0; i < 20; ++i)
  {
    o = options_for(&t);
    switch (i)
    {
      case 0:
        o.id_scope = NULL;
        break;
      case 1:
        o.id_scope = "0ne:1";
        break;
      case 2:
        o.registration_id = "";
        break;
      case 3:
        o.registration_id = "a\"b";
        break;
      case 4:
        o.registration_id = long_id;
        break;
      case 5:
        o.endorsement_key = NULL;
        break;
      case 6:
        o.endorsement_key_len = 0;
        break;
      case 7:
        o.storage_root_key = NULL;
        break;
      case 8:
        o.storage_root_key_len = 0;
        break;
      case 9:
        o.payload = "[1]";
        break;
      case 10:
        o.payload = "{\"a\":1}{}";
        break;
      case 11:
        o.csr = "ab\"c";
        break;
      case 12:
        o.csr = "";
        break;
      case 13:
        o.api_version = "1&x=2";
        break;
      case 14:
        o.csr = "A";
        break;
      case 15:
        o.csr = "====";
        break;
      case 16:
        o.csr = "QR==";
        break;
      case 17:
        o.csr = "QUJ\\/";
        break;
      case 18:
        o.tpm.import_key = NULL;
        break;
      default:
        o.tpm.hmac_sha256 = NULL;
        break;
    }
    assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_ERR_INVALID_ARG);
    char buf[64];
    az_iot_dps_tpm_request req;
    assert_int_equal(
        az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req),
        AZ_IOT_ERR_NOT_INITIALIZED);
  }

  /* The first and last characters must be alphanumeric. */
  static const char* const bad_ends[] = { "a.", "a_", "a:", "a-", "-a", ":a", "_a.b" };
  for (size_t k = 0; k < sizeof(bad_ends) / sizeof(bad_ends[0]); ++k)
  {
    o = options_for(&t);
    o.registration_id = bad_ends[k];
    assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_ERR_INVALID_ARG);
  }
  static const char* const good_ends[] = { "a", "A9", "a-b", "a:b", "a.b_c-d" };
  for (size_t k = 0; k < sizeof(good_ends) / sizeof(good_ends[0]); ++k)
  {
    o = options_for(&t);
    o.registration_id = good_ends[k];
    assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
  }

  o = options_for(&t);
  o.registration_id = long_id + 1; /* exactly AZ_IOT_DPS_TPM_ID_MAX */
  assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
}

static void null_arguments_are_rejected(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client c;
  challenge(&c, &t);
  char buf[16];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(
      az_iot_dps_tpm_client_get_request(NULL, NOW, buf, sizeof(buf), &req), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dps_tpm_client_get_request(&c, NOW, NULL, sizeof(buf), &req), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dps_tpm_client_get_request(&c, NOW, buf, 0, &req), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dps_tpm_client_handle_response(NULL, 200, AZ_SPAN_EMPTY, 0, &next),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dps_tpm_client_handle_response(&c, 200, AZ_SPAN_EMPTY, 0, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  /* No request outstanding. */
  assert_int_equal(
      az_iot_dps_tpm_client_handle_response(&c, 200, AZ_SPAN_EMPTY, 0, &next),
      AZ_IOT_ERR_NOT_INITIALIZED);
  assert_null(az_iot_dps_tpm_client_get_assignment(NULL));
  uint16_t http = 1;
  int32_t code = 1;
  az_iot_dps_tpm_client_get_error(NULL, &http, &code);
  assert_int_equal(http, 0);
  assert_int_equal(code, 0);
  az_iot_dps_tpm_client_get_error(&c, NULL, NULL);
}

static void parse_key_splits_the_parts(void** state)
{
  (void)state;
  az_iot_dps_tpm_key_parts p;
  assert_int_equal(az_iot_dps_tpm_parse_key(challenge_key, sizeof(challenge_key), &p), AZ_IOT_OK);
  assert_ptr_equal(az_span_ptr(p.credential_blob), challenge_key);
  assert_int_equal(az_span_size(p.credential_blob), 4);
  assert_ptr_equal(az_span_ptr(p.secret), challenge_key + 4);
  assert_int_equal(az_span_size(p.secret), 3);
  assert_int_equal(az_span_size(p.duplicate), 2);
  assert_ptr_equal(az_span_ptr(p.in_sym_seed), challenge_key + 9);
  assert_int_equal(az_span_size(p.in_sym_seed), 3);
  assert_ptr_equal(az_span_ptr(p.object_public), challenge_key + 12);
  assert_int_equal(az_span_size(p.object_public), 5);
  assert_ptr_equal(az_span_ptr(p.encrypted_data), challenge_key + 17);
  assert_int_equal(az_span_size(p.encrypted_data), 3);

  /* Without encrypted data. */
  assert_int_equal(az_iot_dps_tpm_parse_key(challenge_key, 17, &p), AZ_IOT_OK);
  assert_int_equal(az_span_size(p.object_public), 5);
  assert_int_equal(az_span_size(p.encrypted_data), 0);

  /* Bytes after the encrypted data are ignored. */
  uint8_t longer[sizeof(challenge_key) + 2];
  memcpy(longer, challenge_key, sizeof(challenge_key));
  longer[sizeof(challenge_key)] = 0x01;
  longer[sizeof(challenge_key) + 1] = 0x02;
  assert_int_equal(az_iot_dps_tpm_parse_key(longer, sizeof(longer), &p), AZ_IOT_OK);
  assert_int_equal(az_span_size(p.encrypted_data), 3);

  /* Every truncation that cuts a part fails and zeroes the output. */
  for (size_t n = 1; n < sizeof(challenge_key); ++n)
  {
    if (n == 17)
    {
      continue;
    }
    assert_int_equal(az_iot_dps_tpm_parse_key(challenge_key, n, &p), AZ_IOT_ERR_PROTOCOL);
    assert_null(az_span_ptr(p.credential_blob));
    assert_int_equal(az_span_size(p.object_public), 0);
  }
  assert_int_equal(az_iot_dps_tpm_parse_key(NULL, 4, &p), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_dps_tpm_parse_key(challenge_key, 0, &p), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dps_tpm_parse_key(challenge_key, sizeof(challenge_key), NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void registration_id_is_base32_of_the_ek_hash(void** state)
{
  (void)state;
  char id[AZ_IOT_DPS_TPM_REGISTRATION_ID_SIZE];
  /* printf abc | openssl dgst -sha256 -binary | base32 | tr A-Z a-z | tr -d = */
  assert_int_equal(
      az_iot_dps_tpm_registration_id_from_ek(TEST_CRYPTO(), (const uint8_t*)"abc", 3, id),
      AZ_IOT_OK);
  assert_string_equal(id, "xj4bnp4pahh6uqkbidpf3lrceoyagyndsylxvhfucd7wd4qacwwq");
  const uint8_t bytes[] = { 1, 2, 3 };
  assert_int_equal(
      az_iot_dps_tpm_registration_id_from_ek(TEST_CRYPTO(), bytes, sizeof(bytes), id), AZ_IOT_OK);
  assert_string_equal(id, "aoifrrxsydfuslcthmfe2fhpo7ga66flzthnkkd5qsq2eai47oaq");

  assert_int_equal(
      az_iot_dps_tpm_registration_id_from_ek(NULL, bytes, sizeof(bytes), id),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(id[0], '\0');
  assert_int_equal(
      az_iot_dps_tpm_registration_id_from_ek(TEST_CRYPTO(), NULL, 3, id), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dps_tpm_registration_id_from_ek(TEST_CRYPTO(), bytes, 0, id), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dps_tpm_registration_id_from_ek(TEST_CRYPTO(), bytes, sizeof(bytes), NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

static void maximum_length_ids_fit(void** state)
{
  (void)state;
  /* Every ':' is encoded as three characters in the path and the token. */
  char scope[AZ_IOT_DPS_TPM_ID_MAX + 1];
  char id[AZ_IOT_DPS_TPM_ID_MAX + 1];
  memset(scope, 'a', AZ_IOT_DPS_TPM_ID_MAX);
  scope[AZ_IOT_DPS_TPM_ID_MAX] = '\0';
  memset(id, ':', AZ_IOT_DPS_TPM_ID_MAX - 1);
  id[0] = 'a';
  id[AZ_IOT_DPS_TPM_ID_MAX - 1] = 'a';
  id[AZ_IOT_DPS_TPM_ID_MAX] = '\0';
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client_options o = options_for(&t);
  o.id_scope = scope;
  o.registration_id = id;
  az_iot_dps_tpm_client c;
  assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
  static char buf[4096];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_int_equal(
      respond(&c, 401, "{\"authenticationKey\":\"" HUB_KEY_B64 "\"}", 0, &next), AZ_IOT_OK);
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_non_null(req.authorization);
  assert_non_null(strstr(req.authorization, "%2fregistrations%2fa%3A%3A"));
}

static void the_default_buffer_fits_the_longest_rsa2048_requests(void** state)
{
  (void)state;
  char scope[AZ_IOT_DPS_TPM_ID_MAX + 1];
  char id[AZ_IOT_DPS_TPM_ID_MAX + 1];
  char version[AZ_IOT_DPS_TPM_ID_MAX + 1];
  memset(scope, 'a', AZ_IOT_DPS_TPM_ID_MAX);
  scope[AZ_IOT_DPS_TPM_ID_MAX] = '\0';
  memset(id, ':', AZ_IOT_DPS_TPM_ID_MAX - 1);
  id[0] = 'a';
  id[AZ_IOT_DPS_TPM_ID_MAX - 1] = 'a';
  id[AZ_IOT_DPS_TPM_ID_MAX] = '\0';
  memset(version, '9', AZ_IOT_DPS_TPM_ID_MAX);
  version[AZ_IOT_DPS_TPM_ID_MAX] = '\0';
  static uint8_t key[AZ_IOT_DPS_TPM_RSA2048_PUBLIC_MAX];
  memset(key, 0xA5, sizeof(key));

  fake_tpm t = { 0 };
  az_iot_dps_tpm_client_options o = options_for(&t);
  o.id_scope = scope;
  o.registration_id = id;
  o.api_version = version;
  o.endorsement_key = key;
  o.endorsement_key_len = sizeof(key);
  o.storage_root_key = key;
  o.storage_root_key_len = sizeof(key);
  az_iot_dps_tpm_client c;
  assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
  static char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);

  /* Longest key name. */
  static char json[512];
  char name[AZ_IOT_DPS_TPM_TOKEN_MAX + 1];
  memset(name, 'k', AZ_IOT_DPS_TPM_TOKEN_MAX);
  name[AZ_IOT_DPS_TPM_TOKEN_MAX] = '\0';
  const char* challenge_parts[]
      = { "{\"authenticationKey\":\"" HUB_KEY_B64 "\",\"keyName\":\"", name, "\"}" };
  size_t len = 0;
  for (size_t k = 0; k < 3; ++k)
  {
    size_t n = strlen(challenge_parts[k]);
    memcpy(json + len, challenge_parts[k], n);
    len += n;
  }
  json[len] = '\0';
  assert_int_equal(respond(&c, 401, json, 0, &next), AZ_IOT_OK);
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_non_null(req.authorization);
  assert_non_null(req.body);

  /* Longest operation ID, every character encoded. */
  char op[AZ_IOT_DPS_TPM_TOKEN_MAX + 1];
  memset(op, ':', AZ_IOT_DPS_TPM_TOKEN_MAX);
  op[AZ_IOT_DPS_TPM_TOKEN_MAX] = '\0';
  const char* op_parts[] = { "{\"operationId\":\"", op, "\",\"status\":\"assigning\"}" };
  len = 0;
  for (size_t k = 0; k < 3; ++k)
  {
    size_t n = strlen(op_parts[k]);
    memcpy(json + len, op_parts[k], n);
    len += n;
  }
  json[len] = '\0';
  assert_int_equal(respond(&c, 202, json, 0, &next), AZ_IOT_OK);
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  assert_string_equal(req.method, "GET");
}

static void an_escaped_value_that_fits_unescaped_is_accepted(void** state)
{
  (void)state;
  fake_tpm t = { 0 };
  az_iot_dps_tpm_client c;
  challenge(&c, &t);
  char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
  az_iot_dps_tpm_request req;
  az_iot_dps_tpm_next next;
  assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
  /* A device ID of AZ_IOT_DPS_TPM_ID_MAX characters, one of them escaped. */
  char device[AZ_IOT_DPS_TPM_ID_MAX + 2];
  memset(device, 'd', sizeof(device) - 1);
  device[0] = '\\';
  device[1] = '/';
  device[sizeof(device) - 1] = '\0';
  char json[512];
  const char* parts[]
      = { "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":"
          "\"h\",\"tpm\":{\"authenticationKey\":\"" HUB_KEY_B64 "\"},\"deviceId\":\"",
          device,
          "\"}}" };
  size_t len = 0;
  for (size_t k = 0; k < sizeof(parts) / sizeof(parts[0]); ++k)
  {
    size_t n = strlen(parts[k]);
    assert_true(len + n < sizeof(json));
    memcpy(json + len, parts[k], n);
    len += n;
  }
  json[len] = '\0';
  assert_int_equal(respond(&c, 200, json, 0, &next), AZ_IOT_OK);
  const char* got = az_iot_dps_tpm_client_get_assignment(&c)->device_id;
  assert_int_equal(strlen(got), AZ_IOT_DPS_TPM_ID_MAX);
  assert_int_equal(got[0], '/');
}

static void content_after_the_response_object_is_rejected(void** state)
{
  (void)state;
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client_options o = options_for(&t);
    az_iot_dps_tpm_client c;
    assert_int_equal(az_iot_dps_tpm_client_init(&c, &o), AZ_IOT_OK);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(
        respond(&c, 401, "{\"authenticationKey\":\"" HUB_KEY_B64 "\"}{}", 0, &next),
        AZ_IOT_ERR_PROTOCOL);
    assert_int_equal(t.imports, 0);
  }
  {
    fake_tpm t = { 0 };
    az_iot_dps_tpm_client c;
    challenge(&c, &t);
    char buf[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE];
    az_iot_dps_tpm_request req;
    az_iot_dps_tpm_next next;
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(
        respond(
            &c,
            200,
            "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\","
            "\"deviceId\":\"d\"}} x",
            0,
            &next),
        AZ_IOT_ERR_PROTOCOL);
    /* Trailing whitespace is fine. */
    challenge(&c, &t);
    assert_int_equal(az_iot_dps_tpm_client_get_request(&c, NOW, buf, sizeof(buf), &req), AZ_IOT_OK);
    assert_int_equal(
        respond(
            &c,
            200,
            "{\"status\":\"assigned\",\"registrationState\":{\"assignedHub\":\"h\","
            "\"tpm\":{\"authenticationKey\":\"" HUB_KEY_B64 "\"},\"deviceId\":\"d\"}} \r\n",
            0,
            &next),
        AZ_IOT_OK);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(full_flow_registers_and_imports_both_keys),
    cmocka_unit_test(assigned_from_register_needs_no_poll),
    cmocka_unit_test(an_mqttv3_assignment_needs_a_hub_key_or_a_certificate),
    cmocka_unit_test(connection_profiles_map_to_the_enum),
    cmocka_unit_test(throttling_and_server_errors_keep_the_request),
    cmocka_unit_test(error_codes_are_numbers_and_the_registration_state_one_wins),
    cmocka_unit_test(statuses_past_599_are_not_retried),
    cmocka_unit_test(a_refused_token_is_an_auth_failure),
    cmocka_unit_test(a_challenge_without_a_key_is_an_auth_failure),
    cmocka_unit_test(malformed_challenges_are_protocol_errors),
    cmocka_unit_test(a_2xx_before_the_challenge_is_a_protocol_error),
    cmocka_unit_test(service_failures_report_the_error_code),
    cmocka_unit_test(assignment_problems_are_protocol_errors),
    cmocka_unit_test(a_failed_status_is_logged),
    cmocka_unit_test(tpm_failures_end_the_registration),
    cmocka_unit_test(a_short_buffer_or_zero_time_is_rejected),
    cmocka_unit_test(optional_members_and_options_are_sent),
    cmocka_unit_test(init_rejects_bad_options),
    cmocka_unit_test(null_arguments_are_rejected),
    cmocka_unit_test(parse_key_splits_the_parts),
    cmocka_unit_test(registration_id_is_base32_of_the_ek_hash),
    cmocka_unit_test(maximum_length_ids_fit),
    cmocka_unit_test(the_default_buffer_fits_the_longest_rsa2048_requests),
    cmocka_unit_test(an_escaped_value_that_fits_unescaped_is_accepted),
    cmocka_unit_test(content_after_the_response_object_is_rejected),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
