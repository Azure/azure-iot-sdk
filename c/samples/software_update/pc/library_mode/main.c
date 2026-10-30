// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* software_update/pc/library_mode - Software Update library mode sample.
 *
 * Portable PC sample (Linux + Windows). The software updates client runs in
 * library mode: it checks for updates, verifies each manifest's signature,
 * sends the reports and retries them, and hands each verified update to this
 * application, which downloads, installs and reports on its own state machine.
 * No platform hooks are set. See README.md and docs/eng/software-updates.md
 * §5.3.
 *
 * The application:
 *   - takes the update on AZ_IOT_SU_EVENT_UPDATE_AVAILABLE and drives it from
 *     its main loop, one file or step per pass, so do_work() keeps running;
 *   - checks each downloaded file with az_iot_su_verify_file_hash();
 *   - reports progress, then the outcome with one result per manifest step,
 *     through az_iot_su_client_report_status();
 *   - keeps the installed update id in a file: library mode persists nothing,
 *     so after a restart the application tells the client what is installed.
 *
 * Real:      connection, update check, manifest verification, per-file
 *            SHA-256 check, status reports.
 * Simulated: download and install (../common/su_sim.c): zero-filled payload
 *            bytes of the declared size; install and apply only log.
 *
 * Needs an IoT Hub, as ../simulated_regular does: the regular route needs the
 * device record that registration creates.
 *
 * Environment variables, all optional:
 *   AZ_IOT_SU_MANUFACTURER=<s>        default "Contoso"   (matched)
 *   AZ_IOT_SU_MODEL=<s>               default "SU-Sim"    (matched)
 *   AZ_IOT_SU_INSTALLED_PROVIDER=<s>  default "Contoso"   (first run only)
 *   AZ_IOT_SU_INSTALLED_NAME=<s>      default "SU-Sim"    (first run only)
 *   AZ_IOT_SU_INSTALLED_VERSION=<s>   default "1.0.0"     (first run only)
 *   AZ_IOT_SU_INSTALLED_FILE=<path>   installed update id kept across runs
 *                                     (default ./su_library_installed.txt)
 *   AZ_IOT_SU_POLL_INTERVAL_S=<s>     seconds between checks (default 14400)
 *   AZ_IOT_SU_LOG_LEVEL=<lvl>         trace|debug|info|warn|error|off
 *   SU_SIM_FAIL_STEP=<n>              fail the install at 1-based step n
 *   SU_SIM_HASH_MISMATCH=1            corrupt the payload (hash check fails)
 *   SU_SIM_DELAY_MS=<ms>              per-download delay
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_su.h"
/* The MQTT adapter this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_SAMPLE_ADAPTER_PAHO or AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT"
#endif
/* The crypto backend this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_CRYPTO_OPENSSL)
#include "az_iot_crypto_openssl.h"
#elif defined(AZ_IOT_SAMPLE_CRYPTO_MBEDTLS)
#include "az_iot_crypto_mbedtls.h"
#else
#error "Define AZ_IOT_SAMPLE_CRYPTO_OPENSSL or AZ_IOT_SAMPLE_CRYPTO_MBEDTLS"
#endif

#include "sample_utils.h"
#include "su_sim.h"

/** @brief Minimum wait before a check resends a final report that lost its session. */
#define OWED_REPORT_RESEND_DELAY_MS 30000u

/** @brief Where the application is in the update handed to it. */
typedef enum
{
  DELEGATED_UPDATE_PHASE_NONE = 0, /**< No update handed over. */
  DELEGATED_UPDATE_PHASE_STARTING, /**< Handed over, not started. */
  DELEGATED_UPDATE_PHASE_DOWNLOADING, /**< Downloading and checking file `next_index`. */
  DELEGATED_UPDATE_PHASE_INSTALLING, /**< Installing and applying step `next_index`. */
} delegated_update_phase;

/** @brief The update the client handed to the application. */
typedef struct
{
  delegated_update_phase phase;
  /** Owned by the client; valid until the workflow ends. */
  const az_iot_su_client_update_request* request;
  /** Owned by the client; valid until the workflow ends. */
  const az_iot_su_client_update_manifest* manifest;
  uint32_t next_index; /**< Next file or step. */
  az_iot_su_step_result step_results[AZ_IOT_SU_MAX_STEP_COUNT]; /**< Reported at the end. */
} delegated_update;

typedef struct
{
  sample_config config;

  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_mqtt_factory* factory_v3;
  az_iot_mqtt_factory* factory_v5;
  const az_iot_crypto* crypto;
  az_iot_su_client su_client;

  char* manufacturer;
  char* model;
  char* installed_file;
  /* What is installed now: sent at init and read by is_offered_update_already_installed(). */
  char installed_provider[SU_SIM_ID_PART_SIZE];
  char installed_name[SU_SIM_ID_PART_SIZE];
  char installed_version[SU_SIM_ID_PART_SIZE];
  az_iot_su_report_update_id installed_update_id;

  su_simulation_control simulation_control;
  delegated_update update;
  bool check_now; /* ask for an update at the next loop pass */
  uint64_t owed_report_resend_ms; /* when to resend a final report that lost its session; 0: no */
  bool check_outstanding; /* an update check was asked and has no verdict yet */
  bool check_once_registered; /* the first check was abandoned before the device registered */
  uint64_t check_not_before_ms; /* the service asked to wait until then before asking again */

  /* Set by the observers. */
  az_iot_connection_state connection_state[AZ_IOT_CONN_SCOPE_COUNT];
  int connection_is_faulted; /* a lifecycle settled at FAULTED: unrecoverable here */
  bool device_registered; /* the hub connected at least once, so a device record exists */
  int su_workflow_active; /* a deployment is in flight */
} sample_state;

/* The software updates client's memory, sized for the default limits; static,
 * as it is too large for a stack frame. */
static uint8_t g_su_workspace[AZ_IOT_SU_WORKSPACE_DEFAULT_SIZE];

static void sample_state_destroy(sample_state* s)
{
  az_iot_su_client_deinit(&s->su_client);

  az_iot_connection_client_deinit(&s->connection_client);
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  az_iot_paho_factory_destroy(s->factory_v3);
  az_iot_paho_factory_destroy(s->factory_v5);
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  az_iot_az_mqtt_factory_destroy(s->factory_v3);
  az_iot_az_mqtt_factory_destroy(s->factory_v5);
#endif
  az_iot_certificate_provider_pem_deinit(&s->certs);
  sample_config_release(&s->config);

  free(s->installed_file);
  free(s->manufacturer);
  free(s->model);
}

/* ------------------------------------------------------------------------- */
/* installed update id, kept across runs                                     */
/* ------------------------------------------------------------------------- */

/**
 * @brief Read one nonempty, newline-terminated line of @p file into @p dst,
 * without its newline.
 * @return false for a truncated, overlong or empty line, or one with a NUL.
 */
static bool read_installed_update_id_line(FILE* file, char* dst, size_t dst_size)
{
  if (fgets(dst, (int)dst_size, file) == NULL)
  {
    return false;
  }
  size_t length = strlen(dst);
  if (length < 2 || dst[length - 1] != '\n')
  {
    return false;
  }
  dst[length - 1] = '\0';
  return true;
}

/**
 * @brief Load the installed update id from the installed file.
 * @return 1 when loaded; 0 when the file does not exist (first run); -1 when it
 * cannot be read or is malformed (anything but exactly three lines).
 */
static int load_installed_update_id(sample_state* s)
{
  errno = 0;
  FILE* file = sample_file_open_read_no_follow(s->installed_file);
  if (file == NULL)
  {
    return (errno == ENOENT) ? 0 : -1;
  }
  bool loaded
      = read_installed_update_id_line(file, s->installed_provider, sizeof(s->installed_provider))
      && read_installed_update_id_line(file, s->installed_name, sizeof(s->installed_name))
      && read_installed_update_id_line(file, s->installed_version, sizeof(s->installed_version))
      && fgetc(file) == EOF && !ferror(file);
  (void)fclose(file);
  return loaded ? 1 : -1;
}

/**
 * @brief Durably replace the installed file with the applied update id, then
 * keep it as the installed one.
 */
static bool save_applied_update_id_as_installed(sample_state* s)
{
  const su_simulation_control* applied = &s->simulation_control;
  char temporary_path[1024];
  int length = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", s->installed_file);
  if (!applied->applied_valid || length < 0 || (size_t)length >= sizeof(temporary_path))
  {
    return false;
  }
  FILE* file = sample_file_open_private(temporary_path, true);
  if (file == NULL)
  {
    return false;
  }
  bool written = fprintf(
                     file,
                     "%s\n%s\n%s\n",
                     applied->applied_provider,
                     applied->applied_name,
                     applied->applied_version)
          > 0
      && sample_file_flush_to_storage(file);
  if (fclose(file) != 0 || !written || !sample_file_replace(temporary_path, s->installed_file))
  {
    (void)remove(temporary_path);
    return false;
  }
  /* applied_* and installed_* have the same capacity, so the copies fit. */
  return sample_string_copy(
             s->installed_provider, sizeof(s->installed_provider), applied->applied_provider)
      && sample_string_copy(s->installed_name, sizeof(s->installed_name), applied->applied_name)
      && sample_string_copy(
             s->installed_version, sizeof(s->installed_version), applied->applied_version);
}

/* ------------------------------------------------------------------------- */
/* the application's update state machine                                    */
/* ------------------------------------------------------------------------- */

/** @brief What az_iot_su_verify_file_hash() reads one downloaded file through. */
typedef struct
{
  const az_iot_su_client_update_manifest_file* file;
  uint32_t file_index;
  su_simulation_control* simulation_control;
} downloaded_file_reader;

/** @brief az_iot_su_read_chunk_callback over a simulated downloaded file. */
static int32_t read_downloaded_file(
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* read_ctx)
{
  downloaded_file_reader* reader = (downloaded_file_reader*)read_ctx;
  return read_back_simulated_update_file(
      reader->file,
      reader->file_index,
      offset,
      buffer,
      buffer_size,
      out_read,
      reader->simulation_control);
}

/** @brief The URL the service sent for @p file, or an empty span. */
static az_span find_file_url(
    const az_iot_su_client_update_request* request,
    const az_iot_su_client_update_manifest_file* file)
{
  for (uint32_t i = 0; i < request->file_urls_count; ++i)
  {
    if (az_span_is_content_equal(request->file_urls[i].id, file->id))
    {
      return request->file_urls[i].url;
    }
  }
  return AZ_SPAN_EMPTY;
}

/** @brief A FAILED result for the phase named by @p facility. */
static az_iot_su_step_result make_failed_result(
    az_iot_su_failure_origin origin,
    uint32_t facility,
    az_span details)
{
  az_iot_su_step_result result = { 0 };
  result.outcome = AZ_IOT_SU_OUTCOME_FAILED;
  result.failure_origin = origin;
  result.result_code = -(int32_t)facility; /* a failure code is never positive */
  result.extended_result_code = AZ_IOT_SU_EXTENDED_RESULT(facility, 1u);
  result.result_details = details;
  return result;
}

/**
 * @brief Report the update's outcome, with the step results; the workflow ends.
 *
 * On success the client is Idle again when this returns, and the update's
 * request and manifest are gone.
 */
static void finish_delegated_update(sample_state* s, az_iot_su_step_result result)
{
  delegated_update* u = &s->update;
  az_iot_result reported = az_iot_su_client_report_status(
      &s->su_client, &result, u->step_results, (int32_t)u->manifest->instructions.steps_count);
  if (reported != AZ_IOT_OK)
  {
    /* Arguments the client refuses: a bug here. The update stays handed over
     * until the client is deinitialized. */
    fprintf(stderr, "Could not report the outcome: %s\n", az_iot_result_to_string(reported));
    u->phase = DELEGATED_UPDATE_PHASE_NONE;
  }
}

/**
 * @brief Do the next unit of the handed-over update: the already-installed
 * check, one file, or one step. Called from the main loop, so do_work() runs
 * between units.
 */
static void advance_delegated_update(sample_state* s)
{
  delegated_update* u = &s->update;
  const az_iot_su_client_update_manifest* manifest = u->manifest;

  switch (u->phase)
  {
    case DELEGATED_UPDATE_PHASE_NONE:
      return;

    case DELEGATED_UPDATE_PHASE_STARTING:
    {
      /* Library mode calls no is_installed_fn: the application checks. */
      if (is_offered_update_already_installed(manifest, &s->simulation_control)
          == AZ_IOT_SU_RESULT_ALREADY_INSTALLED)
      {
        az_iot_su_step_result skipped = { 0 };
        skipped.outcome = AZ_IOT_SU_OUTCOME_SKIPPED;
        skipped.result_details = AZ_SPAN_FROM_STR("already installed");
        finish_delegated_update(s, skipped);
        return;
      }
      u->phase = DELEGATED_UPDATE_PHASE_DOWNLOADING;
      u->next_index = 0;
      return;
    }

    case DELEGATED_UPDATE_PHASE_DOWNLOADING:
    {
      if (u->next_index < manifest->files_count)
      {
        uint32_t index = u->next_index++;
        const az_iot_su_client_update_manifest_file* file = &manifest->files[index];
        downloaded_file_reader reader = { file, index, &s->simulation_control };
        if (simulate_update_file_download(
                file,
                find_file_url(u->request, file),
                index,
                manifest->files_count,
                &s->simulation_control)
            != AZ_IOT_SU_RESULT_SUCCESS)
        {
          finish_delegated_update(
              s,
              make_failed_result(
                  AZ_IOT_SU_FAILURE_ORIGIN_AGENT_CORE,
                  AZ_IOT_SU_FACILITY_DOWNLOAD,
                  AZ_SPAN_FROM_STR("download failed")));
        }
        else if (
            az_iot_su_verify_file_hash(file, s->crypto, read_downloaded_file, &reader) != AZ_IOT_OK)
        {
          fprintf(stderr, "File %u/%u: hash mismatch.\n", index + 1u, manifest->files_count);
          finish_delegated_update(
              s,
              make_failed_result(
                  AZ_IOT_SU_FAILURE_ORIGIN_AGENT_CORE,
                  AZ_IOT_SU_FACILITY_HASH,
                  AZ_SPAN_FROM_STR("hash mismatch")));
        }
        return;
      }
      /* Each report counts against the service's request budget: report
       * milestones, not every file. */
      az_iot_su_step_result progress = { 0 };
      progress.outcome = AZ_IOT_SU_OUTCOME_IN_PROGRESS;
      progress.result_details = AZ_SPAN_FROM_STR("downloaded; installing");
      if (az_iot_su_client_report_status(&s->su_client, &progress, NULL, 0) != AZ_IOT_OK)
      {
        fprintf(stderr, "Could not report progress.\n");
      }
      u->phase = DELEGATED_UPDATE_PHASE_INSTALLING;
      u->next_index = 0;
      return;
    }

    case DELEGATED_UPDATE_PHASE_INSTALLING:
    {
      if (u->next_index < manifest->instructions.steps_count)
      {
        uint32_t step = u->next_index++;
        if (simulate_update_step_install(manifest, step, &s->simulation_control)
                != AZ_IOT_SU_RESULT_SUCCESS
            || simulate_update_step_apply(manifest, step, &s->simulation_control)
                != AZ_IOT_SU_RESULT_SUCCESS)
        {
          /* This step failed; the later ones stay SKIPPED. */
          u->step_results[step] = make_failed_result(
              AZ_IOT_SU_FAILURE_ORIGIN_AGENT_EXTENSION,
              AZ_IOT_SU_FACILITY_INSTALL,
              AZ_SPAN_FROM_STR("install failed"));
          finish_delegated_update(s, u->step_results[step]);
          return;
        }
        u->step_results[step].outcome = AZ_IOT_SU_OUTCOME_SUCCEEDED;
        u->step_results[step].result_code = AZ_IOT_SU_AGENT_RESULT_CODE_SUCCESS;
        return;
      }

      /* Saved before the success report: after a restart in between, the
       * update offered again is reported skipped, not installed twice. */
      if (!save_applied_update_id_as_installed(s))
      {
        finish_delegated_update(
            s,
            make_failed_result(
                AZ_IOT_SU_FAILURE_ORIGIN_DEVICE,
                AZ_IOT_SU_FACILITY_PERSIST,
                AZ_SPAN_FROM_STR("installed update id not saved")));
        return;
      }
      printf(
          "Update applied. Installed: %s/%s/%s (saved to %s)\n",
          s->installed_provider,
          s->installed_name,
          s->installed_version,
          s->installed_file);
      /* From here the client sends the manifest's update id as installed on
       * its own; the file carries it across restarts. */
      az_iot_su_step_result succeeded = { 0 };
      succeeded.outcome = AZ_IOT_SU_OUTCOME_SUCCEEDED;
      succeeded.result_code = AZ_IOT_SU_AGENT_RESULT_CODE_SUCCESS;
      finish_delegated_update(s, succeeded);
      return;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* observers                                                                 */
/* ------------------------------------------------------------------------- */

/* Both lifecycles report here. `state` is meaningless without `scope`. */
static void on_connection_state_event_received(
    const az_iot_connection_state_event* event,
    void* user_ctx)
{
  sample_state* state = (sample_state*)user_ctx;

  /* `scope` indexes an array: ignore one this build does not know. */
  if ((unsigned)event->scope >= AZ_IOT_CONN_SCOPE_COUNT)
  {
    return;
  }

  az_iot_connection_scope scope = event->scope;

  if (state->connection_state[scope] != event->state)
  {
    printf(
        "%s: %s -> %s (%s)\n",
        (scope == AZ_IOT_CONN_SCOPE_DPS) ? "Provisioning" : "Hub",
        az_iot_connection_state_to_string(state->connection_state[scope]),
        az_iot_connection_state_to_string(event->state),
        az_iot_result_to_string(event->reason));
    state->connection_state[scope] = event->state;

    if (scope == AZ_IOT_CONN_SCOPE_HUB && event->state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      state->device_registered = true;
      state->check_now = state->check_now || state->check_once_registered;
      state->check_once_registered = false;
      printf("Hub connection up. Running (Ctrl-C to exit)...\n");
    }
  }

  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    state->connection_is_faulted = 1;
  }
}

/** @brief Whether @p event is the verdict of an update check. */
static bool is_update_check_verdict(const az_iot_su_event* event)
{
  switch (event->kind)
  {
    case AZ_IOT_SU_EVENT_NO_UPDATE:
    case AZ_IOT_SU_EVENT_UPDATE_REFUSED:
    case AZ_IOT_SU_EVENT_UPDATE_DEFERRED:
    case AZ_IOT_SU_EVENT_UPDATE_DUPLICATE:
    case AZ_IOT_SU_EVENT_UPDATE_RETRY_WAIT:
      return true;
    case AZ_IOT_SU_EVENT_OPERATION_ABANDONED:
      return event->operation != AZ_IOT_SU_OP_REPORT_STATUS;
    case AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED:
      /* A delivered update started a workflow. */
      return event->previous_state == AZ_IOT_SU_STATE_IDLE;
    case AZ_IOT_SU_EVENT_PERSIST_FAILED:
    case AZ_IOT_SU_EVENT_PERSIST_RECOVERED:
    case AZ_IOT_SU_EVENT_REPORT_ACCEPTED:
    case AZ_IOT_SU_EVENT_WORKFLOW_COMPLETED:
    case AZ_IOT_SU_EVENT_UPDATE_AVAILABLE:
      return false;
  }
  return false;
}

static void on_su_event(const az_iot_su_event* event, void* user_ctx)
{
  sample_state* state = (sample_state*)user_ctx;
  if (is_update_check_verdict(event))
  {
    state->check_outstanding = false;
  }

  switch (event->kind)
  {
    case AZ_IOT_SU_EVENT_UPDATE_AVAILABLE:
    {
      /* Only take the update here; the main loop does the work. */
      delegated_update* u = &state->update;
      memset(u, 0, sizeof(*u));
      u->request = event->request;
      u->manifest = event->manifest;
      for (uint32_t i = 0; i < event->manifest->instructions.steps_count; ++i)
      {
        u->step_results[i].outcome = AZ_IOT_SU_OUTCOME_SKIPPED;
      }
      u->phase = DELEGATED_UPDATE_PHASE_STARTING;
      state->simulation_control.applied_valid = 0;
      printf(
          "Verified update %.*s/%.*s/%.*s handed over: %u file(s), %u step(s).\n",
          az_span_size(event->manifest->update_id.provider),
          (const char*)az_span_ptr(event->manifest->update_id.provider),
          az_span_size(event->manifest->update_id.name),
          (const char*)az_span_ptr(event->manifest->update_id.name),
          az_span_size(event->manifest->update_id.version),
          (const char*)az_span_ptr(event->manifest->update_id.version),
          (unsigned)event->manifest->files_count,
          (unsigned)event->manifest->instructions.steps_count);
      break;
    }

    case AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED:
      printf(
          "Update workflow: %s -> %s\n",
          su_sample_state_name(event->previous_state),
          su_sample_state_name(event->state));
      if (event->state == AZ_IOT_SU_STATE_FAILED)
      {
        /* Before any hand-off: the manifest did not verify. */
        fprintf(
            stderr,
            "Update failed: %s, extended result 0x%08x\n",
            az_iot_result_to_string(event->reason),
            (unsigned)event->extended_result_code);
      }
      if (event->previous_state == AZ_IOT_SU_STATE_DELEGATED)
      {
        /* The workflow ended: its request and manifest are gone. */
        memset(&state->update, 0, sizeof(state->update));
      }
      state->su_workflow_active = (event->state != AZ_IOT_SU_STATE_IDLE);
      break;

    case AZ_IOT_SU_EVENT_OPERATION_ABANDONED:
      fprintf(
          stderr,
          "%s abandoned: %s. Service said: code=%d message=\"%s\" trackingId=\"%s\"\n",
          su_sample_operation_name(event->operation),
          az_iot_result_to_string(event->reason),
          (int)event->service_error.code,
          event->service_error.message,
          event->service_error.tracking_id);
      if (event->operation == AZ_IOT_SU_OP_GET_UPDATE)
      {
        /* A device that has never registered has no device record yet: check
         * again once registration creates it. */
        state->check_once_registered = state->check_once_registered || !state->device_registered;
        if (event->service_error.retry_after_ms > 0)
        {
          state->check_not_before_ms = sample_now_ms() + event->service_error.retry_after_ms;
        }
      }
      else if (
          event->operation == AZ_IOT_SU_OP_REPORT_STATUS
          && event->reason == AZ_IOT_ERR_NOT_CONNECTED
          && state->update.phase == DELEGATED_UPDATE_PHASE_NONE
          && state->owed_report_resend_ms == 0)
      {
        /* No delegated work in flight, so this was a final report (failed before
         * delegation, or after it). It stays owed with no resend queued: only a
         * check sends it. */
        state->owed_report_resend_ms = sample_now_ms() + OWED_REPORT_RESEND_DELAY_MS;
      }
      break;
    case AZ_IOT_SU_EVENT_UPDATE_REFUSED:
      fprintf(stderr, "Update refused: %s\n", az_iot_result_to_string(event->reason));
      break;
    case AZ_IOT_SU_EVENT_UPDATE_DEFERRED:
      printf("New update deferred until the current one ends.\n");
      break;
    case AZ_IOT_SU_EVENT_NO_UPDATE:
      printf("%s: no update available.\n", su_sample_operation_name(event->operation));
      break;
    case AZ_IOT_SU_EVENT_UPDATE_DUPLICATE:
      printf("Update ignored: it repeats the current workflow.\n");
      break;
    case AZ_IOT_SU_EVENT_REPORT_ACCEPTED:
      printf("Status report accepted%s.\n", event->report_terminal ? " (final)" : "");
      break;
    case AZ_IOT_SU_EVENT_UPDATE_RETRY_WAIT:
      printf(
          "Failed update offered again; retry in %u s.\n",
          (unsigned)(event->retry_after_ms / 1000u));
      break;
    case AZ_IOT_SU_EVENT_WORKFLOW_COMPLETED:
      printf(
          "Workflow completed; final report %s.\n",
          event->reason == AZ_IOT_OK ? "accepted" : "rejected by the service");
      /* The client is Idle before its final report settles: check only now, so
       * the check's timeout does not run while the report is still owed. A
       * check held behind the report already asks for what comes next, and a
       * rejected report is rechecked by the SDK or at the next poll. */
      state->check_now = state->check_now
          || (event->reason == AZ_IOT_OK && event->report != NULL
              && event->report->outcome == AZ_IOT_SU_OUTCOME_SUCCEEDED
              && !state->check_outstanding);
      state->owed_report_resend_ms = 0;
      break;
    case AZ_IOT_SU_EVENT_PERSIST_FAILED:
    case AZ_IOT_SU_EVENT_PERSIST_RECOVERED:
      /* Library mode persists nothing. */
      break;
  }
}

/**
 * @brief Create the certificate provider and the connection client, observed
 * by on_connection_state_event_received() and with both MQTT factories
 * registered. The device registers and connects to its hub.
 *
 * @param[in,out] state Sample state; its config must be loaded.
 * @return 0 on success, nonzero on failure. sample_state_destroy() releases
 * what was created either way.
 */
static int initialize_connection_client(sample_state* state)
{
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = state->config.ca;
  pem.client_cert_pem_path = state->config.cert;
  pem.client_key_pem_path = state->config.key;
  if (az_iot_certificate_provider_pem_init(&state->certs, &pem) != AZ_IOT_OK)
  {
    return 1;
  }

#if defined(AZ_IOT_SAMPLE_CRYPTO_OPENSSL)
  state->crypto = az_iot_crypto_openssl();
#elif defined(AZ_IOT_SAMPLE_CRYPTO_MBEDTLS)
  state->crypto = az_iot_crypto_mbedtls();
#endif
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = state->config.id_scope;
  copts.dps.registration_id = state->config.reg_id;
  copts.dps.global_endpoint = state->config.dps_global_endpoint; /* NULL: the global default */
  copts.certificate_provider = &state->certs.base;
  /* Verifies update manifests; the application hashes downloaded files with it too. */
  copts.crypto = state->crypto;
  if (az_iot_connection_client_init(&state->connection_client, &copts) != AZ_IOT_OK
      || az_iot_connection_client_add_state_observer(
             &state->connection_client, on_connection_state_event_received, state)
          != AZ_IOT_OK)
  {
    return 1;
  }

#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  state->factory_v3 = az_iot_paho_factory_create_v3_1_1();
  state->factory_v5 = az_iot_paho_factory_create_v5();
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  state->factory_v3 = az_iot_az_mqtt_factory_create_v3_1_1();
  state->factory_v5 = az_iot_az_mqtt_factory_create_v5();
#endif
  if (az_iot_connection_client_register_mqtt_factory(&state->connection_client, state->factory_v3)
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(
             &state->connection_client, state->factory_v5)
          != AZ_IOT_OK)
  {
    return 1;
  }
  return 0;
}

/**
 * @brief Load the device identity: compatibility properties from the
 * environment; the installed update id from the installed file or, on a first
 * run, from the environment.
 * @return 0 on success, nonzero on failure.
 */
static int load_device_identity(sample_state* s)
{
  s->manufacturer = sample_env_copy("AZ_IOT_SU_MANUFACTURER", "Contoso");
  s->model = sample_env_copy("AZ_IOT_SU_MODEL", "SU-Sim");
  s->installed_file = sample_env_copy("AZ_IOT_SU_INSTALLED_FILE", "./su_library_installed.txt");
  if (s->manufacturer == NULL || s->model == NULL || s->installed_file == NULL)
  {
    return 1;
  }
  int loaded = load_installed_update_id(s);
  if (loaded < 0)
  {
    fprintf(stderr, "%s cannot be read or is malformed; fix or remove it.\n", s->installed_file);
    return 1;
  }
  if (loaded == 0
      && (!sample_env_copy_to_buffer(
              "AZ_IOT_SU_INSTALLED_PROVIDER",
              "Contoso",
              s->installed_provider,
              sizeof(s->installed_provider))
          || !sample_env_copy_to_buffer(
              "AZ_IOT_SU_INSTALLED_NAME", "SU-Sim", s->installed_name, sizeof(s->installed_name))
          || !sample_env_copy_to_buffer(
              "AZ_IOT_SU_INSTALLED_VERSION",
              "1.0.0",
              s->installed_version,
              sizeof(s->installed_version))))
  {
    return 1;
  }
  s->installed_update_id.provider = s->installed_provider;
  s->installed_update_id.name = s->installed_name;
  s->installed_update_id.version = s->installed_version;
  s->simulation_control.installed = &s->installed_update_id;
  return 0;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
  az_iot_log_sink log = sample_log_sink(su_sample_log_level_from_env());
  az_iot_log_set_global_sink(&log);

  sample_install_stop_handler(); /* Ctrl+C or SIGTERM ends the loop below */

  sample_state state = { 0 };
  state.simulation_control.fail_step = (int)su_sample_env_long("SU_SIM_FAIL_STEP", 0);
  state.simulation_control.hash_mismatch = su_sample_env_flag("SU_SIM_HASH_MISMATCH");
  state.simulation_control.delay_ms = su_sample_env_long("SU_SIM_DELAY_MS", 0);

  /* Every check and report shares the service's request budget, which plans
   * for one check per device every 4 hours. */
  long poll_interval_s = su_sample_env_long("AZ_IOT_SU_POLL_INTERVAL_S", 14400);
  if (poll_interval_s < 0)
  {
    poll_interval_s = 14400;
  }
  /* Each check is bounded to half the poll interval; see ../simulated_regular. */
  uint32_t request_timeout_ms = AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS;
  if (poll_interval_s > 0 && (uint64_t)poll_interval_s * 500u < request_timeout_ms)
  {
    request_timeout_ms = (uint32_t)poll_interval_s * 500u;
  }

  if (sample_config_load(&state.config) != 0)
  {
    return 1;
  }
  if (initialize_connection_client(&state) != 0 || load_device_identity(&state) != 0)
  {
    sample_state_destroy(&state);
    return 1;
  }

  az_iot_su_device_properties device_properties = { 0 };
  device_properties.manufacturer = state.manufacturer;
  device_properties.model = state.model;
  device_properties.installed_update_id.provider = state.installed_provider;
  device_properties.installed_update_id.name = state.installed_name;
  device_properties.installed_update_id.version = state.installed_version;
  size_t device_properties_needed = az_iot_su_device_properties_buffer_size(&device_properties);
  if (device_properties_needed == 0
      || device_properties_needed > AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE)
  {
    fprintf(stderr, "Device properties are invalid or too long.\n");
    sample_state_destroy(&state);
    return 1;
  }

  /* Library mode: no platform hooks. Root keys are still required, so only
   * verified updates are handed over. */
  size_t root_key_count = 0;
  az_iot_su_client_config_options su_opts = az_iot_su_client_config_options_default();
  su_opts.library_mode = true;
  su_opts.root_keys = az_iot_su_microsoft_root_keys(&root_key_count);
  su_opts.root_key_count = root_key_count;
  su_opts.device_properties = &device_properties;
  su_opts.workspace = g_su_workspace;
  su_opts.workspace_size = sizeof(g_su_workspace);
  if (az_iot_su_client_init(&state.su_client, &state.connection_client, &su_opts) != AZ_IOT_OK
      || az_iot_su_client_add_observer(&state.su_client, on_su_event, &state) != AZ_IOT_OK)
  {
    fprintf(stderr, "Could not initialize the software updates client.\n");
    sample_state_destroy(&state);
    return 1;
  }

  /* First check, asked before open() so registration need not wait for it
   * (see ../simulated_regular). Library mode has nothing to resume. */
  if (az_iot_su_client_request_update(&state.su_client, request_timeout_ms) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.check_outstanding = true;

  printf(
      "Library mode. Matched against a deployed update: manufacturer=%s model=%s\n"
      "Installed: %s/%s/%s\n",
      state.manufacturer,
      state.model,
      state.installed_provider,
      state.installed_name,
      state.installed_version);

  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  uint64_t next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
  int rc = 0;
  while (!sample_is_stop_requested())
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    (void)az_iot_su_client_do_work(&state.su_client);
    advance_delegated_update(&state);

    /* FAULTED is settled: the SDK does not retry out of it. Delegated work in
     * flight is finished first; the SDK's own cannot progress without a
     * connection. */
    if (state.connection_is_faulted && state.update.phase == DELEGATED_UPDATE_PHASE_NONE)
    {
      fprintf(stderr, "The connection faulted and will not recover on its own.\n");
      rc = 1;
      break;
    }

    /* A check opens a new provisioning session and resends the owed final
     * report. Held while a check is outstanding: if that one resends it, the
     * report settles and WORKFLOW_COMPLETED disarms this. */
    if (state.owed_report_resend_ms != 0 && !state.check_outstanding
        && sample_now_ms() >= state.owed_report_resend_ms)
    {
      state.owed_report_resend_ms = 0;
      state.check_now = true;
    }

    /* One check at a time, none during a workflow, and none before the
     * service's requested delay. An outstanding check also resends an owed
     * report and asks for what follows it. */
    if (!state.su_workflow_active && !state.check_outstanding
        && sample_now_ms() >= state.check_not_before_ms
        && (state.check_now || (poll_interval_s > 0 && sample_now_ms() >= next_check_ms)))
    {
      state.check_now = false;
      next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
      if (az_iot_su_client_request_update(&state.su_client, request_timeout_ms) == AZ_IOT_OK)
      {
        state.check_outstanding = true;
      }
      else
      {
        fprintf(stderr, "Could not request an update check.\n");
      }
    }
  }

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100
       && (state.connection_state[AZ_IOT_CONN_SCOPE_DPS] != AZ_IOT_CONN_STATE_IDLE
           || state.connection_state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_IDLE);
       ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);
  return rc;
}
