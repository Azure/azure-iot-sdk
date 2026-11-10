// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* A sample, built with the pipeline hooks, run against an e2e hub and checked from the service
 * side (az_iot_e2e_service).
 *
 * Usage: az_iot_tests_e2e_samples <sample> <executable> <work directory>
 *
 * <sample> names an entry of k_samples ("<group>_<name>"); tests/e2e/CMakeLists.txt registers one
 * CTest test per sample and adapter, labelled samples_mqttv3 or, for the CSR samples, samples_csr.
 * The sample runs to its own end, in <work directory>, with the variables it reads; this test then
 * checks its end marker, its exit code and what the service saw. Nothing is skipped: a missing
 * variable fails the test.
 *
 * Environment:
 *   service (not passed to the sample): IOTHUB_CONNECTION_STRING,
 * IOTHUB_EVENTHUB_CONNECTION_STRING, IOTHUB_EVENTHUB_CONSUMER_GROUP; IOT_DPS_CONNECTION_STRING and
 *     IOTHUB_REGISTRY_CONNECTION_STRING for the TPM samples' enrollment and device cleanup;
 *   device: AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID (the X.509 device, or for the CSR
 *     samples the bootstrap device; also the prefix of the other samples' registration IDs),
 *     AZ_IOT_CLIENT_CERT, AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA, optional
 * AZ_IOT_DPS_GLOBAL_ENDPOINT; SAS samples: AZ_IOT_DPS_ENROLLMENT_GROUP_KEY; proxy:
 * AZ_IOT_PROXY_HOST, AZ_IOT_PROXY_PORT, and AZ_IOT_SAMPLES_E2E_PROXY_LOG, the proxy's log;
 *   hsm_pkcs11: AZ_IOT_CLIENT_KEY_URI, AZ_IOT_CRYPTO_ENGINE_ID, SOFTHSM2_CONF, and
 *     AZ_IOT_SAMPLES_E2E_PKCS11_OPENSSL_CONF, the OPENSSL_CONF given to that sample only. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <azure/core/az_base64.h>

#ifdef _WIN32
#include <share.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "az_iot_e2e_service.h"
#include "e2e_json.h"
#include "e2e_notify_time.h"
#include "support/e2e_process.h"
#include "support/test_env.h"

/* What every sample prints (samples/common/sample_utils.h, SDK log at INFO). */
#define E2E_SAMPLE_BEGIN_MARKER "sample-pipeline: begin"
#define E2E_SAMPLE_END_MARKER "sample-pipeline: end rc="
#define E2E_SAMPLE_HUB_CONNECTED_MARKER "hub state=AZ_IOT_CONN_STATE_CONNECTED"

#define E2E_SAMPLE_START_TIMEOUT_S 30
#define E2E_SAMPLE_SERVICE_TIMEOUT_S 120 /* telemetry, notification or REST answer */
#define E2E_SAMPLE_WATCH_TIMEOUT_S 90 /* the Event Hub AMQP handshake can fail transiently */
#define E2E_SAMPLE_RETRY_PAUSE_MS 2000
#define E2E_SAMPLE_PUMP_MS 50
#define E2E_SAMPLE_STOP_GRACE_MS 5000
/* Twin $lastUpdated may precede the start by the clock difference of runner and hub. */
#define E2E_SAMPLE_CLOCK_SKEW_S 5

/** @brief How a sample authenticates, and so what it is given. */
typedef enum
{
  E2E_SAMPLE_IDENTITY_X509, /**< The leg's X.509 device. */
  E2E_SAMPLE_IDENTITY_SAS_GROUP_KEY, /**< AZ_IOT_DPS_ENROLLMENT_GROUP_KEY. */
  E2E_SAMPLE_IDENTITY_SAS_DEVICE_KEY, /**< AZ_IOT_DPS_SYMMETRIC_KEY derived from the group key. */
  E2E_SAMPLE_IDENTITY_PKCS11, /**< The X.509 device, its key in a PKCS#11 token. */
  E2E_SAMPLE_IDENTITY_TPM_SIMULATED, /**< A TPM individual enrollment, AZ_IOT_TPM_SIM_STATE. */
  E2E_SAMPLE_IDENTITY_TPM_SWTPM, /**< A TPM individual enrollment, swtpm through tpm2-tss. */
} e2e_sample_identity;

/** @brief A step of a sample's test: see e2e_sample. */
typedef void (*e2e_sample_step)(void);

/** @brief One sample under test. */
typedef struct
{
  const char* name; /**< "<group>_<name>", as in the test name. */
  e2e_sample_identity identity;
  /** @brief Appended, after '-', to the leg's registration ID; NULL: the leg's X.509 device. */
  const char* registration_suffix;
  /** @brief Its telemetry must reach the hub with a body containing this; NULL: not checked. */
  const char* telemetry_needle;
  e2e_sample_step prepare; /**< Before the sample starts; may be NULL. */
  e2e_sample_step action; /**< From the service, once its hub session is up; may be NULL. */
  e2e_sample_step check; /**< From the service, after it exited; may be NULL. */
  int run_timeout_s; /**< Longest run before the test stops it and fails. */
  /** @brief The sample must print this, after it exited with 0; NULL: not checked. */
  const char* expected_output;
} e2e_sample;

/** @brief The run of the sample under test. */
static struct
{
  const e2e_sample* sample;
  const char* executable;
  const char* work_directory;
  time_t start; /**< Before the sample started, runner clock. */
  char registration_id[129]; /**< Also its device ID: no enrollment here sets another. */
  az_iot_e2e_service* service;
  bool enrollment_created; /**< A TPM enrollment to delete with its device. */
  char proxy_log[1024];
  long proxy_log_start; /**< Size of the proxy log when the sample started. */
  char cloud_to_device_marker[48];
  bool action_succeeded;
} s_run;

static az_iot_e2e_process s_sample;
static az_iot_e2e_process s_swtpm;
static const char* s_child_environment[24];
static char s_child_values[8][1024];

static void sleep_ms(int milliseconds)
{
#ifdef _WIN32
  Sleep((DWORD)milliseconds);
#else
  struct timespec const t = { milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L };
  (void)nanosleep(&t, NULL);
#endif
}

/** @brief One step: the sample's output, then the service transport. */
static void pump(void)
{
  az_iot_e2e_process_read_output(&s_sample);
  if (s_run.service != NULL)
  {
    (void)az_iot_e2e_service_do_work(s_run.service, E2E_SAMPLE_PUMP_MS);
  }
  sleep_ms(10); // Paces the loop when nothing is being watched.
}

/** @brief pump() for az_iot_e2e_process_wait_for_output(). */
static void pump_for_wait(void* context)
{
  (void)context;
  if (s_run.service != NULL)
  {
    (void)az_iot_e2e_service_do_work(s_run.service, E2E_SAMPLE_PUMP_MS);
  }
}

/** @brief Pumps for @p milliseconds. */
static void pump_for(int milliseconds)
{
  time_t const end = time(NULL) + (milliseconds + 999) / 1000;
  while (time(NULL) < end)
  {
    pump();
  }
}

/** @brief Whether the sample printed its end marker. */
static bool sample_has_ended(void)
{
  return az_iot_e2e_process_has_printed(&s_sample, E2E_SAMPLE_END_MARKER);
}

/** @brief Copies environment variable @p name into @p out; fails the test when it is unset. */
static void require_environment(const char* name, char* out, size_t size)
{
  char* value = az_iot_test_env_dup(name);
  if (value == NULL)
  {
    fail_msg("%s is not set", name);
  }
  int const n = snprintf(out, size, "%s", value);
  free(value);
  assert_true(n > 0 && (size_t)n < size);
}

/** @brief Adds "NAME=value" (@p value non-NULL) or "NAME" (removes NAME) to the child's
 * environment. */
static void child_environment_add(const char* name, const char* value)
{
  size_t count = 0;
  while (s_child_environment[count] != NULL)
  {
    ++count;
  }
  assert_true(count + 1 < sizeof(s_child_environment) / sizeof(s_child_environment[0]));
  if (value == NULL)
  {
    s_child_environment[count] = name;
    return;
  }
  static size_t used;
  assert_true(used < sizeof(s_child_values) / sizeof(s_child_values[0]));
  int const n = snprintf(s_child_values[used], sizeof(s_child_values[used]), "%s=%s", name, value);
  assert_true(n > 0 && (size_t)n < sizeof(s_child_values[used]));
  s_child_environment[count] = s_child_values[used++];
}

/**
 * @brief Writes to @p out the device key DPS derives from the base64 group key @p group_key for
 * @p registration_id: base64(HMAC-SHA256(key, registration ID)).
 */
static void derive_device_key(
    const char* group_key,
    const char* registration_id,
    char* out,
    size_t size)
{
  uint8_t key[64];
  int32_t key_length = 0;
  assert_true(az_result_succeeded(az_base64_decode(
      AZ_SPAN_FROM_BUFFER(key),
      az_span_create_from_str((char*)(uintptr_t)group_key),
      &key_length)));
  uint8_t digest[32];
  unsigned digest_length = 0;
  uint8_t const* signed_digest = HMAC(
      EVP_sha256(),
      key,
      key_length,
      (const unsigned char*)registration_id,
      strlen(registration_id),
      digest,
      &digest_length);
  OPENSSL_cleanse(key, sizeof(key));
  assert_non_null(signed_digest);
  int32_t written = 0;
  assert_true(az_result_succeeded(az_base64_encode(
      az_span_create((uint8_t*)out, (int32_t)size - 1),
      az_span_create(digest, (int32_t)digest_length),
      &written)));
  out[written] = '\0';
}

/** @brief Builds the sample's environment: its own values, without the service's. */
static void make_child_environment(void)
{
  static const char* const service_only[] = {
    "IOTHUB_CONNECTION_STRING",       "IOTHUB_EVENTHUB_CONNECTION_STRING",
    "IOTHUB_EVENTHUB_CONSUMER_GROUP", "IOTHUB_EVENTHUB_LISTEN_NAME",
    "IOT_DPS_CONNECTION_STRING",      "IOTHUB_REGISTRY_CONNECTION_STRING",
    "AZ_IOT_SAMPLES_E2E_PROXY_LOG",   "AZ_IOT_SAMPLES_E2E_PKCS11_OPENSSL_CONF",
    "AZ_IOT_DPS_SYMMETRIC_KEY",
  };
  for (size_t i = 0; i < sizeof(service_only) / sizeof(service_only[0]); ++i)
  {
    child_environment_add(service_only[i], NULL);
  }
  e2e_sample_identity const identity = s_run.sample->identity;
  if (identity != E2E_SAMPLE_IDENTITY_SAS_GROUP_KEY)
  {
    child_environment_add("AZ_IOT_DPS_ENROLLMENT_GROUP_KEY", NULL);
  }
  if (s_run.sample->registration_suffix != NULL)
  {
    child_environment_add("AZ_IOT_DPS_REGISTRATION_ID", s_run.registration_id);
  }
  char value[1024];
  switch (identity)
  {
    case E2E_SAMPLE_IDENTITY_SAS_GROUP_KEY:
      require_environment("AZ_IOT_DPS_ENROLLMENT_GROUP_KEY", value, sizeof(value));
      break;
    case E2E_SAMPLE_IDENTITY_SAS_DEVICE_KEY:
    {
      char group_key[128];
      require_environment("AZ_IOT_DPS_ENROLLMENT_GROUP_KEY", group_key, sizeof(group_key));
      derive_device_key(group_key, s_run.registration_id, value, sizeof(value));
      OPENSSL_cleanse(group_key, sizeof(group_key));
      child_environment_add("AZ_IOT_DPS_SYMMETRIC_KEY", value);
      OPENSSL_cleanse(value, sizeof(value));
      break;
    }
    case E2E_SAMPLE_IDENTITY_PKCS11:
      require_environment("AZ_IOT_CLIENT_KEY_URI", value, sizeof(value));
      require_environment("AZ_IOT_CRYPTO_ENGINE_ID", value, sizeof(value));
      require_environment("SOFTHSM2_CONF", value, sizeof(value));
      require_environment("AZ_IOT_SAMPLES_E2E_PKCS11_OPENSSL_CONF", value, sizeof(value));
      child_environment_add("OPENSSL_CONF", value);
      break;
    case E2E_SAMPLE_IDENTITY_TPM_SIMULATED:
      child_environment_add("AZ_IOT_TPM_SIM_STATE", s_run.work_directory);
      break;
    case E2E_SAMPLE_IDENTITY_X509:
    case E2E_SAMPLE_IDENTITY_TPM_SWTPM: // AZ_IOT_TPM_TCTI: start_swtpm().
    default:
      break;
  }
}

/** @brief Starts the sample with @p argument (may be NULL) and waits for its begin marker. */
static void start_sample(const char* argument)
{
  const char* const arguments[] = { s_run.executable, argument, NULL };
  printf(
      "[e2e] starting %s%s%s\n", s_run.executable, argument ? " " : "", argument ? argument : "");
  assert_true(az_iot_e2e_process_start(&s_sample, arguments, s_child_environment, "[sample] "));
  if (!az_iot_e2e_process_wait_for_output(
          &s_sample,
          E2E_SAMPLE_BEGIN_MARKER,
          E2E_SAMPLE_START_TIMEOUT_S * 1000,
          pump_for_wait,
          NULL))
  {
    fail_msg("the sample did not print \"%s\"", E2E_SAMPLE_BEGIN_MARKER);
  }
}

/** @brief Pumps until the sample exits, stopping it after @p timeout_s; checks it ended with 0. */
static void wait_for_sample_success(int timeout_s)
{
  time_t const end = time(NULL) + timeout_s;
  int exit_code = -1;
  bool exited = false;
  while (!(exited = az_iot_e2e_process_wait_for_exit(&s_sample, 0, &exit_code)) && time(NULL) < end)
  {
    pump();
  }
  if (!exited)
  {
    (void)az_iot_e2e_process_stop(&s_sample, E2E_SAMPLE_STOP_GRACE_MS);
    fail_msg("the sample ran past %d s and was stopped", timeout_s);
  }
  (void)az_iot_e2e_process_wait_for_exit(&s_sample, 2000, &exit_code); // The rest of its output.
  (void)az_iot_e2e_process_stop(&s_sample, 0);
  printf("[e2e] the sample exited with %d\n", exit_code);
  assert_true(az_iot_e2e_process_has_printed(&s_sample, E2E_SAMPLE_END_MARKER "0"));
  assert_int_equal(exit_code, 0);
}

/** @brief Creates the service client, and starts the telemetry watch of the sample's device. */
static void start_service(void)
{
  const char* error = NULL;
  s_run.service = az_iot_e2e_service_create(&error);
  if (s_run.service == NULL)
  {
    fail_msg("service: %s", error ? error : "(unknown)");
  }
  if (s_run.sample->telemetry_needle == NULL)
  {
    return;
  }
  bool watching = az_iot_e2e_service_telemetry_watch_device_begin(
      s_run.service, s_run.registration_id, s_run.start);
  for (time_t const start = time(NULL);
       !watching && time(NULL) - start < E2E_SAMPLE_WATCH_TIMEOUT_S;)
  {
    printf("[e2e] telemetry watch: %s; retrying\n", az_iot_e2e_service_last_error(s_run.service));
    sleep_ms(1000);
    watching = az_iot_e2e_service_telemetry_watch_device_begin(
        s_run.service, s_run.registration_id, s_run.start);
  }
  if (!watching)
  {
    fail_msg("telemetry watch: %s", az_iot_e2e_service_last_error(s_run.service));
  }
}

/** @brief Pumps until telemetry of the sample's device containing the sample's needle arrived. */
static void check_telemetry(void)
{
  const char* needle = s_run.sample->telemetry_needle;
  time_t const end = time(NULL) + E2E_SAMPLE_SERVICE_TIMEOUT_S;
  while (!az_iot_e2e_service_telemetry_seen(s_run.service, needle) && time(NULL) < end)
  {
    pump();
  }
  if (!az_iot_e2e_service_telemetry_seen(s_run.service, needle))
  {
    const char* error = az_iot_e2e_service_last_error(s_run.service);
    fail_msg(
        "no telemetry from %s containing %s (service: %s)",
        s_run.registration_id,
        needle,
        error ? error : "no error");
  }
  az_iot_e2e_service_telemetry_dump(s_run.service, needle);
}

/** @brief Writes "<tag>-<time><random>" to @p out, unique per run. */
static void make_marker(char* out, size_t size, const char* tag)
{
  (void)snprintf(
      out, size, "%s-%08lx%04x", tag, (unsigned long)time(NULL), (unsigned)rand() & 0xffffu);
}

/** @brief Pumps until the REST request in flight completes; returns 1, or 0 on timeout, -1 on
 * error. */
static int wait_for_request(int* status, char* body, size_t body_size)
{
  time_t const end = time(NULL) + E2E_SAMPLE_SERVICE_TIMEOUT_S;
  int rc = 0;
  while (rc == 0 && time(NULL) < end)
  {
    pump();
    rc = az_iot_e2e_service_request_poll(s_run.service, status, body, body_size);
  }
  return rc;
}

/** @brief Invokes @p method until the device answers 200 with the payload echoed, or the sample
 * ends, exits or runs past its limit: the device may not be subscribed yet (404). */
static void invoke_method_until_echoed(const char* method)
{
  char marker[48];
  make_marker(marker, sizeof(marker), method);
  char payload[64];
  (void)snprintf(payload, sizeof(payload), "\"%s\"", marker);
  time_t const end = s_run.start + s_run.sample->run_timeout_s;
  while (!s_run.action_succeeded && !sample_has_ended()
         && !az_iot_e2e_process_wait_for_exit(&s_sample, 0, NULL) && time(NULL) < end)
  {
    int status = 0;
    char response[1024] = { 0 };
    int rc = -1;
    if (az_iot_e2e_service_method_invoke_with_timeout_begin(
            s_run.service, s_run.registration_id, method, payload, 30, 10))
    {
      rc = wait_for_request(&status, response, sizeof(response));
    }
    printf("[e2e] %s: rc=%d status=%d body=%s\n", method, rc, status, response);
    static const char* const status_path[] = { "status", NULL };
    static const char* const payload_path[] = { "payload", NULL };
    uint64_t device_status = 0;
    char echoed[64] = { 0 };
    s_run.action_succeeded = rc == 1 && status == 200
        && e2e_json_read_uint64(response, status_path, &device_status) && device_status == 200
        && e2e_json_read_string(response, payload_path, echoed, sizeof(echoed))
        && strcmp(echoed, marker) == 0;
    if (!s_run.action_succeeded)
    {
      pump_for(E2E_SAMPLE_RETRY_PAUSE_MS);
    }
  }
}

static void invoke_echo(void) { invoke_method_until_echoed("echo"); }

static void invoke_slow_echo(void) { invoke_method_until_echoed("slowEcho"); }

/** @brief Sends one cloud-to-device message, its payload a marker the sample prints. */
static void send_cloud_to_device_message(void)
{
  char* marker = s_run.cloud_to_device_marker;
  make_marker(marker, sizeof(s_run.cloud_to_device_marker), "c2d");
  for (int attempt = 0; attempt < 3 && !s_run.action_succeeded; ++attempt)
  {
    s_run.action_succeeded = az_iot_e2e_service_send_c2d(
        s_run.service, s_run.registration_id, (const uint8_t*)marker, strlen(marker));
    printf(
        "[e2e] c2d %s: %s\n",
        marker,
        s_run.action_succeeded ? "accepted" : az_iot_e2e_service_last_error(s_run.service));
  }
}

/** @brief The sample received the message: it printed its payload and counted it. */
static void check_cloud_to_device_message(void)
{
  char printed[64];
  (void)snprintf(printed, sizeof(printed), "=> %s", s_run.cloud_to_device_marker);
  assert_true(az_iot_e2e_process_has_printed(&s_sample, printed));
  const char* received = strstr(s_sample.log, "Received ");
  assert_non_null(received);
  long const messages = strtol(received + strlen("Received "), NULL, 10);
  printf("[e2e] the sample received %ld message(s)\n", messages);
  assert_true(messages >= 1);
}

/** @brief The twin's reported `sample` is "hello", written after the test started. */
static void check_twin_reported_property(void)
{
  static char twin[16384];
  int status = 0;
  assert_true(az_iot_e2e_service_twin_get_begin(s_run.service, s_run.registration_id));
  assert_int_equal(wait_for_request(&status, twin, sizeof(twin)), 1);
  printf("[e2e] twin: status=%d %s\n", status, twin);
  assert_int_equal(status, 200);
  static const char* const value_path[] = { "properties", "reported", "sample", NULL };
  static const char* const updated_path[]
      = { "properties", "reported", "$metadata", "sample", "$lastUpdated", NULL };
  char value[32] = { 0 };
  char updated[64] = { 0 };
  assert_true(e2e_json_read_string(twin, value_path, value, sizeof(value)));
  assert_string_equal(value, "hello");
  assert_true(e2e_json_read_string(twin, updated_path, updated, sizeof(updated)));
  int64_t updated_s = 0;
  const char* end = NULL;
  assert_true(e2e_utc_time_parse(updated, &updated_s, &end));
  assert_true(updated_s >= (int64_t)s_run.start - E2E_SAMPLE_CLOCK_SKEW_S);
}

/** @brief Starts watching file-upload notifications for the sample's device. */
static void watch_file_upload_notifications(void)
{
  bool refused = false;
  bool watching = az_iot_e2e_service_file_notification_watch_begin(
      s_run.service, s_run.registration_id, &refused);
  for (time_t const start = time(NULL);
       !watching && refused && time(NULL) - start < E2E_SAMPLE_WATCH_TIMEOUT_S;)
  {
    printf("[e2e] notification watch refused; retrying\n");
    sleep_ms(E2E_SAMPLE_RETRY_PAUSE_MS);
    watching = az_iot_e2e_service_file_notification_watch_begin(
        s_run.service, s_run.registration_id, &refused);
  }
  if (!watching)
  {
    fail_msg("notification watch: %s", az_iot_e2e_service_last_error(s_run.service));
  }
}

/** @brief The hub notified the upload of the sample's blob by the sample's device. */
static void check_file_upload_notification(void)
{
  time_t const end = time(NULL) + E2E_SAMPLE_SERVICE_TIMEOUT_S;
  while (!az_iot_e2e_service_file_notification_seen(
             s_run.service, AZ_IOT_E2E_SAMPLE_FILE_UPLOAD_BLOB_NAME)
         && time(NULL) < end)
  {
    pump();
  }
  int delivered = 0, captured = 0, released = 0, unwatched = 0, unparsed = 0;
  az_iot_e2e_service_file_notification_stats(
      s_run.service, &delivered, &captured, &released, &unwatched, &unparsed);
  printf(
      "[e2e] notifications: delivered %d, captured %d, released %d, unwatched %d, unparsed %d\n",
      delivered,
      captured,
      released,
      unwatched,
      unparsed);
  assert_true(az_iot_e2e_service_file_notification_seen(
      s_run.service, AZ_IOT_E2E_SAMPLE_FILE_UPLOAD_BLOB_NAME));
}

/** @brief Reads the file @p path into a NUL-terminated heap buffer, or NULL; shared with the
 * process writing it. */
static char* read_shared_file(const char* path, long* size)
{
#ifdef _WIN32
  FILE* f = _fsopen(path, "rb", _SH_DENYNO);
#else
  FILE* f = fopen(path, "rb");
#endif
  if (f == NULL)
  {
    return NULL;
  }
  char* text = NULL;
  long length = -1;
  if (fseek(f, 0, SEEK_END) == 0 && (length = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0
      && (text = (char*)malloc((size_t)length + 1)) != NULL)
  {
    length = (long)fread(text, 1, (size_t)length, f);
    text[length] = '\0';
  }
  (void)fclose(f);
  *size = length;
  return text;
}

/** @brief Notes where the proxy log ends, so only the sample's tunnels are looked at. */
static void note_proxy_log_end(void)
{
  char host[256];
  require_environment("AZ_IOT_PROXY_HOST", host, sizeof(host));
  require_environment("AZ_IOT_SAMPLES_E2E_PROXY_LOG", s_run.proxy_log, sizeof(s_run.proxy_log));
  char* log = read_shared_file(s_run.proxy_log, &s_run.proxy_log_start);
  if (log == NULL)
  {
    fail_msg("cannot read the proxy log %s", s_run.proxy_log);
  }
  free(log);
}

/** @brief Waits for the proxy to log a tunnel to an MQTT port (8883) opened since the start. */
static void check_proxy_tunnel(void)
{
  bool seen = false;
  time_t const end = time(NULL) + 15; // It logs a tunnel once closed.
  while (!seen && time(NULL) < end)
  {
    long size = 0;
    char* log = read_shared_file(s_run.proxy_log, &size);
    for (char* line
         = log != NULL && size > s_run.proxy_log_start ? log + s_run.proxy_log_start : NULL;
         line != NULL && *line != '\0';)
    {
      char* next = strchr(line, '\n');
      if (next != NULL)
      {
        *next++ = '\0';
      }
      char* connect = strstr(line, " - CONNECT ");
      if (connect != NULL && strstr(connect, ":8883 ") != NULL)
      {
        printf("[e2e] proxy: %s\n", line);
        seen = true;
      }
      line = next;
    }
    free(log);
    if (!seen)
    {
      sleep_ms(500);
    }
  }
  assert_true(seen);
}

#if defined(AZ_IOT_E2E_SAMPLES_SWTPM)
/** @brief Starts swtpm on a port of its own, its state in a new directory; sets AZ_IOT_TPM_TCTI. */
static void start_swtpm(void)
{
  unsigned const port = 12000u + ((unsigned)getpid() % 10000u) * 2u;
  char state[512];
  char tpmstate[600];
  char server[64];
  char control[64];
  char tcti[64];
  (void)snprintf(state, sizeof(state), "%s/swtpm-%ld", s_run.work_directory, (long)getpid());
  assert_int_equal(mkdir(state, 0700), 0);
  (void)snprintf(tpmstate, sizeof(tpmstate), "dir=%s", state);
  (void)snprintf(server, sizeof(server), "type=tcp,port=%u,bindaddr=127.0.0.1", port);
  (void)snprintf(control, sizeof(control), "type=tcp,port=%u,bindaddr=127.0.0.1", port + 1u);
  (void)snprintf(tcti, sizeof(tcti), "swtpm:host=127.0.0.1,port=%u", port);
  const char* const arguments[] = { AZ_IOT_E2E_SAMPLES_SWTPM,
                                    "socket",
                                    "--tpm2",
                                    "--server",
                                    server,
                                    "--ctrl",
                                    control,
                                    "--tpmstate",
                                    tpmstate,
                                    "--flags",
                                    "not-need-init,startup-clear",
                                    NULL };
  assert_true(az_iot_e2e_process_start(&s_swtpm, arguments, NULL, "[swtpm] "));
  child_environment_add("AZ_IOT_TPM_TCTI", tcti);
}
#endif

/** @brief Runs the sample with `--enrollment` and creates a TPM enrollment for what it prints;
 * swtpm may take a moment to listen, so it is retried. */
static void create_tpm_enrollment(void)
{
  char endorsement_key[1024] = { 0 };
  for (int attempt = 0; attempt < 20 && endorsement_key[0] == '\0'; ++attempt)
  {
    if (attempt > 0)
    {
      (void)az_iot_e2e_process_stop(&s_sample, 0);
      sleep_ms(250);
    }
    if (s_swtpm.running && az_iot_e2e_process_wait_for_exit(&s_swtpm, 0, NULL))
    {
      fail_msg("swtpm exited");
    }
    start_sample("--enrollment");
    int exit_code = -1;
    if (!az_iot_e2e_process_wait_for_exit(&s_sample, E2E_SAMPLE_START_TIMEOUT_S * 1000, &exit_code)
        || exit_code != 0)
    {
      continue;
    }
    static const char key_label[] = "endorsement key: ";
    const char* key = strstr(s_sample.log, key_label);
    if (key != NULL)
    {
      key += sizeof(key_label) - 1;
      size_t const length = strcspn(key, "\r\n");
      if (length > 0 && length < sizeof(endorsement_key))
      {
        memcpy(endorsement_key, key, length);
      }
    }
  }
  (void)az_iot_e2e_process_stop(&s_sample, 0);
  assert_true(endorsement_key[0] != '\0');
  s_run.enrollment_created = true; // Deleted by teardown() even if this fails part way.
  assert_true(az_iot_e2e_dps_tpm_enrollment_create(s_run.registration_id, endorsement_key));
}

/** @brief Fails the test unless the sample printed @p text. */
static void require_output(const char* text)
{
  if (!az_iot_e2e_process_has_printed(&s_sample, text))
  {
    fail_msg("the sample did not print \"%s\"", text);
  }
}

/** @brief Deletes the files a certificate sample keeps in its work directory (its defaults), so
 * it starts from the bootstrap certificate. */
static void remove_certificate_files(void)
{
  static const char* const names[] = {
    "operational_key.pem",
    "operational_cert.pem",
    "dps_assignment.txt",
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
  {
    char path[1024];
    int const n = snprintf(path, sizeof(path), "%s/%s", s_run.work_directory, names[i]);
    assert_true(n > 0 && (size_t)n < sizeof(path));
    (void)remove(path);
  }
}

/** @brief dps_persist_assignment: a first run registers with DPS and saves the assignment, which
 * the run under test restores. */
static void register_and_save_assignment(void)
{
  remove_certificate_files();
  start_sample(NULL);
  wait_for_sample_success(s_run.sample->run_timeout_s);
  require_output("[persist] no saved assignment");
}

/* What each sample does and how long it runs: samples/<group>/<name>/main.c. */
static const e2e_sample k_samples[] = {
  /* Telemetry every 5 s for 60 s: {"temp":23}, multithreaded {"sensor":n,"temp":t}. */
  { "unified_telemetry", E2E_SAMPLE_IDENTITY_X509, NULL, "\"temp\":", NULL, NULL, NULL, 180, NULL },
  { "unified_connect_first",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    "\"temp\":",
    NULL,
    NULL,
    NULL,
    180,
    NULL },
  { "unified_websockets",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    "\"temp\":",
    NULL,
    NULL,
    NULL,
    180,
    NULL },
  { "unified_proxy",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    "\"temp\":",
    note_proxy_log_end,
    NULL,
    check_proxy_tunnel,
    180,
    NULL },
  { "unified_multithreaded_telemetry",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    "\"sensor\":",
    NULL,
    NULL,
    NULL,
    180,
    NULL },
  /* Listen for 60 s; twin_get_patch reports {"sample":"hello"} on each connect. */
  { "unified_twin_get_patch",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    NULL,
    NULL,
    NULL,
    check_twin_reported_property,
    180,
    NULL },
  { "unified_direct_method_responder",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    NULL,
    NULL,
    invoke_echo,
    NULL,
    180,
    NULL },
  { "unified_direct_method_slow_responder",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    NULL,
    NULL,
    invoke_slow_echo,
    NULL,
    180,
    NULL },
  { "unified_c2d_receiver",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    NULL,
    NULL,
    send_cloud_to_device_message,
    check_cloud_to_device_message,
    180,
    NULL },
  /* One shot: connect (60 s at most), then an upload or one telemetry message. */
  { "unified_file_upload",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    NULL,
    watch_file_upload_notifications,
    NULL,
    check_file_upload_notification,
    240,
    NULL },
  { "authentication_dps_sas_key",
    E2E_SAMPLE_IDENTITY_SAS_GROUP_KEY,
    "sas-key",
    "\"auth\":\"sas-key\"",
    NULL,
    NULL,
    NULL,
    180,
    NULL },
  { "authentication_user_provided_sas_token",
    E2E_SAMPLE_IDENTITY_SAS_DEVICE_KEY,
    "sas-token",
    NULL,
    NULL,
    NULL,
    NULL,
    180,
    NULL },
  { "authentication_hsm_pkcs11",
    E2E_SAMPLE_IDENTITY_PKCS11,
    NULL,
    "\"custody\":\"hardware\"",
    NULL,
    NULL,
    NULL,
    180,
    NULL },
  /* Registration over HTTPS (120 s at most), connect (60 s), one message (60 s). */
  { "authentication_dps_tpm",
    E2E_SAMPLE_IDENTITY_TPM_SIMULATED,
    "tpm",
    "\"attestation\":\"tpm\"",
    NULL,
    NULL,
    NULL,
    300,
    NULL },
  { "authentication_dps_tpm_tss",
    E2E_SAMPLE_IDENTITY_TPM_SWTPM,
    "tpm-tss",
    "\"attestation\":\"tpm\"",
    NULL,
    NULL,
    NULL,
    300,
    NULL },
  /* CSR (samples_csr): the leg's device is a bootstrap identity of an X.509 enrollment group
   * linked to a signing CA. Connect 60 s and send 30 s at most; hub_renew renews and reconnects. */
  { "authentication_dps_csr_managed",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    "\"source\":\"dps_csr_managed\"",
    remove_certificate_files,
    NULL,
    NULL,
    180,
    "[dps_csr] telemetry sent with operational certificate" },
  { "authentication_custom_certificate_provider",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    NULL,
    remove_certificate_files,
    NULL,
    NULL,
    180,
    "[custom_cert] connected with operational identity" },
  { "authentication_dps_persist_assignment",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    NULL,
    register_and_save_assignment,
    NULL,
    NULL,
    180,
    "(restored assignment)" },
  { "authentication_hub_renew",
    E2E_SAMPLE_IDENTITY_X509,
    NULL,
    NULL,
    remove_certificate_files,
    NULL,
    NULL,
    300,
    "[hub_renew] reconnected with the renewed operational certificate" },
  { "authentication_dps_sas_key_issued_cert",
    E2E_SAMPLE_IDENTITY_SAS_GROUP_KEY,
    "sas-cert",
    "\"auth\":\"sas-key,x509-issued\"",
    remove_certificate_files,
    NULL,
    NULL,
    180,
    NULL },
};

/** @brief Runs the sample and checks it: see the file comment. */
static void the_sample_runs_and_the_service_sees_its_work(void** state)
{
  (void)state;
  const e2e_sample* sample = s_run.sample;
  srand((unsigned)time(NULL));
  s_run.start = time(NULL);
  char leg_registration_id[sizeof(s_run.registration_id)];
  require_environment(
      "AZ_IOT_DPS_REGISTRATION_ID", leg_registration_id, sizeof(leg_registration_id));
  int const n = sample->registration_suffix == NULL
      ? snprintf(s_run.registration_id, sizeof(s_run.registration_id), "%s", leg_registration_id)
      : snprintf(
            s_run.registration_id,
            sizeof(s_run.registration_id),
            "%s-%s",
            leg_registration_id,
            sample->registration_suffix);
  assert_true(n > 0 && (size_t)n < sizeof(s_run.registration_id));
  for (char* c = s_run.registration_id; sample->registration_suffix != NULL && *c != '\0'; ++c)
  {
    *c = (char)tolower((unsigned char)*c); // DPS registration IDs are lowercase.
  }
  printf("[e2e] %s as %s\n", sample->name, s_run.registration_id);

  make_child_environment();
  start_service();
  if (sample->prepare != NULL)
  {
    sample->prepare();
  }
  if (sample->identity == E2E_SAMPLE_IDENTITY_TPM_SWTPM)
  {
#if defined(AZ_IOT_E2E_SAMPLES_SWTPM)
    start_swtpm();
#else
    fail_msg("built without swtpm");
#endif
  }
  if (sample->identity == E2E_SAMPLE_IDENTITY_TPM_SIMULATED
      || sample->identity == E2E_SAMPLE_IDENTITY_TPM_SWTPM)
  {
    create_tpm_enrollment();
  }

  start_sample(NULL);
  if (sample->action != NULL)
  {
    if (!az_iot_e2e_process_wait_for_output(
            &s_sample,
            E2E_SAMPLE_HUB_CONNECTED_MARKER,
            sample->run_timeout_s * 1000,
            pump_for_wait,
            NULL))
    {
      fail_msg("the sample did not print \"%s\"", E2E_SAMPLE_HUB_CONNECTED_MARKER);
    }
    sample->action();
    assert_true(s_run.action_succeeded);
  }
  wait_for_sample_success(sample->run_timeout_s);
  if (sample->expected_output != NULL)
  {
    require_output(sample->expected_output);
  }
  if (sample->telemetry_needle != NULL)
  {
    check_telemetry();
  }
  if (sample->check != NULL)
  {
    sample->check();
  }
}

/** @brief Writes what the sample printed last to <work directory>/sample-output.log. */
static void save_sample_output(void)
{
  char path[1024];
  (void)snprintf(path, sizeof(path), "%s/sample-output.log", s_run.work_directory);
#ifdef _WIN32
  FILE* f = NULL;
  if (fopen_s(&f, path, "wb") != 0)
  {
    f = NULL;
  }
#else
  FILE* f = fopen(path, "wb");
#endif
  if (f != NULL)
  {
    (void)fwrite(s_sample.log, 1, s_sample.log_length, f);
    (void)fclose(f);
  }
}

/** @brief Stops what is still running, and deletes a TPM enrollment and its device. */
static int teardown(void** state)
{
  (void)state;
  int rc = 0;
  if (s_sample.running)
  {
    az_iot_e2e_process_read_output(&s_sample);
    (void)az_iot_e2e_process_stop(&s_sample, E2E_SAMPLE_STOP_GRACE_MS);
  }
  save_sample_output();
  (void)az_iot_e2e_process_stop(&s_swtpm, E2E_SAMPLE_STOP_GRACE_MS);
  if (s_run.enrollment_created)
  {
    bool const enrollment_deleted = az_iot_e2e_dps_enrollment_delete(s_run.registration_id);
    bool const device_deleted = s_run.service != NULL
        && az_iot_e2e_service_device_delete(s_run.service, s_run.registration_id);
    printf(
        "[e2e] enrollment %s, device %s\n",
        enrollment_deleted ? "deleted" : "NOT deleted",
        device_deleted ? "deleted" : "NOT deleted");
    rc = enrollment_deleted && device_deleted ? 0 : -1;
  }
  if (s_run.service != NULL)
  {
    az_iot_e2e_service_destroy(s_run.service);
    s_run.service = NULL;
  }
  return rc;
}

int main(int argc, char** argv)
{
  if (argc != 4)
  {
    fprintf(stderr, "usage: %s <sample> <executable> <work directory>\n", argv[0]);
    return 2;
  }
  for (size_t i = 0; i < sizeof(k_samples) / sizeof(k_samples[0]); ++i)
  {
    if (strcmp(k_samples[i].name, argv[1]) == 0)
    {
      s_run.sample = &k_samples[i];
    }
  }
  if (s_run.sample == NULL)
  {
    fprintf(stderr, "unknown sample %s\n", argv[1]);
    return 2;
  }
  (void)setvbuf(stdout, NULL, _IONBF, 0); // In order with the sample's and cmocka's output.
  s_run.executable = argv[2];
  s_run.work_directory = argv[3];
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(the_sample_runs_and_the_service_sees_its_work, teardown),
  };
  return cmocka_run_group_tests_name(argv[1], tests, NULL, NULL);
}
