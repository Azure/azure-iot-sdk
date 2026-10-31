// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file e2e_su_env.h
 * @brief Environment shared by the software updates e2e suites.
 *
 * Every variable listed here is REQUIRED. A suite whose environment is
 * incomplete fails; it never skips.
 */

#ifndef E2E_SU_ENV_H
#define E2E_SU_ENV_H

#include <stdint.h>

#include "azure/iot/az_iot_certificate_provider_pem.h"
#include "azure/iot/az_iot_connection_client.h"

/** @brief Device-side configuration of a software updates e2e run. Owns its strings. */
typedef struct e2e_su_env
{
  char* dps_host; /**< AZ_IOT_E2E_SU_DPS_HOST: DPS global endpoint. */
  char* id_scope; /**< AZ_IOT_E2E_SU_ID_SCOPE. */
  char* registration_id; /**< AZ_IOT_E2E_SU_REG_ID: the identity the cert carries. */
  char* cert_path; /**< AZ_IOT_E2E_SU_CERT: device certificate PEM. */
  char* key_path; /**< AZ_IOT_E2E_SU_KEY: device private key PEM. */
  char* trusted_ca; /**< AZ_IOT_E2E_SU_TRUSTED_CA: CA bundle PEM. */

  /** From `https_proxy`, when set. Empty host means a direct connection. */
  char proxy_host[128];
  uint16_t proxy_port;
} e2e_su_env;

/**
 * @brief Loads the environment, printing every missing variable.
 *
 * @param[out] env Filled on return; release with e2e_su_env_free(), whatever the result.
 * @return 0 when complete; non-zero otherwise, so a cmocka group setup can
 *   return it and fail the suite.
 */
int e2e_su_env_load(e2e_su_env* env);

/**
 * @brief Releases the strings e2e_su_env_load() copied into @p env.
 *
 * @param[in,out] env Loaded environment; zeroed on return.
 */
void e2e_su_env_free(e2e_su_env* env);

/**
 * @brief Reads a required per-suite variable.
 *
 * @param[in] name Variable name.
 * @return A copy of its value for the caller to free, or NULL after printing that it is missing.
 */
char* e2e_su_env_require(const char* name);

/**
 * @brief Applies @p env to connection and certificate options.
 *
 * Sets DPS mode (no hub host), scope, registration id, global endpoint, proxy,
 * the PEM paths, and a workspace for one client at a time. The caller sets the
 * certificate provider pointer.
 */
void e2e_su_env_apply(
    const e2e_su_env* env,
    az_iot_connection_client_options* conn,
    az_iot_certificate_provider_pem_options* pem);

#endif /* E2E_SU_ENV_H */
