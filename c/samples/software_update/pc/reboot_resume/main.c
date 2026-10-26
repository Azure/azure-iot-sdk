// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* software_update/pc/reboot_resume - Software Update sample: an update that
 * needs a reboot, and resuming it after the reboot.
 *
 * Portable PC sample. install_fn or apply_fn returns
 * AZ_IOT_SU_RESULT_REBOOT_REQUIRED at a chosen step; the client saves the
 * workflow through persist_state_fn. Once az_iot_su_client_get_reboot_state()
 * is READY the sample "reboots" (exits). The next run calls
 * az_iot_su_client_resume(), which reads the workflow back through
 * load_state_fn, and the workflow continues where it stopped. Download and
 * the file read-back are simulated (../common/su_sim.c, zero-filled
 * payloads); install and apply only log.
 *
 * Environment variables (all optional unless noted):
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID, AZ_IOT_CLIENT_CERT,
 *   AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA (required) and
 *   AZ_IOT_DPS_GLOBAL_ENDPOINT     the device; see the README.
 *   AZ_IOT_SU_MANUFACTURER, AZ_IOT_SU_MODEL   compatibility (Contoso/SU-Reboot)
 *   AZ_IOT_SU_INSTALLED_PROVIDER, _NAME, _VERSION   installed at startup
 *                                  (Contoso/SU-Reboot/1.0.0)
 *   SAMPLE_REBOOT_AT=<phase>:<n>   install:<n> or apply:<n>, 1-based step;
 *                                  default install:1
 *   SAMPLE_STATE_FILE=<path>       saved workflow (default ./su_reboot_state.bin)
 *   AZ_IOT_SU_EXIT_AFTER_WORKFLOW=1   exit once the workflow completes
 *                                  (0 when it succeeded and its report was
 *                                  accepted, 2 otherwise)
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
} update_hooks_context;

/** @brief Set the resultDetails reported for @p step. */
static void set_step_result_details(update_hooks_context* context, uint32_t step, const char* text)
{
  az_iot_result r = az_iot_su_client_set_step_result_details(
      context->su, step, az_span_create_from_str((char*)text));
  if (r != AZ_IOT_OK)
  {
    fprintf(stderr, "Step %u: details not kept: %s\n", step, az_iot_result_to_string(r));
  }
}

/** @brief Whether this hook call is the one SAMPLE_REBOOT_AT names. */
static int is_reboot_requested_at_step(
    const update_hooks_context* context,
    int on_apply,
    uint32_t step)
{
  return context->reboot_on_apply == on_apply && (uint32_t)(context->reboot_step - 1) == step;
}

static int32_t download_update_file(
    const az_iot_su_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx)
{
  return su_download(file, url, file_index, file_count, &((update_hooks_context*)user_ctx)->sim);
}

static int32_t read_back_downloaded_file(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* user_ctx)
{
  return su_read_file(
      file,
      file_index,
      offset,
      buffer,
      buffer_size,
      out_read,
      &((update_hooks_context*)user_ctx)->sim);
}

/** @brief install_fn: may need a reboot before the step can be applied. */
static int32_t install_update_step(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  update_hooks_context* context = (update_hooks_context*)user_ctx;
  if (is_reboot_requested_at_step(context, 0, step))
  {
    printf("Step %u: installed; a reboot activates it\n", step);
    set_step_result_details(context, step, "installed into the inactive slot; activated by reboot");
    return AZ_IOT_SU_RESULT_REBOOT_REQUIRED;
  }
  printf("Step %u: installed\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief apply_fn: may need a reboot after the step is applied. */
static int32_t apply_update_step(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  update_hooks_context* context = (update_hooks_context*)user_ctx;
  if (is_reboot_requested_at_step(context, 1, step))
  {
    printf("Step %u: applied; takes effect after a reboot\n", step);
    set_step_result_details(context, step, "applied; takes effect after reboot");
    return AZ_IOT_SU_RESULT_REBOOT_REQUIRED;
  }
  printf("Step %u: applied\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/**
 * @brief persist_state_fn: save the workflow so it survives a reboot.
 *
 * Written to a temporary file and renamed over the old one, so a crash
 * mid-write leaves the previous record intact. A zero-length write erases
 * the record: the workflow ended and must not be resumed.
 */
static int32_t save_update_workflow(const uint8_t* blob, size_t len, void* user_ctx)
{
  update_hooks_context* context = (update_hooks_context*)user_ctx;
  if (len == 0)
  {
    if (remove(context->state_file) != 0 && errno != ENOENT)
    {
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    printf("[persist] erased %s\n", context->state_file);
    return AZ_IOT_SU_RESULT_SUCCESS;
  }
  char tmp[512];
  int tmp_len = snprintf(tmp, sizeof(tmp), "%s.tmp", context->state_file);
  if (tmp_len < 0 || (size_t)tmp_len >= sizeof(tmp))
  {
    return AZ_IOT_SU_RESULT_FAILURE; /* the old record stays */
  }
  FILE* f = sample_fopen_private(tmp, true);
  if (f == NULL)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  size_t written = fwrite(blob, 1, len, f);
  int closed = fclose(f); /* reports a delayed write failure */
  if (written != len || closed != 0)
  {
    (void)remove(tmp);
    return AZ_IOT_SU_RESULT_FAILURE;
  }
#ifdef _WIN32
  /* rename() does not replace on Windows. */
  if (!MoveFileExA(tmp, context->state_file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
#else
  if (rename(tmp, context->state_file) != 0)
#endif
  {
    (void)remove(tmp);
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  printf("[persist] %zu bytes -> %s\n", len, context->state_file);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/** @brief load_state_fn: read the saved workflow back; nonzero when there is none. */
static int32_t load_saved_update_workflow(
    uint8_t* blob,
    size_t capacity,
    size_t* out_len,
    void* user_ctx)
{
  update_hooks_context* context = (update_hooks_context*)user_ctx;
  FILE* f = sample_fopen_read_nofollow(context->state_file);
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
  printf("[load] %zu bytes <- %s\n", n, context->state_file);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* observers                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct
{
  int completed; /* 0 not yet, 1 succeeded and accepted, 2 another ending */
  int connection_faulted;
} sample_flags;

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
      /* Also raised by resume() for a restored workflow. */
      printf(
          "Update workflow: %s -> %s\n",
          su_sample_state_name(event->previous_state),
          su_sample_state_name(event->state));
      if (event->previous_state == AZ_IOT_SU_STATE_IDLE)
      {
        flags->completed = 0;
      }
      break;
    case AZ_IOT_SU_EVENT_WORKFLOW_COMPLETED:
    {
      const az_iot_su_report* r = event->report;
      printf(
          "Workflow completed: outcome %d; final report %s\n",
          r != NULL ? (int)r->outcome : -1,
          event->reason == AZ_IOT_OK ? "accepted" : az_iot_result_to_string(event->reason));
      for (int32_t i = 0; r != NULL && i < r->step_results_count; ++i)
      {
        const az_iot_su_step_result* s = &r->step_results[i];
        printf(
            "  step_%d: outcome %d, resultCode %d, details \"%.*s\"\n",
            (int)i,
            (int)s->outcome,
            (int)s->result_code,
            (int)az_span_size(s->result_details),
            az_span_size(s->result_details) > 0 ? (const char*)az_span_ptr(s->result_details) : "");
      }
      flags->completed
          = (event->reason == AZ_IOT_OK && r != NULL && r->outcome == AZ_IOT_SU_OUTCOME_SUCCEEDED)
          ? 1
          : 2;
      break;
    }
    case AZ_IOT_SU_EVENT_PERSIST_FAILED:
      fprintf(
          stderr,
          "State not saved (%u failed writes)%s\n",
          (unsigned)event->persist_attempts,
          event->persist_retrying ? "; retrying" : "; giving up");
      break;
    case AZ_IOT_SU_EVENT_UPDATE_DEFERRED:
      /* In Idle: the last apply asked for a reboot, which has not happened yet. */
      printf(
          "New update deferred until %s\n",
          event->state == AZ_IOT_SU_STATE_IDLE ? "the device reboots" : "the current one ends");
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
    case AZ_IOT_SU_EVENT_UPDATE_RETRY_WAIT:
      printf(
          "Failed update offered again; retry in %u s.\n",
          (unsigned)(event->retry_after_ms / 1000u));
      break;
    case AZ_IOT_SU_EVENT_PERSIST_RECOVERED:
    case AZ_IOT_SU_EVENT_REPORT_ACCEPTED:
    case AZ_IOT_SU_EVENT_UPDATE_REFUSED:
    case AZ_IOT_SU_EVENT_UPDATE_DUPLICATE:
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
  update_hooks_context hooks_context = { 0 };
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

  hooks_context.su = &su;
  hooks_context.state_file = sample_env_dup("SAMPLE_STATE_FILE", "./su_reboot_state.bin");
  int exit_after_workflow = su_sample_env_flag("AZ_IOT_SU_EXIT_AFTER_WORKFLOW");
  long poll_interval_s = su_sample_env_long("AZ_IOT_SU_POLL_INTERVAL_S", 14400);
  if (manufacturer == NULL || model == NULL || reboot_at == NULL || hooks_context.state_file == NULL
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
  hooks_context.reboot_on_apply = (strncmp(reboot_at, "apply", 5) == 0);
  hooks_context.reboot_step = (colon != NULL) ? strtol(colon + 1, NULL, 10) : 1;

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
  hooks.download_fn = download_update_file;
  hooks.read_file_fn = read_back_downloaded_file;
  hooks.install_fn = install_update_step;
  hooks.apply_fn = apply_update_step;
  hooks.persist_state_fn = save_update_workflow;
  hooks.load_state_fn = load_saved_update_workflow;
  hooks.user_ctx = &hooks_context;

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
  if (az_iot_su_client_add_observer(&su, on_su_event, &flags) != AZ_IOT_OK)
  {
    goto cleanup;
  }
  az_iot_result resumed = az_iot_su_client_resume(&su);
  if (resumed != AZ_IOT_OK)
  {
    fprintf(stderr, "Nothing resumed: %s\n", az_iot_result_to_string(resumed));
  }
  /* A resumed workflow finishes first: a check now could deliver another one.
   * A check waits for a restored final report by itself. */
  if (az_iot_su_client_get_state(&su) == AZ_IOT_SU_STATE_IDLE
      && az_iot_su_client_request_update(&su, request_timeout_ms) != AZ_IOT_OK)
  {
    goto cleanup;
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

    /* FAULTED is settled: the SDK does not retry out of it. A saved workflow
     * resumes on the next run. */
    if (flags.connection_faulted)
    {
      fprintf(stderr, "The connection faulted and will not recover on its own.\n");
      rc = 1;
      break;
    }
    /* The "reboot": once the client has stored what resume() needs. */
    az_iot_su_reboot_state reboot_state = az_iot_su_client_get_reboot_state(&su);
    if (reboot_state == AZ_IOT_SU_REBOOT_READY)
    {
      if (exit_after_workflow && flags.completed != 0)
      {
        rc = (flags.completed == 1) ? 0 : 2;
      }
      printf(
          "Rebooting: the workflow is saved in %s. Run the sample again to resume.\n",
          hooks_context.state_file);
      break;
    }
    int idle = az_iot_su_client_get_state(&su) == AZ_IOT_SU_STATE_IDLE;
    /* Not while storage is behind: the next run could resume the finished workflow. */
    if (exit_after_workflow && flags.completed != 0 && idle
        && reboot_state != AZ_IOT_SU_REBOOT_PENDING)
    {
      rc = (flags.completed == 1) ? 0 : 2;
      break;
    }
    if (idle && poll_interval_s > 0 && sample_now_ms() >= next_check_ms)
    {
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
  free(hooks_context.state_file);
  free(manufacturer);
  free(model);
  free(reboot_at);
  return rc;
}
