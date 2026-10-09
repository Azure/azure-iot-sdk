// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for az_iot_base64_decode(): both alphabets, padding, trailing
 * bits, block boundaries, in-place decoding and destination bounds. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include <azure/core/az_base64.h>

#include "internal/base64.h"

static az_span span_of(const char* s)
{
  return az_span_create((uint8_t*)(uintptr_t)s, (int32_t)strlen(s));
}

static az_iot_result decode(const char* in, bool allow_std, uint8_t* out, int32_t cap, az_span* got)
{
  return az_iot_base64_decode(span_of(in), allow_std, az_span_create(out, cap), got);
}

static void expect_decoded(const char* in, bool allow_std, const uint8_t* want, int32_t want_len)
{
  uint8_t out[64];
  az_span got = AZ_SPAN_EMPTY;
  assert_int_equal(decode(in, allow_std, out, (int32_t)sizeof(out), &got), AZ_IOT_OK);
  assert_int_equal(az_span_size(got), want_len);
  assert_memory_equal(az_span_ptr(got), want, (size_t)want_len);
}

static void expect_invalid(const char* in, bool allow_std)
{
  uint8_t out[64];
  az_span got = AZ_SPAN_EMPTY;
  assert_int_equal(decode(in, allow_std, out, (int32_t)sizeof(out), &got), AZ_IOT_ERR_INVALID_ARG);
}

/* Unpadded base64url of `n` bytes (i * 7 + 3) into `dst`. */
static int32_t encode_url(uint8_t* bytes, int32_t n, char* dst, int32_t cap)
{
  for (int32_t i = 0; i < n; ++i)
  {
    bytes[i] = (uint8_t)(i * 7 + 3);
  }
  int32_t w = 0;
  assert_true(az_result_succeeded(
      az_base64_encode(az_span_create((uint8_t*)dst, cap), az_span_create(bytes, n), &w)));
  int32_t o = 0;
  for (int32_t i = 0; i < w; ++i)
  {
    char c = dst[i];
    if (c != '=')
    {
      dst[o++] = c == '+' ? '-' : (c == '/' ? '_' : c);
    }
  }
  dst[o] = '\0';
  return o;
}

static void base64url_decodes_every_tail_length(void** state)
{
  (void)state;
  static const uint8_t k[] = { 0x01, 0x02, 0x03 };
  expect_decoded("AQID", false, k, 3);
  expect_decoded("AQI", false, k, 2);
  expect_decoded("AQ", false, k, 1);
  static const uint8_t ff[] = { 0xFB, 0xFF, 0xFF };
  expect_decoded("-___", false, ff, 3);
}

static void standard_base64_is_accepted_only_when_allowed(void** state)
{
  (void)state;
  static const uint8_t ff[] = { 0xFF, 0xFF, 0xFF };
  static const uint8_t k[] = { 0x01, 0x02, 0x03 };
  expect_decoded("////", true, ff, 3);
  expect_decoded("AQI=", true, k, 2);
  expect_decoded("AQ==", true, k, 1);
  expect_decoded("AQID", true, k, 3);
  expect_invalid("////", false);
  expect_invalid("AQ==", false);
}

static void malformed_input_is_rejected(void** state)
{
  (void)state;
  expect_invalid("", true);
  expect_invalid("A", false);
  expect_invalid("AQIDB", false);
  expect_invalid("AQ!D", false);
  expect_invalid("AQ D", false);
  expect_invalid("-+AA", true); /* mixed alphabets */
  expect_invalid("AQ=D", true); /* '=' inside */
  expect_invalid("A===", true); /* three pads */
  expect_invalid("AQ=", true); /* length not a multiple of 4 */
  expect_invalid("AQI==", true);
  expect_invalid("AQ=", false);
}

static void nonzero_trailing_bits_are_rejected(void** state)
{
  (void)state;
  expect_invalid("AR", false); /* would also decode to 0x01 */
  expect_invalid("AQJ", false);
  expect_invalid("AR==", true);
  expect_invalid("AQJ=", true);
}

static void destination_bounds_are_checked(void** state)
{
  (void)state;
  uint8_t out[3];
  az_span got = AZ_SPAN_EMPTY;
  assert_int_equal(decode("AQID", false, out, 2, &got), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(decode("AQID", false, out, 3, &got), AZ_IOT_OK);
  assert_int_equal(decode("AQ", false, out, 1, &got), AZ_IOT_OK);
  assert_int_equal(decode("AQ==", true, out, 1, &got), AZ_IOT_OK);
  assert_int_equal(az_iot_base64_decode(span_of("AQID"), false, AZ_SPAN_FROM_BUFFER(out), NULL),
                   AZ_IOT_ERR_INVALID_ARG);
}

/* Lengths around the 64-character block, decoded into a separate buffer and in place. */
static void block_boundaries_decode_in_place(void** state)
{
  (void)state;
  static const int32_t lengths[] = { 1, 46, 47, 48, 49, 50, 95, 96, 97, 200 };
  for (size_t t = 0; t < sizeof(lengths) / sizeof(lengths[0]); ++t)
  {
    uint8_t bytes[256];
    char text[512];
    int32_t n = lengths[t];
    int32_t text_len = encode_url(bytes, n, text, (int32_t)sizeof(text));

    uint8_t out[256];
    az_span got = AZ_SPAN_EMPTY;
    assert_int_equal(decode(text, false, out, (int32_t)sizeof(out), &got), AZ_IOT_OK);
    assert_int_equal(az_span_size(got), n);
    assert_memory_equal(az_span_ptr(got), bytes, (size_t)n);

    az_span in_place = az_span_create((uint8_t*)text, text_len);
    assert_int_equal(az_iot_base64_decode(in_place, false, in_place, &got), AZ_IOT_OK);
    assert_ptr_equal(az_span_ptr(got), (uint8_t*)text);
    assert_int_equal(az_span_size(got), n);
    assert_memory_equal(az_span_ptr(got), bytes, (size_t)n);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(base64url_decodes_every_tail_length),
    cmocka_unit_test(standard_base64_is_accepted_only_when_allowed),
    cmocka_unit_test(malformed_input_is_rejected),
    cmocka_unit_test(nonzero_trailing_bits_are_rejected),
    cmocka_unit_test(destination_bounds_are_checked),
    cmocka_unit_test(block_boundaries_decode_in_place),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
