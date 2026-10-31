// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file sample_tpm_sim.h
 * @brief A TPM 2.0 simulated in software (OpenSSL), for samples and tests
 * only: it holds every key in process memory and protects none of them.
 *
 * It does what DPS TPM attestation needs from a TPM: an RSA 2048 endorsement
 * key (EK) and storage root key (SRK), TPM2_ActivateCredential and TPM2_Import
 * of the key DPS sends, and TPM2_HMAC with the imported key. With a real TPM,
 * implement az_iot_dps_tpm_ops with your TSS instead.
 */

#ifndef SAMPLE_TPM_SIM_H
#define SAMPLE_TPM_SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_dps_tpm_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief Simulated TPM; opaque. */
  typedef struct sample_tpm_sim sample_tpm_sim;

  /**
   * @brief Creates a simulated TPM.
   *
   * @param[in] state_dir Directory holding `ek.pem` and `srk.pem`; created
   *                      keys are written there, so the same EK (and
   *                      registration ID) is used on the next run. NULL: new
   *                      keys, kept in memory only.
   * @return The TPM, or NULL on failure.
   */
  sample_tpm_sim* sample_tpm_sim_create(const char* state_dir);

  /**
   * @brief Creates a simulated TPM from PEM private keys, for fixed test vectors.
   *
   * @return The TPM, or NULL on failure.
   */
  sample_tpm_sim* sample_tpm_sim_create_from_pem(const char* ek_pem, const char* srk_pem);

  /** @brief Destroys @p sim, wiping its keys. NULL is ignored. */
  void sample_tpm_sim_destroy(sample_tpm_sim* sim);

  /**
   * @brief Marshalled TPM2B_PUBLIC of the EK, as DPS enrollments and
   * az_iot_dps_tpm_client_options::endorsement_key take it.
   */
  void sample_tpm_sim_ek_public(const sample_tpm_sim* sim, const uint8_t** data, size_t* len);

  /** @brief Marshalled TPM2B_PUBLIC of the SRK. */
  void sample_tpm_sim_srk_public(const sample_tpm_sim* sim, const uint8_t** data, size_t* len);

  /** @brief TPM operations for az_iot_dps_tpm_client_options::tpm, bound to @p sim. */
  az_iot_dps_tpm_ops sample_tpm_sim_ops(sample_tpm_sim* sim);

  /**
   * @brief TPM2_HMAC (SHA-256) with the imported key.
   *
   * @return false when no key has been imported or HMAC fails.
   */
  bool sample_tpm_sim_hmac(
      sample_tpm_sim* sim,
      const uint8_t* data,
      size_t len,
      uint8_t out[AZ_IOT_SHA256_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLE_TPM_SIM_H */
