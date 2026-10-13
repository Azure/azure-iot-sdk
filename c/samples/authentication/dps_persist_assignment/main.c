// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* fileno, fsync */
#endif
/**
 * @file main.c
 * @brief Persists the DPS assignment and restores it on the next run.
 *
 * First run: registers with DPS (bootstrap X.509, CSR), the managed provider
 * persists the issued operational chain, and the assignment callback saves the
 * hub, device id and profile to SAMPLE_ASSIGNMENT_FILE.
 *
 * Later runs: the saved assignment is loaded into opts.iothub_hostname, opts.device_id and
 * opts.connection_profile, so open() connects straight to the hub with the
 * persisted operational certificate. DPS stays configured: if the hub refuses
 * the identity, the client re-provisions and the new assignment is saved.
 *
 * Requires OpenSSL 3.0+ and the Paho adapter. See README.md.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "az_iot_certificate_provider_managed.h"

#include "sample_utils.h"

/** @brief Longest wait for provisioning plus the hub connection. */
#define SAMPLE_CONNECT_TIMEOUT_MS 60000u
/** @brief Longest wait for a graceful disconnect. */
#define SAMPLE_CLOSE_TIMEOUT_MS 5000u
/** @brief Duration of one do_work() tick. */
#define SAMPLE_TICK_MS 50u
/** @brief Longest saved line (hub hostname or device id) plus terminator. */
#define SAMPLE_LINE_MAX 256
/** @brief Where the assignment is saved, in the working directory. */
#define SAMPLE_ASSIGNMENT_FILE "dps_assignment.txt"
/** @brief Written first, then renamed over SAMPLE_ASSIGNMENT_FILE. */
#define SAMPLE_ASSIGNMENT_TMP_FILE SAMPLE_ASSIGNMENT_FILE ".tmp"
/** @brief Saved profile names. */
#define SAMPLE_PROFILE_MQTTV3 "mqttv3"
#define SAMPLE_PROFILE_MQTTV5 "mqttv5"

/** @brief A saved assignment. */
typedef struct
{
  char hub[SAMPLE_LINE_MAX]; /**< Hub hostname. */
  char device_id[SAMPLE_LINE_MAX]; /**< Device id. */
  az_iot_connection_profile profile; /**< Hub generation. */
} saved_assignment;

/** @brief State shared with the client callbacks. */
typedef struct
{
  az_iot_connection_state hub_state; /**< Latest hub-scope state. */
  bool failed; /**< A fault, or a failure retrying cannot fix. */
  az_iot_result failed_reason; /**< Reason of that failure. */
  const char* assignment_path; /**< Where the assignment is saved. */
  bool assigned; /**< This run adopted a DPS assignment. */
  bool assignment_saved; /**< This run saved it. */
} sample_context;

/** @brief Opens @p path for reading; NULL if it cannot be opened. */
static FILE* open_for_read(const char* path)
{
#ifdef _WIN32
  FILE* f = NULL;
  return fopen_s(&f, path, "rb") == 0 ? f : NULL;
#else
  return fopen(path, "rb");
#endif
}

/** @brief Reads one non-empty line into @p dst without its line ending. */
static bool read_line(FILE* f, char* dst, size_t dst_size)
{
  if (fgets(dst, (int)dst_size, f) == NULL)
  {
    return false;
  }
  dst[strcspn(dst, "\r\n")] = '\0';
  return dst[0] != '\0';
}

/** @brief Loads a saved assignment. False when there is none or it is invalid. */
static bool load_assignment(const char* path, saved_assignment* out)
{
  FILE* f = open_for_read(path);
  if (f == NULL)
  {
    return false;
  }
  char profile[16] = { 0 };
  bool ok = read_line(f, out->hub, sizeof(out->hub))
      && read_line(f, out->device_id, sizeof(out->device_id))
      && read_line(f, profile, sizeof(profile));
  (void)fclose(f);
  if (!ok)
  {
    return false;
  }
  if (strcmp(profile, SAMPLE_PROFILE_MQTTV3) == 0)
  {
    out->profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
  }
  else if (strcmp(profile, SAMPLE_PROFILE_MQTTV5) == 0)
  {
    out->profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  }
  else
  {
    return false;
  }
  return true;
}

/** @brief Flushes @p f to storage. True on success. */
static bool sync_file(FILE* f)
{
  if (fflush(f) != 0)
  {
    return false;
  }
#ifdef _WIN32
  return _commit(_fileno(f)) == 0;
#else
  return fsync(fileno(f)) == 0;
#endif
}

/** @brief Atomically replaces @p to with @p from. True on success. */
static bool replace_file(const char* from, const char* to)
{
#ifdef _WIN32
  return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return rename(from, to) == 0;
#endif
}

/** @brief Saves the adopted assignment, replacing any earlier one. The record
 *  is written to a temporary file and renamed, so a failure or power loss
 *  leaves the previous record intact. */
static void on_assignment(const az_iot_dps_assignment* assignment, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  const char* profile = assignment->profile->connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
      ? SAMPLE_PROFILE_MQTTV5
      : SAMPLE_PROFILE_MQTTV3;
  FILE* f = sample_fopen_private(SAMPLE_ASSIGNMENT_TMP_FILE);
  bool ok = f != NULL
      && fprintf(f, "%s\n%s\n%s\n", assignment->hub_hostname, assignment->device_id, profile) > 0
      && sync_file(f);
  if (f != NULL && fclose(f) != 0)
  {
    ok = false;
  }
  ok = ok && replace_file(SAMPLE_ASSIGNMENT_TMP_FILE, ctx->assignment_path);
  if (!ok)
  {
    (void)remove(SAMPLE_ASSIGNMENT_TMP_FILE);
  }
  ctx->assigned = true;
  ctx->assignment_saved = ok;
  fprintf(
      stderr,
      "[persist] assigned hub=%s device_id=%s profile=%s; save to %s: %s\n",
      assignment->hub_hostname,
      assignment->device_id,
      profile,
      ctx->assignment_path,
      ok ? "ok" : "failed");
}

/** @brief Logs transitions and records the ones the main loop acts on. */
static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  fprintf(
      stderr,
      "[persist] %s: %s (%s)\n",
      event->scope == AZ_IOT_CONN_SCOPE_DPS ? "dps" : "hub",
      sample_connection_state_name(event->state),
      az_iot_result_to_string(event->reason));
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    ctx->hub_state = event->state;
  }
  /* An identity refusal is not retriable as such, but identity recovery
   * re-provisions after it, so only a fault or another unfixable cause ends the
   * wait. */
  bool unfixable = event->state == AZ_IOT_CONN_STATE_RETRY_PENDING && !event->is_retriable
      && (event->recovery == NULL
          || event->recovery->classification != AZ_IOT_CONN_FAILURE_IDENTITY);
  if (event->reason != AZ_IOT_OK && !ctx->failed
      && (event->state == AZ_IOT_CONN_STATE_FAULTED || unfixable))
  {
    ctx->failed = true;
    ctx->failed_reason = event->reason;
  }
}

/** @brief Runs one do_work() tick of at least SAMPLE_TICK_MS. */
static void pump(az_iot_connection_client* client)
{
  uint64_t start = sample_now_ms();
  (void)az_iot_connection_client_do_work(client, SAMPLE_TICK_MS);
  uint64_t spent = sample_now_ms() - start;
  if (spent < SAMPLE_TICK_MS)
  {
    sample_sleep_ms((long)(SAMPLE_TICK_MS - spent));
  }
}

int main(void)
{
  az_iot_log_sink log = sample_log_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sample_config config = { 0 };
  if (sample_config_load(&config) != 0)
  {
    return 1;
  }

  /* Every string cleanup frees is assigned before the first goto. */
  char* op_key = sample_env_dup("AZ_IOT_OPERATIONAL_KEY", "operational_key.pem");
  char* op_cert = sample_env_dup("AZ_IOT_OPERATIONAL_CERT", "operational_cert.pem");

  int rc = 1;
  sample_context ctx = { 0 };
  ctx.assignment_path = SAMPLE_ASSIGNMENT_FILE;
  saved_assignment saved = { 0 };
  az_iot_certificate_provider_managed provider = { 0 };
  az_iot_connection_client client = { 0 };

  if (op_key == NULL || op_cert == NULL)
  {
    fprintf(stderr, "[persist] out of memory\n");
    goto cleanup;
  }

  /* Serves the persisted operational chain when present, else the bootstrap. */
  az_iot_certificate_provider_managed_options provider_opts
      = az_iot_certificate_provider_managed_options_default();
  provider_opts.bootstrap_cert_pem_path = config.cert;
  provider_opts.bootstrap_key_pem_path = config.key;
  provider_opts.trusted_ca_pem_path = config.ca;
  provider_opts.operational_key_pem_path = op_key;
  provider_opts.operational_cert_pem_path = op_cert;
  provider_opts.key_type = AZ_IOT_MANAGED_KEY_EC_P256;
  if (az_iot_certificate_provider_managed_init(&provider, &provider_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "[persist] managed certificate provider init failed\n");
    goto cleanup;
  }

  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&opts, &config);
  opts.certificate_provider = &provider.base;
  AZ_IOT_CSR_PAYLOAD_STORAGE(csr_payload_buf);
  opts.csr_payload_buffer = az_span_create(csr_payload_buf, sizeof(csr_payload_buf));
  opts.dps.request_operational_certificate = true;
  /* A saved identity the hub refuses goes back to DPS, on the reconnection
   * schedule rather than the 5 minute default identity recovery delay. */
  opts.identity_recovery.mode = AZ_IOT_IDENTITY_RECOVERY_REPROVISION;
  opts.identity_recovery.policy = opts.reconnection_policy;

  bool restored = load_assignment(SAMPLE_ASSIGNMENT_FILE, &saved);
  if (restored)
  {
    opts.iothub_hostname = saved.hub;
    opts.device_id = saved.device_id;
    opts.connection_profile = saved.profile;
    fprintf(
        stderr,
        "[persist] restoring hub=%s device_id=%s profile=%s from %s\n",
        saved.hub,
        saved.device_id,
        sample_connection_profile_name(saved.profile),
        SAMPLE_ASSIGNMENT_FILE);
  }
  else
  {
    fprintf(
        stderr,
        "[persist] no saved assignment in %s; registering with DPS\n",
        SAMPLE_ASSIGNMENT_FILE);
  }

  if (az_iot_connection_client_init(&client, &opts) != AZ_IOT_OK
      || az_iot_connection_client_add_state_observer(&client, on_conn_state, &ctx) != AZ_IOT_OK
      || az_iot_connection_client_set_dps_assignment_callback(&client, on_assignment, &ctx)
          != AZ_IOT_OK
      /* DPS and an mqttv3 hub use MQTT 3.1.1; an mqttv5 hub uses MQTT 5. */
      || az_iot_connection_client_register_mqtt_factory(
             &client, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&client, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK)
  {
    fprintf(stderr, "[persist] connection client setup failed\n");
    goto cleanup;
  }

  if (az_iot_connection_client_open(&client) != AZ_IOT_OK)
  {
    fprintf(stderr, "[persist] open failed; see the SDK log above\n");
    goto cleanup;
  }

  uint64_t deadline = sample_now_ms() + SAMPLE_CONNECT_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED && !ctx.failed && sample_now_ms() < deadline)
  {
    pump(&client);
  }

  if (ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(
        stderr,
        "[persist] connected to %s (%s)\n",
        az_iot_connection_client_get_iothub_address(&client),
        ctx.assigned ? "new assignment" : "restored assignment");
    /* A new assignment that could not be saved would re-register next run. */
    rc = (!ctx.assigned || ctx.assignment_saved) ? 0 : 1;
  }
  else if (ctx.failed)
  {
    fprintf(stderr, "[persist] failed: %s\n", az_iot_result_to_string(ctx.failed_reason));
  }
  else
  {
    fprintf(
        stderr,
        "[persist] timed out after %u s waiting for the hub connection\n",
        SAMPLE_CONNECT_TIMEOUT_MS / 1000u);
  }

  az_iot_connection_client_close(&client);
  deadline = sample_now_ms() + SAMPLE_CLOSE_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_IDLE && sample_now_ms() < deadline)
  {
    pump(&client);
  }

cleanup:
  az_iot_connection_client_deinit(&client);
  az_iot_certificate_provider_managed_deinit(&provider);
  free(op_key);
  free(op_cert);
  sample_config_release(&config);
  return rc;
}
