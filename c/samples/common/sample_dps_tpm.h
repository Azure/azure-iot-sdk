// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file sample_dps_tpm.h
 * @brief Runs a DPS TPM registration (az_iot_dps_tpm_client.h) over an HTTPS
 * function the caller supplies, and signs hub SAS tokens with the TPM.
 */

#ifndef SAMPLE_DPS_TPM_H
#define SAMPLE_DPS_TPM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_dps_tpm_client.h"
#include "sample_tpm_sim.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Response bytes kept per request. */
#define SAMPLE_DPS_TPM_RESPONSE_MAX 8192

  /** @brief A response to one request. */
  typedef struct sample_https_response
  {
    uint16_t status; /**< HTTP status. */
    char body[SAMPLE_DPS_TPM_RESPONSE_MAX]; /**< Body; not terminated. */
    size_t body_len; /**< Bytes in body. */
    uint32_t retry_after_seconds; /**< Retry-After; 0 when absent. */
  } sample_https_response;

  /**
   * @brief Sends @p request to `https://<host><request->path>` with the
   * headers the request needs (az_iot_dps_tpm_client.h).
   *
   * @return false when no response was received.
   */
  typedef bool (*sample_https_send)(
      void* ctx,
      const char* host,
      const az_iot_dps_tpm_request* request,
      sample_https_response* response);

  /**
   * @brief Registers until assigned or failed, waiting as DPS asks between
   * requests. @p client must be initialized.
   *
   * @param[in]  timeout_ms Overall limit.
   * @param[out] response   Storage for each response; the assignment's
   *                        issued_certificate_chain points into it.
   * @return AZ_IOT_OK when assigned; AZ_IOT_ERR_TIMEOUT; AZ_IOT_ERR_NOT_CONNECTED
   * when the HTTPS function fails; the client's error otherwise.
   */
  az_iot_result sample_dps_tpm_register(
      az_iot_dps_tpm_client* client,
      const char* host,
      sample_https_send send,
      void* send_ctx,
      uint64_t timeout_ms,
      sample_https_response* response);

  /**
   * @brief A SAS token for @p resource_uri and @p key_name (az_iot_sas_token_request),
   * signed by the TPM's imported key.
   *
   * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE; AZ_IOT_ERR_INTERNAL when the TPM fails.
   */
  az_iot_result sample_dps_tpm_sign_token(
      sample_tpm_sim* tpm,
      const char* resource_uri,
      const char* key_name,
      uint64_t expiry_unix_seconds,
      char* out,
      size_t out_size,
      size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLE_DPS_TPM_H */
