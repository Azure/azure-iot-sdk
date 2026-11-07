// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The child az_iot_tests_e2e_process starts. Modes (argv[1]):
 *   exit <code>       exits with <code>
 *   print [args...]   prints two lines, one on stderr, and its arguments; exits 0
 *   environment NAME  prints NAME=<its value, or "(unset)">; exits 0
 *   flood <count>     prints, in one write, <count> 100-byte lines, "line <n>" (5 digits) then
 *                     padding; exits 0
 *   wait              prints "ready", then on SIGTERM (CTRL_BREAK on Windows) prints "stopping
 *                     gracefully" and exits 3
 *   ignore-stop       prints "ready" and ignores SIGTERM (CTRL_BREAK) until killed */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // F_SETPIPE_SZ
#elif !defined(__linux__) && !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

#include "support/test_env.h"

static volatile sig_atomic_t s_stop_requested;

#ifdef _WIN32
static BOOL WINAPI on_console_event(DWORD event)
{
  if (event == CTRL_BREAK_EVENT)
  {
    s_stop_requested = 1;
    return TRUE;
  }
  return FALSE;
}

static BOOL WINAPI ignore_console_event(DWORD event) { return event == CTRL_BREAK_EVENT; }

static void sleep_briefly(void) { Sleep(10); }
#else
static void on_terminate(int signal_number)
{
  (void)signal_number;
  s_stop_requested = 1;
}

static void sleep_briefly(void)
{
  struct timespec t = { 0, 10 * 1000 * 1000 };
  (void)nanosleep(&t, NULL);
}
#endif

int main(int argc, char** argv)
{
  (void)setvbuf(stdout, NULL, _IONBF, 0);
  const char* mode = argc > 1 ? argv[1] : "";
  if (strcmp(mode, "exit") == 0 && argc > 2)
  {
    return atoi(argv[2]);
  }
  if (strcmp(mode, "print") == 0)
  {
    printf("first line\n");
    fprintf(stderr, "line on stderr\n");
    for (int i = 2; i < argc; ++i)
    {
      printf("argument: <%s>\n", argv[i]);
    }
    printf("last line\n");
    return 0;
  }
  if (strcmp(mode, "environment") == 0 && argc > 2)
  {
    char* value = az_iot_test_env_dup(argv[2]);
    printf("%s=%s\n", argv[2], value != NULL ? value : "(unset)");
    free(value);
    return 0;
  }
  if (strcmp(mode, "flood") == 0 && argc > 2)
  {
    // One write, so the reader gets it as one burst; on Linux, all of it waits in the pipe.
#ifdef F_SETPIPE_SZ
    (void)fcntl(STDOUT_FILENO, F_SETPIPE_SZ, 1024 * 1024);
#endif
    int const count = atoi(argv[2]);
    char* lines = malloc((size_t)count * 100 + 1);
    if (lines == NULL)
    {
      return 2;
    }
    for (int i = 0; i < count; ++i)
    {
      char line[128];
      (void)snprintf(line, sizeof(line), "line %05d %088d\n", i % 100000, 0);
      memcpy(lines + (size_t)i * 100, line, 100);
    }
    (void)fwrite(lines, 100, (size_t)count, stdout);
    free(lines);
    return 0;
  }
  if (strcmp(mode, "wait") == 0 || strcmp(mode, "ignore-stop") == 0)
  {
    int const ignore = strcmp(mode, "ignore-stop") == 0;
#ifdef _WIN32
    (void)SetConsoleCtrlHandler(ignore ? ignore_console_event : on_console_event, TRUE);
#else
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = ignore ? SIG_IGN : on_terminate;
    (void)sigaction(SIGTERM, &action, NULL);
#endif
    printf("ready\n");
    while (!s_stop_requested)
    {
      sleep_briefly();
    }
    printf("stopping gracefully\n");
    return 3;
  }
  fprintf(stderr, "unknown mode: %s\n", mode);
  return 2;
}
