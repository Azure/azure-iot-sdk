// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* software_update/pc/reboot_resume - Software Update sample: an update that
 * needs a reboot, and resuming it after the reboot.
 *
 * Portable PC sample. A step's install_fn or apply_fn may need the device to
 * restart before the update takes effect. The client then saves the workflow
 * through persist_state_fn; after the restart, az_iot_su_client_resume() reads
 * it back through load_state_fn and the workflow continues where it stopped.
 * This sample shows:
 *   - persist_state_fn / load_state_fn on a file, written atomically
 *     (temporary file, then rename), and erased on a zero-length write;
 *   - install_fn or apply_fn returning AZ_IOT_SU_RESULT_REBOOT_REQUIRED at a
 *     chosen step; the "reboot" is the process exiting, and the next run is
 *     the device starting again;
 *   - resume() before the first update check, and no check while a resumed
 *     workflow is still running;
 *   - AZ_IOT_SU_EVENT_PERSIST_FAILED / _RECOVERED while the state file cannot
 *     be written: the workflow is held, so the device must not reboot;
 *   - step result details set before the reboot and reported after it;
 *   - a reboot after the last apply: the workflow has succeeded, and a new
 *     update is deferred (AZ_IOT_SU_EVENT_UPDATE_DEFERRED) until the reboot.
 * Download and the file read-back are simulated (../common/su_sim.c, zero-
 * filled payloads); install and apply only log.
 *
 * Environment variables (all optional unless noted):
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID, AZ_IOT_CLIENT_CERT,
 *   AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA (required) and
 *   AZ_IOT_DPS_GLOBAL_ENDPOINT     the device; see the README.
 *   AZ_IOT_SU_MANUFACTURER, AZ_IOT_SU_MODEL   compatibility (Contoso/SU-Reboot)
 *   AZ_IOT_SU_INSTALLED_PROVIDER, _NAME, _VERSION   installed at the first start
 *                                  (Contoso/SU-Reboot/1.0.0)
 *   SAMPLE_REBOOT_AT=<phase>:<n>   install:<n> or apply:<n>, 1-based step;
 *                                  default install:1
 *   SAMPLE_STATE_FILE=<path>       saved workflow (default ./su_reboot_state.bin)
 *   SAMPLE_PERSIST_FAIL_COUNT=<n>  fail the first n state writes
 *   AZ_IOT_SU_EXIT_AFTER_WORKFLOW=1   exit once the final report is accepted
 *                                  (exit code 0 on success, 2 on failure)
 *   AZ_IOT_SU_POLL_INTERVAL_S      seconds between checks (default 14400)
 *   AZ_IOT_SU_LOG_LEVEL            trace|debug|info|warn|error|off
 */
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo)
{
  (void)signo;
  g_stop = 1;
}

/** @brief What the hooks share; passed to them as user_ctx. */
typedef struct
{
  az_iot_su_client* su; /* to set step result details */
  su_simulation_control sim; /* download and read-back only (../common/su_sim.c) */
  char* state_file; /* owned */
  int reboot_on_apply; /* 0: install_fn asks, 1: apply_fn asks */
  long reboot_step; /* 1-based */
  long persist_failures_left;
  int reboot_requested; /* a hook asked for a reboot in this run */
  /* Set by reboot_persist() itself: PERSIST_FAILED is raised only once per
   * episode, so a later failure in it would go unseen. */
  int write_failing; /* the last record write failed */
  int erase_failing; /* the last erase failed */
  int persist_gave_up; /* the client stopped retrying the failing write */
} reboot_context;

/** @brief Set the resultDetails reported for @p step. */
static void set_details(reboot_context* c, uint32_t step, const char* text)
{
  az_iot_result r
      = az_iot_su_client_set_step_result_details(c->su, step, az_span_create_from_str((char*)text));
  if (r != AZ_IOT_OK)
  {
    fprintf(stderr, "Step %u: details not kept: %s\n", step, az_iot_result_to_string(r));
  }
}

/** @brief Whether this hook call is the one SAMPLE_REBOOT_AT names. */
static int reboot_here(const reboot_context* c, int on_apply, uint32_t step)
{
  return c->reboot_on_apply == on_apply && (uint32_t)(c->reboot_step - 1) == step;
}

static int32_t reboot_download(
    const az_iot_su_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx)
{
  return su_download(file, url, file_index, file_count, &((reboot_context*)user_ctx)->sim);
}

static int32_t reboot_read_file(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* user_ctx)
{
  return su_read_file(
      file, file_index, offset, buffer, buffer_size, out_read, &((reboot_context*)user_ctx)->sim);
}

/** @brief install_fn: may need a reboot before the step can be applied. */
static int32_t reboot_install(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  reboot_context* c = (reboot_context*)user_ctx;
  if (reboot_here(c, 0, step))
  {
    printf("Step %u: installed; a reboot activates it\n", step);
    set_details(c, step, "installed into the inactive slot; activated by reboot");
    c->reboot_requested = 1;
    return AZ_IOT_SU_RESULT_REBOOT_REQUIRED;
  }
  printf("Step %u: installed\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief apply_fn: may need a reboot after the step is applied. */
static int32_t reboot_apply(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  reboot_context* c = (reboot_context*)user_ctx;
  if (reboot_here(c, 1, step))
  {
    printf("Step %u: applied; takes effect after a reboot\n", step);
    set_details(c, step, "applied; takes effect after reboot");
    c->reboot_requested = 1;
    return AZ_IOT_SU_RESULT_REBOOT_REQUIRED;
  }
  printf("Step %u: applied\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief Record a write's result in @p c (erase or record write); returns @p r. */
static int32_t persist_result(reboot_context* c, size_t len, int32_t r)
{
  int failed = r != AZ_IOT_SU_RESULT_SUCCESS;
  if (len == 0)
  {
    c->erase_failing = failed;
  }
  else
  {
    c->write_failing = failed;
    if (!failed)
    {
      c->erase_failing = 0; /* the new record replaces the one not erased */
    }
  }
  return r;
}

/**
 * @brief persist_state_fn: save the workflow so it survives a reboot; records
 * whether the write failed.
 *
 * Written to a temporary file and renamed over the old one, so a crash
 * mid-write leaves the previous record intact. A zero-length write erases
 * the record: the workflow ended and must not be resumed.
 */
static int32_t reboot_persist(const uint8_t* blob, size_t len, void* user_ctx)
{
  reboot_context* c = (reboot_context*)user_ctx;
  if (len == 0)
  {
    if (remove(c->state_file) != 0 && errno != ENOENT)
    {
      return persist_result(c, len, AZ_IOT_SU_RESULT_FAILURE);
    }
    printf("[persist] erased %s\n", c->state_file);
    return persist_result(c, len, AZ_IOT_SU_RESULT_SUCCESS);
  }
  if (c->persist_failures_left > 0)
  {
    c->persist_failures_left--;
    printf("[persist] simulated write failure\n");
    return persist_result(c, len, AZ_IOT_SU_RESULT_FAILURE);
  }
  char tmp[512];
  int tmp_len = snprintf(tmp, sizeof(tmp), "%s.tmp", c->state_file);
  if (tmp_len < 0 || (size_t)tmp_len >= sizeof(tmp))
  {
    return persist_result(c, len, AZ_IOT_SU_RESULT_FAILURE); /* the old record stays */
  }
  FILE* f = sample_fopen_private(tmp, true);
  if (f == NULL)
  {
    return persist_result(c, len, AZ_IOT_SU_RESULT_FAILURE);
  }
  size_t written = fwrite(blob, 1, len, f);
  int closed = fclose(f); /* reports a delayed write failure */
  if (written != len || closed != 0)
  {
    (void)remove(tmp);
    return persist_result(c, len, AZ_IOT_SU_RESULT_FAILURE);
  }
#ifdef _WIN32
  /* rename() does not replace on Windows. */
  if (!MoveFileExA(tmp, c->state_file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
#else
  if (rename(tmp, c->state_file) != 0)
#endif
  {
    (void)remove(tmp);
    return persist_result(c, len, AZ_IOT_SU_RESULT_FAILURE);
  }
  printf("[persist] %zu bytes -> %s\n", len, c->state_file);
  return persist_result(c, len, AZ_IOT_SU_RESULT_SUCCESS);
}

/** @brief Whether the stored record may be missing or stale. */
static int persist_failing(const reboot_context* c) { return c->write_failing || c->erase_failing; }

/** @brief load_state_fn: read the saved workflow back; nonzero when there is none. */
static int32_t reboot_load(uint8_t* blob, size_t capacity, size_t* out_len, void* user_ctx)
{
  reboot_context* c = (reboot_context*)user_ctx;
  FILE* f = fopen(c->state_file, "rb");
  if (f == NULL)
  {
    return 1;
  }
  size_t n = fread(blob, 1, capacity, f);
  int more = fgetc(f) != EOF; /* larger than the client can take: ignore it */
  int failed = ferror(f);
  (void)fclose(f);
  if (more || failed)
  {
    return 1;
  }
  *out_len = n;
  printf("[load] %zu bytes <- %s\n", n, c->state_file);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* observers                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct
{
  int workflow_active;
  int final_report; /* 0 none yet, 1 succeeded, 2 another outcome */
  int awaiting_final_report; /* a workflow started; its final report is not accepted yet */
  int final_report_dropped; /* the client retired the final report unaccepted */
  int resend_final_report; /* a lost session left the final report owed */
  int dropped_succeeded; /* the dropped final report was a succeeded one */
  int installed_from_dropped; /* installed_pending came from a dropped report */
  int installed_unadvertised; /* a dropped report's new installed id is not advertised */
  int connection_faulted;
  /* A succeeded report's installed update id, not yet in the device properties. */
  int installed_pending;
  char installed_provider[SU_SIM_ID_PART_SIZE];
  char installed_name[SU_SIM_ID_PART_SIZE];
  char installed_version[SU_SIM_ID_PART_SIZE];
} sample_flags;

typedef struct
{
  sample_flags* flags;
  reboot_context* reboot;
} observer_context;

static void on_connection_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_flags* flags = (sample_flags*)user_ctx;
  printf(
      "%s: %s (%s)\n",
      event->scope == AZ_IOT_CONN_SCOPE_DPS ? "Provisioning" : "Hub",
      sample_connection_state_name(event->state),
      az_iot_result_to_string(event->reason));
  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    flags->connection_faulted = 1;
  }
}

/* Notes a succeeded terminal report's installed update id for the device properties. */
static void note_installed(sample_flags* flags, const az_iot_su_report* report)
{
  /* Copied: the report is valid only during the callback. */
  const az_iot_su_report_update_id* id = report->installed_update_id;
  if (report->outcome == AZ_IOT_SU_OUTCOME_SUCCEEDED && id != NULL)
  {
    flags->installed_pending
        = sample_copy_str(
              flags->installed_provider, sizeof(flags->installed_provider), id->provider)
        && sample_copy_str(flags->installed_name, sizeof(flags->installed_name), id->name)
        && sample_copy_str(flags->installed_version, sizeof(flags->installed_version), id->version);
  }
}

static void on_su_event(const az_iot_su_event* event, void* user_ctx)
{
  observer_context* o = (observer_context*)user_ctx;
  switch (event->kind)
  {
    case AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED:
      /* Also raised by resume() for a restored workflow. */
      printf(
          "Update workflow: %s -> %s\n",
          su_sample_state_name(event->previous_state),
          su_sample_state_name(event->state));
      o->flags->workflow_active = (event->state != AZ_IOT_SU_STATE_IDLE);
      if (event->previous_state == AZ_IOT_SU_STATE_IDLE && event->state != AZ_IOT_SU_STATE_IDLE)
      {
        o->flags->final_report = 0; /* a new workflow: its report is not in yet */
        o->flags->awaiting_final_report = 1;
        o->flags->final_report_dropped = 0;
        o->flags->dropped_succeeded = 0;
      }
      if (event->state == AZ_IOT_SU_STATE_FAILED)
      {
        o->reboot->reboot_requested = 0; /* nothing left to activate */
      }
      break;
    case AZ_IOT_SU_EVENT_PERSIST_FAILED:
      /* The workflow is held at the reboot boundary until a write succeeds. */
      o->reboot->persist_gave_up = !event->persist_retrying;
      fprintf(
          stderr,
          "State not saved (%u failed writes)%s; not rebooting\n",
          (unsigned)event->persist_attempts,
          event->persist_retrying ? ", retrying" : ", giving up");
      break;
    case AZ_IOT_SU_EVENT_PERSIST_RECOVERED:
      o->reboot->persist_gave_up = 0;
      printf("State saved after %u failed writes\n", (unsigned)event->persist_attempts);
      break;
    case AZ_IOT_SU_EVENT_UPDATE_DEFERRED:
      /* In Idle: the last apply asked for a reboot, which has not happened yet. */
      printf(
          "New update deferred until %s\n",
          event->state == AZ_IOT_SU_STATE_IDLE ? "the device reboots" : "the current one ends");
      break;
    case AZ_IOT_SU_EVENT_REPORT_ACCEPTED:
      if (event->report != NULL)
      {
        printf("Final report accepted: outcome %d\n", (int)event->report->outcome);
        for (int32_t i = 0; i < event->report->step_results_count; ++i)
        {
          const az_iot_su_step_result* s = &event->report->step_results[i];
          printf(
              "  step_%d: outcome %d, resultCode %d, details \"%.*s\"\n",
              (int)i,
              (int)s->outcome,
              (int)s->result_code,
              (int)az_span_size(s->result_details),
              az_span_size(s->result_details) > 0 ? (const char*)az_span_ptr(s->result_details)
                                                  : "");
        }
        o->flags->final_report = (event->report->outcome == AZ_IOT_SU_OUTCOME_SUCCEEDED) ? 1 : 2;
        o->flags->awaiting_final_report = 0;
        note_installed(o->flags, event->report);
        o->flags->installed_from_dropped = 0;
      }
      break;
    case AZ_IOT_SU_EVENT_UPDATE_RETRY_WAIT:
      /* A later update check offers it again; it is retried once the wait elapses. */
      printf(
          "Failed update offered again; retry in %u s.\n",
          (unsigned)(event->retry_after_ms / 1000u));
      break;
    case AZ_IOT_SU_EVENT_OPERATION_ABANDONED:
      fprintf(
          stderr,
          "%s abandoned: %s (service code %d)\n",
          su_sample_operation_name(event->operation),
          az_iot_result_to_string(event->reason),
          (int)event->service_error.code);
      /* A lost session keeps the final report owed: send it again. Any other
       * verdict retires it; the update it reports is still installed. */
      if (event->operation == AZ_IOT_SU_OP_REPORT_STATUS && event->report_terminal)
      {
        if (event->reason == AZ_IOT_ERR_NOT_CONNECTED)
        {
          o->flags->resend_final_report = 1;
          fprintf(stderr, "Final report not delivered; sending it again.\n");
        }
        else
        {
          o->flags->awaiting_final_report = 0;
          o->flags->final_report_dropped = 1;
          o->flags->dropped_succeeded
              = event->report != NULL && event->report->outcome == AZ_IOT_SU_OUTCOME_SUCCEEDED;
          if (event->report != NULL)
          {
            note_installed(o->flags, event->report);
            o->flags->installed_from_dropped = o->flags->installed_pending;
          }
          fprintf(stderr, "Final report dropped; it is not sent again.\n");
        }
      }
      break;
    case AZ_IOT_SU_EVENT_NO_UPDATE:
      printf("No update available.\n");
      break;
    case AZ_IOT_SU_EVENT_UPDATE_REFUSED:
    case AZ_IOT_SU_EVENT_UPDATE_DUPLICATE:
      printf(
          "Software updates event %d: %s\n",
          (int)event->kind,
          az_iot_result_to_string(event->reason));
      break;
  }
}

/* The software updates client's memory, sized for the limits main() sets;
 * static, as it is too large for a stack frame. */
static uint8_t g_su_workspace[AZ_IOT_SU_WORKSPACE_DEFAULT_SIZE];

int main(void)
{
  az_iot_log_sink log = sample_log_sink(su_sample_log_level_from_env());
  az_iot_log_set_global_sink(&log);
  (void)signal(SIGINT, on_sigint);

  int rc = 1;
  sample_config config = { 0 };
  sample_flags flags = { 0 };
  reboot_context reboot = { 0 };
  observer_context observer = { &flags, &reboot };
  az_iot_certificate_provider_pem certs = { 0 };
  az_iot_connection_client connection;
  az_iot_su_client su;
  az_iot_mqtt_factory* factory_v3 = NULL;
  az_iot_mqtt_factory* factory_v5 = NULL;
  int connection_ready = 0;
  int su_ready = 0;
  char installed_provider[SU_SIM_ID_PART_SIZE];
  char installed_name[SU_SIM_ID_PART_SIZE];
  char installed_version[SU_SIM_ID_PART_SIZE];
  char* manufacturer = sample_env_dup("AZ_IOT_SU_MANUFACTURER", "Contoso");
  char* model = sample_env_dup("AZ_IOT_SU_MODEL", "SU-Reboot");
  char* reboot_at = sample_env_dup("SAMPLE_REBOOT_AT", "install:1");

  reboot.su = &su;
  reboot.state_file = sample_env_dup("SAMPLE_STATE_FILE", "./su_reboot_state.bin");
  reboot.persist_failures_left = su_sample_env_long("SAMPLE_PERSIST_FAIL_COUNT", 0);
  int exit_after_workflow = su_sample_env_flag("AZ_IOT_SU_EXIT_AFTER_WORKFLOW");
  long poll_interval_s = su_sample_env_long("AZ_IOT_SU_POLL_INTERVAL_S", 14400);
  if (manufacturer == NULL || model == NULL || reboot_at == NULL || reboot.state_file == NULL
      || poll_interval_s < 0
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_PROVIDER", "Contoso", installed_provider, sizeof(installed_provider))
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_NAME", "SU-Reboot", installed_name, sizeof(installed_name))
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_VERSION", "1.0.0", installed_version, sizeof(installed_version))
      || sample_config_load(&config) != 0)
  {
    goto cleanup;
  }
  const char* colon = strchr(reboot_at, ':');
  reboot.reboot_on_apply = (strncmp(reboot_at, "apply", 5) == 0);
  reboot.reboot_step = (colon != NULL) ? strtol(colon + 1, NULL, 10) : 1;

  /* Certificate provider and connection client. */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = config.ca;
  pem.client_cert_pem_path = config.cert;
  pem.client_key_pem_path = config.key;
  if (az_iot_certificate_provider_pem_init(&certs, &pem) != AZ_IOT_OK)
  {
    goto cleanup;
  }
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = config.id_scope;
  copts.dps.registration_id = config.reg_id;
  copts.dps.global_endpoint = config.dps_global_endpoint; /* NULL: the global default */
  copts.certificate_provider = &certs.base;
#if defined(AZ_IOT_SAMPLE_CRYPTO_OPENSSL)
  copts.crypto = az_iot_crypto_openssl(); /* verifies manifests and file hashes */
#elif defined(AZ_IOT_SAMPLE_CRYPTO_MBEDTLS)
  copts.crypto = az_iot_crypto_mbedtls(); /* verifies manifests and file hashes */
#endif
  if (az_iot_connection_client_init(&connection, &copts) != AZ_IOT_OK)
  {
    goto cleanup;
  }
  connection_ready = 1;
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  factory_v3 = az_iot_paho_factory_create_v3_1_1();
  factory_v5 = az_iot_paho_factory_create_v5();
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  factory_v3 = az_iot_az_mqtt_factory_create_v3_1_1();
  factory_v5 = az_iot_az_mqtt_factory_create_v5();
#endif
  if (az_iot_connection_client_add_state_observer(&connection, on_connection_state, &flags)
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&connection, factory_v3) != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&connection, factory_v5) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  /* Software updates client, with the persistence hooks a reboot needs. */
  az_iot_su_platform_hooks hooks = { 0 };
  hooks.download_fn = reboot_download;
  hooks.read_file_fn = reboot_read_file;
  hooks.install_fn = reboot_install;
  hooks.apply_fn = reboot_apply;
  hooks.persist_state_fn = reboot_persist;
  hooks.load_state_fn = reboot_load;
  hooks.user_ctx = &reboot;

  az_iot_su_device_properties dp = { 0 };
  dp.manufacturer = manufacturer;
  dp.model = model;
  dp.installed_update_id.provider = installed_provider;
  dp.installed_update_id.name = installed_name;
  dp.installed_update_id.version = installed_version;

  size_t root_key_count = 0;
  const az_iot_su_root_key* root_keys = az_iot_su_microsoft_root_keys(&root_key_count);
  az_iot_su_client_config_options su_opts = az_iot_su_client_config_options_default();
  su_opts.hooks = &hooks;
  su_opts.root_keys = root_keys;
  su_opts.root_key_count = root_key_count;
  su_opts.device_properties = &dp;
  /* Limits, each the SDK default; the saved workflow is at most
   * AZ_IOT_SU_STATE_BLOB_MAX_SIZE(max_update_size, result_details_size) bytes. */
  su_opts.limits.max_update_size = AZ_IOT_SU_DEFAULT_MAX_UPDATE_SIZE;
  su_opts.limits.scratch_size = AZ_IOT_SU_VERIFY_SCRATCH_SIZE;
  su_opts.limits.max_message_size = AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE;
  su_opts.limits.device_properties_size = AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE;
  su_opts.limits.result_details_size = AZ_IOT_SU_DEFAULT_RESULT_DETAILS_SIZE;
  su_opts.workspace = g_su_workspace;
  su_opts.workspace_size = sizeof(g_su_workspace);
  if (az_iot_su_client_init(&su, &connection, &su_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "az_iot_su_client_init failed\n");
    goto cleanup;
  }
  su_ready = 1;
  /* Each check is bounded to half the poll interval: a new request resets the
   * deadline, so a longer bound would let an unanswered check outlive every
   * poll and never be abandoned. */
  uint32_t request_timeout_ms = AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS;
  if (poll_interval_s > 0 && (uint64_t)poll_interval_s * 500u < request_timeout_ms)
  {
    request_timeout_ms = (uint32_t)poll_interval_s * 500u;
  }

  /* Observe before resume(): it reports a restored workflow through events. */
  if (az_iot_su_client_add_observer(&su, on_su_event, &observer) != AZ_IOT_OK)
  {
    goto cleanup;
  }
  az_iot_result resumed = az_iot_su_client_resume(&su);
  if (resumed != AZ_IOT_OK)
  {
    fprintf(stderr, "Nothing resumed: %s\n", az_iot_result_to_string(resumed));
  }
  /* A restored final report settles before any check (see the loop); a
   * resumed workflow's start already set the flag. */
  flags.awaiting_final_report
      = flags.awaiting_final_report || az_iot_su_client_is_terminal_report_owed(&su);
  /* A resumed workflow finishes first: a check now could deliver another one. */
  int check_due = 0;
  if (az_iot_su_client_get_state(&su) == AZ_IOT_SU_STATE_IDLE)
  {
    check_due = flags.awaiting_final_report;
    if (!check_due && az_iot_su_client_request_update(&su, request_timeout_ms) != AZ_IOT_OK)
    {
      goto cleanup;
    }
  }
  if (az_iot_connection_client_open(&connection) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  uint64_t next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
  rc = 0;
  while (!g_stop)
  {
    (void)az_iot_connection_client_do_work(&connection, 20);
    (void)az_iot_su_client_do_work(&su);

    int report_settled
        = (flags.final_report != 0 || flags.final_report_dropped) && !flags.workflow_active;
    /* The final report may be accepted or dropped before a failed write is
     * retried. After a success the client has nothing left to save, so any
     * record left is stale: erase it, and reboot or exit only once that
     * succeeds. Another outcome's write keeps its retry schedule: wait for it,
     * unless the client gave up on it. */
    int succeeded = flags.final_report == 1 || flags.dropped_succeeded;
    if (report_settled && persist_failing(&reboot) && (succeeded || reboot.persist_gave_up)
        && reboot_persist(NULL, 0, &reboot) == AZ_IOT_SU_RESULT_SUCCESS)
    {
      reboot.write_failing = 0; /* the record it was for is no longer needed */
    }
    /* The "reboot": exit once the state is saved. While writes fail the
     * workflow is held, and rebooting would lose it. */
    if (reboot.reboot_requested && !persist_failing(&reboot))
    {
      if (exit_after_workflow && report_settled)
      {
        rc = (flags.final_report == 1) ? 0 : 2;
      }
      printf(
          "Rebooting: the workflow is saved in %s. Run the sample again to resume.\n",
          reboot.state_file);
      break;
    }
    /* After a succeeded update, checks advertise what it installed. */
    /* A repeated report of the same update changes nothing: refreshing again
     * would send that report once more. */
    if (flags.installed_pending && strcmp(flags.installed_provider, installed_provider) == 0
        && strcmp(flags.installed_name, installed_name) == 0
        && strcmp(flags.installed_version, installed_version) == 0)
    {
      flags.installed_pending = 0;
    }
    /* A new id from a dropped report is not advertised: that would send the
     * dropped report again. */
    if (flags.installed_from_dropped)
    {
      flags.installed_from_dropped = 0;
      flags.installed_unadvertised = flags.installed_unadvertised || flags.installed_pending;
      flags.installed_pending = 0;
    }
    /* Sending the device properties again also re-sends an owed report. */
    if (flags.resend_final_report)
    {
      flags.resend_final_report = 0;
      if (az_iot_su_client_update_device_properties(&su, &dp) != AZ_IOT_OK)
      {
        fprintf(stderr, "Could not send the final report again.\n");
      }
    }
    if (flags.installed_pending)
    {
      flags.installed_pending = 0;
      az_iot_su_device_properties next = dp;
      next.installed_update_id.provider = flags.installed_provider;
      next.installed_update_id.name = flags.installed_name;
      next.installed_update_id.version = flags.installed_version;
      if (az_iot_su_client_update_device_properties(&su, &next) != AZ_IOT_OK)
      {
        fprintf(stderr, "Could not record the applied update as installed; not checking again.\n");
        poll_interval_s = 0;
        check_due = 0;
      }
      else
      {
        /* Same capacity, so the copies fit. */
        (void)sample_copy_str(
            installed_provider, sizeof(installed_provider), flags.installed_provider);
        (void)sample_copy_str(installed_name, sizeof(installed_name), flags.installed_name);
        (void)sample_copy_str(
            installed_version, sizeof(installed_version), flags.installed_version);
        printf("Installed: %s/%s/%s\n", installed_provider, installed_name, installed_version);
      }
    }
    if (exit_after_workflow && report_settled && !persist_failing(&reboot)
        && !reboot.reboot_requested)
    {
      rc = (flags.final_report == 1) ? 0 : 2;
      break;
    }
    /* FAULTED is settled: the SDK does not retry out of it, and a workflow in
     * flight cannot report again, so it would never reach Idle. */
    if (flags.connection_faulted)
    {
      fprintf(
          stderr,
          flags.workflow_active
              ? "The connection faulted and will not recover on its own; the update in progress "
                "stops here (a saved record, if any, resumes it on the next run).\n"
              : "The connection faulted and will not recover on its own.\n");
      rc = 1;
      break;
    }
    /* Until the final report is accepted, a check would advertise the
     * previous installed id and could be offered the same update again. */
    if (flags.installed_unadvertised && (poll_interval_s > 0 || check_due))
    {
      fprintf(stderr, "The applied update's id is not advertised; not checking again.\n");
      poll_interval_s = 0;
      check_due = 0;
    }
    if (!flags.workflow_active && !flags.awaiting_final_report
        && (check_due || (poll_interval_s > 0 && sample_now_ms() >= next_check_ms)))
    {
      check_due = 0;
      next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
      if (az_iot_su_client_request_update(&su, request_timeout_ms) != AZ_IOT_OK)
      {
        fprintf(stderr, "Could not request an update check.\n");
      }
    }
  }

  az_iot_connection_client_close(&connection);
  for (int i = 0; i < 100; ++i)
  {
    (void)az_iot_connection_client_do_work(&connection, 50);
  }

cleanup:
  if (su_ready)
  {
    az_iot_su_client_deinit(&su);
  }
  if (connection_ready)
  {
    az_iot_connection_client_deinit(&connection);
  }
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  az_iot_paho_factory_destroy(factory_v3);
  az_iot_paho_factory_destroy(factory_v5);
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  az_iot_az_mqtt_factory_destroy(factory_v3);
  az_iot_az_mqtt_factory_destroy(factory_v5);
#endif
  az_iot_certificate_provider_pem_deinit(&certs);
  sample_config_release(&config);
  free(reboot.state_file);
  free(manufacturer);
  free(model);
  free(reboot_at);
  return rc;
}
