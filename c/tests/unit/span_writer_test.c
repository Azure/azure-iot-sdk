// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for the bounded span writer.
 *
 * The interesting cases are the boundaries: exact fits, one byte short, the
 * terminator's reserved byte, and the integer extremes. Several tests compare
 * against snprintf directly, because the writer's contract is to produce the
 * same bytes for the shapes the library actually builds while reporting
 * truncation instead of relying on the caller to notice a return value.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "internal/span_writer.h"

static void empty_destination_latches_failure(void** state)
{
  (void)state;
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_EMPTY);
  az_iot_span_writer_append_str(&writer, "anything");
  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void builds_a_topic_that_fits_exactly(void** state)
{
  (void)state;
  char buffer[sizeof("ih/dev-1/srv/telemetry")]; /* content plus terminator */
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));

  az_iot_span_writer_append_str(&writer, "ih/");
  az_iot_span_writer_append_str(&writer, "dev-1");
  az_iot_span_writer_append_str(&writer, "/srv/telemetry");

  size_t length = 0;
  assert_int_equal(az_iot_span_writer_end_str(&writer, &length), AZ_IOT_OK);
  assert_string_equal(buffer, "ih/dev-1/srv/telemetry");
  assert_int_equal(length, strlen("ih/dev-1/srv/telemetry"));
}

static void content_fitting_without_room_for_terminator_fails(void** state)
{
  (void)state;
  char buffer[4];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));

  az_iot_span_writer_append_str(&writer, "abcd"); /* fills the buffer exactly */
  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  /* An unterminated build of the same bytes is legitimate. */
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));
  az_iot_span_writer_append_str(&writer, "abcd");
  az_span written = AZ_SPAN_EMPTY;
  assert_int_equal(az_iot_span_writer_end(&writer, &written), AZ_IOT_OK);
  assert_int_equal(az_span_size(written), 4);
  assert_memory_equal(az_span_ptr(written), "abcd", 4);
}

static void overflow_leaves_an_empty_string_not_a_partial_one(void** state)
{
  (void)state;
  char buffer[8];
  memset(buffer, 'x', sizeof(buffer));
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));

  az_iot_span_writer_append_str(&writer, "ih/dev");
  az_iot_span_writer_append_str(&writer, "/srv/telemetry"); /* does not fit */

  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_string_equal(buffer, "");
}

static void first_failure_is_latched(void** state)
{
  (void)state;
  char buffer[8];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));

  az_iot_span_writer_append_str(&writer, NULL); /* invalid arg wins */
  az_iot_span_writer_append_str(&writer, "way too long for this buffer");
  az_iot_span_writer_append_str(&writer, "ok"); /* would have fit */

  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void null_string_is_reported_rather_than_undefined(void** state)
{
  (void)state;
  char buffer[32];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));

  az_iot_span_writer_append_str(&writer, "device=");
  az_iot_span_writer_append_str(&writer, NULL);

  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void empty_appends_are_no_ops(void** state)
{
  (void)state;
  char buffer[8];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));

  az_iot_span_writer_append_str(&writer, "");
  az_iot_span_writer_append_span(&writer, AZ_SPAN_EMPTY);
  az_iot_span_writer_append_str(&writer, "ok");

  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_OK);
  assert_string_equal(buffer, "ok");
}

static void decimal_matches_snprintf_at_the_extremes(void** state)
{
  (void)state;
  static const uint32_t k_unsigned[] = { 0u, 1u, 9u, 10u, 4294967295u };
  static const int32_t k_signed[] = { 0, -1, 42, INT32_MAX, INT32_MIN };

  for (size_t i = 0; i < sizeof(k_unsigned) / sizeof(k_unsigned[0]); ++i)
  {
    char expected[16];
    char actual[16];
    (void)snprintf(expected, sizeof(expected), "%u", (unsigned)k_unsigned[i]);

    az_iot_span_writer writer;
    az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(actual));
    az_iot_span_writer_append_u32(&writer, k_unsigned[i]);
    assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_OK);
    assert_string_equal(actual, expected);
  }

  for (size_t i = 0; i < sizeof(k_signed) / sizeof(k_signed[0]); ++i)
  {
    char expected[16];
    char actual[16];
    (void)snprintf(expected, sizeof(expected), "%d", (int)k_signed[i]);

    az_iot_span_writer writer;
    az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(actual));
    az_iot_span_writer_append_i32(&writer, k_signed[i]);
    assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_OK);
    assert_string_equal(actual, expected);
  }
}

static void hex_pads_clamps_and_widens(void** state)
{
  (void)state;
  struct
  {
    uint32_t value;
    uint8_t min_digits;
    const char* expected;
  } const cases[] = {
    { 0u, 8u, "00000000" },
    { 0u, 0u, "0" }, /* min_digits clamps up to 1 */
    { 0xDEADBEEFu, 1u, "deadbeef" }, /* widens past min_digits */
    { 0x1Fu, 4u, "001f" },
    { 0xFFFFFFFFu, 200u, "ffffffff" }, /* min_digits clamps down to 8 */
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    char actual[16];
    az_iot_span_writer writer;
    az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(actual));
    az_iot_span_writer_append_hex32(&writer, cases[i].value, cases[i].min_digits);
    assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_OK);
    assert_string_equal(actual, cases[i].expected);
  }
}

static void a_number_that_does_not_fit_writes_nothing(void** state)
{
  (void)state;
  char buffer[4];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));

  az_iot_span_writer_append_str(&writer, "r=");
  az_iot_span_writer_append_u32(&writer, 4294967295u); /* ten digits, no room */

  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void matches_snprintf_for_a_real_url(void** state)
{
  (void)state;
  static const char k_host[] = "contoso.azure-devices.net";
  static const char k_device[] = "dev-1";
  static const char k_api[] = "2021-04-12";

  char expected[128];
  (void)snprintf(
      expected,
      sizeof(expected),
      "https://%s/devices/%s/files?api-version=%s",
      k_host,
      k_device,
      k_api);

  char actual[128];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(actual));
  az_iot_span_writer_append_str(&writer, "https://");
  az_iot_span_writer_append_str(&writer, k_host);
  az_iot_span_writer_append_str(&writer, "/devices/");
  az_iot_span_writer_append_str(&writer, k_device);
  az_iot_span_writer_append_str(&writer, "/files?api-version=");
  az_iot_span_writer_append_str(&writer, k_api);

  size_t length = 0;
  assert_int_equal(az_iot_span_writer_end_str(&writer, &length), AZ_IOT_OK);
  assert_string_equal(actual, expected);
  assert_int_equal(length, strlen(expected));
}

static void matches_snprintf_for_a_mixed_topic(void** state)
{
  (void)state;
  char expected[64];
  (void)snprintf(expected, sizeof(expected), "$iothub/methods/res/%d/?$rid=%s", 200, "42");

  char actual[64];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(actual));
  az_iot_span_writer_append_str(&writer, "$iothub/methods/res/");
  az_iot_span_writer_append_i32(&writer, 200);
  az_iot_span_writer_append_str(&writer, "/?$rid=");
  az_iot_span_writer_append_str(&writer, "42");

  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_OK);
  assert_string_equal(actual, expected);
}

static void null_writer_is_rejected(void** state)
{
  (void)state;
  az_iot_span_writer_init(NULL, AZ_SPAN_EMPTY);
  az_iot_span_writer_append_str(NULL, "x");
  az_iot_span_writer_append_u32(NULL, 1u);
  az_iot_span_writer_append_u8(NULL, 'x');
  az_iot_span_writer_append_hex32(NULL, 1u, 8u);
  assert_int_equal(az_iot_span_writer_end(NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_span_writer_end_str(NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void appends_spans_and_bytes(void** state)
{
  (void)state;
  char buffer[32];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));

  az_iot_span_writer_append_span(&writer, AZ_SPAN_FROM_STR("ih"));
  az_iot_span_writer_append_u8(&writer, (uint8_t)'/');
  az_iot_span_writer_append_span(&writer, AZ_SPAN_FROM_STR("dev-1"));

  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_OK);
  assert_string_equal(buffer, "ih/dev-1");
}

static void url_encoding_follows_rfc3986_unreserved(void** state)
{
  (void)state;
  struct
  {
    const char* input;
    const char* expected;
  } const cases[] = { /* The unreserved set survives untouched. */
                      { "AZaz09-_.~", "AZaz09-_.~" },
                      /* Everything a property bag reads as structure is escaped. */
                      { "a&b=c", "a%26b%3Dc" },
                      { "application/json", "application%2Fjson" },
                      { "100%", "100%25" },
                      { " ", "%20" },
                      /* Uppercase hex digits, matching what az_core emits for query values. */
                      { "\x01\xff", "%01%FF" },
                      { "", "" }
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    char actual[64];
    az_iot_span_writer writer;
    az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(actual));
    az_iot_span_writer_append_url_encoded(&writer, cases[i].input);
    assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_OK);
    assert_string_equal(actual, cases[i].expected);
  }
}

static void url_encoding_is_all_or_nothing_on_overflow(void** state)
{
  (void)state;
  /* "&&&" needs nine bytes encoded plus a terminator; ten is one short of
   * what the escape at the end requires, and nothing may be left behind. */
  char buffer[9];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));
  az_iot_span_writer_append_url_encoded(&writer, "&&&");
  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_string_equal(buffer, "");

  char exact[10];
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(exact));
  az_iot_span_writer_append_url_encoded(&writer, "&&&");
  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_OK);
  assert_string_equal(exact, "%26%26%26");
}

static void url_encoding_rejects_null(void** state)
{
  (void)state;
  char buffer[16];
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(buffer));
  az_iot_span_writer_append_url_encoded(&writer, NULL);
  assert_int_equal(az_iot_span_writer_end_str(&writer, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void build_str_concatenates_parts(void** state)
{
  (void)state;
  char topic[32];
  size_t length = 0;
  const char* parts[] = { "ih/", "dev-1", "/dev/c2d" };

  assert_int_equal(
      az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), &length, parts, 3), AZ_IOT_OK);
  assert_string_equal(topic, "ih/dev-1/dev/c2d");
  assert_int_equal(length, strlen("ih/dev-1/dev/c2d"));
}

static void build_str_reports_its_failures(void** state)
{
  (void)state;
  char topic[8];

  const char* too_long[] = { "ih/", "a-very-long-device-id", "/dev/c2d" };
  assert_int_equal(
      az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, too_long, 3),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  const char* with_null[] = { "ih/", NULL, "/dev/c2d" };
  assert_int_equal(
      az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, with_null, 3),
      AZ_IOT_ERR_INVALID_ARG);

  assert_int_equal(
      az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, NULL, 3),
      AZ_IOT_ERR_INVALID_ARG);

  /* No parts at all is an empty string, not a failure. */
  size_t length = 1;
  assert_int_equal(
      az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), &length, NULL, 0), AZ_IOT_OK);
  assert_string_equal(topic, "");
  assert_int_equal(length, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(empty_destination_latches_failure),
    cmocka_unit_test(builds_a_topic_that_fits_exactly),
    cmocka_unit_test(content_fitting_without_room_for_terminator_fails),
    cmocka_unit_test(overflow_leaves_an_empty_string_not_a_partial_one),
    cmocka_unit_test(first_failure_is_latched),
    cmocka_unit_test(null_string_is_reported_rather_than_undefined),
    cmocka_unit_test(empty_appends_are_no_ops),
    cmocka_unit_test(decimal_matches_snprintf_at_the_extremes),
    cmocka_unit_test(hex_pads_clamps_and_widens),
    cmocka_unit_test(a_number_that_does_not_fit_writes_nothing),
    cmocka_unit_test(matches_snprintf_for_a_real_url),
    cmocka_unit_test(matches_snprintf_for_a_mixed_topic),
    cmocka_unit_test(null_writer_is_rejected),
    cmocka_unit_test(appends_spans_and_bytes),
    cmocka_unit_test(url_encoding_follows_rfc3986_unreserved),
    cmocka_unit_test(url_encoding_is_all_or_nothing_on_overflow),
    cmocka_unit_test(url_encoding_rejects_null),
    cmocka_unit_test(build_str_concatenates_parts),
    cmocka_unit_test(build_str_reports_its_failures),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
