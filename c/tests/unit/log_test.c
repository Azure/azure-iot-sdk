// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for the logging facade.
 *
 * The facade is the only way SDK code and MQTT adapters are allowed to trace,
 * so the properties that matter are: nothing escapes when no sink is
 * registered, the minimum level is honoured before any formatting happens, and
 * a message longer than the buffer is truncated rather than dropped. The file
 * sink must write the documented line format, append, and rotate.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "azure/iot/az_iot_log.h"

typedef struct capture
{
  int count;
  az_iot_log_level level;
  char msg[AZ_IOT_LOG_MESSAGE_MAX];
  const char* file;
  int line;
} capture;

static capture g_capture;

static void capture_sink(
    void* user_ctx,
    az_iot_log_level level,
    const char* file,
    int line,
    const char* msg)
{
  capture* c = (capture*)user_ctx;
  c->count++;
  c->level = level;
  c->file = file;
  c->line = line;
  if (msg != NULL)
  {
    size_t n = strlen(msg);
    if (n >= sizeof(c->msg))
    {
      n = sizeof(c->msg) - 1;
    }
    memcpy(c->msg, msg, n);
    c->msg[n] = '\0';
  }
}

static int setup(void** state)
{
  memset(&g_capture, 0, sizeof(g_capture));
  az_iot_log_sink sink;
  sink.sink = capture_sink;
  sink.user_ctx = &g_capture;
  sink.min_level = AZ_IOT_LOG_LEVEL_TRACE;
  az_iot_log_set_global_sink(&sink);
  *state = &g_capture;
  return 0;
}

static int teardown(void** state)
{
  (void)state;
  az_iot_log_set_global_sink(NULL);
  return 0;
}

static void nothing_is_emitted_without_a_sink(void** state)
{
  (void)state;
  az_iot_log_set_global_sink(NULL);
  memset(&g_capture, 0, sizeof(g_capture));

  assert_false(az_iot_log_is_enabled(AZ_IOT_LOG_LEVEL_ERROR));
  AZ_IOT_LOG_ERROR("dropped");
  AZ_IOT_LOG_ERRORF("also %s", "dropped");
  assert_int_equal(g_capture.count, 0);
}

static void plain_and_formatted_messages_reach_the_sink(void** state)
{
  (void)state;
  AZ_IOT_LOG_WARN("plain message");
  assert_int_equal(g_capture.count, 1);
  assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_WARN);
  assert_string_equal(g_capture.msg, "plain message");
  assert_non_null(g_capture.file);
  assert_true(g_capture.line > 0);

  AZ_IOT_LOG_DEBUGF("device %s port %u", "dev-1", 8883u);
  assert_int_equal(g_capture.count, 2);
  assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_DEBUG);
  assert_string_equal(g_capture.msg, "device dev-1 port 8883");
}

static void every_level_is_routed(void** state)
{
  (void)state;
  AZ_IOT_LOG_TRACE("t");
  assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_TRACE);
  AZ_IOT_LOG_DEBUG("d");
  assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_DEBUG);
  AZ_IOT_LOG_INFO("i");
  assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_INFO);
  AZ_IOT_LOG_WARN("w");
  assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_WARN);
  AZ_IOT_LOG_ERROR("e");
  assert_int_equal(g_capture.level, AZ_IOT_LOG_LEVEL_ERROR);
  assert_int_equal(g_capture.count, 5);
}

static void levels_below_the_minimum_are_dropped(void** state)
{
  (void)state;
  az_iot_log_sink sink;
  sink.sink = capture_sink;
  sink.user_ctx = &g_capture;
  sink.min_level = AZ_IOT_LOG_LEVEL_WARN;
  az_iot_log_set_global_sink(&sink);
  memset(&g_capture, 0, sizeof(g_capture));

  assert_false(az_iot_log_is_enabled(AZ_IOT_LOG_LEVEL_INFO));
  assert_true(az_iot_log_is_enabled(AZ_IOT_LOG_LEVEL_WARN));

  AZ_IOT_LOG_DEBUG("no");
  AZ_IOT_LOG_INFOF("no %d", 1);
  assert_int_equal(g_capture.count, 0);

  AZ_IOT_LOG_WARN("yes");
  assert_int_equal(g_capture.count, 1);
}

static void an_oversized_message_is_truncated_not_dropped(void** state)
{
  (void)state;
  char big[AZ_IOT_LOG_MESSAGE_MAX * 2];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';

  AZ_IOT_LOG_ERRORF("%s", big);

  /* The diagnostic still arrives, shortened to the buffer and marked as cut. */
  assert_int_equal(g_capture.count, 1);
  assert_int_equal(strlen(g_capture.msg), AZ_IOT_LOG_MESSAGE_MAX - 1);
  assert_memory_equal(g_capture.msg + AZ_IOT_LOG_MESSAGE_MAX - 4, "...", 3);
}

static void a_message_that_fits_is_not_marked(void** state)
{
  (void)state;
  char exact[AZ_IOT_LOG_MESSAGE_MAX];
  memset(exact, 'y', sizeof(exact) - 1);
  exact[sizeof(exact) - 1] = '\0';

  AZ_IOT_LOG_ERRORF("%s", exact);
  assert_string_equal(g_capture.msg, exact);
}

/* ---- file sink ---------------------------------------------------------- */

#define FILE_BUF 8192

/* Process-unique name in the cwd (the test's binary directory). */
static void temp_log_path(char* out, size_t cap, const char* tag)
{
#if defined(_WIN32)
  unsigned pid = (unsigned)_getpid();
#else
  unsigned pid = (unsigned)getpid();
#endif
  int n = snprintf(out, cap, "az_iot_log_%u_%s.log", pid, tag);
  assert_true(n > 0 && (size_t)n < cap);
}

static void rotated_path(char* out, size_t cap, const char* base, unsigned index)
{
  int n = snprintf(out, cap, "%s.%u", base, index);
  assert_true(n > 0 && (size_t)n < cap);
}

/* Whole file into @p buf; -1 when it does not exist. */
static long read_file(const char* path, char* buf, size_t cap)
{
  FILE* f = fopen(path, "rb");
  if (f == NULL)
  {
    return -1;
  }
  size_t n = fread(buf, 1, cap - 1, f);
  buf[n] = '\0';
  assert_int_equal(0, fclose(f));
  return (long)n;
}

static void remove_log_files(const char* base)
{
  char p[300];
  remove(base);
  for (unsigned i = 1; i <= 4; ++i)
  {
    rotated_path(p, sizeof(p), base, i);
    remove(p);
  }
}

static size_t count_lines(const char* text)
{
  size_t n = 0;
  for (const char* c = text; *c != '\0'; ++c)
  {
    n += (*c == '\n') ? 1u : 0u;
  }
  return n;
}

static void file_sink_writes_the_documented_line_format(void** state)
{
  (void)state;
  char path[256];
  char text[FILE_BUF];
  az_iot_log_file_sink fs;
  az_iot_log_sink sink;
  temp_log_path(path, sizeof(path), "format");
  remove_log_files(path);

  assert_int_equal(
      AZ_IOT_OK, az_iot_log_file_sink_open(&fs, path, NULL, AZ_IOT_LOG_LEVEL_INFO, &sink));
  az_iot_log_set_global_sink(&sink);
  AZ_IOT_LOG_DEBUG("app: below the minimum");
  AZ_IOT_LOG_INFO("app: hello");
  az_iot_log_set_global_sink(NULL);
  az_iot_log_file_sink_close(&fs);

  assert_true(read_file(path, text, sizeof(text)) > 0);
  assert_int_equal(1, count_lines(text));
  /* 2026-10-03T18:04:05.123Z [INFO ] ... log_test.c:<line>: app: hello */
  assert_true(strlen(text) > 25);
  assert_int_equal('-', text[4]);
  assert_int_equal('-', text[7]);
  assert_int_equal('T', text[10]);
  assert_int_equal(':', text[13]);
  assert_int_equal(':', text[16]);
  assert_int_equal('.', text[19]);
  assert_int_equal('Z', text[23]);
  assert_memory_equal(text + 24, " [INFO ] ", 9);
  /* File name without its directory. */
  const char* where = strstr(text, "log_test.c:");
  assert_non_null(where);
  assert_true(where[-1] == ' ');
  const char* tail = strstr(text, ": app: hello\n");
  assert_non_null(tail);
  assert_int_equal('\0', tail[strlen(": app: hello\n")]);

  remove_log_files(path);
}

static void file_sink_appends_across_opens(void** state)
{
  (void)state;
  char path[256];
  char text[FILE_BUF];
  az_iot_log_file_sink fs;
  az_iot_log_sink sink;
  temp_log_path(path, sizeof(path), "append");
  remove_log_files(path);

  for (int i = 0; i < 2; ++i)
  {
    assert_int_equal(
        AZ_IOT_OK, az_iot_log_file_sink_open(&fs, path, NULL, AZ_IOT_LOG_LEVEL_TRACE, &sink));
    az_iot_log_set_global_sink(&sink);
    AZ_IOT_LOG_WARNF("app: run %d", i);
    az_iot_log_set_global_sink(NULL);
    az_iot_log_file_sink_close(&fs);
  }

  assert_true(read_file(path, text, sizeof(text)) > 0);
  assert_int_equal(2, count_lines(text));
  assert_non_null(strstr(text, "app: run 0\n"));
  assert_non_null(strstr(text, "app: run 1\n"));

  remove_log_files(path);
}

static void file_sink_rotates_and_keeps_max_files(void** state)
{
  (void)state;
  char path[256];
  char p[300];
  char text[FILE_BUF];
  az_iot_log_file_sink fs;
  az_iot_log_sink sink;
  az_iot_log_file_sink_options o = az_iot_log_file_sink_options_default();
  o.max_file_bytes = 300;
  o.max_files = 2;
  temp_log_path(path, sizeof(path), "rotate");
  remove_log_files(path);

  assert_int_equal(
      AZ_IOT_OK, az_iot_log_file_sink_open(&fs, path, &o, AZ_IOT_LOG_LEVEL_TRACE, &sink));
  az_iot_log_set_global_sink(&sink);
  for (int i = 0; i < 20; ++i)
  {
    AZ_IOT_LOG_INFOF("app: line %02d %s", i, "padding-padding-padding-padding");
  }
  az_iot_log_set_global_sink(NULL);
  az_iot_log_file_sink_close(&fs);

  /* Newest last: the active file holds line 19, .1 the lines before it. */
  long active = read_file(path, text, sizeof(text));
  assert_true(active > 0 && active <= 300);
  assert_non_null(strstr(text, "app: line 19 "));
  for (unsigned i = 1; i <= 2; ++i)
  {
    rotated_path(p, sizeof(p), path, i);
    long n = read_file(p, text, sizeof(text));
    assert_true(n > 0 && n <= 300);
  }
  rotated_path(p, sizeof(p), path, 3);
  assert_int_equal(-1, read_file(p, text, sizeof(text)));

  remove_log_files(path);
}

static void file_sink_marks_a_truncated_line(void** state)
{
  (void)state;
  char path[256];
  char text[FILE_BUF];
  char big[AZ_IOT_LOG_MESSAGE_MAX * 2];
  az_iot_log_file_sink fs;
  az_iot_log_sink sink;
  memset(big, 'z', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  temp_log_path(path, sizeof(path), "trunc");
  remove_log_files(path);

  assert_int_equal(
      AZ_IOT_OK, az_iot_log_file_sink_open(&fs, path, NULL, AZ_IOT_LOG_LEVEL_TRACE, &sink));
  az_iot_log_set_global_sink(&sink);
  /* emit() does not bound the message; the line formatter must. */
  AZ_IOT_LOG_ERROR(big);
  az_iot_log_set_global_sink(NULL);
  az_iot_log_file_sink_close(&fs);

  long n = read_file(path, text, sizeof(text));
  assert_true(n > 4);
  assert_int_equal(1, count_lines(text));
  assert_memory_equal(text + n - 4, "...\n", 4);

  remove_log_files(path);
}

static void file_sink_rejects_bad_arguments(void** state)
{
  (void)state;
  az_iot_log_file_sink fs;
  az_iot_log_sink sink;
  az_iot_log_file_sink_options o = az_iot_log_file_sink_options_default();
  char long_path[AZ_IOT_LOG_FILE_PATH_MAX];
  memset(long_path, 'p', sizeof(long_path) - 1);
  long_path[sizeof(long_path) - 1] = '\0';

  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG,
      az_iot_log_file_sink_open(NULL, "x.log", NULL, AZ_IOT_LOG_LEVEL_INFO, &sink));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG,
      az_iot_log_file_sink_open(&fs, NULL, NULL, AZ_IOT_LOG_LEVEL_INFO, &sink));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG,
      az_iot_log_file_sink_open(&fs, "", NULL, AZ_IOT_LOG_LEVEL_INFO, &sink));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG,
      az_iot_log_file_sink_open(&fs, "x.log", NULL, AZ_IOT_LOG_LEVEL_INFO, NULL));
  o.max_files = 100;
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG,
      az_iot_log_file_sink_open(&fs, "x.log", &o, AZ_IOT_LOG_LEVEL_INFO, &sink));
  assert_int_equal(
      AZ_IOT_ERR_NOT_ENOUGH_SPACE,
      az_iot_log_file_sink_open(&fs, long_path, NULL, AZ_IOT_LOG_LEVEL_INFO, &sink));
  /* NULL is ignored. */
  az_iot_log_file_sink_close(NULL);
}

static void a_precision_bounded_argument_is_not_over_read(void** state)
{
  (void)state;
  /* The DPS failure path logs a response body that is not NUL-terminated,
   * using a precision to bound it. */
  const char payload[] = { 'a', 'b', 'c', 'd' };
  AZ_IOT_LOG_ERRORF("body: %.*s", 2, payload);
  assert_string_equal(g_capture.msg, "body: ab");
}

static void null_message_and_file_reach_the_sink_as_text(void** state)
{
  (void)state;
  /* emit is public, so a sink can be handed a NULL that the SDK never
   * produced itself. Sinks format both with "%s" -- the built-in stderr one
   * does -- so the substitution has to happen before the call, not in the
   * sink. */
  az_iot_log_emit(AZ_IOT_LOG_LEVEL_ERROR, NULL, 7, NULL);
  assert_int_equal(g_capture.count, 1);
  assert_string_equal(g_capture.msg, "");
  assert_string_equal(g_capture.file, "?");
  assert_int_equal(g_capture.line, 7);

  az_iot_log_emitf(AZ_IOT_LOG_LEVEL_WARN, NULL, 9, "value %d", 3);
  assert_int_equal(g_capture.count, 2);
  assert_string_equal(g_capture.msg, "value 3");
  assert_string_equal(g_capture.file, "?");

  /* A NULL format has no message to build, so nothing is emitted. */
  az_iot_log_emitf(AZ_IOT_LOG_LEVEL_WARN, __FILE__, __LINE__, NULL);
  assert_int_equal(g_capture.count, 2);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(nothing_is_emitted_without_a_sink, setup, teardown),
    cmocka_unit_test_setup_teardown(plain_and_formatted_messages_reach_the_sink, setup, teardown),
    cmocka_unit_test_setup_teardown(every_level_is_routed, setup, teardown),
    cmocka_unit_test_setup_teardown(levels_below_the_minimum_are_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(an_oversized_message_is_truncated_not_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(a_message_that_fits_is_not_marked, setup, teardown),
    cmocka_unit_test_setup_teardown(a_precision_bounded_argument_is_not_over_read, setup, teardown),
    cmocka_unit_test_setup_teardown(null_message_and_file_reach_the_sink_as_text, setup, teardown),
    cmocka_unit_test_setup_teardown(file_sink_writes_the_documented_line_format, setup, teardown),
    cmocka_unit_test_setup_teardown(file_sink_appends_across_opens, setup, teardown),
    cmocka_unit_test_setup_teardown(file_sink_rotates_and_keeps_max_files, setup, teardown),
    cmocka_unit_test_setup_teardown(file_sink_marks_a_truncated_line, setup, teardown),
    cmocka_unit_test_setup_teardown(file_sink_rejects_bad_arguments, setup, teardown),
  };
  return cmocka_run_group_tests_name("log", tests, NULL, NULL);
}
