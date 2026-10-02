// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_crypto_openssl.c
 * @brief az_iot_crypto on the public OpenSSL 3.0 EVP / OSSL_PARAM APIs.
 *
 * The SHA-256 context holds a pointer to an EVP_MD_CTX, which OpenSSL keeps
 * opaque.
 */
#include "az_iot_crypto_openssl.h"

#include <limits.h>
#include <stdint.h>

#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>

/* Build an EVP_PKEY (RSA public key) from raw big-endian modulus + exponent.
 * Returns NULL on failure; caller frees with EVP_PKEY_free. */
static EVP_PKEY* rsa_pubkey_from_raw(
    const uint8_t* modulus,
    size_t modulus_len,
    const uint8_t* exponent,
    size_t exponent_len)
{
  EVP_PKEY* pkey = NULL;
  BIGNUM* n = NULL;
  BIGNUM* e = NULL;
  OSSL_PARAM_BLD* bld = NULL;
  OSSL_PARAM* params = NULL;
  EVP_PKEY_CTX* ctx = NULL;

  if (modulus == NULL || modulus_len == 0 || exponent == NULL || exponent_len == 0)
  {
    return NULL;
  }

  n = BN_bin2bn(modulus, (int)modulus_len, NULL);
  e = BN_bin2bn(exponent, (int)exponent_len, NULL);
  /* A valid RSA public exponent is odd and at least 3 (RFC 8017 3.1); OpenSSL does not
   * check this on import or verify, and with e = 1 any encoded message verifies. */
  if (n == NULL || e == NULL || !BN_is_odd(e) || BN_is_one(e))
  {
    goto cleanup;
  }

  bld = OSSL_PARAM_BLD_new();
  if (bld == NULL || OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) != 1
      || OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e) != 1)
  {
    goto cleanup;
  }

  params = OSSL_PARAM_BLD_to_param(bld);
  if (params == NULL)
  {
    goto cleanup;
  }

  ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
  if (ctx == NULL || EVP_PKEY_fromdata_init(ctx) != 1
      || EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) != 1)
  {
    pkey = NULL; /* fromdata may have left pkey unset; ensure NULL */
  }

cleanup:
  if (ctx != NULL)
  {
    EVP_PKEY_CTX_free(ctx);
  }
  if (params != NULL)
  {
    OSSL_PARAM_free(params);
  }
  if (bld != NULL)
  {
    OSSL_PARAM_BLD_free(bld);
  }
  if (e != NULL)
  {
    BN_free(e);
  }
  if (n != NULL)
  {
    BN_free(n);
  }
  return pkey;
}

/** @brief The EVP_MD_CTX stored in @p ctx. */
static EVP_MD_CTX* md_of(const az_iot_sha256_ctx* ctx)
{
  return (EVP_MD_CTX*)(uintptr_t)ctx->opaque[0];
}

static az_iot_result openssl_verify_rs256(
    const az_iot_crypto* self,
    const uint8_t* modulus,
    size_t modulus_len,
    const uint8_t* exponent,
    size_t exponent_len,
    const uint8_t* data,
    size_t data_len,
    const uint8_t* signature,
    size_t signature_len)
{
  (void)self;
  if (modulus == NULL || modulus_len == 0 || exponent == NULL || exponent_len == 0 || data == NULL
      || signature == NULL || signature_len == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* BN_bin2bn() takes an int length. */
  if (modulus_len > (size_t)INT_MAX || exponent_len > (size_t)INT_MAX)
  {
    return AZ_IOT_ERR_AUTH;
  }

  EVP_PKEY* pkey = rsa_pubkey_from_raw(modulus, modulus_len, exponent, exponent_len);
  if (pkey == NULL)
  {
    return AZ_IOT_ERR_AUTH;
  }

  az_iot_result result = AZ_IOT_ERR_AUTH;
  EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
  if (md_ctx != NULL)
  {
    /* EVP_DigestVerifyInit defaults to RSASSA-PKCS1-v1_5 padding for RSA
     * keys, which is exactly RS256 when paired with SHA-256. */
    if (EVP_DigestVerifyInit(md_ctx, NULL, EVP_sha256(), NULL, pkey) == 1
        && EVP_DigestVerify(md_ctx, signature, signature_len, data, data_len) == 1)
    {
      result = AZ_IOT_OK;
    }
    EVP_MD_CTX_free(md_ctx);
  }
  else
  {
    result = AZ_IOT_ERR_OUT_OF_MEMORY;
  }

  EVP_PKEY_free(pkey);
  return result;
}

static az_iot_result openssl_sha256_init(const az_iot_crypto* self, az_iot_sha256_ctx* ctx)
{
  (void)self;
  if (ctx == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
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
  .verify_rs256 = openssl_verify_rs256,
};

const az_iot_crypto* az_iot_crypto_openssl(void) { return &k_openssl; }
