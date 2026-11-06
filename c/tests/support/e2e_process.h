// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* A child process for tests that drive a program (a sample, or firmware in an emulator) and read
 * what it prints. POSIX and Windows. */
#ifndef AZ_IOT_E2E_PROCESS_H
#define AZ_IOT_E2E_PROCESS_H

#include <stdbool.h>
#include <stddef.h>

/** @brief Bytes of the child's most recent output kept for az_iot_e2e_process_has_printed(). */
#define AZ_IOT_E2E_PROCESS_LOG_SIZE (128 * 1024)

/**
 * @brief A child process, its stdout and stderr captured through one pipe. Large: keep it static.
 * Fields are private.
 */
typedef struct az_iot_e2e_process
{
#ifdef _WIN32
  void* process_handle;
  void* job_handle; /* the process and its descendants; NULL when unavailable */
  void* output_handle; /* read end of the pipe */
  unsigned long process_id;
#else
  int process_id; /* also the process group id */
  int output_descriptor; /* read end of the pipe, non-blocking */
#endif
  bool running; /* started and not yet stopped */
  bool exited;
  bool output_ended; /* every writer closed the pipe */
  int exit_code;
  const char* echo_prefix;
  char log[AZ_IOT_E2E_PROCESS_LOG_SIZE]; /* NUL-terminated */
  size_t log_length;
  size_t echoed_length; /* log bytes already echoed */
} az_iot_e2e_process;

/** @brief Called by az_iot_e2e_process_wait_for_output() each tick, with its @p context. */
typedef void (*az_iot_e2e_process_pump)(void* context);

/**
 * @brief Starts @p arguments[0] (searched in PATH) with @p arguments (NULL-terminated), in a new
 * process group. @p process must not be running.
 *
 * @param environment NULL, or "NAME=value" strings (NULL-terminated) set for the child only.
 * @param echo_prefix Printed before each whole line the child prints, echoed to stdout. Not copied.
 * @return false when it could not be started.
 */
bool az_iot_e2e_process_start(
    az_iot_e2e_process* process,
    const char* const* arguments,
    const char* const* environment,
    const char* echo_prefix);

/**
 * @brief az_iot_e2e_process_start() for @p command run by the shell: `/bin/sh -c`, or on Windows
 * `%ComSpec% /d /s /c`.
 */
bool az_iot_e2e_process_start_shell(
    az_iot_e2e_process* process,
    const char* command,
    const char* const* environment,
    const char* echo_prefix);

/** @brief Reads what the child printed, without blocking, and echoes its whole lines. */
void az_iot_e2e_process_read_output(az_iot_e2e_process* process);

/** @brief Whether the kept output contains @p text. */
bool az_iot_e2e_process_has_printed(const az_iot_e2e_process* process, const char* text);

/**
 * @brief Reads output until it contains @p text, calling @p pump (may be NULL) each tick.
 *
 * @return false after @p timeout_ms, or once the child exited and closed its output without
 * printing @p text.
 */
bool az_iot_e2e_process_wait_for_output(
    az_iot_e2e_process* process,
    const char* text,
    int timeout_ms,
    az_iot_e2e_process_pump pump,
    void* context);

/**
 * @brief Reads output until the child exits, for at most @p timeout_ms.
 *
 * @param[out] exit_code Its exit code; 128 + the signal when a signal ended it (POSIX). May be
 * NULL.
 * @return false when it is still running.
 */
bool az_iot_e2e_process_wait_for_exit(az_iot_e2e_process* process, int timeout_ms, int* exit_code);

/**
 * @brief Stops the child's process group and releases it; nothing when not running.
 *
 * Asks first (SIGTERM; CTRL_BREAK_EVENT on Windows), then kills the group after @p grace_ms.
 * Its exit code stays available to az_iot_e2e_process_wait_for_exit().
 *
 * @return false when it had to be killed.
 */
bool az_iot_e2e_process_stop(az_iot_e2e_process* process, int grace_ms);

#endif /* AZ_IOT_E2E_PROCESS_H */
