// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file su_sim.c
 * @brief Simulated platform hooks and helpers shared by the PC Software Update
 * samples. See su_sim.h.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sample_utils.h"
#include "su_sim.h"

int su_sample_env_flag(const char* name)
{
  char* v = sample_env_dup(name, NULL);
  int on = (v != NULL && strcmp(v, "0") != 0);
  free(v);
  return on;
}

long su_sample_env_long(const char* name, long fallback)
{
  char* v = sample_env_dup(name, NULL);
  long out = fallback;
  if (v != NULL)
  {
    char* end = NULL;
    errno = 0;
    long parsed = strtol(v, &end, 10);
    if (errno == 0 && end != v && *end == '\0')
    {
      out = parsed;
    }
    free(v);
  }
  return out;
}

az_iot_log_level su_sample_log_level_from_env(void)
{
  /* In az_iot_log_level order. INFO by default: the SDK's "su:" and "dps:"
   * protocol lines are DEBUG and would bury the sample's own output. */
  static const char* const k_names[] = { "trace", "debug", "info", "warn", "error", "off" };
  az_iot_log_level out = AZ_IOT_LOG_LEVEL_INFO;
  char* v = sample_env_dup("AZ_IOT_SU_LOG_LEVEL", NULL);
  for (size_t i = 0; v != NULL && i < sizeof(k_names) / sizeof(k_names[0]); ++i)
  {
    if (strcmp(v, k_names[i]) == 0)
    {
      out = (az_iot_log_level)i;
      break;
    }
  }
  free(v);
  return out;
}

/* Indexed by az_iot_su_state / az_iot_su_operation. */
static const char* const k_su_state_names[] = {
  "Idle",           "ManifestReceived", "VerifyingManifest", "DownloadStarted", "DownloadComplete",
  "BackupStarted",  "BackupComplete",   "InstallStarted",    "InstallComplete", "ApplyStarted",
  "RestoreStarted", "Failed",
};
static const char* const k_su_operation_names[] = {
  "onboarding update check",
  "update check",
  "status report",
};

#define SU_SAMPLE_NAME_OF(table, i) \
  (((size_t)(i) < sizeof(table) / sizeof((table)[0])) ? (table)[(size_t)(i)] : "?")

const char* su_sample_state_name(az_iot_su_state state)
{
  return SU_SAMPLE_NAME_OF(k_su_state_names, state);
}

const char* su_sample_operation_name(az_iot_su_operation operation)
{
  return SU_SAMPLE_NAME_OF(k_su_operation_names, operation);
}

int su_sim_init(su_sim* sim)
{
  memset(sim, 0, sizeof(*sim));
  sim->fail_step = (int)su_sample_env_long("SU_SIM_FAIL_STEP", 0);
  sim->hash_mismatch = su_sample_env_flag("SU_SIM_HASH_MISMATCH");
  sim->reboot = su_sample_env_flag("SU_SIM_REBOOT");
  sim->delay_ms = su_sample_env_long("SU_SIM_DELAY_MS", 0);
  sim->state_file = sample_env_dup("SU_SIM_STATE_FILE", "./su_sim_state.blob");
  return (sim->state_file == NULL) ? 1 : 0;
}

void su_sim_deinit(su_sim* sim)
{
  free(sim->state_file);
  sim->state_file = NULL;
}

static int32_t sim_download(
    const az_iot_su_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx)
{
  (void)url;
  su_sim* s = (su_sim*)user_ctx;
  printf(
      "  [download] file %u/%u (%lld bytes) [simulated]\n",
      file_index + 1,
      file_count,
      (long long)file->size_in_bytes);
  sample_sleep_ms(s->delay_ms);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/* Serve deterministic payload bytes so the SHA-256 the core computes is
 * reproducible. To make the REAL hash check pass against a deployment, the
 * imported payload must be byte-identical (see README: zero-filled file). */
static int32_t sim_read_file(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* user_ctx)
{
  (void)file_index;
  su_sim* s = (su_sim*)user_ctx;
  size_t size = (file->size_in_bytes > 0) ? (size_t)file->size_in_bytes : 0;
  if (offset >= size)
  {
    *out_read = 0; /* EOF */
    return AZ_IOT_SU_RESULT_SUCCESS;
  }
  size_t remain = size - offset;
  size_t n = remain < buffer_size ? remain : buffer_size;
  memset(buffer, 0x00, n);
  if (s->hash_mismatch && offset == 0 && n > 0)
  {
    buffer[0] = 0xFF; /* corrupt the first byte -> hash verification fails */
  }
  *out_read = n;
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int span_equals_str(az_span span, const char* str)
{
  return str != NULL && az_span_is_content_equal(span, az_span_create_from_str((char*)str));
}

static int32_t sim_is_installed(const az_iot_su_client_update_manifest* manifest, void* user_ctx)
{
  su_sim* s = (su_sim*)user_ctx;
  const az_iot_su_report_update_id* id = s->installed;
  if (id != NULL && span_equals_str(manifest->update_id.provider, id->provider)
      && span_equals_str(manifest->update_id.name, id->name)
      && span_equals_str(manifest->update_id.version, id->version))
  {
    printf("  [is_installed] %s/%s/%s is already installed\n", id->provider, id->name, id->version);
    return AZ_IOT_SU_RESULT_ALREADY_INSTALLED;
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t sim_backup(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  (void)user_ctx;
  printf("  [backup]  step %u [simulated]\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t sim_install(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  su_sim* s = (su_sim*)user_ctx;
  if (s->fail_step > 0 && (uint32_t)(s->fail_step - 1) == step)
  {
    printf("  [install] step %u -> FORCED FAILURE (SU_SIM_FAIL_STEP)\n", step);
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  if (s->reboot && !s->reboot_signalled)
  {
    s->reboot_signalled = 1;
    printf("  [install] step %u -> REBOOT_REQUIRED (SU_SIM_REBOOT)\n", step);
    return AZ_IOT_SU_RESULT_REBOOT_REQUIRED;
  }
  printf("  [install] step %u [simulated]\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int copy_span(char* dst, size_t dst_size, az_span src)
{
  int32_t n = az_span_size(src);
  if (n <= 0 || (size_t)n >= dst_size)
  {
    return 0;
  }
  memcpy(dst, az_span_ptr(src), (size_t)n);
  dst[n] = '\0';
  return 1;
}

static int32_t sim_apply(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  su_sim* s = (su_sim*)user_ctx;
  s->applied_valid
      = copy_span(s->applied_provider, sizeof(s->applied_provider), manifest->update_id.provider)
      && copy_span(s->applied_name, sizeof(s->applied_name), manifest->update_id.name)
      && copy_span(s->applied_version, sizeof(s->applied_version), manifest->update_id.version);
  printf("  [apply]   step %u [simulated]\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t sim_restore(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  (void)user_ctx;
  printf("  [restore] step %u (rollback) [simulated]\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t sim_persist(const uint8_t* blob, size_t len, void* user_ctx)
{
  su_sim* s = (su_sim*)user_ctx;
  if (len == 0)
  {
    /* Invalidation: remove the file so a later run finds no checkpoint. */
    if (remove(s->state_file) != 0 && errno != ENOENT)
    {
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    printf("  [persist] cleared %s\n", s->state_file);
    return AZ_IOT_SU_RESULT_SUCCESS;
  }
  FILE* f = fopen(s->state_file, "wb");
  if (f == NULL)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  size_t w = fwrite(blob, 1, len, f);
  fclose(f);
  if (w != len)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  printf("  [persist] %zu bytes -> %s\n", len, s->state_file);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t sim_load(uint8_t* blob, size_t cap, size_t* out_len, void* user_ctx)
{
  su_sim* s = (su_sim*)user_ctx;
  FILE* f = fopen(s->state_file, "rb");
  if (f == NULL)
  {
    return 1; /* nothing persisted */
  }
  size_t r = fread(blob, 1, cap, f);
  int eof = feof(f);
  fclose(f);
  if (!eof)
  {
    return 1; /* blob did not fit in cap -> treat as no state */
  }
  *out_len = r;
  return 0;
}

az_iot_su_platform_hooks su_sim_hooks(su_sim* sim)
{
  az_iot_su_platform_hooks hooks = { 0 };
  hooks.download_fn = sim_download;
  hooks.read_file_fn = sim_read_file;
  hooks.install_fn = sim_install;
  hooks.apply_fn = sim_apply;
  hooks.backup_fn = sim_backup;
  hooks.restore_fn = sim_restore;
  hooks.is_installed_fn = sim_is_installed;
  hooks.persist_state_fn = sim_persist;
  hooks.load_state_fn = sim_load;
  hooks.user_ctx = sim;
  return hooks;
}
