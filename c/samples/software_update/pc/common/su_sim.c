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
  char* v = sample_env_copy(name, NULL);
  int on = (v != NULL && strcmp(v, "0") != 0);
  free(v);
  return on;
}

long su_sample_env_long(const char* name, long fallback)
{
  char* v = sample_env_copy(name, NULL);
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
  char* v = sample_env_copy("AZ_IOT_SU_LOG_LEVEL", NULL);
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
  "RestoreStarted", "Failed",           "Delegated",
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

bool su_sample_decode_file_id(
    const az_iot_su_client_update_manifest_file* file,
    char* name,
    size_t name_size)
{
  az_span decoded;
  if (name_size == 0
      || az_iot_su_decode_manifest_string(
             file->id, az_span_create((uint8_t*)name, (int32_t)(name_size - 1)), &decoded)
          != AZ_IOT_OK
      || !sample_is_plain_file_name(decoded))
  {
    return false;
  }
  name[az_span_size(decoded)] = '\0';
  return true;
}

int32_t su_sim_get_update_file_path(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    char* path,
    size_t path_size,
    void* user_ctx)
{
  (void)file_index;
  const su_simulation_control* s = (const su_simulation_control*)user_ctx;
  char name[SU_SAMPLE_FILE_NAME_SIZE];
  if (s->file_directory == NULL || !su_sample_decode_file_id(file, name, sizeof(name)))
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  int n = snprintf(path, path_size, "%s/%s", s->file_directory, name);
  return (n >= 0 && (size_t)n < path_size) ? AZ_IOT_SU_RESULT_SUCCESS : AZ_IOT_SU_RESULT_FAILURE;
}

/* Writes @p size zero bytes to @p path. */
static int32_t write_zero_filled_file(const char* path, int64_t size)
{
  FILE* f = sample_file_open_private(path, true);
  if (f == NULL)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  static const uint8_t k_zeros[4096] = { 0 };
  int ok = 1;
  for (int64_t left = size; ok && left > 0;)
  {
    size_t n = left < (int64_t)sizeof(k_zeros) ? (size_t)left : sizeof(k_zeros);
    ok = fwrite(k_zeros, 1, n, f) == n;
    left -= (int64_t)n;
  }
  ok = (fclose(f) == 0) && ok;
  return ok ? AZ_IOT_SU_RESULT_SUCCESS : AZ_IOT_SU_RESULT_FAILURE;
}

int32_t simulate_update_file_download(
    const az_iot_su_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx)
{
  (void)url;
  su_simulation_control* s = (su_simulation_control*)user_ctx;
  printf(
      "  [download] %.*s for file %u/%u (%lld bytes) [simulated]\n",
      (int)az_span_size(file->file_name),
      (const char*)az_span_ptr(file->file_name),
      file_index + 1,
      file_count,
      (long long)file->size_in_bytes);
  sample_sleep_ms(s->delay_ms);
  if (s->file_directory != NULL)
  {
    char path[1024];
    if (su_sim_get_update_file_path(file, file_index, path, sizeof(path), s)
            != AZ_IOT_SU_RESULT_SUCCESS
        || write_zero_filled_file(path, file->size_in_bytes) != AZ_IOT_SU_RESULT_SUCCESS)
    {
      fprintf(stderr, "  [download] could not write the file in %s\n", s->file_directory);
      return AZ_IOT_SU_RESULT_FAILURE;
    }
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

int32_t simulate_delta_apply(
    const char* source_path,
    const char* delta_path,
    const char* target_path,
    void* user_ctx)
{
  (void)user_ctx;
  printf(
      "  [delta] %s + %s -> %s [simulated: copies the source]\n",
      source_path,
      delta_path,
      target_path);
  FILE* in = fopen(source_path, "rb");
  if (in == NULL)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  /* The handler created target_path and holds a lock on it: write that file, not a new one. */
  FILE* out = sample_file_rewrite_existing_no_follow(target_path);
  int ok = out != NULL;
  uint8_t buffer[4096];
  size_t n = 0;
  while (ok && (n = fread(buffer, 1, sizeof(buffer), in)) > 0)
  {
    ok = fwrite(buffer, 1, n, out) == n;
  }
  ok = ok && !ferror(in);
  (void)fclose(in);
  if (out != NULL)
  {
    ok = (fclose(out) == 0) && ok;
  }
  return ok ? AZ_IOT_SU_RESULT_SUCCESS : AZ_IOT_SU_RESULT_FAILURE;
}

/* Serve deterministic payload bytes so the SHA-256 the core computes is
 * reproducible. To make the REAL hash check pass against a deployment, the
 * imported payload must be byte-identical (see README: zero-filled file). */
int32_t read_back_simulated_update_file(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* user_ctx)
{
  (void)file_index;
  su_simulation_control* s = (su_simulation_control*)user_ctx;
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

int32_t is_offered_update_already_installed(
    const az_iot_su_client_update_manifest* manifest,
    void* user_ctx)
{
  su_simulation_control* s = (su_simulation_control*)user_ctx;
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

int32_t simulate_update_step_backup(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  (void)user_ctx;
  printf("  [backup]  step %u [simulated]\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

int32_t simulate_update_step_install(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  su_simulation_control* s = (su_simulation_control*)user_ctx;
  if (s->fail_step > 0 && (uint32_t)(s->fail_step - 1) == step)
  {
    printf("  [install] step %u -> FORCED FAILURE (SU_SIM_FAIL_STEP)\n", step);
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  if (s->reboot && !s->reboot_signalled)
  {
    s->reboot_signalled = 1;
    s->reboot_pending = 1;
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

int32_t simulate_update_step_apply(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  su_simulation_control* s = (su_simulation_control*)user_ctx;
  s->applied_valid
      = copy_span(s->applied_provider, sizeof(s->applied_provider), manifest->update_id.provider)
      && copy_span(s->applied_name, sizeof(s->applied_name), manifest->update_id.name)
      && copy_span(s->applied_version, sizeof(s->applied_version), manifest->update_id.version);
  printf("  [apply]   step %u [simulated]\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

int32_t simulate_update_step_restore(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  su_simulation_control* s = (su_simulation_control*)user_ctx;
  /* Rolled back: the requested reboot is no longer wanted, and a later
   * workflow may request its own. */
  s->reboot_pending = 0;
  s->reboot_signalled = 0;
  printf("  [restore] step %u (rollback) [simulated]\n", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

int32_t save_update_workflow_to_file(const uint8_t* blob, size_t len, void* user_ctx)
{
  su_simulation_control* s = (su_simulation_control*)user_ctx;
  if (len == 0)
  {
    /* Invalidation: remove the file, durably, so a later run finds no checkpoint. */
    if (!sample_file_remove_durably(s->state_file))
    {
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    printf("  [persist] cleared %s\n", s->state_file);
    return AZ_IOT_SU_RESULT_SUCCESS;
  }
  /* Not rewritten in place: a crash then leaves the previous record, not a torn one. */
  if (!sample_file_write_atomically(s->state_file, blob, len))
  {
    s->persist_failed = 1;
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  s->persist_failed = 0;
  printf("  [persist] %zu bytes -> %s\n", len, s->state_file);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

int32_t load_update_workflow_from_file(uint8_t* blob, size_t cap, size_t* out_len, void* user_ctx)
{
  su_simulation_control* s = (su_simulation_control*)user_ctx;
  FILE* f = sample_file_open_read_no_follow(s->state_file);
  if (f == NULL)
  {
    return 1; /* nothing persisted */
  }
  size_t r = fread(blob, 1, cap, f);
  /* A blob of exactly cap bytes fits: EOF is only seen by reading past it. */
  int overflow = (r == cap && fgetc(f) != EOF);
  int failed = ferror(f);
  (void)fclose(f);
  if (overflow || failed)
  {
    return 1; /* did not fit in cap, or unreadable -> treat as no state */
  }
  *out_len = r;
  return 0;
}
