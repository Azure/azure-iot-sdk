// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

#include "sample_dps_tpm.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot_sas_token.h"
#include "sample_utils.h"

/** @brief Longest resource URI signed here. */
#define SAMPLE_URI_MAX 512

az_iot_result sample_dps_tpm_register(
    az_iot_dps_tpm_client* client,
    const char* host,
    sample_https_send send,
    void* send_ctx,
    uint64_t timeout_ms,
    sample_https_response* response)
{
  static char buffer[AZ_IOT_DPS_TPM_REQUEST_BUFFER_SIZE + 4096]; /* room for a CSR and payload */
  uint64_t deadline = sample_now_ms() + timeout_ms;
  while (sample_now_ms() < deadline)
  {
    az_iot_dps_tpm_request request;
    az_iot_result r = az_iot_dps_tpm_client_get_request(
        client, (uint64_t)time(NULL), buffer, sizeof(buffer), &request);
    if (r != AZ_IOT_OK)
    {
      return r;
    }
    memset(response, 0, sizeof(*response));
    if (!send(send_ctx, host, &request, response))
    {
      return AZ_IOT_ERR_NOT_CONNECTED;
    }
    fprintf(stderr, "[dps_tpm] %s %s -> %u\n", request.method, request.path, response->status);

    az_iot_dps_tpm_next next;
    r = az_iot_dps_tpm_client_handle_response(
        client,
        response->status,
        az_span_create((uint8_t*)response->body, (int32_t)response->body_len),
        response->retry_after_seconds,
        &next);
    if (r != AZ_IOT_OK)
    {
      uint16_t status = 0;
      int32_t code = 0;
      az_iot_dps_tpm_client_get_error(client, &status, &code);
      fprintf(
          stderr,
          "[dps_tpm] registration failed: %s (HTTP %u, errorCode %ld)\n",
          az_iot_result_to_string(r),
          status,
          (long)code);
      return r;
    }
    if (next.assigned)
    {
      return AZ_IOT_OK;
    }
    sample_sleep_ms((long)next.retry_after_seconds * 1000L);
  }
  return AZ_IOT_ERR_TIMEOUT;
}

az_iot_result sample_dps_tpm_sign_token(
    sample_tpm_sim* tpm,
    const char* resource_uri,
    const char* key_name,
    uint64_t expiry_unix_seconds,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  char to_sign[AZ_IOT_SAS_STRING_TO_SIGN_SIZE(SAMPLE_URI_MAX)];
  size_t to_sign_len = 0;
  az_iot_result r = az_iot_sas_token_string_to_sign(
      resource_uri, expiry_unix_seconds, to_sign, sizeof(to_sign), &to_sign_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  r = sample_tpm_sim_hmac(tpm, (const uint8_t*)to_sign, to_sign_len, mac)
      ? az_iot_sas_token_from_signature(
            resource_uri, key_name, expiry_unix_seconds, mac, out, out_size, out_len)
      : AZ_IOT_ERR_INTERNAL;
  memset(mac, 0, sizeof(mac));
  return r;
}
