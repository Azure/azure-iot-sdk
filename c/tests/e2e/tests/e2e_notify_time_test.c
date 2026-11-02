// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Parsing of a file-upload notification's enqueuedTimeUtc, and which notifications have no
 * watcher. The watcher accepts (deletes) other devices' notifications without one, so anything
 * malformed must be rejected. No network or Azure resources are used. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "az_iot_e2e_service.h"
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

static void test_separator(void** state)
{
  (void)state;
  int64_t s = 0;
  assert_true(e2e_notify_enqueued_time("{\"enqueuedTimeUtc\" :\t\"1970-01-01T00:00:01Z\"}", &s));
  assert_int_equal(1, s);
  assert_true(
      e2e_notify_enqueued_time("{\"enqueuedTimeUtc\"\r\n:\n \"1970-01-01T00:00:01Z\"}", &s));
  assert_false(e2e_notify_enqueued_time("{\"enqueuedTimeUtc\" \"1970-01-01T00:00:01Z\"}", &s));
  assert_false(e2e_notify_enqueued_time("{\"enqueuedTimeUtc\":::\"1970-01-01T00:00:01Z\"}", &s));
  assert_false(e2e_notify_enqueued_time("{\"enqueuedTimeUtc\": :\"1970-01-01T00:00:01Z\"}", &s));
}

/** @brief A notification body for blob @p blob_name of device "dev", enqueued at the epoch + 60 s.
 */
static const char* notification(const char* blob_name)
{
  static char body[256];
  (void)snprintf(
      body,
      sizeof(body),
      "{\"deviceId\":\"dev\",\"blobName\":\"dev/%s\","
      "\"enqueuedTimeUtc\":\"1970-01-01T00:01:00Z\"}",
      blob_name);
  return body;
}

/* Only a recent notification for a watched blob may have a watcher: it is released. */
static void test_no_watcher_for_unwatched_or_stale_blobs(void** state)
{
  (void)state;
  const char* watched = notification(AZ_IOT_E2E_FILE_UPLOAD_BLOB_DIRECTORY "ok-1-5.txt");
  assert_false(e2e_notify_has_no_watcher(watched, 60, 600));
  assert_false(e2e_notify_has_no_watcher(watched, 660, 600));
  assert_true(e2e_notify_has_no_watcher(watched, 661, 600));

  assert_true(e2e_notify_has_no_watcher(
      notification(AZ_IOT_E2E_FILE_UPLOAD_BLOB_DIRECTORY AZ_IOT_E2E_UNWATCHED_BLOB_TAG "sas-5.txt"),
      60,
      600));
  assert_true(e2e_notify_has_no_watcher(notification("sample-data/test.txt"), 60, 600));
  /* The directory counts only at the start of the blob name. */
  assert_true(e2e_notify_has_no_watcher(
      notification("archive/" AZ_IOT_E2E_FILE_UPLOAD_BLOB_DIRECTORY "sample.txt"), 60, 600));
  /* Without a blobName or an enqueued time, it may have a watcher. */
  assert_false(e2e_notify_has_no_watcher("{\"deviceId\":\"dev\"}", 60, 600));
}

/* blobName is decoded as a JSON string; one that cannot be read leaves only its age to decide. */
static void test_blob_name_is_decoded_or_decides_nothing(void** state)
{
  (void)state;
  /* "/" may be escaped. */
  assert_false(e2e_notify_has_no_watcher(
      "{\"blobName\":\"dev\\/e2e-fileupload\\/ok-1.txt\","
      "\"enqueuedTimeUtc\":\"1970-01-01T00:01:00Z\"}",
      60,
      600));
  assert_true(e2e_notify_has_no_watcher(
      "{\"blobName\" : \"dev\\/e2e-fileupload\\/unwatched-sas.txt\"}", 60, 600));

  const char* const unreadable[] = {
    "{\"blobName\":\"dev/sample-data/test.txt", /* truncated */
    "{\"blobName\":\"dev/sample-data/test.txt\\", /* truncated in an escape */
    "{\"blobName\":\"dev/sample\\u002ddata/test.txt\"}", /* \u escape */
    "{\"blobName\":\"sample-data\"}", /* no device id prefix */
    "{\"blobName\":42}",
    "{\"blobName\" \"dev/sample-data/test.txt\"}",
  };
  for (size_t i = 0; i < sizeof(unreadable) / sizeof(unreadable[0]); i++)
  {
    assert_false(e2e_notify_has_no_watcher(unreadable[i], 60, 600));
  }
  /* Too long for the decoder: unreadable too. */
  char body[700];
  int n = snprintf(body, sizeof(body), "{\"blobName\":\"dev/");
  memset(body + n, 'x', 600);
  (void)snprintf(body + n + 600, sizeof(body) - (size_t)n - 600, "\"}");
  assert_false(e2e_notify_has_no_watcher(body, 60, 600));
  /* Its age still decides: stale, it has no watcher. */
  static const char unreadable_but_dated[] = "{\"blobName\":\"dev/sample\\u002ddata/test.txt\","
                                             "\"enqueuedTimeUtc\":\"1970-01-01T00:01:00Z\"}";
  assert_false(e2e_notify_has_no_watcher(unreadable_but_dated, 660, 600));
  assert_true(e2e_notify_has_no_watcher(unreadable_but_dated, 661, 600));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_valid_timestamps),
    cmocka_unit_test(test_out_of_range_rejected),
    cmocka_unit_test(test_bad_suffix_rejected),
    cmocka_unit_test(test_truncated_rejected),
    cmocka_unit_test(test_missing_field_rejected),
    cmocka_unit_test(test_separator),
    cmocka_unit_test(test_no_watcher_for_unwatched_or_stale_blobs),
    cmocka_unit_test(test_blob_name_is_decoded_or_decides_nothing),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
