// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Concurrent writers through the rotating file sink (az_iot_log_file.h: "Safe
 * to call from several threads"). Several threads log through the global sink
 * while files rotate. Every line must arrive exactly once, whole, and in its
 * thread's order across the rotated files. Labelled "race", so CI also runs it
 * under helgrind and DRD (eng/run-race-detectors.sh). */

#if defined(_WIN32)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS /* fopen, sscanf */
#endif
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_log_components.h"
#include "azure/iot/az_iot_log_file.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
typedef HANDLE race_thread;
#define RACE_THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
#define RACE_THREAD_RETURN return 0
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t race_thread;
#define RACE_THREAD_FN(name) static void* name(void* arg)
#define RACE_THREAD_RETURN return NULL
#endif

/* Small files, so the writers rotate many times; enough of them to keep every
 * line, so loss can be told apart from the oldest file being dropped. */
#define WRITERS 4u
#define LINES_PER_WRITER 250u
#define MAX_FILE_BYTES 4096u
#define MAX_FILES 99u

RACE_THREAD_FN(writer)
{
  unsigned const id = (unsigned)(uintptr_t)arg;
  for (unsigned i = 0; i < LINES_PER_WRITER; ++i)
  {
    AZ_IOT_LOG_INFOF(AZ_IOT_LOG_COMPONENT_APP, "w%u n%u", id, i);
  }
  RACE_THREAD_RETURN;
}

static void thread_start(race_thread* t, unsigned id)
{
#if defined(_WIN32)
  *t = CreateThread(NULL, 0, writer, (LPVOID)(uintptr_t)id, 0, NULL);
  assert_non_null(*t);
#else
  assert_int_equal(0, pthread_create(t, NULL, writer, (void*)(uintptr_t)id));
#endif
}

static void thread_join(race_thread t)
{
#if defined(_WIN32)
  assert_int_equal(WAIT_OBJECT_0, WaitForSingleObject(t, INFINITE));
  CloseHandle(t);
#else
  assert_int_equal(0, pthread_join(t, NULL));
#endif
}

/* Active file for index 0, `<base>.<index>` otherwise. */
static void file_name(char* out, size_t cap, const char* base, unsigned index)
{
  int n = index == 0u ? snprintf(out, cap, "%s", base) : snprintf(out, cap, "%s.%u", base, index);
  assert_true(n > 0 && (size_t)n < cap);
}

/* Whole file, NUL-terminated, or NULL when it does not exist. Caller frees. */
static char* read_all(const char* path, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (f == NULL)
  {
    return NULL;
  }
  size_t cap = 2u * MAX_FILE_BYTES;
  char* buf = (char*)malloc(cap + 1u);
  assert_non_null(buf);
  size_t n = fread(buf, 1u, cap, f);
  assert_true(n < cap); /* no file may exceed MAX_FILE_BYTES */
  assert_int_equal(0, fclose(f));
  buf[n] = '\0';
  *out_len = n;
  return buf;
}

static void writers_rotate_without_losing_or_tearing_lines(void** state)
{
  (void)state;
  char base[64];
  char path[96];
#if defined(_WIN32)
  unsigned const pid = (unsigned)_getpid();
#else
  unsigned const pid = (unsigned)getpid();
#endif
  assert_true(snprintf(base, sizeof(base), "az_iot_log_race_%u.log", pid) > 0);
  for (unsigned i = 0; i <= MAX_FILES; ++i)
  {
    file_name(path, sizeof(path), base, i);
    (void)remove(path);
  }

  az_iot_log_file_sink fs;
  az_iot_log_sink sink;
  az_iot_log_file_sink_options opts = az_iot_log_file_sink_options_default();
  opts.max_file_bytes = MAX_FILE_BYTES;
  opts.max_files = MAX_FILES;
  assert_int_equal(
      AZ_IOT_OK, az_iot_log_file_sink_open(&fs, base, &opts, AZ_IOT_LOG_LEVEL_INFO, &sink));
  az_iot_log_set_global_sink(&sink);

  race_thread threads[WRITERS];
  for (unsigned w = 0; w < WRITERS; ++w)
  {
    thread_start(&threads[w], w);
  }
  for (unsigned w = 0; w < WRITERS; ++w)
  {
    thread_join(threads[w]);
  }

  az_iot_log_set_global_sink(NULL);
  az_iot_log_file_sink_close(&fs);

  /* Oldest file first, the active one last: each writer's lines must then
   * read 0, 1, 2, ... with nothing missing, repeated or torn. */
  unsigned next[WRITERS] = { 0 };
  unsigned files = 0;
  for (unsigned i = MAX_FILES + 1u; i-- > 0u;)
  {
    size_t len = 0;
    file_name(path, sizeof(path), base, i);
    char* text = read_all(path, &len);
    if (text == NULL)
    {
      continue;
    }
    ++files;
    assert_true(len <= MAX_FILE_BYTES);
    assert_true(len == 0u || text[len - 1u] == '\n');
    for (char* line = text; *line != '\0';)
    {
      char* end = strchr(line, '\n');
      assert_non_null(end);
      *end = '\0';
      /* Message after the last "] ": "w<id> n<seq>". */
      char* msg = NULL;
      for (char* p = strstr(line, "] "); p != NULL; p = strstr(p + 1, "] "))
      {
        msg = p + 2;
      }
      assert_non_null(msg);
      unsigned w = 0;
      unsigned n = 0;
      int used = 0;
      assert_int_equal(2, sscanf(msg, "w%u n%u%n", &w, &n, &used));
      assert_int_equal('\0', msg[used]);
      assert_true(w < WRITERS);
      assert_int_equal(next[w], n);
      ++next[w];
      line = end + 1;
    }
    free(text);
    assert_int_equal(0, remove(path));
  }

  for (unsigned w = 0; w < WRITERS; ++w)
  {
    assert_int_equal(LINES_PER_WRITER, next[w]);
  }
  /* The run must actually have rotated, or it proved nothing about rotation. */
  assert_true(files > 1u);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(writers_rotate_without_losing_or_tearing_lines),
  };
  return cmocka_run_group_tests_name("log_file_race_tests", tests, NULL, NULL);
}
