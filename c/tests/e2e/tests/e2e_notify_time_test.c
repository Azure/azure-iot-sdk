// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Parsing of a file-upload notification's enqueuedTimeUtc. The watcher accepts
 * (deletes) other devices' notifications it parses as stale, so anything
 * malformed must be rejected. No network or Azure resources are used. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "e2e_notify_time.h"

/** @brief Parses `{"enqueuedTimeUtc":"<value>"}`. */
static bool parse(const char* value, int64_t* out)
{
  char body[128];
  (void)snprintf(body, sizeof(body), "{\"enqueuedTimeUtc\":\"%s\"}", value);
  return e2e_notify_enqueued_time(body, out);
}

static void test_valid_timestamps(void** state)
{
  (void)state;
  int64_t s = 0;
  assert_true(parse("2026-10-01T04:36:14.1234567Z", &s));
  assert_int_equal(1790829374, s);
  assert_true(parse("2026-10-01T04:36:14Z", &s));
  assert_int_equal(1790829374, s);
  assert_true(parse("2026-10-01T04:36:14+00:00", &s));
  assert_int_equal(1790829374, s);
  assert_true(parse("2024-02-29T00:00:00Z", &s));
  assert_int_equal(1709164800, s);
  assert_true(parse("1970-01-01T00:00:00Z", &s));
  assert_int_equal(0, s);
}

static void test_out_of_range_rejected(void** state)
{
  (void)state;
  int64_t s = 0;
  assert_false(parse("2025-02-29T00:00:00Z", &s));
  assert_false(parse("2026-02-31T00:00:00Z", &s));
  assert_false(parse("2026-04-31T00:00:00Z", &s));
  assert_false(parse("2026-13-01T00:00:00Z", &s));
  assert_false(parse("2026-00-01T00:00:00Z", &s));
  assert_false(parse("2026-10-01T24:00:00Z", &s));
  assert_false(parse("2026-10-01T23:60:00Z", &s));
  assert_false(parse("2026-10-01T23:59:60Z", &s));
}

static void test_bad_suffix_rejected(void** state)
{
  (void)state;
  int64_t s = 0;
  assert_false(parse("2026-10-01T04:36:14garbage", &s));
  assert_false(parse("2026-10-01T04:36:14", &s));
  assert_false(parse("2026-10-01T04:36:14.Z", &s));
  assert_false(parse("2026-10-01T04:36:14.123", &s));
  assert_false(parse("2026-10-01T04:36:14Zx", &s));
  assert_false(parse("2026-10-01T04:36:14+01:00", &s));
}

/* A body cut off inside the timestamp (the watcher truncates long bodies). */
static void test_truncated_rejected(void** state)
{
  (void)state;
  static const char full[] = "{\"enqueuedTimeUtc\":\"2026-10-01T04:36:14.1234567Z\"}";
  int64_t s = 0;
  for (size_t n = 0; n + 1 < sizeof(full) - 1; n++)
  {
    /* Exactly n + 1 bytes, so a sanitizer flags any read past the terminator. */
    char* body = (char*)malloc(n + 1);
    assert_non_null(body);
    memcpy(body, full, n);
    body[n] = '\0';
    assert_false(e2e_notify_enqueued_time(body, &s));
    free(body);
  }
  assert_true(e2e_notify_enqueued_time(full, &s));
}

static void test_missing_field_rejected(void** state)
{
  (void)state;
  int64_t s = 0;
  assert_false(e2e_notify_enqueued_time("{\"deviceId\":\"d\"}", &s));
  assert_false(e2e_notify_enqueued_time("{\"enqueuedTimeUtc\":123}", &s));
  assert_false(e2e_notify_enqueued_time("", &s));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_valid_timestamps),       cmocka_unit_test(test_out_of_range_rejected),
    cmocka_unit_test(test_bad_suffix_rejected),    cmocka_unit_test(test_truncated_rejected),
    cmocka_unit_test(test_missing_field_rejected),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
