// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_crypto_mbedtls.c
 * @brief az_iot_crypto on PSA Crypto.
 *
 * mbedTLS 4.x (ESP-IDF 6.0) makes the legacy mbedtls_sha256_* APIs private,
 * so this uses PSA Crypto (psa/crypto.h) only.
 *
 * SHA-256: the psa_hash_operation_t lives inside az_iot_sha256_ctx.
 */
#include "az_iot_crypto_mbedtls.h"

#include <stddef.h>

#include "psa/crypto.h"

/** @brief Fails to compile when psa_hash_operation_t does not fit az_iot_sha256_ctx. */
typedef char az_iot_psa_hash_fits_ctx
    [(sizeof(psa_hash_operation_t) <= sizeof(((az_iot_sha256_ctx*)0)->opaque)) ? 1 : -1];

/** @brief Measures the alignment of psa_hash_operation_t (C99 has no alignof). */
struct psa_hash_align
{
  char c;
  psa_hash_operation_t op;
};
/** @brief Measures the alignment of az_iot_sha256_ctx::opaque. */
struct ctx_align
{
  char c;
  uint64_t u;
};
/** @brief Fails to compile when psa_hash_operation_t needs stricter alignment than the context. */
typedef char az_iot_psa_hash_aligned_in_ctx
    [(offsetof(struct psa_hash_align, op) <= offsetof(struct ctx_align, u)) ? 1 : -1];

/* The operation lives in az_iot_sha256_ctx storage of another declared type.
 * may_alias exempts accesses through this wrapper from GCC/Clang type-based
 * alias analysis. The SDK touches the context only through this backend and
 * as bytes. */
#if defined(__GNUC__) || defined(__clang__)
#define AZ_IOT_MAY_ALIAS __attribute__((__may_alias__))
#else
#define AZ_IOT_MAY_ALIAS
#endif

/** @brief The PSA operation as stored in az_iot_sha256_ctx. */
typedef struct AZ_IOT_MAY_ALIAS psa_hash_box
{
  psa_hash_operation_t op; /**< The operation. */
} psa_hash_box;

/* ------------------------------------------------------------------------- */
/* backend                                                                   */
/* ------------------------------------------------------------------------- */

/** @brief The PSA operation stored in @p ctx. */
static psa_hash_operation_t* op_of(az_iot_sha256_ctx* ctx)
{
  return &((psa_hash_box*)(void*)ctx->opaque)->op;
}

static az_iot_result psa_sha256_init(const az_iot_crypto* self, az_iot_sha256_ctx* ctx)
{
  (void)self;
  if (ctx == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  psa_hash_operation_t* op = op_of(ctx);
  *op = psa_hash_operation_init(); /* inactive: safe to abort until setup succeeds */
  if (psa_crypto_init() != PSA_SUCCESS)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  if (psa_hash_setup(op, PSA_ALG_SHA_256) != PSA_SUCCESS)
  {
    psa_hash_abort(op);
    return AZ_IOT_ERR_INTERNAL;
  }
  return AZ_IOT_OK;
}

static az_iot_result psa_sha256_update(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    const uint8_t* data,
    size_t len)
{
  (void)self;
  if (ctx == NULL || (data == NULL && len != 0))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return psa_hash_update(op_of(ctx), data, len) == PSA_SUCCESS ? AZ_IOT_OK : AZ_IOT_ERR_INTERNAL;
}

static az_iot_result psa_sha256_final(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  (void)self;
  if (ctx == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (out == NULL)
  {
    return psa_hash_abort(op_of(ctx)) == PSA_SUCCESS ? AZ_IOT_OK : AZ_IOT_ERR_INTERNAL;
  }
  size_t hash_len = 0;
  psa_status_t st = psa_hash_finish(op_of(ctx), out, AZ_IOT_SHA256_SIZE, &hash_len);
  if (st != PSA_SUCCESS)
  {
    psa_hash_abort(op_of(ctx));
    return AZ_IOT_ERR_INTERNAL;
  }
  return hash_len == AZ_IOT_SHA256_SIZE ? AZ_IOT_OK : AZ_IOT_ERR_INTERNAL;
}

static const az_iot_crypto k_mbedtls = {
  .version = AZ_IOT_CRYPTO_VERSION,
  .sha256_init = psa_sha256_init,
  .sha256_update = psa_sha256_update,
  .sha256_final = psa_sha256_final,
};

const az_iot_crypto* az_iot_crypto_mbedtls(void) { return &k_mbedtls; }
