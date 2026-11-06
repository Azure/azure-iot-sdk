// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "support/e2e_process.h"
#include "support/test_env.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define TICK_MS 10

#ifdef _WIN32

static int64_t now_ms(void) { return (int64_t)GetTickCount64(); }

static void sleep_ms(int milliseconds) { Sleep((DWORD)milliseconds); }

/** @brief UTF-16 copy of @p text, or NULL; free() it. */
static wchar_t* to_wide(const char* text)
{
  int const length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  wchar_t* wide = length > 0 ? malloc((size_t)length * sizeof(wchar_t)) : NULL;
  if (wide != NULL && MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length) != length)
  {
    free(wide);
    wide = NULL;
  }
  return wide;
}

/** @brief Appends @p argument to @p out, quoted as CommandLineToArgvW and the C runtime parse it.
 */
static char* append_quoted_argument(char* out, const char* argument)
{
  *out++ = '"';
  for (const char* c = argument;; ++c)
  {
    size_t backslashes = 0;
    while (*c == '\\')
    {
      ++backslashes;
      ++c;
    }
    // Backslashes are literal unless a quote follows, here or as the closing one.
    size_t const repeat = *c == '\0' || *c == '"' ? backslashes * 2 : backslashes;
    memset(out, '\\', repeat);
    out += repeat;
    if (*c == '\0')
    {
      break;
    }
    if (*c == '"')
    {
      *out++ = '\\';
    }
    *out++ = *c;
  }
  *out++ = '"';
  return out;
}

/** @brief Length of the name of the "NAME=value" entry @p entry ("=C:=C:\\" is named "=C:"). */
static size_t environment_name_length(const wchar_t* entry)
{
  const wchar_t* equals = wcschr(entry + 1, L'=');
  return equals != NULL ? (size_t)(equals - entry) : wcslen(entry);
}

/** @brief The parent's environment block with @p overrides set, or NULL; free() it. */
static wchar_t* make_environment_block(const char* const* overrides)
{
  size_t count = 0;
  while (overrides[count] != NULL)
  {
    ++count;
  }
  wchar_t** wide_overrides = calloc(count + 1, sizeof(wchar_t*));
  wchar_t* parent = GetEnvironmentStringsW();
  wchar_t* block = NULL;
  size_t size = 1;
  bool ok = wide_overrides != NULL && parent != NULL;
  for (size_t i = 0; ok && i < count; ++i)
  {
    wide_overrides[i] = to_wide(overrides[i]);
    ok = wide_overrides[i] != NULL && wcschr(wide_overrides[i], L'=') != NULL;
    size += ok ? wcslen(wide_overrides[i]) + 1 : 0;
  }
  for (const wchar_t* entry = parent; ok && *entry != L'\0'; entry += wcslen(entry) + 1)
  {
    size += wcslen(entry) + 1;
  }
  block = ok ? malloc(size * sizeof(wchar_t)) : NULL;
  if (block != NULL)
  {
    wchar_t* out = block;
    for (const wchar_t* entry = parent; *entry != L'\0'; entry += wcslen(entry) + 1)
    {
      size_t const name_length = environment_name_length(entry);
      bool overridden = false;
      for (size_t i = 0; i < count && !overridden; ++i)
      {
        overridden = environment_name_length(wide_overrides[i]) == name_length
            && _wcsnicmp(entry, wide_overrides[i], name_length) == 0;
      }
      if (!overridden)
      {
        size_t const length = wcslen(entry) + 1;
        memcpy(out, entry, length * sizeof(wchar_t));
        out += length;
      }
    }
    for (size_t i = 0; i < count; ++i)
    {
      size_t const length = wcslen(wide_overrides[i]) + 1;
      memcpy(out, wide_overrides[i], length * sizeof(wchar_t));
      out += length;
    }
    *out = L'\0';
  }
  if (parent != NULL)
  {
    (void)FreeEnvironmentStringsW(parent);
  }
  for (size_t i = 0; wide_overrides != NULL && i < count; ++i)
  {
    free(wide_overrides[i]);
  }
  free(wide_overrides);
  return block;
}

/**
 * @brief Gives this process a console when it has none: CTRL_BREAK_EVENT reaches only processes
 * that share the caller's console, and the child inherits this one.
 */
static void attach_console_if_none(void)
{
  DWORD process_ids[1];
  if (GetConsoleProcessList(process_ids, 1) != 0)
  {
    return;
  }
  DWORD const kinds[] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
  HANDLE kept[3];
  for (int i = 0; i < 3; ++i)
  {
    kept[i] = GetStdHandle(kinds[i]);
  }
  if (AllocConsole())
  {
    // AllocConsole() points the standard handles at the new console: keep any that were set.
    for (int i = 0; i < 3; ++i)
    {
      if (kept[i] != NULL && kept[i] != INVALID_HANDLE_VALUE)
      {
        (void)SetStdHandle(kinds[i], kept[i]);
      }
    }
  }
}

/** @brief Starts @p command_line (UTF-8) in a new process group and job, output piped to us. */
static bool start_command_line(
    az_iot_e2e_process* process,
    const char* command_line,
    const char* const* environment)
{
  attach_console_if_none();
  SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
  HANDLE output_read = NULL;
  HANDLE output_write = NULL;
  if (!CreatePipe(&output_read, &output_write, &inherit, 0))
  {
    return false;
  }
  (void)SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
  HANDLE input = CreateFileW(
      L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
  wchar_t* wide_command_line = to_wide(command_line);
  wchar_t* block = environment != NULL ? make_environment_block(environment) : NULL;

  STARTUPINFOW startup = { 0 };
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input;
  startup.hStdOutput = output_write;
  startup.hStdError = output_write;
  PROCESS_INFORMATION created = { 0 };
  bool const started = input != INVALID_HANDLE_VALUE && wide_command_line != NULL
      && (environment == NULL || block != NULL)
      && CreateProcessW(NULL,
                        wide_command_line,
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        block,
                        NULL,
                        &startup,
                        &created);
  free(block);
  free(wide_command_line);
  if (input != INVALID_HANDLE_VALUE)
  {
    (void)CloseHandle(input);
  }
  (void)CloseHandle(output_write);
  if (!started)
  {
    (void)CloseHandle(output_read);
    return false;
  }

  // In a job, so a forced stop also ends what the child started, as a POSIX process group.
  HANDLE job = CreateJobObjectW(NULL, NULL);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = { 0 };
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (job != NULL
      && (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))
          || !AssignProcessToJobObject(job, created.hProcess)))
  {
    (void)CloseHandle(job);
    job = NULL;
  }
  (void)ResumeThread(created.hThread);
  (void)CloseHandle(created.hThread);
  process->process_handle = created.hProcess;
  process->job_handle = job;
  process->output_handle = output_read;
  process->process_id = created.dwProcessId;
  return true;
}

/** @brief Reads available output into @p buffer: the byte count, 0 for none, -1 at its end. */
static long read_available(az_iot_e2e_process* process, char* buffer, size_t size)
{
  DWORD available = 0;
  if (!PeekNamedPipe(process->output_handle, NULL, 0, NULL, &available, NULL))
  {
    return -1;
  }
  if (available == 0)
  {
    return 0;
  }
  DWORD bytes_read = 0;
  DWORD const wanted = available < size ? available : (DWORD)size;
  return ReadFile(process->output_handle, buffer, wanted, &bytes_read, NULL) ? (long)bytes_read
                                                                             : -1;
}

/** @brief Whether the child exited, recording its exit code; does not block. */
static bool poll_exit(az_iot_e2e_process* process)
{
  DWORD code = 0;
  if (!process->exited && WaitForSingleObject(process->process_handle, 0) == WAIT_OBJECT_0
      && GetExitCodeProcess(process->process_handle, &code))
  {
    process->exit_code = (int)code;
    process->exited = true;
  }
  return process->exited;
}

/** @brief Asks the child's process group to stop; false when it could not be asked. */
static bool ask_to_stop(az_iot_e2e_process* process)
{
  return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process->process_id) != 0;
}

/** @brief Kills the child and what it started, and waits for the child. */
static void kill_and_wait(az_iot_e2e_process* process)
{
  if (process->job_handle != NULL)
  {
    (void)TerminateJobObject(process->job_handle, 1);
  }
  if (!process->exited)
  {
    (void)TerminateProcess(process->process_handle, 1);
    (void)WaitForSingleObject(process->process_handle, INFINITE);
    (void)poll_exit(process);
  }
}

/** @brief Closes the child's handles. */
static void release(az_iot_e2e_process* process)
{
  (void)CloseHandle(process->output_handle);
  (void)CloseHandle(process->process_handle);
  if (process->job_handle != NULL)
  {
    (void)CloseHandle(process->job_handle);
  }
}

#else /* POSIX */

static int64_t now_ms(void)
{
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int milliseconds)
{
  struct timespec t = { milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L };
  (void)nanosleep(&t, NULL);
}

/** @brief Sets @p environment ("NAME=value" strings) in this process; false on a malformed one. */
static bool set_environment(const char* const* environment)
{
  for (; environment != NULL && *environment != NULL; ++environment)
  {
    const char* equals = strchr(*environment, '=');
    char name[256];
    size_t const length = equals != NULL ? (size_t)(equals - *environment) : 0;
    if (length == 0 || length >= sizeof(name))
    {
      return false;
    }
    memcpy(name, *environment, length);
    name[length] = '\0';
    if (setenv(name, equals + 1, 1) != 0)
    {
      return false;
    }
  }
  return true;
}

/** @brief Closes both ends of @p pipe_fds. */
static void close_pipe(int const pipe_fds[2])
{
  (void)close(pipe_fds[0]);
  (void)close(pipe_fds[1]);
}

/**
 * @brief Starts @p arguments in its own process group, output piped to us; false, with errno set,
 * when it could not be run.
 */
static bool start_arguments(
    az_iot_e2e_process* process,
    const char* const* arguments,
    const char* const* environment)
{
  int output[2];
  int exec_status[2]; // Closed by a successful exec; otherwise carries the child's errno.
  if (pipe(output) != 0)
  {
    return false;
  }
  if (pipe(exec_status) != 0)
  {
    close_pipe(output);
    return false;
  }
  if (fcntl(exec_status[1], F_SETFD, FD_CLOEXEC) != 0 || fcntl(output[0], F_SETFD, FD_CLOEXEC) != 0)
  {
    close_pipe(output);
    close_pipe(exec_status);
    return false;
  }
  pid_t const pid = fork();
  if (pid == 0)
  {
    (void)setpgid(0, 0);
    // Above stderr: with our stdout or stderr closed, pipe() may have returned 1 or 2, which the
    // redirection below would overwrite.
    int const writer = fcntl(output[1], F_DUPFD, STDERR_FILENO + 1);
    int const status = fcntl(exec_status[1], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    int error = errno;
    if (writer >= 0 && status >= 0)
    {
      close_pipe(output);
      close_pipe(exec_status);
      (void)dup2(writer, STDOUT_FILENO);
      (void)dup2(writer, STDERR_FILENO);
      (void)close(writer);
      errno = EINVAL; // A malformed environment entry.
      if (set_environment(environment))
      {
        execvp(arguments[0], (char* const*)(uintptr_t)arguments);
      }
      error = errno;
    }
    ssize_t const written = write(status >= 0 ? status : exec_status[1], &error, sizeof(error));
    (void)written;
    _exit(127);
  }
  (void)close(output[1]);
  (void)close(exec_status[1]);
  if (pid < 0)
  {
    (void)close(output[0]);
    (void)close(exec_status[0]);
    return false;
  }
  (void)setpgid(pid, pid); // Also here: a stop right after the start must reach the group.
  int child_error = 0;
  ssize_t n;
  do
  {
    n = read(exec_status[0], &child_error, sizeof(child_error));
  } while (n < 0 && errno == EINTR);
  (void)close(exec_status[0]);
  if (n != 0 || fcntl(output[0], F_SETFL, O_NONBLOCK) != 0)
  {
    int const error = n > 0 ? child_error : errno;
    (void)kill(-pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    (void)close(output[0]);
    errno = error;
    return false;
  }
  process->process_id = pid;
  process->output_descriptor = output[0];
  return true;
}

/** @brief Reads available output into @p buffer: the byte count, 0 for none, -1 at its end. */
static long read_available(az_iot_e2e_process* process, char* buffer, size_t size)
{
  ssize_t const n = read(process->output_descriptor, buffer, size);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
  {
    return 0;
  }
  return n > 0 ? (long)n : -1;
}

/** @brief Records the wait status @p status. */
static void record_exit(az_iot_e2e_process* process, int status)
{
  process->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
      : WIFSIGNALED(status)              ? 128 + WTERMSIG(status)
                                         : -1;
  process->exited = true;
}

/** @brief Whether the child exited, recording its exit code; does not block. */
static bool poll_exit(az_iot_e2e_process* process)
{
  int status = 0;
  if (!process->exited && waitpid(process->process_id, &status, WNOHANG) == process->process_id)
  {
    record_exit(process, status);
  }
  return process->exited;
}

/** @brief Asks the child's process group to stop; false when it could not be asked. */
static bool ask_to_stop(az_iot_e2e_process* process)
{
  return kill(-process->process_id, SIGTERM) == 0;
}

/** @brief Kills the child's process group, and waits for the child. */
static void kill_and_wait(az_iot_e2e_process* process)
{
  (void)kill(-process->process_id, SIGKILL);
  int status = 0;
  if (!process->exited && waitpid(process->process_id, &status, 0) == process->process_id)
  {
    record_exit(process, status);
  }
}

/** @brief Closes the read end of the pipe. */
static void release(az_iot_e2e_process* process) { (void)close(process->output_descriptor); }

#endif

/** @brief Clears @p process's state, the log included, for a new child. */
static void reset(az_iot_e2e_process* process, const char* echo_prefix)
{
  process->running = false;
  process->exited = false;
  process->output_ended = false;
  process->exit_code = -1;
  process->echo_prefix = echo_prefix;
  process->log[0] = '\0';
  process->log_length = 0;
  process->echoed_length = 0;
}

bool az_iot_e2e_process_start(
    az_iot_e2e_process* process,
    const char* const* arguments,
    const char* const* environment,
    const char* echo_prefix)
{
  reset(process, echo_prefix);
  if (arguments == NULL || arguments[0] == NULL)
  {
    return false;
  }
#ifdef _WIN32
  size_t size = 1;
  for (const char* const* argument = arguments; *argument != NULL; ++argument)
  {
    size += strlen(*argument) * 2 + 3; // Each character escaped, two quotes, a space.
  }
  char* command_line = malloc(size);
  if (command_line == NULL)
  {
    return false;
  }
  char* end = command_line;
  for (const char* const* argument = arguments; *argument != NULL; ++argument)
  {
    end = append_quoted_argument(end, *argument);
    *end++ = ' ';
  }
  end[-1] = '\0';
  process->running = start_command_line(process, command_line, environment);
  free(command_line);
#else
  process->running = start_arguments(process, arguments, environment);
#endif
  return process->running;
}

bool az_iot_e2e_process_start_shell(
    az_iot_e2e_process* process,
    const char* command,
    const char* const* environment,
    const char* echo_prefix)
{
#ifdef _WIN32
  reset(process, echo_prefix);
  // `/s` with the whole command quoted: cmd.exe strips the outer quotes and runs the rest as is.
  char* shell = az_iot_test_env_dup("ComSpec");
  const char* shell_path = shell != NULL ? shell : "cmd.exe";
  size_t const size = strlen(shell_path) + strlen(command) + 16;
  char* command_line = malloc(size);
  if (command_line != NULL)
  {
    (void)snprintf(command_line, size, "\"%s\" /d /s /c \"%s\"", shell_path, command);
    process->running = start_command_line(process, command_line, environment);
  }
  free(command_line);
  free(shell);
  return process->running;
#else
  const char* const arguments[] = { "/bin/sh", "-c", command, NULL };
  return az_iot_e2e_process_start(process, arguments, environment, echo_prefix);
#endif
}

/** @brief Echoes, prefixed, the whole lines of the log not echoed yet. */
static void echo_complete_lines(az_iot_e2e_process* process)
{
  char* line_end;
  while (process->echo_prefix != NULL
         && (line_end = memchr(
                 process->log + process->echoed_length,
                 '\n',
                 process->log_length - process->echoed_length))
             != NULL)
  {
    const char* line = process->log + process->echoed_length;
    int length = (int)(line_end - line);
    if (length > 0 && line[length - 1] == '\r')
    {
      --length;
    }
    printf("%s%.*s\n", process->echo_prefix, length, line);
    process->echoed_length = (size_t)(line_end - process->log) + 1;
  }
}

/**
 * @brief Reads what the child printed, without blocking, and echoes its whole lines.
 *
 * @return Whether @p text (may be NULL) was in the log at some point of this read, including in
 * output it dropped to stay bounded.
 */
static bool read_output_finding(az_iot_e2e_process* process, const char* text)
{
  bool found = false;
  if (!process->running)
  {
    return false;
  }
  while (!process->output_ended)
  {
    if (process->log_length + 1 >= sizeof(process->log)) // Full: keep the newer half.
    {
      process->log[process->log_length] = '\0';
      found = found || (text != NULL && strstr(process->log, text) != NULL);
      echo_complete_lines(process);
      size_t const keep = sizeof(process->log) / 2;
      size_t const dropped = process->log_length - keep;
      memmove(process->log, process->log + dropped, keep);
      process->echoed_length
          = process->echoed_length > dropped ? process->echoed_length - dropped : 0;
      process->log_length = keep;
    }
    long const n = read_available(
        process,
        process->log + process->log_length,
        sizeof(process->log) - 1 - process->log_length);
    if (n == 0)
    {
      break;
    }
    if (n < 0)
    {
      process->output_ended = true;
    }
    else
    {
      process->log_length += (size_t)n;
    }
  }
  process->log[process->log_length] = '\0';
  echo_complete_lines(process);
  (void)fflush(stdout);
  return found || (text != NULL && strstr(process->log, text) != NULL);
}

void az_iot_e2e_process_read_output(az_iot_e2e_process* process)
{
  (void)read_output_finding(process, NULL);
}

bool az_iot_e2e_process_has_printed(const az_iot_e2e_process* process, const char* text)
{
  return strstr(process->log, text) != NULL;
}

bool az_iot_e2e_process_wait_for_output(
    az_iot_e2e_process* process,
    const char* text,
    int timeout_ms,
    az_iot_e2e_process_pump pump,
    void* context)
{
  int64_t const end = now_ms() + timeout_ms;
  for (;;)
  {
    if (read_output_finding(process, text))
    {
      return true;
    }
    if (now_ms() >= end || !process->running || (process->output_ended && poll_exit(process)))
    {
      return false;
    }
    // At most one read per tick, whatever the pump takes.
    int64_t const tick_end = now_ms() + TICK_MS;
    if (pump != NULL)
    {
      pump(context);
    }
    int64_t const left = tick_end - now_ms();
    if (left > 0)
    {
      sleep_ms((int)left);
    }
  }
}

bool az_iot_e2e_process_wait_for_exit(az_iot_e2e_process* process, int timeout_ms, int* exit_code)
{
  int64_t const end = now_ms() + timeout_ms;
  while (process->running && !poll_exit(process) && now_ms() < end)
  {
    az_iot_e2e_process_read_output(process);
    sleep_ms(TICK_MS);
  }
  az_iot_e2e_process_read_output(process); // What it printed before it exited is in the pipe.
  if (exit_code != NULL)
  {
    *exit_code = process->exit_code;
  }
  return process->exited;
}

/**
 * @brief Whether the child exited and its output closed (what it started holds the pipe too),
 * from fresh state: its exit first, then its output to the end.
 */
static bool has_finished(az_iot_e2e_process* process)
{
  bool const exited = poll_exit(process);
  az_iot_e2e_process_read_output(process);
  return exited && process->output_ended;
}

bool az_iot_e2e_process_stop(az_iot_e2e_process* process, int grace_ms)
{
  if (!process->running)
  {
    return true;
  }
  if (!has_finished(process))
  {
    // Not asked (e.g. the group just ended): no grace, but the state is checked again below.
    int64_t const end = now_ms() + (ask_to_stop(process) ? grace_ms : 0);
    while (now_ms() < end && !has_finished(process))
    {
      sleep_ms(TICK_MS);
    }
  }
  bool const graceful = has_finished(process);
  kill_and_wait(process); // Also what the child started and left running.
  az_iot_e2e_process_read_output(process);
  release(process);
  process->running = false;
  return graceful;
}
