// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop stand-in for the PSA Crypto calls the esp_secure_cert provider makes. */
#ifndef FAKE_PSA_CRYPTO_H
#define FAKE_PSA_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t psa_status_t;
typedef uint32_t psa_key_id_t;
typedef uint32_t psa_key_lifetime_t;
typedef uint32_t psa_algorithm_t;
typedef uint16_t psa_key_type_t;
typedef uint32_t psa_key_usage_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_NOT_SUPPORTED ((psa_status_t) - 134)
#define PSA_KEY_TYPE_RSA_KEY_PAIR ((psa_key_type_t)0x7001)
#define PSA_KEY_USAGE_SIGN_HASH ((psa_key_usage_t)0x1000)
#define PSA_ALG_ANY_HASH ((psa_algorithm_t)0x020000ff)
#define PSA_ALG_RSA_PKCS1V15_SIGN(hash) ((psa_algorithm_t)(0x06000200 | ((hash) & 0xff)))

/** @brief What the provider sets, so a test can check it. */
typedef struct psa_key_attributes_t
{
  psa_key_type_t type;
  size_t bits;
  psa_key_usage_t usage;
  psa_algorithm_t algorithm;
  psa_key_lifetime_t lifetime;
} psa_key_attributes_t;

// clang-format off
#define PSA_KEY_ATTRIBUTES_INIT { 0 }
// clang-format on

#define psa_set_key_type(a, t) ((a)->type = (t))
#define psa_set_key_bits(a, b) ((a)->bits = (b))
#define psa_set_key_usage_flags(a, u) ((a)->usage = (u))
#define psa_set_key_algorithm(a, alg) ((a)->algorithm = (alg))
#define psa_set_key_lifetime(a, l) ((a)->lifetime = (l))

psa_status_t psa_crypto_init(void);
psa_status_t psa_import_key(
    const psa_key_attributes_t* attributes,
    const uint8_t* data,
    size_t data_length,
    psa_key_id_t* key);
psa_status_t psa_destroy_key(psa_key_id_t key);
void psa_reset_key_attributes(psa_key_attributes_t* attributes);

#endif /* FAKE_PSA_CRYPTO_H */
