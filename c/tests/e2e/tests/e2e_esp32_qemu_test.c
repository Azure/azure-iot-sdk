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
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "az_iot_e2e_service.h"
#include "e2e_json.h"
#include "support/e2e_process.h"

#define E2E_READY_TIMEOUT_S 420 /* boot, DHCP, DPS and hub, TLS emulated */
#define E2E_TELEMETRY_TIMEOUT_S 180
#define E2E_REQUEST_TIMEOUT_S 120
#define E2E_WATCH_TIMEOUT_S 90 /* the Event Hub AMQP handshake can fail transiently */
#define E2E_PUMP_MS 50
#define E2E_STOP_GRACE_MS 5000

static az_iot_e2e_service* s_service;
static az_iot_e2e_process s_firmware;
static char s_device_id[129];

/** @brief The service transport, one step. */
static void pump_service(void* context)
{
  (void)context;
  (void)az_iot_e2e_service_do_work(s_service, E2E_PUMP_MS);
}

/** @brief One step: the firmware's output, then the service transport. */
static void pump(void)
{
  az_iot_e2e_process_read_output(&s_firmware);
  pump_service(NULL);
}

/** @brief Pumps until the firmware printed @p text or @p seconds pass. */
static bool wait_for_firmware(const char* text, int seconds)
{
  return az_iot_e2e_process_wait_for_output(&s_firmware, text, seconds * 1000, pump_service, NULL);
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
  (void)az_iot_e2e_process_stop(&s_firmware, E2E_STOP_GRACE_MS);
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
  /* Capture only this device's telemetry: the shared hub replays other runs' messages too. The
   * cold AMQP handshake to the Event Hub-compatible endpoint can fail transiently (e.g. the CBS
   * put-token): retry about every second, as e2e_custody_test.c does. */
  bool watching = az_iot_e2e_service_telemetry_watch_begin(s_service, needle);
  for (time_t const start = time(NULL); !watching && time(NULL) - start < E2E_WATCH_TIMEOUT_S;)
  {
    fprintf(
        stderr, "[e2e] telemetry watch: %s; retrying\n", az_iot_e2e_service_last_error(s_service));
    struct timespec const second = { 1, 0 };
    (void)nanosleep(&second, NULL);
    watching = az_iot_e2e_service_telemetry_watch_begin(s_service, needle);
  }
  if (!watching || !az_iot_e2e_service_telemetry_watch_needle(s_service, needle))
  {
    fprintf(stderr, "[e2e] telemetry watch: %s\n", az_iot_e2e_service_last_error(s_service));
    return fail_group_setup();
  }

  printf("[e2e] starting the firmware: %s\n", command);
  if (!az_iot_e2e_process_start_shell(&s_firmware, command, NULL, "[esp32] "))
  {
    fprintf(stderr, "[e2e] could not start the firmware: %s\n", strerror(errno));
    return fail_group_setup();
  }
  if (!wait_for_firmware("hub_esp32: ready on ", E2E_READY_TIMEOUT_S))
  {
    fprintf(
        stderr, "[e2e] the firmware did not connect within %d s, or exited\n", E2E_READY_TIMEOUT_S);
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
  // The HTTP status is the service's; the body's "status" is the device's.
  static const char* const method_status[] = { "status", NULL };
  uint64_t device_status = 0;
  assert_true(e2e_json_read_uint64(response, method_status, &device_status));
  assert_int_equal(device_status, 200);
  static const char* const method_payload[] = { "payload", NULL };
  char echoed[64] = { 0 };
  assert_true(e2e_json_read_string(response, method_payload, echoed, sizeof(echoed)));
  assert_string_equal(echoed, marker);
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
  static const char* const desired_version_path[] = { "properties", "desired", "$version", NULL };
  uint64_t version = 0;
  assert_true(e2e_json_read_uint64(twin, desired_version_path, &version));
  printf("[e2e] desired patch: version %" PRIu64 "\n", version);
  assert_true(version > 0);

  char acknowledged[96];
  snprintf(acknowledged, sizeof(acknowledged), "desired version %" PRIu64 " acknowledged", version);
  assert_true(wait_for_firmware(acknowledged, E2E_REQUEST_TIMEOUT_S));

  static const char* const reported_path[] = { "properties", "reported", "desiredVersion", NULL };
  bool seen = false;
  for (int attempt = 0; attempt < 10 && !seen; ++attempt)
  {
    assert_true(az_iot_e2e_service_twin_get_begin(s_service, s_device_id));
    assert_int_equal(wait_for_request(&status, twin, sizeof(twin)), 1);
    uint64_t reported = 0;
    seen = status == 200 && e2e_json_read_uint64(twin, reported_path, &reported)
        && reported == version;
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
