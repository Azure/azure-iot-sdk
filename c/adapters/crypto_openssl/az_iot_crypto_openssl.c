// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_crypto_openssl.c
 * @brief az_iot_crypto on the public OpenSSL 3.0 EVP APIs.
 *
 * The SHA-256 context holds a pointer to an EVP_MD_CTX, which OpenSSL keeps
 * opaque.
 */
#include "az_iot_crypto_openssl.h"

#include <stdint.h>

#include <openssl/evp.h>

/** @brief The EVP_MD_CTX stored in @p ctx. */
static EVP_MD_CTX* md_of(const az_iot_sha256_ctx* ctx)
{
  return (EVP_MD_CTX*)(uintptr_t)ctx->opaque[0];
}

static az_iot_result openssl_sha256_init(const az_iot_crypto* self, az_iot_sha256_ctx* ctx)
{
  (void)self;
  if (ctx == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  ctx->opaque[0] = 0; /* nothing to release until init succeeds */
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  if (md == NULL)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  if (EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1)
  {
    EVP_MD_CTX_free(md);
    return AZ_IOT_ERR_INTERNAL;
  }
  ctx->opaque[0] = (uint64_t)(uintptr_t)md;
  return AZ_IOT_OK;
}

static az_iot_result openssl_sha256_update(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    const uint8_t* data,
    size_t len)
{
  (void)self;
  if (ctx == NULL || md_of(ctx) == NULL || (data == NULL && len != 0))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return EVP_DigestUpdate(md_of(ctx), data, len) == 1 ? AZ_IOT_OK : AZ_IOT_ERR_INTERNAL;
}

static az_iot_result openssl_sha256_final(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  (void)self;
  if (ctx == NULL || md_of(ctx) == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  EVP_MD_CTX* md = md_of(ctx);
  ctx->opaque[0] = 0;

  az_iot_result result = AZ_IOT_OK;
  if (out != NULL)
  {
    unsigned int out_len = 0;
    if (EVP_DigestFinal_ex(md, out, &out_len) != 1 || out_len != AZ_IOT_SHA256_SIZE)
    {
      result = AZ_IOT_ERR_INTERNAL;
    }
  }
  EVP_MD_CTX_free(md);
  return result;
}

static const az_iot_crypto k_openssl = {
  .version = AZ_IOT_CRYPTO_VERSION,
  .sha256_init = openssl_sha256_init,
  .sha256_update = openssl_sha256_update,
  .sha256_final = openssl_sha256_final,
};

const az_iot_crypto* az_iot_crypto_openssl(void) { return &k_openssl; }
