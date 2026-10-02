// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief Template for a custom az_iot_sas_signer, e.g. one whose symmetric key
 * stays in a TPM, HSM or secure element.
 *
 * Builds with no crypto dependency and does not connect; main() exercises the
 * vtable. Replace the body of my_sign() with an HMAC-SHA256 computed by your
 * key store, then set the signer on az_iot_connection_client_options:
 *   opts.sas.onboarding  = &signer.base;   SAS to DPS
 *   opts.sas.operational = &signer.base;   SAS to the hub
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_sas_signer.h"

/** @brief Signer state. The base must be first. */
typedef struct
{
  az_iot_sas_signer base; /**< Vtable pointer. */
  uint32_t key_handle; /**< E.g. a TPM persistent handle for the HMAC key. */
} my_signer;

static az_iot_result my_sign(
    az_iot_sas_signer* self,
    const uint8_t* data,
    size_t data_len,
    uint8_t* out_hmac,
    size_t out_hmac_cap,
    size_t* out_hmac_len)
{
  my_signer* s = (my_signer*)self;
  (void)s;
  (void)data;
  (void)data_len;
  if (out_hmac == NULL || out_hmac_len == NULL || out_hmac_cap < 32)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* REPLACE ME: HMAC-SHA256 over `data` with the key behind s->key_handle,
   * writing the 32 raw bytes to out_hmac. */
  memset(out_hmac, 0, 32);
  *out_hmac_len = 32;
  return AZ_IOT_OK;
}

static void my_deinit(az_iot_sas_signer* self) { (void)self; }

static const az_iot_sas_signer_vtable k_my_vtable = {
  .version = AZ_IOT_SAS_SIGNER_VTABLE_VERSION,
  .sign = my_sign,
  .deinit = my_deinit,
};

int main(void)
{
  my_signer signer = { .base = { .vtable = &k_my_vtable }, .key_handle = 0x81000001u };

  static const uint8_t to_sign[] = "scope/registrations/my-device-id\n1700000000";
  uint8_t hmac[32];
  size_t hmac_len = 0;
  az_iot_result r = signer.base.vtable->sign(
      &signer.base, to_sign, sizeof(to_sign) - 1, hmac, sizeof(hmac), &hmac_len);
  fprintf(stderr, "[sas_signer] sign: %s, %zu bytes\n", az_iot_result_to_string(r), hmac_len);

  signer.base.vtable->deinit(&signer.base);
  return r == AZ_IOT_OK ? 0 : 1;
}
