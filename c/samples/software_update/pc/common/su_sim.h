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
 * the SDK runs over read_file_fn's bytes is real. Behavior is driven by the
 * SU_SIM_* environment variables read by su_sim_init().
 */
#ifndef SU_SIM_H
#define SU_SIM_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_su.h"

/** @brief Capacity of each part of an update id captured by su_sim. */
#define SU_SIM_ID_PART_SIZE 128

/**
 * @brief True when an event stamped by the SDK is long enough to carry @p field.
 *
 * Events grow by appending, so test against the last field read, not the
 * struct size.
 */
#define SU_SAMPLE_EVENT_HAS(ev, type, field) \
  ((ev)->_internal_size >= offsetof(type, field) + sizeof((ev)->field))

/** @brief Simulation state, passed to every hook as user_ctx. */
typedef struct su_sim
{
  int fail_step; /**< SU_SIM_FAIL_STEP: 1-based install step to fail; 0 = never. */
  int hash_mismatch; /**< SU_SIM_HASH_MISMATCH: corrupt the served payload. */
  int reboot; /**< SU_SIM_REBOOT: install returns REBOOT_REQUIRED once. */
  long delay_ms; /**< SU_SIM_DELAY_MS: per-download delay. */
  char* state_file; /**< SU_SIM_STATE_FILE: resume blob path. Owned. */

  int reboot_signalled; /**< Set when install returned REBOOT_REQUIRED. */

  /** What is installed now. is_installed_fn answers ALREADY_INSTALLED for a
   * manifest with this id. NULL: every update proceeds. */
  const az_iot_su_report_update_id* installed;

  /** Id of the last manifest applied, captured by apply_fn. */
  char applied_provider[SU_SIM_ID_PART_SIZE];
  char applied_name[SU_SIM_ID_PART_SIZE];
  char applied_version[SU_SIM_ID_PART_SIZE];
  int applied_valid; /**< Nonzero when the three applied_* strings are set. */
} su_sim;

/**
 * @brief Zero @p sim and read the SU_SIM_* environment variables.
 * @return 0 on success; nonzero if out of memory.
 */
int su_sim_init(su_sim* sim);

/** @brief Release what su_sim_init() allocated. */
void su_sim_deinit(su_sim* sim);

/** @brief Simulated platform hooks bound to @p sim, which must outlive the client. */
az_iot_su_platform_hooks su_sim_hooks(su_sim* sim);

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
