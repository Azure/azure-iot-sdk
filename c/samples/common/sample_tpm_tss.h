// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file sample_tpm_tss.h
 * @brief az_iot_dps_tpm_ops on a TPM 2.0 through tpm2-tss ESAPI, for samples
 * and tests.
 *
 * The EK and SRK are the persistent keys at SAMPLE_TPM_TSS_EK_HANDLE and
 * SAMPLE_TPM_TSS_SRK_HANDLE when the TPM has them, otherwise primaries created
 * from the TCG templates (EK: RSA 2048 template L-1; SRK: RSA 2048 storage key)
 * and flushed on destroy. import_key runs TPM2_ActivateCredential, TPM2_Import
 * and TPM2_Load, then persists the key with TPM2_EvictControl, replacing what
 * the persistent handle held. hmac_sha256 runs TPM2_HMAC with it. Owner,
 * endorsement and key authorizations must be empty.
 */

#ifndef SAMPLE_TPM_TSS_H
#define SAMPLE_TPM_TSS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_dps_tpm_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Persistent handle of the EK, when provisioned (TCG EK profile). */
#define SAMPLE_TPM_TSS_EK_HANDLE 0x81010001u
/** @brief Persistent handle of the SRK, when provisioned (TCG provisioning guidance). */
#define SAMPLE_TPM_TSS_SRK_HANDLE 0x81000001u
/** @brief Default persistent handle of the key DPS sends. */
#define SAMPLE_TPM_TSS_DEFAULT_HMAC_KEY_HANDLE 0x81000100u

  /** @brief A TPM opened through ESAPI; opaque. */
  typedef struct sample_tpm_tss sample_tpm_tss;

  /**
   * @brief Opens the TPM and loads its EK and SRK.
   *
   * @param[in] tcti_config     TCTI configuration for Tss2_TctiLdr_Initialize(),
   *                            e.g. `device:/dev/tpmrm0` or
   *                            `swtpm:host=127.0.0.1,port=2321`. NULL: the
   *                            tpm2-tss default.
   * @param[in] hmac_key_handle Persistent handle (0x81000000-0x817FFFFF) for the
   *                            key DPS sends, other than #SAMPLE_TPM_TSS_EK_HANDLE
   *                            and #SAMPLE_TPM_TSS_SRK_HANDLE; a key already there
   *                            is used by sample_tpm_tss_hmac() until import_key
   *                            replaces it.
   * @return The TPM, or NULL on failure.
   */
  sample_tpm_tss* sample_tpm_tss_create(const char* tcti_config, uint32_t hmac_key_handle);

  /** @brief Flushes the transient keys and closes @p tpm; the persisted key stays. NULL is
   * ignored. */
  void sample_tpm_tss_destroy(sample_tpm_tss* tpm);

  /**
   * @brief Marshalled TPM2B_PUBLIC of the EK, as DPS enrollments and
   * az_iot_dps_tpm_client_options::endorsement_key take it.
   */
  void sample_tpm_tss_ek_public(const sample_tpm_tss* tpm, const uint8_t** data, size_t* len);

  /** @brief Marshalled TPM2B_PUBLIC of the SRK. */
  void sample_tpm_tss_srk_public(const sample_tpm_tss* tpm, const uint8_t** data, size_t* len);

  /** @brief TPM operations for az_iot_dps_tpm_client_options::tpm, bound to @p tpm. */
  az_iot_dps_tpm_ops sample_tpm_tss_ops(sample_tpm_tss* tpm);

  /**
   * @brief TPM2_HMAC (SHA-256) with the persisted key.
   *
   * @param[in]  tpm  TPM.
   * @param[in]  data Data to sign; at most 1024 bytes (TPM2_MAX_DIGEST_BUFFER).
   * @param[in]  len  Bytes in @p data.
   * @param[out] out  MAC.
   * @return false when no key is persisted, @p data is too long or the TPM fails.
   */
  bool sample_tpm_tss_hmac(
      sample_tpm_tss* tpm,
      const uint8_t* data,
      size_t len,
      uint8_t out[AZ_IOT_SHA256_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLE_TPM_TSS_H */
