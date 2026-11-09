// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* support/e2e_process.h against a helper child (e2e_process_test_child.c, path in
 * AZ_IOT_E2E_PROCESS_TEST_CHILD): exit codes, captured output, the child-only environment,
 * waiting with a pump, both stop paths, and a child that outlives its shell. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include "support/e2e_process.h"
#include "support/test_env.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#define START_TIMEOUT_MS 10000 /* generous: a loaded CI runner */
#define ENVIRONMENT_NAME "AZ_IOT_E2E_PROCESS_TEST_VALUE"

static az_iot_e2e_process s_process;

/** @brief Starts the helper child with @p mode and @p argument (may be NULL). */
static void start_child(const char* mode, const char* argument, const char* const* environment)
{
  const char* arguments[4] = { AZ_IOT_E2E_PROCESS_TEST_CHILD, NULL, NULL, NULL };
  arguments[1] = mode;
  arguments[2] = argument;
  assert_true(az_iot_e2e_process_start(&s_process, arguments, environment, "[child] "));
}

/** @brief Sleeps 200 ms: time for a child to fill the pipe. */
static void sleep_briefly(void)
{
#ifdef _WIN32
  Sleep(200);
#else
  struct timespec t = { 0, 200 * 1000 * 1000 };
  (void)nanosleep(&t, NULL);
#endif
}

/** @brief Counts its calls in the int @p context points to. */
static void count_pump(void* context) { ++*(int*)context; }

/** @brief Releases the child, whatever the test left. */
static int teardown(void** state)
{
  (void)state;
  (void)az_iot_e2e_process_stop(&s_process, 0);
  return 0;
}

static void exit_codes_are_reported(void** state)
{
  (void)state;
  for (int code = 0; code <= 1; ++code)
  {
    char text[8];
    (void)snprintf(text, sizeof(text), "%d", code);
    start_child("exit", text, NULL);
    int exit_code = -1;
    assert_true(az_iot_e2e_process_wait_for_exit(&s_process, START_TIMEOUT_MS, &exit_code));
    assert_int_equal(exit_code, code);
    assert_true(az_iot_e2e_process_stop(&s_process, 0));
  }
}

static void stdout_stderr_and_arguments_are_captured(void** state)
{
  (void)state;
  const char* const arguments[] = {
    AZ_IOT_E2E_PROCESS_TEST_CHILD, "print", "two words", "say \"hi\"", "trailing\\", "", NULL
  };
  assert_true(az_iot_e2e_process_start(&s_process, arguments, NULL, "[child] "));
  int exit_code = -1;
  assert_true(az_iot_e2e_process_wait_for_exit(&s_process, START_TIMEOUT_MS, &exit_code));
  assert_int_equal(exit_code, 0);
  assert_true(az_iot_e2e_process_has_printed(&s_process, "first line"));
  assert_true(az_iot_e2e_process_has_printed(&s_process, "line on stderr"));
  assert_true(az_iot_e2e_process_has_printed(&s_process, "argument: <two words>"));
  assert_true(az_iot_e2e_process_has_printed(&s_process, "argument: <say \"hi\">"));
  assert_true(az_iot_e2e_process_has_printed(&s_process, "argument: <trailing\\>"));
  assert_true(az_iot_e2e_process_has_printed(&s_process, "argument: <>"));
  assert_true(az_iot_e2e_process_has_printed(&s_process, "last line"));
  assert_false(az_iot_e2e_process_has_printed(&s_process, "never printed"));
}

static void a_missing_program_does_not_start(void** state)
{
  (void)state;
  const char* const arguments[] = { "az-iot-e2e-process-test-no-such-program", NULL };
  assert_false(az_iot_e2e_process_start(&s_process, arguments, NULL, "[child] "));
}

static void the_log_keeps_the_newest_output(void** state)
{
  (void)state;
  // 300 KB, over AZ_IOT_E2E_PROCESS_LOG_SIZE; not echoed.
  const char* const arguments[] = { AZ_IOT_E2E_PROCESS_TEST_CHILD, "flood", "3000", NULL };
  assert_true(az_iot_e2e_process_start(&s_process, arguments, NULL, NULL));
  assert_true(az_iot_e2e_process_wait_for_exit(&s_process, START_TIMEOUT_MS, NULL));
  assert_true(az_iot_e2e_process_has_printed(&s_process, "line 02999 "));
  assert_false(az_iot_e2e_process_has_printed(&s_process, "line 00000 "));
}

static void waiting_finds_a_line_the_same_read_drops_from_the_log(void** state)
{
  (void)state;
  const char* const arguments[] = { AZ_IOT_E2E_PROCESS_TEST_CHILD, "flood", "3000", NULL };
  assert_true(az_iot_e2e_process_start(&s_process, arguments, NULL, NULL));
  sleep_briefly(); // Its first line, then 300 KB, are waiting in the pipe.
  assert_true(
      az_iot_e2e_process_wait_for_output(&s_process, "line 00000 ", START_TIMEOUT_MS, NULL, NULL));
}

static void a_child_that_writes_without_end_cannot_hold_waits_or_stop(void** state)
{
  (void)state;
  const char* const arguments[] = { AZ_IOT_E2E_PROCESS_TEST_CHILD, "spew", NULL };
  assert_true(az_iot_e2e_process_start(&s_process, arguments, NULL, NULL)); // Too much to echo.
  assert_true(
      az_iot_e2e_process_wait_for_output(&s_process, "ready", START_TIMEOUT_MS, NULL, NULL));
  time_t const start = time(NULL);
  assert_false(az_iot_e2e_process_wait_for_output(&s_process, "never printed", 200, NULL, NULL));
  assert_false(az_iot_e2e_process_wait_for_exit(&s_process, 200, NULL));
  (void)az_iot_e2e_process_stop(&s_process, 300);
  assert_true(time(NULL) - start < 10);
}

static void extra_environment_is_set_for_the_child_only(void** state)
{
  (void)state;
  const char* const environment[] = { ENVIRONMENT_NAME "=value with spaces", NULL };
  start_child("environment", ENVIRONMENT_NAME, environment);
  assert_true(az_iot_e2e_process_wait_for_exit(&s_process, START_TIMEOUT_MS, NULL));
  assert_true(az_iot_e2e_process_has_printed(&s_process, ENVIRONMENT_NAME "=value with spaces"));
  char* parent_value = az_iot_test_env_dup(ENVIRONMENT_NAME);
  assert_null(parent_value);
}

static void a_shell_command_runs(void** state)
{
  (void)state;
  char command[1024];
#ifdef _WIN32
  (void)snprintf(command, sizeof(command), "\"%s\" exit 4", AZ_IOT_E2E_PROCESS_TEST_CHILD);
  for (char* c = command; *c != '\0'; ++c)
  {
    *c = *c == '/' ? '\\' : *c;
  }
#else
  (void)snprintf(command, sizeof(command), "'%s' exit 4", AZ_IOT_E2E_PROCESS_TEST_CHILD);
#endif
  assert_true(az_iot_e2e_process_start_shell(&s_process, command, NULL, "[child] "));
  int exit_code = -1;
  assert_true(az_iot_e2e_process_wait_for_exit(&s_process, START_TIMEOUT_MS, &exit_code));
  assert_int_equal(exit_code, 4);
}

static void waiting_pumps_until_printed_or_timed_out_then_stop_asks_first(void** state)
{
  (void)state;
  start_child("wait", NULL, NULL);
  int pumps = 0;
  assert_true(az_iot_e2e_process_wait_for_output(
      &s_process, "ready", START_TIMEOUT_MS, count_pump, &pumps));

  pumps = 0;
  time_t const start = time(NULL);
  assert_false(
      az_iot_e2e_process_wait_for_output(&s_process, "never printed", 200, count_pump, &pumps));
  assert_true(pumps > 0);
  assert_true(pumps <= 200 / 10 + 1); // Paced: one tick of at least 10 ms per call.
  assert_true(time(NULL) - start < 5);
  assert_false(az_iot_e2e_process_wait_for_exit(&s_process, 100, NULL));

  assert_true(az_iot_e2e_process_stop(&s_process, START_TIMEOUT_MS));
  assert_true(az_iot_e2e_process_has_printed(&s_process, "stopping gracefully"));
  int exit_code = -1;
  assert_true(az_iot_e2e_process_wait_for_exit(&s_process, 0, &exit_code));
  assert_int_equal(exit_code, 3);
}

static void stop_kills_a_child_that_ignores_the_request(void** state)
{
  (void)state;
  start_child("ignore-stop", NULL, NULL);
  assert_true(
      az_iot_e2e_process_wait_for_output(&s_process, "ready", START_TIMEOUT_MS, NULL, NULL));
  assert_false(az_iot_e2e_process_stop(&s_process, 300));
  assert_false(az_iot_e2e_process_has_printed(&s_process, "stopping gracefully"));
  int exit_code = 0;
  assert_true(az_iot_e2e_process_wait_for_exit(&s_process, 0, &exit_code));
#ifdef _WIN32
  assert_int_equal(exit_code, 1);
#else
  assert_int_equal(exit_code, 128 + 9); // SIGKILL
#endif
}

#ifndef _WIN32
/** @brief With stdout and stderr closed, pipe() returns 1 and 2: the child must still be captured.
 */
static void output_is_captured_when_our_stdout_and_stderr_are_closed(void** state)
{
  (void)state;
  pid_t const pid = fork(); // Closing them here would silence cmocka.
  assert_true(pid >= 0);
  if (pid == 0)
  {
    (void)close(STDOUT_FILENO);
    (void)close(STDERR_FILENO);
    const char* const arguments[] = { AZ_IOT_E2E_PROCESS_TEST_CHILD, "print", NULL };
    bool const captured = az_iot_e2e_process_start(&s_process, arguments, NULL, NULL)
        && az_iot_e2e_process_wait_for_exit(&s_process, START_TIMEOUT_MS, NULL)
        && az_iot_e2e_process_has_printed(&s_process, "last line")
        && az_iot_e2e_process_has_printed(&s_process, "line on stderr");
    (void)az_iot_e2e_process_stop(&s_process, 0);
    _exit(captured ? 0 : 1);
  }
  int status = 0;
  assert_int_equal(waitpid(pid, &status, 0), pid);
  assert_true(WIFEXITED(status));
  assert_int_equal(WEXITSTATUS(status), 0);
}
#endif

static void waiting_ends_when_the_child_exits_without_printing(void** state)
{
  (void)state;
  start_child("exit", "0", NULL);
  time_t const start = time(NULL);
  assert_false(
      az_iot_e2e_process_wait_for_output(&s_process, "never printed", 60 * 1000, NULL, NULL));
  assert_true(time(NULL) - start < 30);
  assert_true(az_iot_e2e_process_stop(&s_process, 0)); // It ended by itself: nothing was forced.
}

static void stop_ends_what_an_exited_shell_left_running(void** state)
{
  (void)state;
  char child[512];
  (void)snprintf(child, sizeof(child), "%s", AZ_IOT_E2E_PROCESS_TEST_CHILD);
  char command[1024];
#ifdef _WIN32
  for (char* c = child; *c != '\0'; ++c)
  {
    *c = *c == '/' ? '\\' : *c;
  }
  (void)snprintf(command, sizeof(command), "start \"\" /b \"%s\" wait", child);
#else
  (void)snprintf(command, sizeof(command), "'%s' wait &", child);
#endif
  assert_true(az_iot_e2e_process_start_shell(&s_process, command, NULL, "[child] "));
  assert_true(
      az_iot_e2e_process_wait_for_output(&s_process, "ready", START_TIMEOUT_MS, NULL, NULL));
  int exit_code = -1;
  assert_true(az_iot_e2e_process_wait_for_exit(&s_process, START_TIMEOUT_MS, &exit_code));
  assert_int_equal(exit_code, 0); // The shell is gone; its child still holds the output.

  time_t const start = time(NULL);
#ifdef _WIN32
  // CTRL_BREAK may not reach a child `start /b` began; the job kill ends it either way.
  (void)az_iot_e2e_process_stop(&s_process, 300);
#else
  assert_true(az_iot_e2e_process_stop(&s_process, START_TIMEOUT_MS));
  assert_true(az_iot_e2e_process_has_printed(&s_process, "stopping gracefully"));
#endif
  assert_true(time(NULL) - start < 30);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(exit_codes_are_reported, teardown),
    cmocka_unit_test_teardown(stdout_stderr_and_arguments_are_captured, teardown),
    cmocka_unit_test_teardown(a_missing_program_does_not_start, teardown),
    cmocka_unit_test_teardown(the_log_keeps_the_newest_output, teardown),
    cmocka_unit_test_teardown(waiting_finds_a_line_the_same_read_drops_from_the_log, teardown),
    cmocka_unit_test_teardown(a_child_that_writes_without_end_cannot_hold_waits_or_stop, teardown),
    cmocka_unit_test_teardown(extra_environment_is_set_for_the_child_only, teardown),
    cmocka_unit_test_teardown(a_shell_command_runs, teardown),
    cmocka_unit_test_teardown(
        waiting_pumps_until_printed_or_timed_out_then_stop_asks_first, teardown),
    cmocka_unit_test_teardown(stop_kills_a_child_that_ignores_the_request, teardown),
    cmocka_unit_test_teardown(waiting_ends_when_the_child_exits_without_printing, teardown),
    cmocka_unit_test_teardown(stop_ends_what_an_exited_shell_left_running, teardown),
#ifndef _WIN32
    cmocka_unit_test_teardown(output_is_captured_when_our_stdout_and_stderr_are_closed, teardown),
#endif
  };
  return cmocka_run_group_tests_name("e2e_process", tests, NULL, NULL);
}
