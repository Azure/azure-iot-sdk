// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The ESP32 IoT Hub sample (samples/unified/esp32) in QEMU, against the e2e hub.
 *
 * E2E_ESP32_QEMU_COMMAND starts the firmware with its UART on stdout (eng/esp-idf-qemu.sh --run);
 * E2E_ESP32_DEVICE_ID is the id it provisions as. The firmware is the device: this test plays the
 * service through az_iot_e2e_service and reads the sample's stable "hub_esp32:" log lines. */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "az_iot_e2e_service.h"

#define E2E_READY_TIMEOUT_S 420 /* boot, DHCP, DPS and hub, TLS emulated */
#define E2E_TELEMETRY_TIMEOUT_S 180
#define E2E_REQUEST_TIMEOUT_S 120
#define E2E_PUMP_MS 50

/** @brief The firmware's QEMU process and what it has printed. */
typedef struct
{
  pid_t pid;
  int output; /* read end of its stdout and stderr, non-blocking */
  char log[128 * 1024]; /* most recent output, NUL-terminated */
  size_t log_length;
  size_t echoed; /* log bytes already echoed to stdout */
} firmware_process;

static az_iot_e2e_service* s_service;
static firmware_process s_firmware;
static char s_device_id[129];

/** @brief Starts @p command through /bin/sh in its own process group, output piped to us. */
static bool firmware_start(firmware_process* f, const char* command)
{
  int fds[2];
  if (pipe(fds) != 0)
  {
    return false;
  }
  f->pid = fork();
  if (f->pid == 0)
  {
    (void)setpgid(0, 0);
    (void)dup2(fds[1], STDOUT_FILENO);
    (void)dup2(fds[1], STDERR_FILENO);
    (void)close(fds[0]);
    (void)close(fds[1]);
    execl("/bin/sh", "sh", "-c", command, (char*)NULL);
    _exit(127);
  }
  (void)close(fds[1]);
  if (f->pid < 0)
  {
    (void)close(fds[0]);
    return false;
  }
  f->output = fds[0];
  return fcntl(f->output, F_SETFL, O_NONBLOCK) == 0;
}

/** @brief Reads what the firmware printed and echoes whole lines, prefixed, to stdout. */
static void firmware_read(firmware_process* f)
{
  for (;;)
  {
    if (f->log_length + 1 >= sizeof(f->log)) // Full: keep the newer half.
    {
      size_t const keep = sizeof(f->log) / 2;
      memmove(f->log, f->log + f->log_length - keep, keep);
      f->echoed = f->echoed > f->log_length - keep ? f->echoed - (f->log_length - keep) : 0;
      f->log_length = keep;
    }
    ssize_t n = read(f->output, f->log + f->log_length, sizeof(f->log) - 1 - f->log_length);
    if (n <= 0)
    {
      break;
    }
    f->log_length += (size_t)n;
  }
  f->log[f->log_length] = '\0';
  char* line_end;
  while ((line_end = memchr(f->log + f->echoed, '\n', f->log_length - f->echoed)) != NULL)
  {
    printf("[esp32] %.*s\n", (int)(line_end - (f->log + f->echoed)), f->log + f->echoed);
    f->echoed = (size_t)(line_end - f->log) + 1;
  }
  (void)fflush(stdout);
}

/** @brief Whether the firmware printed @p text (in the retained log). */
static bool firmware_printed(const firmware_process* f, const char* text)
{
  return strstr(f->log, text) != NULL;
}

/** @brief Stops the process group (sh, the script and QEMU). */
static void firmware_stop(firmware_process* f)
{
  if (f->pid <= 0)
  {
    return;
  }
  (void)kill(-f->pid, SIGTERM);
  for (int i = 0; i < 50 && waitpid(f->pid, NULL, WNOHANG) == 0; ++i)
  {
    struct timespec t = { 0, 100 * 1000 * 1000 };
    (void)nanosleep(&t, NULL);
  }
  (void)kill(-f->pid, SIGKILL);
  (void)waitpid(f->pid, NULL, 0);
  firmware_read(f);
  (void)close(f->output);
  f->pid = 0;
}

/** @brief One step: the firmware's output, then the service transport. */
static void pump(void)
{
  firmware_read(&s_firmware);
  (void)az_iot_e2e_service_do_work(s_service, E2E_PUMP_MS);
}

/** @brief Pumps until the firmware printed @p text or @p seconds pass. */
static bool wait_for_firmware(const char* text, int seconds)
{
  time_t const end = time(NULL) + seconds;
  while (!firmware_printed(&s_firmware, text) && time(NULL) < end)
  {
    pump();
  }
  return firmware_printed(&s_firmware, text);
}

/** @brief Pumps until the REST request in flight completes; returns 1, or 0 on timeout, -1 on
 * error. */
static int wait_for_request(int* status, char* body, size_t body_size)
{
  time_t const end = time(NULL) + E2E_REQUEST_TIMEOUT_S;
  int rc = 0;
  while (rc == 0 && time(NULL) < end)
  {
    pump();
    rc = az_iot_e2e_service_request_poll(s_service, status, body, body_size);
  }
  return rc;
}

/** @brief Writes "<tag>-<time><random>" to @p out, unique per run. */
static void make_marker(char* out, size_t size, const char* tag)
{
  snprintf(out, size, "%s-%08lx%04x", tag, (unsigned long)time(NULL), (unsigned)rand() & 0xffffu);
}

/** @brief Stops the firmware and releases the service. */
static int group_teardown(void** state)
{
  (void)state;
  firmware_stop(&s_firmware);
  if (s_service != NULL)
  {
    az_iot_e2e_service_telemetry_watch_end(s_service);
    az_iot_e2e_service_destroy(s_service);
    s_service = NULL;
  }
  return 0;
}

/** @brief Group setup failed: cmocka then skips the group teardown, so run it here. */
static int fail_group_setup(void)
{
  (void)group_teardown(NULL);
  return -1;
}

/** @brief Creates the service, starts the telemetry watch, then the firmware, and waits for it to
 * connect. */
static int group_setup(void** state)
{
  (void)state;
  srand((unsigned)time(NULL));
  const char* command = getenv("E2E_ESP32_QEMU_COMMAND");
  const char* device_id = getenv("E2E_ESP32_DEVICE_ID");
  if (command == NULL || command[0] == '\0' || device_id == NULL || device_id[0] == '\0'
      || strlen(device_id) >= sizeof(s_device_id))
  {
    fprintf(stderr, "[e2e] set E2E_ESP32_QEMU_COMMAND and E2E_ESP32_DEVICE_ID\n");
    return -1;
  }
  strcpy(s_device_id, device_id);

  const char* error = NULL;
  s_service = az_iot_e2e_service_create(&error);
  if (s_service == NULL)
  {
    fprintf(stderr, "[e2e] service: %s\n", error ? error : "(unknown)");
    return fail_group_setup();
  }
  char needle[160];
  snprintf(needle, sizeof(needle), "\"deviceId\":\"%s\"", s_device_id);
  if (!az_iot_e2e_service_telemetry_watch_begin(s_service)
      || !az_iot_e2e_service_telemetry_watch_needle(s_service, needle))
  {
    fprintf(stderr, "[e2e] telemetry watch: %s\n", az_iot_e2e_service_last_error(s_service));
    return fail_group_setup();
  }

  printf("[e2e] starting the firmware: %s\n", command);
  if (!firmware_start(&s_firmware, command))
  {
    fprintf(stderr, "[e2e] could not start the firmware: %s\n", strerror(errno));
    return fail_group_setup();
  }
  if (!wait_for_firmware("hub_esp32: ready on ", E2E_READY_TIMEOUT_S))
  {
    fprintf(stderr, "[e2e] the firmware did not connect within %d s\n", E2E_READY_TIMEOUT_S);
    return fail_group_setup();
  }
  return 0;
}

/** @brief The sample sends {"deviceId":"<id>","count":n}; one must reach the Event Hub endpoint. */
static void telemetry_reaches_the_hub(void** state)
{
  (void)state;
  char needle[160];
  snprintf(needle, sizeof(needle), "\"deviceId\":\"%s\"", s_device_id);
  time_t const end = time(NULL) + E2E_TELEMETRY_TIMEOUT_S;
  while (!az_iot_e2e_service_telemetry_needle_seen(s_service, needle) && time(NULL) < end)
  {
    pump();
  }
  assert_true(az_iot_e2e_service_telemetry_needle_seen(s_service, needle));
}

/** @brief "echo" answers 200 with the request payload. */
static void the_echo_method_returns_its_payload(void** state)
{
  (void)state;
  char marker[48];
  make_marker(marker, sizeof(marker), "echo");
  char payload[64];
  snprintf(payload, sizeof(payload), "\"%s\"", marker);
  assert_true(az_iot_e2e_service_method_invoke_begin(s_service, s_device_id, "echo", payload));
  int status = 0;
  char response[1024] = { 0 };
  int const rc = wait_for_request(&status, response, sizeof(response));
  printf("[e2e] echo: rc=%d status=%d body=%s\n", rc, status, response);
  assert_int_equal(rc, 1);
  assert_int_equal(status, 200);
  assert_non_null(strstr(response, marker));
}

/** @brief The desired "$version" in the twin JSON @p twin, or 0. */
static uint64_t desired_version(const char* twin)
{
  const char* desired = strstr(twin, "\"desired\"");
  const char* reported = strstr(twin, "\"reported\"");
  if (desired == NULL)
  {
    return 0;
  }
  const char* version = strstr(desired, "\"$version\":");
  if (version == NULL || (reported != NULL && reported > desired && version > reported))
  {
    return 0;
  }
  return strtoull(version + strlen("\"$version\":"), NULL, 10);
}

/** @brief A desired patch reaches the device, which acknowledges its version in reported. */
static void a_desired_patch_is_acknowledged_in_reported(void** state)
{
  (void)state;
  char marker[48];
  make_marker(marker, sizeof(marker), "twin");
  char desired[96];
  snprintf(desired, sizeof(desired), "{\"e2e\":\"%s\"}", marker);
  assert_true(az_iot_e2e_service_twin_patch_desired_begin(s_service, s_device_id, desired));
  int status = 0;
  static char twin[16384];
  assert_int_equal(wait_for_request(&status, twin, sizeof(twin)), 1);
  assert_int_equal(status, 200);
  uint64_t const version = desired_version(twin);
  printf("[e2e] desired patch: version %" PRIu64 "\n", version);
  assert_true(version > 0);

  char acknowledged[96];
  snprintf(acknowledged, sizeof(acknowledged), "desired version %" PRIu64 " acknowledged", version);
  assert_true(wait_for_firmware(acknowledged, E2E_REQUEST_TIMEOUT_S));

  char reported[64];
  snprintf(reported, sizeof(reported), "\"desiredVersion\":%" PRIu64, version);
  bool seen = false;
  for (int attempt = 0; attempt < 10 && !seen; ++attempt)
  {
    assert_true(az_iot_e2e_service_twin_get_begin(s_service, s_device_id));
    assert_int_equal(wait_for_request(&status, twin, sizeof(twin)), 1);
    seen = status == 200 && strstr(twin, reported) != NULL;
    for (int i = 0; !seen && i < 100; ++i) // About 5 s between reads.
    {
      pump();
    }
  }
  assert_true(seen);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(telemetry_reaches_the_hub),
    cmocka_unit_test(the_echo_method_returns_its_payload),
    cmocka_unit_test(a_desired_patch_is_acknowledged_in_reported),
  };
  return cmocka_run_group_tests_name("esp32_qemu", tests, group_setup, group_teardown);
}
