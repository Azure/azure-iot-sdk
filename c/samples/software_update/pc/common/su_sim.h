// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file su_sim.h
 * @brief Simulated platform hooks and helpers shared by the PC Software Update
 * samples.
 *
 * Download, install, apply, backup and restore are simulated; the SHA-256 check
 * the SDK runs over read_back_simulated_update_file()'s bytes is real. Every hook takes an
 * su_simulation_control as user_ctx.
 */
#ifndef SU_SIM_H
#define SU_SIM_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_su.h"

/** @brief Capacity of each part of an update id captured by simulate_update_step_apply(): the
 * SDK's applied-update-ID capacity (AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE, 192 bytes),
 * so any part it accepts fits. */
#define SU_SIM_ID_PART_SIZE 192

/** @brief What the simulated hooks do, and what they report back. */
typedef struct su_simulation_control
{
  int fail_step; /**< SU_SIM_FAIL_STEP: 1-based install step to fail; 0 = never. */
  int hash_mismatch; /**< SU_SIM_HASH_MISMATCH: corrupt the served payload. */
  int reboot; /**< SU_SIM_REBOOT: install returns REBOOT_REQUIRED once. */
  long delay_ms; /**< SU_SIM_DELAY_MS: per-download delay. */
  char* state_file; /**< SU_SIM_STATE_FILE: resume blob path. Owned. */
  /** SU_SIM_FILE_DIR: existing directory where simulate_update_file_download() also writes the
   * synthesized bytes, at su_sim_get_update_file_path(), for a download handler. NULL: no file is
   * written. Owned. */
  char* file_directory;

  int reboot_signalled; /**< Set when install returned REBOOT_REQUIRED; cleared by a rollback. */
  int reboot_pending; /**< The requested reboot is still wanted; cleared by a rollback. */
  int persist_failed; /**< Nonzero when the last checkpoint write failed. */

  /** What is installed now. is_offered_update_already_installed() answers ALREADY_INSTALLED for a
   * manifest with this id. NULL: every update proceeds. */
  const az_iot_su_report_update_id* installed;

  /** Id of the last manifest applied, captured by simulate_update_step_apply(). */
  char applied_provider[SU_SIM_ID_PART_SIZE];
  char applied_name[SU_SIM_ID_PART_SIZE];
  char applied_version[SU_SIM_ID_PART_SIZE];
  int applied_valid; /**< Nonzero when the three applied_* strings are set. */
} su_simulation_control;

/** @brief download_fn: logs the file; bytes are synthesized by read_back_simulated_update_file().
 * Also writes them to su_sim_get_update_file_path() when su_simulation_control::file_directory is
 * set. */
int32_t simulate_update_file_download(
    const az_iot_su_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx);

/**
 * @brief Path of @p file in su_simulation_control::file_directory: the directory and the file id.
 * Matches az_iot_su_delta_get_file_path_fn.
 *
 * @return AZ_IOT_SU_RESULT_SUCCESS; AZ_IOT_SU_RESULT_FAILURE without a directory or when the path
 * does not fit @p path_size.
 */
int32_t su_sim_get_update_file_path(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    char* path,
    size_t path_size,
    void* user_ctx);

/**
 * @brief Simulated delta processor (az_iot_su_delta_apply_fn): copies @p source_path to
 * @p target_path. read_back_simulated_update_file() serves synthesized bytes, so the built file's
 * hash check passes whatever it holds.
 *
 * @return AZ_IOT_SU_RESULT_SUCCESS, or AZ_IOT_SU_RESULT_FAILURE when a file cannot be read or
 * written.
 */
int32_t simulate_delta_apply(
    const char* source_path,
    const char* delta_path,
    const char* target_path,
    void* user_ctx);

/** @brief read_file_fn: zero-filled bytes of the declared size (first byte
 * corrupted when hash_mismatch is set). */
int32_t read_back_simulated_update_file(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* user_ctx);

/** @brief is_installed_fn: ALREADY_INSTALLED when the manifest id equals
 * su_simulation_control::installed. */
int32_t is_offered_update_already_installed(
    const az_iot_su_client_update_manifest* manifest,
    void* user_ctx);

/** @brief backup_fn: logs only. */
int32_t simulate_update_step_backup(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx);

/** @brief install_fn: logs; fails at fail_step or asks for a reboot once. */
int32_t simulate_update_step_install(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx);

/** @brief apply_fn: logs and captures the manifest's update id. */
int32_t simulate_update_step_apply(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx);

/** @brief restore_fn: logs only. */
int32_t simulate_update_step_restore(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx);

/** @brief persist_workflow_state_fn: writes the blob to state_file; @p len 0 removes it. */
int32_t save_update_workflow_to_file(const uint8_t* blob, size_t len, void* user_ctx);

/** @brief load_workflow_state_fn: reads state_file; 1 when absent, larger than @p cap
 * or unreadable. */
int32_t load_update_workflow_from_file(uint8_t* blob, size_t cap, size_t* out_len, void* user_ctx);

/** @brief True when @p name is set to anything other than "0". */
int su_sample_env_flag(const char* name);

/** @brief @p name as a decimal number, or @p fallback when unset or not one. */
long su_sample_env_long(const char* name, long fallback);

/**
 * @brief SDK log level from AZ_IOT_SU_LOG_LEVEL
 * (trace|debug|info|warn|error|off); info when unset or unknown.
 */
az_iot_log_level su_sample_log_level_from_env(void);

/** @brief Printable name of @p state. */
const char* su_sample_state_name(az_iot_su_state state);

/** @brief Printable name of @p operation. */
const char* su_sample_operation_name(az_iot_su_operation operation);

#endif /* SU_SIM_H */
