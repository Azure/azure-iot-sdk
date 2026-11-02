// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for az_iot_su__verify_and_parse_detached_manifest_file(): size and SHA-256 checks,
 * the `updateManifest` wrapper, in-place decoding, parsing and step file references. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include <azure/core/az_base64.h>

#include "internal/su_detached_manifest_internal.h"

/** @brief A full manifest with one step using file `f1`, longer than one 256-byte hash chunk. */
#define FULL_MANIFEST                                                                      \
  "{\"manifestVersion\":\"5\","                                                            \
  "\"updateId\":{\"provider\":\"Contoso\",\"name\":\"Toaster\",\"version\":\"1.2\"},"      \
  "\"compatibility\":[{\"deviceManufacturer\":\"Contoso\"}],"                              \
  "\"instructions\":{\"steps\":[{\"handler\":\"microsoft/swupdate:2\",\"files\":[\"f1\"]," \
  "\"handlerProperties\":{\"installedCriteria\":\"1.2\"}}]},"                              \
  "\"files\":{\"f1\":{\"fileName\":\"a.swu\",\"sizeInBytes\":4096,"                        \
  "\"hashes\":{\"sha256\":\"AAA=\"}}},"                                                    \
  "\"createdDateTime\":\"2022-07-07T03:02:48.8449038Z\"}"

/** @brief The detached manifest file under test. */
static uint8_t g_file[2048];

/** @brief Bytes of g_file in use. */
static az_span g_file_content;

/** @brief Base64 SHA-256 of g_file_content, as the mini manifest signs it. */
static char g_hash_base64[64];

/** @brief The mini manifest `files` entry describing g_file. */
static az_iot_su_client_update_manifest_file g_detached_file;

/** @brief Output of the last process(). */
static az_iot_su_client_update_manifest g_manifest;

/** @brief Error of the last process(). */
static az_iot_su__detached_manifest_error g_error;

/** @brief Download URLs of the request. */
static const az_iot_su_client_file_url g_file_urls[] = {
  { AZ_SPAN_LITERAL_FROM_STR("f1"), AZ_SPAN_LITERAL_FROM_STR("https://example.com/f1") },
};

/** @brief Test SHA-256 stand-in: an order-dependent fold of the bytes into 32 bytes. */
static az_iot_result test_sha256_init(const az_iot_crypto* self, az_iot_sha256_ctx* ctx)
{
  (void)self;
  memset(ctx, 0, sizeof(*ctx));
  return AZ_IOT_OK;
}

static az_iot_result test_sha256_update(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    const uint8_t* data,
    size_t len)
{
  (void)self;
  uint8_t* digest = (uint8_t*)ctx->opaque;
  uint64_t* position = &ctx->opaque[63];
  for (size_t i = 0; i < len; ++i, ++*position)
  {
    uint8_t* d = &digest[*position % AZ_IOT_SHA256_SIZE];
    *d = (uint8_t)(*d * 31u + data[i] + 1u);
  }
  return AZ_IOT_OK;
}

static az_iot_result test_sha256_final(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  (void)self;
  if (out != NULL)
  {
    memcpy(out, ctx->opaque, AZ_IOT_SHA256_SIZE);
  }
  return AZ_IOT_OK;
}

static const az_iot_crypto g_crypto
    = { AZ_IOT_CRYPTO_VERSION, test_sha256_init, test_sha256_update, test_sha256_final, NULL };

/** @brief Make @p text the file, and g_detached_file describe it with its size and hash. */
static void set_file(const char* text)
{
  size_t size = strlen(text);
  assert_true(size <= sizeof(g_file));
  memcpy(g_file, text, size);
  g_file_content = az_span_create(g_file, (int32_t)size);

  uint8_t digest[AZ_IOT_SHA256_SIZE];
  az_iot_sha256_ctx ctx;
  assert_int_equal(test_sha256_init(&g_crypto, &ctx), AZ_IOT_OK);
  assert_int_equal(test_sha256_update(&g_crypto, &ctx, g_file, size), AZ_IOT_OK);
  assert_int_equal(test_sha256_final(&g_crypto, &ctx, digest), AZ_IOT_OK);
  int32_t written = 0;
  assert_true(az_result_succeeded(az_base64_encode(
      az_span_create((uint8_t*)g_hash_base64, (int32_t)sizeof(g_hash_base64)),
      AZ_SPAN_FROM_BUFFER(digest),
      &written)));

  memset(&g_detached_file, 0, sizeof(g_detached_file));
  g_detached_file.id = AZ_SPAN_FROM_STR("detached");
  g_detached_file.size_in_bytes = (int64_t)size;
  g_detached_file.hashes[0].hash_type = AZ_SPAN_FROM_STR("sha256");
  g_detached_file.hashes[0].hash_value = az_span_create((uint8_t*)g_hash_base64, written);
  g_detached_file.hashes_count = 1;
}

/** @brief set_file() to `{<before>"<name>":"<manifest, escaped>"<after>}`. */
static void set_wrapped_property(
    const char* before,
    const char* name,
    const char* manifest,
    const char* after)
{
  char text[sizeof(g_file) + 1];
  size_t n = 0;
  text[n++] = '{';
  for (const char* c = before; *c != '\0'; ++c)
  {
    text[n++] = *c;
  }
  text[n++] = '"';
  for (const char* c = name; *c != '\0'; ++c)
  {
    text[n++] = *c;
  }
  for (const char* c = "\":\""; *c != '\0'; ++c)
  {
    text[n++] = *c;
  }
  for (const char* c = manifest; *c != '\0'; ++c)
  {
    if (*c == '"' || *c == '\\')
    {
      text[n++] = '\\';
    }
    text[n++] = *c;
  }
  text[n++] = '"';
  for (const char* c = after; *c != '\0'; ++c)
  {
    text[n++] = *c;
  }
  text[n++] = '}';
  assert_true(n < sizeof(text));
  text[n] = '\0';
  set_file(text);
}

/** @brief set_wrapped_property() with the name `updateManifest`. */
static void set_wrapped_manifest(const char* before, const char* manifest, const char* after)
{
  set_wrapped_property(before, "updateManifest", manifest, after);
}

/** @brief Run the routine on g_file_content into g_manifest and g_error. */
static az_iot_result process(void)
{
  memset(&g_manifest, 0xA5, sizeof(g_manifest));
  return az_iot_su__verify_and_parse_detached_manifest_file(
      &g_crypto,
      &g_detached_file,
      g_file_content,
      g_file_urls,
      (uint32_t)(sizeof(g_file_urls) / sizeof(g_file_urls[0])),
      NULL,
      0,
      &g_manifest,
      &g_error);
}

/** @brief Assert @p actual holds @p expected. */
static void assert_span_equal(az_span actual, const char* expected)
{
  assert_true(az_span_is_content_equal(
      actual, az_span_create((uint8_t*)(uintptr_t)expected, (int32_t)strlen(expected))));
}

/** @brief Assert the last process() failed with @p result and @p error, leaving no manifest. */
static void assert_rejected(az_iot_result result, az_iot_result expected, int expected_error)
{
  static const az_iot_su_client_update_manifest empty = { 0 };
  assert_int_equal(result, expected);
  assert_int_equal(g_error, expected_error);
  assert_memory_equal(&g_manifest, &empty, sizeof(empty));
}

static void full_manifest_is_unwrapped_and_parsed(void** state)
{
  (void)state;
  set_wrapped_manifest("", FULL_MANIFEST, "");
  assert_true(az_span_size(g_file_content) > 256);

  assert_int_equal(process(), AZ_IOT_OK);
  assert_int_equal(g_error, AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NONE);
  assert_span_equal(g_manifest.manifest_version, "5");
  assert_span_equal(g_manifest.update_id.provider, "Contoso");
  assert_span_equal(g_manifest.update_id.version, "1.2");
  assert_int_equal(g_manifest.instructions.steps_count, 1);
  assert_span_equal(g_manifest.instructions.steps[0].files[0], "f1");
  assert_int_equal(g_manifest.files_count, 1);
  assert_span_equal(g_manifest.files[0].file_name, "a.swu");
  /* Decoded in place: the manifest points into the file. */
  assert_true(az_span_ptr(g_manifest.manifest_version) > g_file);
  assert_true(az_span_ptr(g_manifest.manifest_version) < g_file + az_span_size(g_file_content));
}

static void other_wrapper_properties_are_ignored(void** state)
{
  (void)state;
  set_wrapped_manifest(
      "\"a\":{\"updateManifest\":1},\"b\":[\"updateManifest\",2],", FULL_MANIFEST, ",\"c\":null");
  assert_int_equal(process(), AZ_IOT_OK);
  assert_span_equal(g_manifest.update_id.name, "Toaster");
}

static void escaped_property_name_is_decoded(void** state)
{
  (void)state;
  set_wrapped_property("", "\\u0075pdateManifest", FULL_MANIFEST, "");
  assert_int_equal(process(), AZ_IOT_OK);
  assert_span_equal(g_manifest.update_id.name, "Toaster");

  /* The last one wins, however it is spelled. */
  set_wrapped_property("\"updateManifest\":5,", "\\u0075pdateManifest", FULL_MANIFEST, "");
  assert_int_equal(process(), AZ_IOT_OK);
  set_wrapped_manifest("", FULL_MANIFEST, ",\"\\u0075pdateManifest\":5");
  assert_rejected(
      process(),
      AZ_IOT_ERR_INVALID_ARG,
      AZ_IOT_SU__DETACHED_MANIFEST_ERROR_UPDATE_MANIFEST_MISSING);
}

static void wrong_size_is_rejected(void** state)
{
  (void)state;
  set_wrapped_manifest("", FULL_MANIFEST, "");
  g_detached_file.size_in_bytes += 1;
  assert_rejected(process(), AZ_IOT_ERR_AUTH, AZ_IOT_SU__DETACHED_MANIFEST_ERROR_SIZE_MISMATCH);
}

static void wrong_hash_is_rejected(void** state)
{
  (void)state;
  set_wrapped_manifest("", FULL_MANIFEST, "");
  g_file[az_span_size(g_file_content) - 300] ^= 0x01u; /* same size, other bytes */
  assert_rejected(process(), AZ_IOT_ERR_AUTH, AZ_IOT_SU__DETACHED_MANIFEST_ERROR_HASH_MISMATCH);

  set_wrapped_manifest("", FULL_MANIFEST, "");
  g_detached_file.hashes[0].hash_type = AZ_SPAN_FROM_STR("sha384");
  assert_rejected(process(), AZ_IOT_ERR_AUTH, AZ_IOT_SU__DETACHED_MANIFEST_ERROR_HASH_MISMATCH);
}

static void file_not_json_object_is_rejected(void** state)
{
  (void)state;
  static const char* const files[] = {
    "not json",
    "[\"updateManifest\"]",
    "\"{}\"",
    "{\"updateManifest\":\"{}\"",
    "{\"updateManifest\":\"{}\"} x",
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
  {
    set_file(files[i]);
    assert_rejected(
        process(), AZ_IOT_ERR_INVALID_ARG, AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NOT_JSON_OBJECT);
  }
}

static void missing_update_manifest_is_rejected(void** state)
{
  (void)state;
  static const char* const files[] = {
    "{}",
    "{\"manifest\":\"{}\"}",
    "{\"updateManifest\":\"\"}",
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
  {
    set_file(files[i]);
    assert_rejected(
        process(),
        AZ_IOT_ERR_INVALID_ARG,
        AZ_IOT_SU__DETACHED_MANIFEST_ERROR_UPDATE_MANIFEST_MISSING);
  }
}

static void non_string_update_manifest_is_rejected(void** state)
{
  (void)state;
  static const char* const files[] = {
    "{\"updateManifest\":{\"manifestVersion\":\"5\"}}",
    "{\"updateManifest\":[]}",
    "{\"updateManifest\":5}",
    "{\"updateManifest\":null}",
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
  {
    set_file(files[i]);
    assert_rejected(
        process(),
        AZ_IOT_ERR_INVALID_ARG,
        AZ_IOT_SU__DETACHED_MANIFEST_ERROR_UPDATE_MANIFEST_MISSING);
  }
}

static void undecodable_update_manifest_is_rejected(void** state)
{
  (void)state;
  /* Valid JSON escapes the decoder refuses: NUL and a lone surrogate. */
  static const char* const files[] = {
    "{\"updateManifest\":\"{\\u0000}\"}",
    "{\"updateManifest\":\"{\\uD800}\"}",
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
  {
    set_file(files[i]);
    assert_rejected(
        process(),
        AZ_IOT_ERR_INVALID_ARG,
        AZ_IOT_SU__DETACHED_MANIFEST_ERROR_UPDATE_MANIFEST_UNDECODABLE);
  }
}

static void unparsable_full_manifest_is_rejected(void** state)
{
  (void)state;
  set_wrapped_manifest("", "{\"manifestVersion\":5}", "");
  assert_rejected(
      process(), AZ_IOT_ERR_INVALID_ARG, AZ_IOT_SU__DETACHED_MANIFEST_ERROR_MANIFEST_INVALID);

  set_wrapped_manifest("", "not a manifest", "");
  assert_rejected(
      process(), AZ_IOT_ERR_INVALID_ARG, AZ_IOT_SU__DETACHED_MANIFEST_ERROR_MANIFEST_INVALID);
}

static void step_file_missing_from_files_is_rejected(void** state)
{
  (void)state;
  set_wrapped_manifest(
      "",
      "{\"manifestVersion\":\"5\","
      "\"updateId\":{\"provider\":\"Contoso\",\"name\":\"Toaster\",\"version\":\"1.2\"},"
      "\"instructions\":{\"steps\":[{\"handler\":\"microsoft/script:1\",\"files\":[\"f2\"]}]},"
      "\"files\":{\"f1\":{\"fileName\":\"a.swu\",\"sizeInBytes\":1,"
      "\"hashes\":{\"sha256\":\"AAA=\"}}}}",
      "");
  assert_rejected(
      process(),
      AZ_IOT_ERR_INVALID_ARG,
      AZ_IOT_SU__DETACHED_MANIFEST_ERROR_FILE_REFERENCE_UNRESOLVED);
}

static void invalid_arguments_are_rejected(void** state)
{
  (void)state;
  set_wrapped_manifest("", FULL_MANIFEST, "");
  az_iot_crypto no_sha256 = g_crypto;
  no_sha256.sha256_update = NULL;
  assert_int_equal(
      az_iot_su__verify_and_parse_detached_manifest_file(
          &no_sha256, &g_detached_file, g_file_content, NULL, 0, NULL, 0, &g_manifest, &g_error),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_su__verify_and_parse_detached_manifest_file(
          &g_crypto, NULL, g_file_content, NULL, 0, NULL, 0, &g_manifest, &g_error),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_su__verify_and_parse_detached_manifest_file(
          &g_crypto, &g_detached_file, g_file_content, NULL, 0, NULL, 0, NULL, &g_error),
      AZ_IOT_ERR_INVALID_ARG);
  memset(&g_manifest, 0xA5, sizeof(g_manifest));
  assert_int_equal(
      az_iot_su__verify_and_parse_detached_manifest_file(
          &g_crypto, &g_detached_file, g_file_content, NULL, 1, NULL, 0, &g_manifest, &g_error),
      AZ_IOT_ERR_INVALID_ARG);
  assert_rejected(
      AZ_IOT_ERR_INVALID_ARG, AZ_IOT_ERR_INVALID_ARG, AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NONE);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(full_manifest_is_unwrapped_and_parsed),
    cmocka_unit_test(other_wrapper_properties_are_ignored),
    cmocka_unit_test(escaped_property_name_is_decoded),
    cmocka_unit_test(wrong_size_is_rejected),
    cmocka_unit_test(wrong_hash_is_rejected),
    cmocka_unit_test(file_not_json_object_is_rejected),
    cmocka_unit_test(missing_update_manifest_is_rejected),
    cmocka_unit_test(non_string_update_manifest_is_rejected),
    cmocka_unit_test(undecodable_update_manifest_is_rejected),
    cmocka_unit_test(unparsable_full_manifest_is_rejected),
    cmocka_unit_test(step_file_missing_from_files_is_rejected),
    cmocka_unit_test(invalid_arguments_are_rejected),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
