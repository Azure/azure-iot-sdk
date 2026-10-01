// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_sas_signer.h
 * @brief Pluggable SAS (symmetric key) signer.
 *
 * Set one per role in az_iot_connection_client_options::sas. A role with a
 * signer authenticates with a SAS token in the MQTT password; a role without
 * one uses X.509 from the certificate provider. The SDK builds the string to
 * sign and the token; the signer only computes the HMAC, so the key can stay
 * in a TPM or HSM.
 */
#ifndef AZ_IOT_SAS_SIGNER_H
#define AZ_IOT_SAS_SIGNER_H

#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Vtable version this header defines. */
#define AZ_IOT_SAS_SIGNER_VTABLE_VERSION 1u

  typedef struct az_iot_sas_signer az_iot_sas_signer;

  /** @brief Signer operations. */
  typedef struct az_iot_sas_signer_vtable
  {
    /** @brief Must be AZ_IOT_SAS_SIGNER_VTABLE_VERSION. */
    uint32_t version;

    /**
     * @brief HMAC-SHA256 over @p data with the signer's key.
     *
     * @param[in] self          Signer.
     * @param[in] data          String to sign.
     * @param[in] data_len      Length of @p data in bytes.
     * @param[out] out_hmac     Receives the raw (not base64) HMAC.
     * @param[in] out_hmac_cap  Capacity of @p out_hmac; at least 32.
     * @param[out] out_hmac_len Bytes written.
     * @return AZ_IOT_OK, or an error that fails the connect attempt.
     */
    az_iot_result (*sign)(
        az_iot_sas_signer* self,
        const uint8_t* data,
        size_t data_len,
        uint8_t* out_hmac,
        size_t out_hmac_cap,
        size_t* out_hmac_len);

    /**
     * @brief Releases the signer's resources. The SDK never calls it; the
     * owner does. May be NULL.
     */
    void (*deinit)(az_iot_sas_signer* self);
  } az_iot_sas_signer_vtable;

  /** @brief Signer base. Embed as the first member of an implementation. */
  struct az_iot_sas_signer
  {
    const az_iot_sas_signer_vtable* vtable; /**< Operations. */
  };

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SAS_SIGNER_H */
