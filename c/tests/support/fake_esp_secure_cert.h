// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file fake_esp_secure_cert.h
 * @brief State of the desktop esp_secure_cert and PSA fakes (fake_esp_secure_cert/).
 *
 * Buffers handed out are heap copies without a NUL terminator, as partition data may be.
 */
#ifndef FAKE_ESP_SECURE_CERT_H
#define FAKE_ESP_SECURE_CERT_H

#include <stdbool.h>

#include "esp_secure_cert_read.h"

/** @brief Partition contents, PSA behaviour and call counts. */
typedef struct fake_esp_secure_cert_state
{
  const char* device_cert; /**< NULL: not in the partition. */
  const char* private_key; /**< NULL: not in the partition. */
  bool has_ds_context;
  psa_status_t import_status;
  int device_cert_reads, device_cert_frees;
  int private_key_reads, private_key_frees;
  int ds_context_reads, ds_context_frees;
  int imports, destroys;
  psa_key_attributes_t imported_attributes;
  const esp_ds_data_ctx_t* imported_ds_context;
  psa_key_id_t destroyed_key;
} fake_esp_secure_cert_state;

/** @brief The fakes' state; reset with fake_esp_secure_cert_reset(). */
extern fake_esp_secure_cert_state g_fake_esp_secure_cert;

/** @brief A partition with @p device_cert and @p private_key, a DS context, PSA succeeding. */
void fake_esp_secure_cert_reset(const char* device_cert, const char* private_key);

#endif /* FAKE_ESP_SECURE_CERT_H */
