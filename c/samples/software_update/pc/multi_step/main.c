// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* software_update/pc/multi_step - Software Update sample: a multi-step update
 * and the result of each step.
 *
 * Portable PC sample. Runs a deployment whose manifest has several steps and
 * shows what the device reports for each one:
 *   - backup_fn / install_fn / apply_fn / restore_fn per step, with a backup
 *     record per step and a rollback in reverse order when a step fails;
 *   - device failure codes (0x100 and up), which land in the low bits of the
 *     failing step's extendedResultCodes;
 *   - az_iot_su_client_set_step_result_details(), the human-readable
 *     resultDetails of each step;
 *   - the terminal report, step by step, from AZ_IOT_SU_EVENT_REPORT_ACCEPTED.
 * Download and the file read-back are simulated (../common/su_sim.c serves
 * zero-filled bytes, so the import must declare zero-filled files); install,
 * apply, backup and restore only log.
 *
 * The device asks on the regular route, so it registers and connects to its
 * IoT Hub, as ../simulated_regular does.
 *
 * Environment variables (all optional unless noted):
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID, AZ_IOT_CLIENT_CERT,
 *   AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA (required) and
 *   AZ_IOT_DPS_GLOBAL_ENDPOINT     the device; see the README.
 *   AZ_IOT_SU_MANUFACTURER, AZ_IOT_SU_MODEL   compatibility (Contoso/SU-MultiStep)
 *   AZ_IOT_SU_INSTALLED_PROVIDER, _NAME, _VERSION   installed now
 *                                  (Contoso/SU-MultiStep/1.0.0)
 *   SAMPLE_FAIL_STEP=<n>           1-based step that fails; 0 (default) none
 *   SAMPLE_FAIL_PHASE=<p>          backup|install|apply (default install)
 *   SAMPLE_RESTORE_FAILS=1         restore_fn fails too
 *   AZ_IOT_SU_EXIT_AFTER_WORKFLOW=1   exit once the final report is accepted
 *                                  (exit code 0 on success, 2 on failure)
 *   AZ_IOT_SU_POLL_INTERVAL_S      seconds between checks (default 14400)
 *   AZ_IOT_SU_LOG_LEVEL            trace|debug|info|warn|error|off
 */
#include <signal.h>
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

/** @brief Device failure sub-codes, from 0x100 so they never read as an
 * AZ_IOT_SU_RESULT_* value. Reported in the low bits of the failing step's
 * extended result code, below the SDK's facility (bits 31..28). */
#define SAMPLE_ERR_BACKUP 0x101
#define SAMPLE_ERR_INSTALL 0x102
#define SAMPLE_ERR_APPLY 0x103
#define SAMPLE_ERR_RESTORE 0x104

/** @brief Steps this sample keeps a backup record for. */
#define SAMPLE_MAX_STEPS 8

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo)
{
  (void)signo;
  g_stop = 1;
}

/** @brief Which phase SAMPLE_FAIL_STEP fails in. */
typedef enum
{
  FAIL_BACKUP,
  FAIL_INSTALL,
  FAIL_APPLY
} fail_phase;

/** @brief What the hooks share; passed to them as user_ctx. */
typedef struct
{
  az_iot_su_client* su; /* to set each step's result details */
  su_simulation_control sim; /* download and read-back only (../common/su_sim.c) */
  long fail_step; /* 1-based; 0 = none */
  fail_phase fail_phase;
  int restore_fails;
  int backed_up[SAMPLE_MAX_STEPS]; /* a backup exists for the step */

  /* What is installed now; replaced by the last step's apply. */
  char installed_provider[SU_SIM_ID_PART_SIZE];
  char installed_name[SU_SIM_ID_PART_SIZE];
  char installed_version[SU_SIM_ID_PART_SIZE];
} step_context;

/** @brief Set the resultDetails reported for @p step. */
static void set_details(step_context* c, uint32_t step, const char* text)
{
  az_iot_result r
      = az_iot_su_client_set_step_result_details(c->su, step, az_span_create_from_str((char*)text));
  if (r != AZ_IOT_OK)
  {
    fprintf(stderr, "Step %u: details not kept: %s\n", step, az_iot_result_to_string(r));
  }
}

/** @brief True when SAMPLE_FAIL_STEP / SAMPLE_FAIL_PHASE select @p step and @p phase. */
static int fails_here(const step_context* c, uint32_t step, fail_phase phase)
{
  return c->fail_step > 0 && (uint32_t)(c->fail_step - 1) == step && c->fail_phase == phase;
}

static int32_t step_download(
    const az_iot_su_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx)
{
  return su_download(file, url, file_index, file_count, &((step_context*)user_ctx)->sim);
}

static int32_t step_read_file(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* user_ctx)
{
  return su_read_file(
      file, file_index, offset, buffer, buffer_size, out_read, &((step_context*)user_ctx)->sim);
}

static int32_t step_is_installed(const az_iot_su_client_update_manifest* manifest, void* user_ctx)
{
  step_context* c = (step_context*)user_ctx;
  return (az_span_is_content_equal(
              manifest->update_id.provider, az_span_create_from_str(c->installed_provider))
          && az_span_is_content_equal(
              manifest->update_id.name, az_span_create_from_str(c->installed_name))
          && az_span_is_content_equal(
              manifest->update_id.version, az_span_create_from_str(c->installed_version)))
      ? AZ_IOT_SU_RESULT_ALREADY_INSTALLED
      : AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief backup_fn: keep what this step will replace, so it can be restored. */
static int32_t step_backup(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  step_context* c = (step_context*)user_ctx;
  printf(
      "Step %u (%.*s): backup\n",
      step,
      (int)az_span_size(manifest->instructions.steps[step].handler),
      (const char*)az_span_ptr(manifest->instructions.steps[step].handler));
  if (fails_here(c, step, FAIL_BACKUP))
  {
    set_details(c, step, "backup: not enough space for the backup");
    return SAMPLE_ERR_BACKUP;
  }
  if (step < SAMPLE_MAX_STEPS)
  {
    c->backed_up[step] = 1;
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief install_fn: write this step's payload. */
static int32_t step_install(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  step_context* c = (step_context*)user_ctx;
  printf("Step %u: install\n", step);
  if (fails_here(c, step, FAIL_INSTALL))
  {
    set_details(c, step, "install: payload rejected by the target component");
    return SAMPLE_ERR_INSTALL;
  }
  set_details(c, step, "installed");
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief apply_fn: activate this step; the last one makes the update installed. */
static int32_t step_apply(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  step_context* c = (step_context*)user_ctx;
  printf("Step %u: apply\n", step);
  if (fails_here(c, step, FAIL_APPLY))
  {
    set_details(c, step, "apply: component did not start");
    return SAMPLE_ERR_APPLY;
  }
  set_details(c, step, "installed and applied");
  if (step + 1 == manifest->instructions.steps_count)
  {
    (void)snprintf(
        c->installed_provider,
        sizeof(c->installed_provider),
        "%.*s",
        (int)az_span_size(manifest->update_id.provider),
        (const char*)az_span_ptr(manifest->update_id.provider));
    (void)snprintf(
        c->installed_name,
        sizeof(c->installed_name),
        "%.*s",
        (int)az_span_size(manifest->update_id.name),
        (const char*)az_span_ptr(manifest->update_id.name));
    (void)snprintf(
        c->installed_version,
        sizeof(c->installed_version),
        "%.*s",
        (int)az_span_size(manifest->update_id.version),
        (const char*)az_span_ptr(manifest->update_id.version));
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/**
 * @brief restore_fn: put back what @p step replaced.
 *
 * Called in reverse step order after a failure, for every step that was
 * backed up. The failing step keeps its own result; a restore failure is
 * added to the overall result by the SDK.
 */
static int32_t step_restore(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  step_context* c = (step_context*)user_ctx;
  printf("Step %u: restore from backup\n", step);
  if (step < SAMPLE_MAX_STEPS)
  {
    c->backed_up[step] = 0;
  }
  return c->restore_fails ? SAMPLE_ERR_RESTORE : AZ_IOT_SU_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* observers                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct
{
  int workflow_active;
  int workflow_succeeded;
  int final_report; /* 0 none yet, 1 succeeded, 2 another outcome */
  int connection_faulted;
} sample_flags;

static const char* outcome_name(az_iot_su_outcome outcome)
{
  switch (outcome)
  {
    case AZ_IOT_SU_OUTCOME_IN_PROGRESS:
      return "IN_PROGRESS";
    case AZ_IOT_SU_OUTCOME_SUCCEEDED:
      return "SUCCEEDED";
    case AZ_IOT_SU_OUTCOME_FAILED:
      return "FAILED";
    case AZ_IOT_SU_OUTCOME_CANCELED:
      return "CANCELED";
    case AZ_IOT_SU_OUTCOME_SKIPPED:
      return "SKIPPED";
  }
  return "?";
}

/** @brief Print the terminal report: overall, then each step. */
static void print_report(const az_iot_su_report* report)
{
  printf(
      "Final report for workflow %s: %s, resultCode %d, extendedResultCodes %s\n",
      report->workflow_id,
      outcome_name(report->outcome),
      (int)report->result_code,
      report->extended_result_codes);
  for (int32_t i = 0; i < report->step_results_count; ++i)
  {
    const az_iot_su_step_result* s = &report->step_results[i];
    uint32_t ext = (uint32_t)s->extended_result_code;
    printf(
        "  step_%d: %s, resultCode %d, facility 0x%x, sub-code 0x%x, details \"%.*s\"\n",
        (int)i,
        outcome_name(s->outcome),
        (int)s->result_code,
        (unsigned)(ext >> 28),
        (unsigned)(ext & 0x0FFFFFFFu),
        (int)az_span_size(s->result_details),
        az_span_size(s->result_details) > 0 ? (const char*)az_span_ptr(s->result_details) : "");
  }
}

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

static void on_su_event(const az_iot_su_event* event, void* user_ctx)
{
  sample_flags* flags = (sample_flags*)user_ctx;
  switch (event->kind)
  {
    case AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED:
      printf(
          "Update workflow: %s -> %s\n",
          su_sample_state_name(event->previous_state),
          su_sample_state_name(event->state));
      flags->workflow_active = (event->state != AZ_IOT_SU_STATE_IDLE);
      if (event->state == AZ_IOT_SU_STATE_IDLE
          && event->previous_state == AZ_IOT_SU_STATE_APPLY_STARTED)
      {
        flags->workflow_succeeded = 1;
      }
      break;
    case AZ_IOT_SU_EVENT_REPORT_ACCEPTED:
      /* The final report carries every step's result. */
      if (event->report != NULL)
      {
        print_report(event->report);
        flags->final_report = (event->report->outcome == AZ_IOT_SU_OUTCOME_SUCCEEDED) ? 1 : 2;
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
      break;
    case AZ_IOT_SU_EVENT_NO_UPDATE:
      printf("No update available.\n");
      break;
    case AZ_IOT_SU_EVENT_PERSIST_FAILED:
    case AZ_IOT_SU_EVENT_PERSIST_RECOVERED:
    case AZ_IOT_SU_EVENT_UPDATE_REFUSED:
    case AZ_IOT_SU_EVENT_UPDATE_DEFERRED:
    case AZ_IOT_SU_EVENT_UPDATE_DUPLICATE:
      printf(
          "Software updates event %d: %s\n",
          (int)event->kind,
          az_iot_result_to_string(event->reason));
      break;
  }
}

/* The software updates client's memory: update copy, persisted-state record,
 * scratch, message body, device-property strings and step details. Sized for
 * the limits main() sets; static, as it is too large for a stack frame. */
static uint8_t g_su_workspace[AZ_IOT_SU_WORKSPACE_DEFAULT_SIZE];

int main(void)
{
  az_iot_log_sink log = sample_log_sink(su_sample_log_level_from_env());
  az_iot_log_set_global_sink(&log);
  (void)signal(SIGINT, on_sigint);

  int rc = 1;
  sample_config config = { 0 };
  sample_flags flags = { 0 };
  step_context steps = { 0 };
  az_iot_certificate_provider_pem certs = { 0 };
  az_iot_connection_client connection;
  az_iot_su_client su;
  az_iot_mqtt_factory* factory_v3 = NULL;
  az_iot_mqtt_factory* factory_v5 = NULL;
  int connection_ready = 0;
  int su_ready = 0;
  char* manufacturer = sample_env_dup("AZ_IOT_SU_MANUFACTURER", "Contoso");
  char* model = sample_env_dup("AZ_IOT_SU_MODEL", "SU-MultiStep");
  char* phase = sample_env_dup("SAMPLE_FAIL_PHASE", "install");

  steps.su = &su;
  steps.fail_step = su_sample_env_long("SAMPLE_FAIL_STEP", 0);
  steps.restore_fails = su_sample_env_flag("SAMPLE_RESTORE_FAILS");
  int exit_after_workflow = su_sample_env_flag("AZ_IOT_SU_EXIT_AFTER_WORKFLOW");
  long poll_interval_s = su_sample_env_long("AZ_IOT_SU_POLL_INTERVAL_S", 14400);
  if (manufacturer == NULL || model == NULL || phase == NULL || poll_interval_s < 0
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_PROVIDER",
          "Contoso",
          steps.installed_provider,
          sizeof(steps.installed_provider))
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_NAME",
          "SU-MultiStep",
          steps.installed_name,
          sizeof(steps.installed_name))
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_VERSION",
          "1.0.0",
          steps.installed_version,
          sizeof(steps.installed_version))
      || sample_config_load(&config) != 0)
  {
    goto cleanup;
  }
  steps.fail_phase = (strcmp(phase, "backup") == 0) ? FAIL_BACKUP
      : (strcmp(phase, "apply") == 0)               ? FAIL_APPLY
                                                    : FAIL_INSTALL;

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

  /* Software updates client: one hook per phase, each called per step. */
  az_iot_su_platform_hooks hooks = { 0 };
  hooks.download_fn = step_download;
  hooks.read_file_fn = step_read_file;
  hooks.is_installed_fn = step_is_installed;
  hooks.backup_fn = step_backup;
  hooks.install_fn = step_install;
  hooks.apply_fn = step_apply;
  hooks.restore_fn = step_restore;
  hooks.user_ctx = &steps;

  az_iot_su_device_properties dp = { 0 };
  dp.manufacturer = manufacturer;
  dp.model = model;
  dp.installed_update_id.provider = steps.installed_provider;
  dp.installed_update_id.name = steps.installed_name;
  dp.installed_update_id.version = steps.installed_version;

  size_t root_key_count = 0;
  const az_iot_su_root_key* root_keys = az_iot_su_microsoft_root_keys(&root_key_count);
  az_iot_su_client_config_options su_opts = az_iot_su_client_config_options_default();
  su_opts.hooks = &hooks;
  su_opts.root_keys = root_keys;
  su_opts.root_key_count = root_key_count;
  su_opts.device_properties = &dp;
  /* Limits, each the SDK default; the workspace is sized for them. */
  su_opts.limits.max_update_size = AZ_IOT_SU_DEFAULT_MAX_UPDATE_SIZE;
  su_opts.limits.scratch_size = AZ_IOT_SU_VERIFY_SCRATCH_SIZE;
  su_opts.limits.max_message_size = AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE;
  su_opts.limits.device_properties_size = AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE;
  su_opts.limits.result_details_size = AZ_IOT_SU_DEFAULT_RESULT_DETAILS_SIZE; /* per step */
  su_opts.workspace = g_su_workspace;
  su_opts.workspace_size = sizeof(g_su_workspace);
  if (az_iot_su_client_init(&su, &connection, &su_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "az_iot_su_client_init failed\n");
    goto cleanup;
  }
  su_ready = 1;
  if (az_iot_su_client_add_observer(&su, on_su_event, &flags) != AZ_IOT_OK
      || az_iot_su_client_request_update(&su, AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS) != AZ_IOT_OK
      || az_iot_connection_client_open(&connection) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  printf(
      "Matched against: manufacturer=%s model=%s. Installed: %s/%s/%s\n",
      manufacturer,
      model,
      steps.installed_provider,
      steps.installed_name,
      steps.installed_version);
  uint64_t next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
  rc = 0;
  while (!g_stop)
  {
    (void)az_iot_connection_client_do_work(&connection, 20);
    (void)az_iot_su_client_do_work(&su);

    if (exit_after_workflow && flags.final_report != 0 && !flags.workflow_active)
    {
      rc = (flags.final_report == 1) ? 0 : 2;
      break;
    }

    /* After an applied update, report it as installed and ask again at once. */
    int check_now = 0;
    if (flags.workflow_succeeded)
    {
      flags.workflow_succeeded = 0;
      if (az_iot_su_client_update_device_properties(&su, &dp) != AZ_IOT_OK)
      {
        fprintf(stderr, "Could not record the applied update as installed.\n");
      }
      check_now = 1;
    }
    if (flags.connection_faulted && !flags.workflow_active)
    {
      fprintf(stderr, "The connection faulted and will not recover on its own.\n");
      rc = 1;
      break;
    }
    if (!flags.workflow_active
        && (check_now || (poll_interval_s > 0 && sample_now_ms() >= next_check_ms)))
    {
      next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
      if (az_iot_su_client_request_update(&su, AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS) != AZ_IOT_OK)
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
  free(manufacturer);
  free(model);
  free(phase);
  return rc;
}
