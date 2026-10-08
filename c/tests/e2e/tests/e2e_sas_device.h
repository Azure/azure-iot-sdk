// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Device side of the SAS e2e suites: configuration from the environment and a
 * DPS-to-hub run that sends one telemetry message. */

#ifndef E2E_SAS_DEVICE_H
#define E2E_SAS_DEVICE_H

#include "azure/iot/az_iot.h"

/** @brief Environment the SAS suites need; strings are owned (heap copies). */
typedef struct
{
  char* id_scope; /**< AZ_IOT_DPS_ID_SCOPE */
  char* group_key; /**< AZ_IOT_DPS_SAS_GROUP_KEY: base64 enrollment-group key */
  char* registration_id; /**< AZ_IOT_DPS_SAS_REGISTRATION_ID */
  char* trusted_ca; /**< AZ_IOT_TRUSTED_CA: PEM file path */
  char* global_endpoint; /**< AZ_IOT_DPS_GLOBAL_ENDPOINT, or NULL */
} e2e_sas_config;

/** @brief Credentials each scope connected with. */
typedef struct
{
  az_iot_auth_source dps_source;
  az_iot_auth_source hub_source;
  int hub_renewals; /**< Hub CONNECTED events flagged is_credential_renewal. */
  az_iot_connection_client* client; /**< The run's client while it exists; else NULL. */
} e2e_sas_run;

/** @brief Loads @p cfg; fails the test when a required variable is unset. */
void e2e_sas_config_load(e2e_sas_config* cfg);

/** @brief Frees what e2e_sas_config_load() copied. */
void e2e_sas_config_free(e2e_sas_config* cfg);

/** @brief Sets DPS, trust, crypto, sas_buffer and dps_auth (group key) in
 * @p copts. @p cfg must outlive the client. */
void e2e_sas_apply_dps(const e2e_sas_config* cfg, az_iot_connection_client_options* copts);

/** @brief Opens a client with @p copts, asserts it reaches hub CONNECTED and
 * sends one telemetry message, then closes it. Fills @p run. */
void e2e_sas_connect_and_send(
    e2e_sas_run* run,
    const az_iot_connection_client_options* copts,
    const char* label);

/** @brief As e2e_sas_connect_and_send(), but first waits until the hub token
 * has been renewed @p renewals times. */
void e2e_sas_connect_renew_and_send(
    e2e_sas_run* run,
    const az_iot_connection_client_options* copts,
    const char* label,
    int renewals);

#endif /* E2E_SAS_DEVICE_H */
