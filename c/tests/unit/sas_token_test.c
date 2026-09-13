// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/*
 * Shared access signature token assembly.
 *
 * The format is exact and every mistake in it is SILENT: a wrong audience, a
 * '/' left unescaped, a missing or spurious `skn` -- none of them produce a
 * diagnosable error. The service simply refuses the connection. So the shape is
 * pinned here against the form measured against a live endpoint rather than
 * against a second reading of the same code.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "internal/sas_token.h"

/* A signing callback that returns a fixed, recognisable MAC. The point of these
 * tests is the assembly around the signature, not the HMAC itself. */
static az_iot_result fake_sign(
    const uint8_t* string_to_sign,
    size_t string_to_sign_len,
    uint8_t* out_signature,
    size_t out_signature_size,
    size_t* out_signature_len,
    void* user_ctx)
{
  if (user_ctx != NULL)
  {
    /* Let a test capture exactly what it was asked to sign. */
    memcpy(user_ctx, string_to_sign, string_to_sign_len);
    ((char*)user_ctx)[string_to_sign_len] = '\0';
  }
  if (out_signature_size < 32)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  memset(out_signature, 0xAB, 32);
  *out_signature_len = 32;
  return AZ_IOT_OK;
}

static az_iot_result
failing_sign(const uint8_t* s, size_t sl, uint8_t* o, size_t os, size_t* ol, void* ctx)
{
  (void)s;
  (void)sl;
  (void)o;
  (void)os;
  (void)ol;
  (void)ctx;
  return AZ_IOT_ERR_AUTH;
}

/* A callback that claims a length it never wrote. Without a check this would
 * sign a token with uninitialized stack and send it. */
static az_iot_result
lying_sign(const uint8_t* s, size_t sl, uint8_t* o, size_t os, size_t* ol, void* ctx)
{
  (void)s;
  (void)sl;
  (void)o;
  (void)os;
  (void)ctx;
  *ol = 0;
  return AZ_IOT_OK;
}

/* The two audiences differ in the resource shape AND in whether skn is present.
 * A token scoped to one is rejected by the other, so this is not cosmetic. */
static void the_provisioning_audience_is_scoped_to_the_registration(void** state)
{
  (void)state;
  char resource[256];
  size_t len = 0;
  assert_int_equal(
      az_iot_sas__build_resource(
          AZ_IOT_SAS_AUDIENCE_PROVISIONING,
          "0ne00000000",
          "my-device",
          resource,
          sizeof(resource),
          &len),
      AZ_IOT_OK);
  assert_string_equal(resource, "0ne00000000/registrations/my-device");
  assert_int_equal(len, strlen(resource));
}

static void the_hub_audience_is_scoped_to_the_device(void** state)
{
  (void)state;
  char resource[256];
  assert_int_equal(
      az_iot_sas__build_resource(
          AZ_IOT_SAS_AUDIENCE_HUB,
          "myhub.azure-devices.net",
          "my-device",
          resource,
          sizeof(resource),
          NULL),
      AZ_IOT_OK);
  assert_string_equal(resource, "myhub.azure-devices.net/devices/my-device");
}

/* The separators must travel escaped. A raw '/' in `sr` is the single most
 * common way to get a token the service refuses without explanation. */
static void the_resource_is_percent_encoded(void** state)
{
  (void)state;
  char escaped[256];
  const char* resource = "0ne00000000/registrations/my-device";
  assert_int_equal(
      az_iot_sas__url_encode(resource, strlen(resource), escaped, sizeof(escaped), NULL),
      AZ_IOT_OK);
  assert_string_equal(escaped, "0ne00000000%2Fregistrations%2Fmy-device");
}

/* The newline is signed content, not a separator we are free to choose. */
static void the_string_to_sign_is_resource_newline_expiry(void** state)
{
  (void)state;
  char to_sign[256];
  size_t len = 0;
  assert_int_equal(
      az_iot_sas__build_string_to_sign(
          "0ne00000000%2Fregistrations%2Fmy-device", 1789000000ull, to_sign, sizeof(to_sign), &len),
      AZ_IOT_OK);
  assert_string_equal(to_sign, "0ne00000000%2Fregistrations%2Fmy-device\n1789000000");
  assert_int_equal(len, strlen(to_sign));
}

/* skn=registration belongs on the provisioning token only: the DPS device
 * endpoint rejects a token without it, and a hub rejects one with it. */
static void only_the_provisioning_token_carries_skn(void** state)
{
  (void)state;
  uint8_t sig[32];
  memset(sig, 0xAB, sizeof(sig));

  char dps[512];
  assert_int_equal(
      az_iot_sas__build_token(
          AZ_IOT_SAS_AUDIENCE_PROVISIONING,
          "0ne00000000%2Fregistrations%2Fmy-device",
          sig,
          sizeof(sig),
          1789000000ull,
          dps,
          sizeof(dps),
          NULL),
      AZ_IOT_OK);
  assert_non_null(strstr(dps, "&skn=registration"));
  assert_true(strncmp(dps, "SharedAccessSignature sr=", 25) == 0);
  assert_non_null(strstr(dps, "&se=1789000000"));

  char hub[512];
  assert_int_equal(
      az_iot_sas__build_token(
          AZ_IOT_SAS_AUDIENCE_HUB,
          "myhub.azure-devices.net%2Fdevices%2Fmy-device",
          sig,
          sizeof(sig),
          1789000000ull,
          hub,
          sizeof(hub),
          NULL),
      AZ_IOT_OK);
  assert_null(strstr(hub, "skn"));
}

/* base64 emits '+', '/' and '=', and all three change meaning in a query
 * string, so the signature is escaped after encoding. */
static void the_signature_is_base64_then_percent_encoded(void** state)
{
  (void)state;
  /* 0xFB 0xFF ... base64-encodes with both '+' and '/'. */
  uint8_t sig[32];
  memset(sig, 0xFB, sizeof(sig));

  char token[512];
  assert_int_equal(
      az_iot_sas__build_token(
          AZ_IOT_SAS_AUDIENCE_PROVISIONING,
          "scope%2Fregistrations%2Fdev",
          sig,
          sizeof(sig),
          1ull,
          token,
          sizeof(token),
          NULL),
      AZ_IOT_OK);

  const char* sig_field = strstr(token, "&sig=");
  assert_non_null(sig_field);
  /* Nothing between &sig= and the next & may be a raw base64 special. */
  for (const char* p = sig_field + 5; *p != '\0' && *p != '&'; ++p)
  {
    assert_true(*p != '+');
    assert_true(*p != '/');
    assert_true(*p != '=');
  }
}

/* The whole sequence, and the thing a caller actually gets. */
static void mint_produces_the_measured_token_shape(void** state)
{
  (void)state;
  char signed_string[256];
  char token[512];
  size_t len = 0;

  assert_int_equal(
      az_iot_sas__mint(
          AZ_IOT_SAS_AUDIENCE_PROVISIONING,
          "0ne00000000",
          "my-device",
          1789000000ull,
          fake_sign,
          signed_string,
          token,
          sizeof(token),
          &len),
      AZ_IOT_OK);

  /* What was signed is the escaped resource, not the raw one. */
  assert_string_equal(signed_string, "0ne00000000%2Fregistrations%2Fmy-device\n1789000000");

  assert_non_null(strstr(token, "sr=0ne00000000%2Fregistrations%2Fmy-device"));
  assert_non_null(strstr(token, "&se=1789000000"));
  assert_non_null(strstr(token, "&skn=registration"));
  assert_int_equal(len, strlen(token));
}

static void a_failing_signature_fails_the_mint(void** state)
{
  (void)state;
  char token[512];
  assert_int_equal(
      az_iot_sas__mint(
          AZ_IOT_SAS_AUDIENCE_PROVISIONING,
          "scope",
          "dev",
          1ull,
          failing_sign,
          NULL,
          token,
          sizeof(token),
          NULL),
      AZ_IOT_ERR_AUTH);
}

/* A callback that reports a length it did not write must not produce a token:
 * that token would be signed with whatever was on the stack. */
static void a_signature_of_no_length_is_rejected(void** state)
{
  (void)state;
  char token[512];
  assert_int_not_equal(
      az_iot_sas__mint(
          AZ_IOT_SAS_AUDIENCE_PROVISIONING,
          "scope",
          "dev",
          1ull,
          lying_sign,
          NULL,
          token,
          sizeof(token),
          NULL),
      AZ_IOT_OK);
}

/* A truncated token is worse than none: it would be sent and refused, and the
 * refusal says nothing about why. */
static void a_buffer_too_small_is_reported_not_truncated(void** state)
{
  (void)state;
  char token[16];
  assert_int_equal(
      az_iot_sas__mint(
          AZ_IOT_SAS_AUDIENCE_PROVISIONING,
          "0ne00000000",
          "my-device",
          1789000000ull,
          fake_sign,
          NULL,
          token,
          sizeof(token),
          NULL),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void missing_arguments_are_rejected(void** state)
{
  (void)state;
  char token[512];
  assert_int_equal(
      az_iot_sas__mint(
          AZ_IOT_SAS_AUDIENCE_PROVISIONING,
          "scope",
          "dev",
          1ull,
          NULL, /* no signing callback */
          NULL,
          token,
          sizeof(token),
          NULL),
      AZ_IOT_ERR_INVALID_ARG);

  char resource[64];
  assert_int_equal(
      az_iot_sas__build_resource(
          AZ_IOT_SAS_AUDIENCE_PROVISIONING, NULL, "dev", resource, sizeof(resource), NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(the_provisioning_audience_is_scoped_to_the_registration),
    cmocka_unit_test(the_hub_audience_is_scoped_to_the_device),
    cmocka_unit_test(the_resource_is_percent_encoded),
    cmocka_unit_test(the_string_to_sign_is_resource_newline_expiry),
    cmocka_unit_test(only_the_provisioning_token_carries_skn),
    cmocka_unit_test(the_signature_is_base64_then_percent_encoded),
    cmocka_unit_test(mint_produces_the_measured_token_shape),
    cmocka_unit_test(a_failing_signature_fails_the_mint),
    cmocka_unit_test(a_signature_of_no_length_is_rejected),
    cmocka_unit_test(a_buffer_too_small_is_reported_not_truncated),
    cmocka_unit_test(missing_arguments_are_rejected),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
