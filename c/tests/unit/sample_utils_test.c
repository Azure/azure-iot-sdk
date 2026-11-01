// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file sample_utils_test.c
 * @brief The samples' file helpers (samples/common/sample_utils.c). Files are
 * created in the working directory and removed by each test.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* symlink, lstat, umask, mkdir, chmod */
#endif

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "sample_utils.h"

#define SOURCE_FILE "sample_utils_test_source.tmp"
#define DESTINATION_FILE "sample_utils_test_destination.tmp"
#define LINK_FILE "sample_utils_test_link.tmp"
#define DIRECTORY "sample_utils_test_directory.tmp"

static void remove_test_files(void)
{
  (void)remove(SOURCE_FILE);
  (void)remove(DESTINATION_FILE);
  (void)remove(LINK_FILE);
#if !defined(_WIN32)
  (void)chmod(DIRECTORY, 0700);
  (void)remove(DIRECTORY "/" DESTINATION_FILE);
  (void)rmdir(DIRECTORY);
#endif
}

static int setup(void** state)
{
  (void)state;
  remove_test_files();
  return 0;
}

static int teardown(void** state)
{
  (void)state;
  remove_test_files();
  return 0;
}

/** @brief Write @p text to @p path through sample_file_open_private(). */
static void write_text_file(const char* path, const char* text)
{
  FILE* file = sample_file_open_private(path, false);
  assert_non_null(file);
  assert_int_equal(fputs(text, file) >= 0, 1);
  assert_int_equal(fclose(file), 0);
}

/** @brief Assert that @p path holds exactly @p expected. */
static void assert_file_holds(const char* path, const char* expected)
{
  char contents[64] = { 0 };
  FILE* file = sample_file_open_read_no_follow(path);
  assert_non_null(file);
  size_t read = fread(contents, 1, sizeof(contents) - 1, file);
  assert_int_equal(fclose(file), 0);
  assert_int_equal(read, strlen(expected));
  assert_string_equal(contents, expected);
}

static void file_replace_overwrites_the_destination(void** state)
{
  (void)state;
  write_text_file(DESTINATION_FILE, "old");
  write_text_file(SOURCE_FILE, "new");

  assert_true(sample_file_replace(SOURCE_FILE, DESTINATION_FILE));

  assert_file_holds(DESTINATION_FILE, "new");
  assert_null(sample_file_open_read_no_follow(SOURCE_FILE));
}

static void file_replace_creates_a_missing_destination_given_with_a_directory(void** state)
{
  (void)state;
  write_text_file(SOURCE_FILE, "new");

  /* A path with a directory part: the directory is synced, not the working one. */
  assert_true(sample_file_replace(SOURCE_FILE, "./" DESTINATION_FILE));

  assert_file_holds(DESTINATION_FILE, "new");
}

static void file_replace_fails_without_a_source_and_keeps_the_destination(void** state)
{
  (void)state;
  write_text_file(DESTINATION_FILE, "old");

  assert_false(sample_file_replace(SOURCE_FILE, DESTINATION_FILE));
  assert_false(sample_file_replace(NULL, DESTINATION_FILE));
  assert_false(sample_file_replace(SOURCE_FILE, NULL));

  assert_file_holds(DESTINATION_FILE, "old");
}

static void file_replace_reports_a_done_rename_when_the_directory_sync_fails(void** state)
{
  (void)state;
  write_text_file(SOURCE_FILE, "new");
#if !defined(_WIN32)
  /* Write and search but no read: the rename succeeds, opening the directory
   * for its sync does not (unless run as root). */
  assert_int_equal(mkdir(DIRECTORY, 0700), 0);
  assert_int_equal(chmod(DIRECTORY, 0300), 0);
  bool replaced = sample_file_replace(SOURCE_FILE, DIRECTORY "/" DESTINATION_FILE);
  assert_int_equal(chmod(DIRECTORY, 0700), 0);
  assert_true(replaced);
  assert_file_holds(DIRECTORY "/" DESTINATION_FILE, "new");
#else
  assert_true(sample_file_replace(SOURCE_FILE, DESTINATION_FILE));
  assert_file_holds(DESTINATION_FILE, "new");
#endif
}

static void file_flush_to_storage_writes_through_the_buffer(void** state)
{
  (void)state;
  FILE* file = sample_file_open_private(DESTINATION_FILE, false);
  assert_non_null(file);
  assert_int_equal(fputs("data", file) >= 0, 1);

  assert_true(sample_file_flush_to_storage(file));
#if !defined(_WIN32)
  /* Visible to another reader before the writer closes. Windows refuses a
   * reader while the writer holds the file open with _SH_DENYWR. */
  assert_file_holds(DESTINATION_FILE, "data");
#endif

  assert_int_equal(fclose(file), 0);
  assert_file_holds(DESTINATION_FILE, "data");
  assert_false(sample_file_flush_to_storage(NULL));
}

static void file_open_private_creates_an_owner_only_file(void** state)
{
  (void)state;
#if !defined(_WIN32)
  /* A umask that leaves fopen()'s files readable by everyone. */
  mode_t previous_umask = umask(022);
#endif
  write_text_file(DESTINATION_FILE, "secret");
#if !defined(_WIN32)
  (void)umask(previous_umask);
  struct stat st;
  assert_int_equal(stat(DESTINATION_FILE, &st), 0);
  assert_int_equal(st.st_mode & 0777, 0600);
#endif
  assert_file_holds(DESTINATION_FILE, "secret");
}

static void file_open_private_replace_does_not_write_through_a_link(void** state)
{
  (void)state;
#if !defined(_WIN32)
  write_text_file(DESTINATION_FILE, "target");
  assert_int_equal(symlink(DESTINATION_FILE, LINK_FILE), 0);

  FILE* file = sample_file_open_private(LINK_FILE, true);
  assert_non_null(file);
  assert_int_equal(fputs("written", file) >= 0, 1);
  assert_int_equal(fclose(file), 0);

  struct stat st;
  assert_int_equal(lstat(LINK_FILE, &st), 0);
  assert_true(S_ISREG(st.st_mode));
  assert_file_holds(DESTINATION_FILE, "target");
  assert_file_holds(LINK_FILE, "written");
#else
  /* Creating a symbolic link needs a privilege Windows test runs lack: check
   * that an existing file is replaced, not appended to. */
  write_text_file(LINK_FILE, "old contents");
  FILE* file = sample_file_open_private(LINK_FILE, true);
  assert_non_null(file);
  assert_int_equal(fputs("written", file) >= 0, 1);
  assert_int_equal(fclose(file), 0);
  assert_file_holds(LINK_FILE, "written");
#endif
}

static void file_create_private_refuses_an_existing_name(void** state)
{
  (void)state;
  write_text_file(DESTINATION_FILE, "kept");
  assert_null(sample_file_create_private(DESTINATION_FILE));
  assert_file_holds(DESTINATION_FILE, "kept");
  assert_null(sample_file_create_private(NULL));

  FILE* file = sample_file_create_private(SOURCE_FILE);
  assert_non_null(file);
  assert_int_equal(fputs("new", file) >= 0, 1);
  assert_int_equal(fclose(file), 0);
  assert_file_holds(SOURCE_FILE, "new");
#if !defined(_WIN32)
  struct stat st;
  assert_int_equal(stat(SOURCE_FILE, &st), 0);
  assert_int_equal(st.st_mode & 0777, 0600);
  /* A link is an existing name too: its target is not written. */
  assert_int_equal(symlink(DESTINATION_FILE, LINK_FILE), 0);
  assert_null(sample_file_create_private(LINK_FILE));
  assert_file_holds(DESTINATION_FILE, "kept");
#endif
}

static void file_create_private_unique_creates_a_new_name_beside_the_path(void** state)
{
  (void)state;
  write_text_file(DESTINATION_FILE, "kept");
  char* first = NULL;
  char* second = NULL;
  FILE* first_file = sample_file_create_private_unique(DESTINATION_FILE, &first);
  FILE* second_file = sample_file_create_private_unique(DESTINATION_FILE, &second);
  assert_non_null(first_file);
  assert_non_null(second_file);
  assert_string_not_equal(first, second);
  assert_int_equal(strncmp(first, DESTINATION_FILE ".", strlen(DESTINATION_FILE ".")), 0);
  assert_int_equal(fputs("new", first_file) >= 0, 1);
  assert_int_equal(fclose(first_file), 0);
  assert_int_equal(fclose(second_file), 0);
  assert_file_holds(DESTINATION_FILE, "kept");
#if !defined(_WIN32)
  struct stat st;
  assert_int_equal(stat(first, &st), 0);
  assert_int_equal(st.st_mode & 0777, 0600);
#endif
  /* Beside the path, so it can replace it. */
  assert_true(sample_file_replace(first, DESTINATION_FILE));
  assert_file_holds(DESTINATION_FILE, "new");
  assert_int_equal(remove(second), 0);
  free(first);
  free(second);

  char* none = NULL;
  assert_null(sample_file_create_private_unique(NULL, &none));
  assert_null(none);
  assert_null(sample_file_create_private_unique(DESTINATION_FILE, NULL));
  assert_null(sample_file_create_private_unique("missing_directory.tmp/name", &none));
  assert_null(none);
}

static void file_open_read_no_follow_refuses_a_link_and_a_missing_file(void** state)
{
  (void)state;
  assert_null(sample_file_open_read_no_follow(DESTINATION_FILE));
  assert_null(sample_file_open_read_no_follow(NULL));
#if !defined(_WIN32)
  write_text_file(DESTINATION_FILE, "target");
  assert_int_equal(symlink(DESTINATION_FILE, LINK_FILE), 0);
  assert_null(sample_file_open_read_no_follow(LINK_FILE));
#endif
}

#if !defined(_WIN32)
/** @brief Number of entries in @p directory, "." and ".." excluded. */
static int count_directory_entries(const char* directory)
{
  DIR* opened = opendir(directory);
  assert_non_null(opened);
  int count = 0;
  struct dirent* entry;
  while ((entry = readdir(opened)) != NULL)
  {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
    {
      ++count;
    }
  }
  assert_int_equal(closedir(opened), 0);
  return count;
}
#endif

static void file_write_atomically_replaces_the_destination(void** state)
{
  (void)state;
  assert_true(sample_file_write_atomically(DESTINATION_FILE, "old", 3));
  assert_file_holds(DESTINATION_FILE, "old");
  assert_true(sample_file_write_atomically(DESTINATION_FILE, "new", 3));
  assert_file_holds(DESTINATION_FILE, "new");
  assert_true(sample_file_write_atomically(DESTINATION_FILE, NULL, 0));
  assert_file_holds(DESTINATION_FILE, "");
#if !defined(_WIN32)
  struct stat st;
  assert_int_equal(stat(DESTINATION_FILE, &st), 0);
  assert_int_equal(st.st_mode & 0777, 0600);
  /* No temporary file is left beside the destination. */
  assert_int_equal(mkdir(DIRECTORY, 0700), 0);
  assert_true(sample_file_write_atomically(DIRECTORY "/" DESTINATION_FILE, "new", 3));
  assert_true(sample_file_write_atomically(DIRECTORY "/" DESTINATION_FILE, "newer", 5));
  assert_file_holds(DIRECTORY "/" DESTINATION_FILE, "newer");
  assert_int_equal(count_directory_entries(DIRECTORY), 1);
#endif
}

static void file_write_atomically_fails_and_keeps_the_destination(void** state)
{
  (void)state;
  write_text_file(DESTINATION_FILE, "old");

  assert_false(sample_file_write_atomically(DESTINATION_FILE, NULL, 3));
  assert_false(sample_file_write_atomically(NULL, "new", 3));
  assert_false(sample_file_write_atomically("missing_directory.tmp/name", "new", 3));

  assert_file_holds(DESTINATION_FILE, "old");
#if !defined(_WIN32)
  /* The rename fails over a directory: the temporary file is removed. */
  assert_int_equal(mkdir(DIRECTORY, 0700), 0);
  assert_int_equal(mkdir(DIRECTORY "/" DESTINATION_FILE, 0700), 0);
  assert_false(sample_file_write_atomically(DIRECTORY "/" DESTINATION_FILE, "new", 3));
  assert_int_equal(count_directory_entries(DIRECTORY), 1);
#endif
}

#if !defined(_WIN32)
/**
 * @brief Whether @p signal_number, raised in a new child process after
 * sample_install_stop_handler(), moves the stop request from false to true without
 * ending the process.
 */
static bool signal_requests_stop_in_new_process(int signal_number)
{
  pid_t child = fork();
  if (child == 0)
  {
    sample_install_stop_handler();
    bool before = sample_is_stop_requested();
    bool raised = raise(signal_number) == 0;
    _exit(!before && raised && sample_is_stop_requested() ? 0 : 1);
  }
  int status = 0;
  return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status)
      && WEXITSTATUS(status) == 0;
}
#endif

static void stop_handler_records_sigterm_and_sigint(void** state)
{
  (void)state;
#if !defined(_WIN32)
  /* One process per signal, so each must set the flag on its own. */
  assert_true(signal_requests_stop_in_new_process(SIGINT));
  assert_true(signal_requests_stop_in_new_process(SIGTERM));
#else
  /* No fork(): both signals must reach the same installed handler, which sets the flag. */
  sample_install_stop_handler();
  void (*on_sigint)(int) = signal(SIGINT, SIG_DFL);
  void (*on_sigterm)(int) = signal(SIGTERM, SIG_DFL);
  assert_true(on_sigint != SIG_DFL && on_sigint != SIG_IGN && on_sigint != SIG_ERR);
  assert_true(on_sigint == on_sigterm);
  sample_install_stop_handler();
  assert_false(sample_is_stop_requested());
  assert_int_equal(raise(SIGINT), 0);
  assert_true(sample_is_stop_requested());
#endif
}

static void sleep_until_tick_end_waits_out_the_rest_of_the_tick(void** state)
{
  (void)state;
  uint64_t start = sample_now_ms();
  sample_sleep_until_tick_end(start, 100u);
  uint64_t elapsed = sample_now_ms() - start;
  /* Clock granularity is ~16 ms on Windows; the upper bound only catches a hang. */
  assert_true(elapsed >= 80u);
  assert_true(elapsed < 2000u);
}

static void sleep_until_tick_end_returns_at_once_after_the_tick(void** state)
{
  (void)state;
  uint64_t start = sample_now_ms();
  sample_sleep_until_tick_end(start, 0u);
  sample_sleep_until_tick_end(start > 1000u ? start - 1000u : 0u, 100u);
  assert_true(sample_now_ms() - start < 50u);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(file_replace_overwrites_the_destination, setup, teardown),
    cmocka_unit_test_setup_teardown(
        file_replace_creates_a_missing_destination_given_with_a_directory, setup, teardown),
    cmocka_unit_test_setup_teardown(
        file_replace_fails_without_a_source_and_keeps_the_destination, setup, teardown),
    cmocka_unit_test_setup_teardown(
        file_replace_reports_a_done_rename_when_the_directory_sync_fails, setup, teardown),
    cmocka_unit_test_setup_teardown(
        file_flush_to_storage_writes_through_the_buffer, setup, teardown),
    cmocka_unit_test_setup_teardown(file_open_private_creates_an_owner_only_file, setup, teardown),
    cmocka_unit_test_setup_teardown(
        file_open_private_replace_does_not_write_through_a_link, setup, teardown),
    cmocka_unit_test_setup_teardown(file_create_private_refuses_an_existing_name, setup, teardown),
    cmocka_unit_test_setup_teardown(
        file_create_private_unique_creates_a_new_name_beside_the_path, setup, teardown),
    cmocka_unit_test_setup_teardown(
        file_open_read_no_follow_refuses_a_link_and_a_missing_file, setup, teardown),
    cmocka_unit_test_setup_teardown(
        file_write_atomically_replaces_the_destination, setup, teardown),
    cmocka_unit_test_setup_teardown(
        file_write_atomically_fails_and_keeps_the_destination, setup, teardown),
    cmocka_unit_test(stop_handler_records_sigterm_and_sigint),
    cmocka_unit_test(sleep_until_tick_end_waits_out_the_rest_of_the_tick),
    cmocka_unit_test(sleep_until_tick_end_returns_at_once_after_the_tick),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
