// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for az_iot_json_string_decode(): every escape form, each UTF-8
 * width, surrogate pairs, truncation, and destination bounds; and for
 * az_iot_json_string_equals(). */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "internal/json_string.h"

/* Decode `in` into a fresh buffer and compare with `expected` (length given,
 * so embedded bytes are exact). */
static void expect_decoded(const char* in, const char* expected, size_t expected_len)
{
  uint8_t out[64];
  az_span decoded = AZ_SPAN_EMPTY;
  assert_int_equal(
      az_iot_json_string_decode(
          az_span_create((uint8_t*)(uintptr_t)in, (int32_t)strlen(in)),
          AZ_SPAN_FROM_BUFFER(out),
          &decoded),
      AZ_IOT_OK);
  assert_int_equal(az_span_size(decoded), (int32_t)expected_len);
  assert_memory_equal(az_span_ptr(decoded), expected, expected_len);
}

static az_iot_result decode_status(const char* in)
{
  uint8_t out[64];
  az_span decoded = AZ_SPAN_EMPTY;
  return az_iot_json_string_decode(
      az_span_create((uint8_t*)(uintptr_t)in, (int32_t)strlen(in)),
      AZ_SPAN_FROM_BUFFER(out),
      &decoded);
}

static void plain_text_is_copied(void** state)
{
  (void)state;
  expect_decoded("abc-123", "abc-123", 7);
  expect_decoded("", "", 0);
}

static void every_two_character_escape_decodes(void** state)
{
  (void)state;
  expect_decoded("\\\"\\\\\\/\\b\\f\\n\\r\\t", "\"\\/\b\f\n\r\t", 8);
}

static void unicode_escapes_cover_each_utf8_width(void** state)
{
  (void)state;
  expect_decoded("\\u0041", "A", 1); /* 1 byte */
  expect_decoded("\\u007F", "\x7F", 1); /* 1-byte upper bound */
  expect_decoded("\\u0080", "\xC2\x80", 2); /* 2-byte lower bound */
  expect_decoded("\\u00e9", "\xC3\xA9", 2); /* hex case-insensitive */
  expect_decoded("\\u07FF", "\xDF\xBF", 2); /* 2-byte upper bound */
  expect_decoded("\\u0800", "\xE0\xA0\x80", 3); /* 3-byte lower bound */
  expect_decoded("\\uFFFF", "\xEF\xBF\xBF", 3); /* 3-byte upper bound */
  expect_decoded("a\\u0026b", "a&b", 3);
}

static void surrogate_pairs_decode_to_four_bytes(void** state)
{
  (void)state;
  expect_decoded("\\uD83D\\uDE00", "\xF0\x9F\x98\x80", 4); /* U+1F600 */
  expect_decoded("\\uD800\\uDC00", "\xF0\x90\x80\x80", 4); /* U+10000 */
  expect_decoded("\\uDBFF\\uDFFF", "\xF4\x8F\xBF\xBF", 4); /* U+10FFFF */
}

static void malformed_escapes_are_rejected(void** state)
{
  (void)state;
  const char* bad[] = {
    "\\", /* trailing backslash */
    "\\q", /* unknown escape */
    "\\u", /* truncated \u */
    "\\u12", /* truncated \u */
    "\\u12G4", /* non-hex digit */
    "\\uD800", /* lone high surrogate */
    "\\uD800x", /* high surrogate not followed by an escape */
    "\\uD800\\u0041", /* high surrogate followed by a non-low */
    "\\uD800\\uDC0", /* truncated low surrogate */
    "\\uDC00", /* lone low surrogate */
    "\\uDC00\\uD800", /* reversed pair */
    "\\u0000", /* embedded NUL */
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
  {
    assert_int_equal(decode_status(bad[i]), AZ_IOT_ERR_INVALID_ARG);
  }
}

static void a_destination_too_small_is_reported(void** state)
{
  (void)state;
  uint8_t out[3];
  az_span decoded = AZ_SPAN_FROM_STR("untouched");
  const char* in[] = { "abcd", "ab\\u00e9", "\\uD83D\\uDE00" };
  for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); ++i)
  {
    assert_int_equal(
        az_iot_json_string_decode(
            az_span_create((uint8_t*)(uintptr_t)in[i], (int32_t)strlen(in[i])),
            AZ_SPAN_FROM_BUFFER(out),
            &decoded),
        AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  }
  assert_true(az_span_is_content_equal(decoded, AZ_SPAN_FROM_STR("untouched")));

  /* Exactly the decoded length fits. */
  const char* fits = "a\\u00e9";
  assert_int_equal(
      az_iot_json_string_decode(
          az_span_create((uint8_t*)(uintptr_t)fits, (int32_t)strlen(fits)),
          AZ_SPAN_FROM_BUFFER(out),
          &decoded),
      AZ_IOT_OK);
  assert_int_equal(az_span_size(decoded), 3);
}

static void decoding_in_place_is_supported(void** state)
{
  (void)state;
  char buf[] = "x\\u0026\\uD83D\\uDE00\\ny";
  az_span s = az_span_create((uint8_t*)buf, (int32_t)strlen(buf));
  az_span decoded;
  assert_int_equal(az_iot_json_string_decode(s, s, &decoded), AZ_IOT_OK);
  assert_int_equal(az_span_size(decoded), 8);
  assert_memory_equal(az_span_ptr(decoded), "x&\xF0\x9F\x98\x80\ny", 8);
  assert_ptr_equal(az_span_ptr(decoded), (uint8_t*)buf);
}

static void a_null_output_is_rejected(void** state)
{
  (void)state;
  uint8_t out[4];
  assert_int_equal(
      az_iot_json_string_decode(AZ_SPAN_FROM_STR("a"), AZ_SPAN_FROM_BUFFER(out), NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

/** @brief az_iot_json_string_equals() of two C strings. */
static bool strings_equal(const char* escaped, const char* other, bool other_is_escaped)
{
  return az_iot_json_string_equals(
      az_span_create((uint8_t*)(uintptr_t)escaped, (int32_t)strlen(escaped)),
      az_span_create((uint8_t*)(uintptr_t)other, (int32_t)strlen(other)),
      other_is_escaped);
}

static void escaped_strings_equal_their_decoded_text(void** state)
{
  (void)state;
  assert_true(strings_equal("f1", "f1", false));
  assert_true(strings_equal("", "", false));
  assert_true(strings_equal("f\\u0031", "f1", false));
  assert_true(strings_equal("a\\/b\\n", "a/b\n", false));
  assert_true(strings_equal("\\u00e9\\uD83D\\uDE00", "\xC3\xA9\xF0\x9F\x98\x80", false));

  /* Decoded text is compared as is: its backslash is not an escape. */
  assert_false(strings_equal("f1", "f\\u0031", false));
  assert_true(strings_equal("f\\\\u0031", "f\\u0031", false));

  assert_false(strings_equal("f1", "f10", false));
  assert_false(strings_equal("f10", "f1", false));
  assert_false(strings_equal("f1", "F1", false));
  assert_false(strings_equal("f1", "", false));
}

static void two_escaped_strings_are_compared_decoded(void** state)
{
  (void)state;
  assert_true(strings_equal("f\\u0031", "\\u00661", true));
  assert_true(strings_equal("f1", "f\\u0031", true));
  /* An escape and a literal UTF-8 character decode to the same bytes. */
  assert_true(strings_equal("\\u00e9x", "\xC3\xA9x", true));
  assert_true(strings_equal("\xC3\xA9x", "\\u00e9x", true));
  assert_false(strings_equal("f\\u0031", "f\\u0032", true));
  assert_false(strings_equal("f\\u0031", "f\\u00310", true));
}

static void a_malformed_escape_is_never_equal(void** state)
{
  (void)state;
  assert_false(strings_equal("f\\x31", "f\\x31", true));
  assert_false(strings_equal("f\\u00", "f\\u00", false));
  assert_false(strings_equal("f1", "f\\", true));
  assert_false(strings_equal("\\uD83D", "\\uD83D", true));
  assert_false(strings_equal("\\u0000", "", false));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(plain_text_is_copied),
    cmocka_unit_test(every_two_character_escape_decodes),
    cmocka_unit_test(unicode_escapes_cover_each_utf8_width),
    cmocka_unit_test(surrogate_pairs_decode_to_four_bytes),
    cmocka_unit_test(malformed_escapes_are_rejected),
    cmocka_unit_test(a_destination_too_small_is_reported),
    cmocka_unit_test(decoding_in_place_is_supported),
    cmocka_unit_test(a_null_output_is_rejected),
    cmocka_unit_test(escaped_strings_equal_their_decoded_text),
    cmocka_unit_test(two_escaped_strings_are_compared_decoded),
    cmocka_unit_test(a_malformed_escape_is_never_equal),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
