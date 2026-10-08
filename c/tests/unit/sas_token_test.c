// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* SAS token helpers: the tokens match the ones the connection client signs
 * from the same keys (connection_sas_test.c), byte for byte. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <string.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_sas_token.h"

#if defined(AZ_IOT_TEST_CRYPTO_MBEDTLS)
#include "az_iot_crypto_mbedtls.h"
#define TEST_CRYPTO() az_iot_crypto_mbedtls()
#else
#include "az_iot_crypto_openssl.h"
#define TEST_CRYPTO() az_iot_crypto_openssl()
#endif

#define HUB_URI "broker.example%2Fdevices%2Fut-device"
#define DPS_URI "0ne00000001%2fregistrations%2fut-device"
#define EXPIRY 1700003600u
/* Tokens the connection client signs (connection_sas_test.c). */
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
#define ENCODED_ID_HUB_TOKEN                                                               \
  "SharedAccessSignature sr=broker.example%2Fdevices%2Fd%40v%201%2Fx&sig=tcKOOmzVO8ju8u3X" \
  "MjhaYBAcaJFUnYwOhPtV5soYaiY%3D&se=1700003600"

/** @brief Bytes @p first .. @p first + @p n - 1. */
static void key_bytes(uint8_t* key, size_t n, uint8_t first)
{
  for (size_t i = 0; i < n; ++i)
  {
    key[i] = (uint8_t)(first + i);
  }
}

static bool all_zero(const char* p, size_t n)
{
  for (size_t i = 0; i < n; ++i)
  {
    if (p[i] != 0)
    {
      return false;
    }
  }
  return true;
}

static void assert_signed(
    const uint8_t* key,
    size_t key_len,
    const char* uri,
    const char* key_name,
    uint64_t expiry,
    const char* expected)
{
  char token[AZ_IOT_SAS_TOKEN_SIZE(256)];
  size_t len = 0;
  assert_int_equal(
      az_iot_sas_token_sign(
          TEST_CRYPTO(), key, key_len, uri, key_name, expiry, token, sizeof(token), &len),
      AZ_IOT_OK);
  assert_string_equal(token, expected);
  assert_int_equal(len, strlen(expected));
}

/* The tokens the client signs from the same keys. */
static void sign_matches_the_client_tokens(void** state)
{
  (void)state;
  uint8_t key[32];
  key_bytes(key, sizeof(key), 0);
  assert_signed(key, sizeof(key), HUB_URI, NULL, EXPIRY, HUB_TOKEN);
  assert_signed(key, sizeof(key), HUB_URI, "", EXPIRY, HUB_TOKEN);
  assert_signed(key, sizeof(key), DPS_URI, "registration", EXPIRY, DPS_TOKEN);
  assert_signed(
      key,
      sizeof(key),
      "broker.example%2Fdevices%2Fd%40v%201%2Fx",
      NULL,
      EXPIRY,
      ENCODED_ID_HUB_TOKEN);
  uint8_t key2[32];
  key_bytes(key2, sizeof(key2), 32);
  assert_signed(key2, sizeof(key2), HUB_URI, NULL, EXPIRY, SECONDARY_HUB_TOKEN);
}

/* A group key derives the device key the client derives. */
static void a_derived_key_matches_the_client_token(void** state)
{
  (void)state;
  uint8_t group[64];
  key_bytes(group, sizeof(group), 0);
  uint8_t device[AZ_IOT_SHA256_SIZE];
  assert_int_equal(
      az_iot_sas_derive_device_key(TEST_CRYPTO(), group, sizeof(group), "ut-device", device),
      AZ_IOT_OK);
  assert_signed(device, sizeof(device), DPS_URI, "registration", 1700000600u, GROUP_DPS_TOKEN);
}

static void the_string_to_sign_is_uri_newline_expiry(void** state)
{
  (void)state;
  char out[AZ_IOT_SAS_STRING_TO_SIGN_SIZE(sizeof(DPS_URI) - 1u)];
  size_t len = 0;
  assert_int_equal(
      az_iot_sas_token_string_to_sign(DPS_URI, EXPIRY, out, sizeof(out), &len), AZ_IOT_OK);
  assert_string_equal(out, DPS_URI "\n1700003600");
  assert_int_equal(len, strlen(out));
  /* The largest expiry fits the macro's size. */
  assert_int_equal(
      az_iot_sas_token_string_to_sign(DPS_URI, UINT64_MAX, out, sizeof(out), &len), AZ_IOT_OK);
  assert_string_equal(out, DPS_URI "\n18446744073709551615");
  assert_int_equal(len + 1u, sizeof(out));
  assert_int_equal(
      az_iot_sas_token_string_to_sign(DPS_URI, UINT64_MAX, out, sizeof(out) - 1u, &len),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(out[0], '\0');
}

/* The signature from an external signer: base64 with `+`, `/` and `=` URL-encoded. */
static void a_signature_is_url_encoded_base64(void** state)
{
  (void)state;
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  memset(mac, 0, sizeof(mac));
  char token[AZ_IOT_SAS_TOKEN_SIZE_FOR(sizeof(HUB_URI) - 1u, 0)];
  size_t len = 0;
  assert_int_equal(
      az_iot_sas_token_from_signature(HUB_URI, NULL, 5u, mac, token, sizeof(token), &len),
      AZ_IOT_OK);
  assert_string_equal(
      token,
      "SharedAccessSignature sr=" HUB_URI
      "&sig=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA%3D&se=5");
  /* 0xFB 0xEF 0xBE is `++++`, 0xFF 0xFF 0xFF is `////`. */
  for (size_t i = 0; i + 2u < sizeof(mac); i += 3u)
  {
    mac[i] = i < 15u ? 0xFBu : 0xFFu;
    mac[i + 1u] = i < 15u ? 0xEFu : 0xFFu;
    mac[i + 2u] = i < 15u ? 0xBEu : 0xFFu;
  }
  assert_int_equal(
      az_iot_sas_token_from_signature(HUB_URI, NULL, 5u, mac, token, sizeof(token), &len),
      AZ_IOT_OK);
  assert_non_null(strstr(token, "&sig=%2B%2B%2B%2B"));
  assert_non_null(strstr(token, "%2F%2F%2F%2F"));
  assert_int_equal(len, strlen(token));
}

/* AZ_IOT_SAS_TOKEN_SIZE_FOR() is enough for the worst signature and expiry,
 * and AZ_IOT_SAS_TOKEN_SIZE() for a request's URI and key name. */
static void the_size_macros_cover_the_worst_case(void** state)
{
  (void)state;
  /* A DPS URI of 256 ID characters, all encoded. */
  char uri[3u * 256u + 20u];
  size_t n = 0;
  for (size_t i = 0; i < 11u; ++i)
  {
    memcpy(uri + n, "%41", 3u);
    n += 3u;
  }
  memcpy(uri + n, "%2fregistrations%2f", 19u);
  n += 19u;
  for (size_t i = 0; i < 245u; ++i)
  {
    memcpy(uri + n, "%40", 3u);
    n += 3u;
  }
  uri[n] = '\0';
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  memset(mac, 0xFF, sizeof(mac));
  char token[AZ_IOT_SAS_TOKEN_SIZE(256)];
  assert_int_equal(AZ_IOT_SAS_TOKEN_SIZE_FOR(n, 12u), sizeof(token));
  size_t len = 0;
  assert_int_equal(
      az_iot_sas_token_from_signature(
          uri, "registration", UINT64_MAX, mac, token, sizeof(token), &len),
      AZ_IOT_OK);
  assert_true(len + 1u <= sizeof(token));
}

/* One byte short: NOT_ENOUGH_SPACE and the buffer is zeroed. */
static void a_short_buffer_fails_and_is_wiped(void** state)
{
  (void)state;
  uint8_t key[32];
  key_bytes(key, sizeof(key), 0);
  char token[sizeof(DPS_TOKEN)];
  size_t len = 0;
  assert_int_equal(
      az_iot_sas_token_sign(
          TEST_CRYPTO(),
          key,
          sizeof(key),
          DPS_URI,
          "registration",
          EXPIRY,
          token,
          sizeof(token),
          &len),
      AZ_IOT_OK);
  assert_string_equal(token, DPS_TOKEN);
  memset(token, 'x', sizeof(token));
  assert_int_equal(
      az_iot_sas_token_sign(
          TEST_CRYPTO(),
          key,
          sizeof(key),
          DPS_URI,
          "registration",
          EXPIRY,
          token,
          sizeof(token) - 1u,
          &len),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_true(all_zero(token, sizeof(token) - 1u));
  /* Too short even for the string to sign. */
  memset(token, 'x', sizeof(token));
  assert_int_equal(
      az_iot_sas_token_sign(
          TEST_CRYPTO(), key, sizeof(key), DPS_URI, "registration", EXPIRY, token, 8u, &len),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_true(all_zero(token, 8u));
}

static void invalid_arguments_are_rejected(void** state)
{
  (void)state;
  uint8_t key[32];
  key_bytes(key, sizeof(key), 0);
  uint8_t mac[AZ_IOT_SHA256_SIZE] = { 0 };
  char out[AZ_IOT_SAS_TOKEN_SIZE(256)];
  size_t len = 0;
  const az_iot_crypto* crypto = TEST_CRYPTO();

  assert_int_equal(
      az_iot_sas_token_string_to_sign(NULL, EXPIRY, out, sizeof(out), &len),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas_token_string_to_sign("", EXPIRY, out, sizeof(out), &len), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas_token_string_to_sign(HUB_URI, 0u, out, sizeof(out), &len), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas_token_string_to_sign(HUB_URI, EXPIRY, NULL, sizeof(out), &len),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas_token_string_to_sign(HUB_URI, EXPIRY, out, sizeof(out), NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas_token_string_to_sign(HUB_URI, EXPIRY, out, 0u, &len), AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  /* Characters that would change the token's fields. */
  static const char* const k_bad[] = { "a&skn=x", "a b", "a\nb", "a=b", "a/b" };
  for (size_t i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); ++i)
  {
    assert_int_equal(
        az_iot_sas_token_from_signature(k_bad[i], NULL, EXPIRY, mac, out, sizeof(out), &len),
        AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(
        az_iot_sas_token_from_signature(HUB_URI, k_bad[i], EXPIRY, mac, out, sizeof(out), &len),
        AZ_IOT_ERR_INVALID_ARG);
  }
  assert_int_equal(
      az_iot_sas_token_from_signature(HUB_URI, NULL, EXPIRY, NULL, out, sizeof(out), &len),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas_token_from_signature(HUB_URI, NULL, 0u, mac, out, sizeof(out), &len),
      AZ_IOT_ERR_INVALID_ARG);

  assert_int_equal(
      az_iot_sas_token_sign(NULL, key, sizeof(key), HUB_URI, NULL, EXPIRY, out, sizeof(out), &len),
      AZ_IOT_ERR_INVALID_ARG);
  az_iot_crypto wrong_version = *crypto;
  wrong_version.version = AZ_IOT_CRYPTO_VERSION + 1u;
  assert_int_equal(
      az_iot_sas_token_sign(
          &wrong_version, key, sizeof(key), HUB_URI, NULL, EXPIRY, out, sizeof(out), &len),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas_token_sign(crypto, NULL, 32u, HUB_URI, NULL, EXPIRY, out, sizeof(out), &len),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas_token_sign(crypto, key, 0u, HUB_URI, NULL, EXPIRY, out, sizeof(out), &len),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_sas_token_sign(crypto, key, sizeof(key), "a&b", NULL, EXPIRY, out, sizeof(out), &len),
      AZ_IOT_ERR_INVALID_ARG);
}

static void derive_rejects_bad_arguments_and_zeroes_its_output(void** state)
{
  (void)state;
  uint8_t group[64];
  key_bytes(group, sizeof(group), 0);
  uint8_t out[AZ_IOT_SHA256_SIZE];
  const az_iot_crypto* crypto = TEST_CRYPTO();
  const uint8_t zero[AZ_IOT_SHA256_SIZE] = { 0 };

  memset(out, 0xAA, sizeof(out));
  assert_int_equal(
      az_iot_sas_derive_device_key(crypto, group, sizeof(group), "", out), AZ_IOT_ERR_INVALID_ARG);
  assert_memory_equal(out, zero, sizeof(out));
  memset(out, 0xAA, sizeof(out));
  assert_int_equal(
      az_iot_sas_derive_device_key(crypto, group, sizeof(group), NULL, out),
      AZ_IOT_ERR_INVALID_ARG);
  assert_memory_equal(out, zero, sizeof(out));
  memset(out, 0xAA, sizeof(out));
  assert_int_equal(
      az_iot_sas_derive_device_key(crypto, NULL, 64u, "id", out), AZ_IOT_ERR_INVALID_ARG);
  assert_memory_equal(out, zero, sizeof(out));
  memset(out, 0xAA, sizeof(out));
  assert_int_equal(
      az_iot_sas_derive_device_key(crypto, group, 0u, "id", out), AZ_IOT_ERR_INVALID_ARG);
  assert_memory_equal(out, zero, sizeof(out));
  memset(out, 0xAA, sizeof(out));
  assert_int_equal(
      az_iot_sas_derive_device_key(NULL, group, sizeof(group), "id", out), AZ_IOT_ERR_INVALID_ARG);
  assert_memory_equal(out, zero, sizeof(out));
  assert_int_equal(
      az_iot_sas_derive_device_key(crypto, group, sizeof(group), "id", NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(sign_matches_the_client_tokens),
    cmocka_unit_test(a_derived_key_matches_the_client_token),
    cmocka_unit_test(the_string_to_sign_is_uri_newline_expiry),
    cmocka_unit_test(a_signature_is_url_encoded_base64),
    cmocka_unit_test(the_size_macros_cover_the_worst_case),
    cmocka_unit_test(a_short_buffer_fails_and_is_wiped),
    cmocka_unit_test(invalid_arguments_are_rejected),
    cmocka_unit_test(derive_rejects_bad_arguments_and_zeroes_its_output),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
