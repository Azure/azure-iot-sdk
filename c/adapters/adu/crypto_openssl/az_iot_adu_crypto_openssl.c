// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL 3.0+ implementation of the ADU crypto primitive hooks. See the header
 * and docs/azure-device-update.md section 6 for the hooks-only design rationale.
 *
 * Uses only the public OpenSSL 3.0 EVP / OSSL_PARAM APIs (no deprecated
 * low-level RSA_* / SHA256_* calls), so it builds clean against OpenSSL 3.x. */
#include "az_iot_adu_crypto_openssl.h"

#include <string.h>

#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>

/* Build an EVP_PKEY (RSA public key) from raw big-endian modulus + exponent.
 * Returns NULL on failure; caller frees with EVP_PKEY_free. */
static EVP_PKEY* rsa_pubkey_from_raw(
    const uint8_t* modulus, size_t modulus_len,
    const uint8_t* exponent, size_t exponent_len)
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
    if (n == NULL || e == NULL)
    {
        goto cleanup;
    }

    bld = OSSL_PARAM_BLD_new();
    if (bld == NULL
        || OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) != 1
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
    if (ctx == NULL
        || EVP_PKEY_fromdata_init(ctx) != 1
        || EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) != 1)
    {
        pkey = NULL; /* fromdata may have left pkey unset; ensure NULL */
    }

cleanup:
    if (ctx != NULL) EVP_PKEY_CTX_free(ctx);
    if (params != NULL) OSSL_PARAM_free(params);
    if (bld != NULL) OSSL_PARAM_BLD_free(bld);
    if (e != NULL) BN_free(e);
    if (n != NULL) BN_free(n);
    return pkey;
}

static int32_t openssl_verify_rs256(
    const uint8_t* modulus, size_t modulus_len,
    const uint8_t* exponent, size_t exponent_len,
    const uint8_t* signed_data, size_t signed_data_len,
    const uint8_t* signature, size_t signature_len,
    void* user_ctx)
{
    (void)user_ctx;

    if (signed_data == NULL || signature == NULL || signature_len == 0)
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }

    EVP_PKEY* pkey = rsa_pubkey_from_raw(modulus, modulus_len, exponent, exponent_len);
    if (pkey == NULL)
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }

    int32_t result = AZ_IOT_ADU_RESULT_FAILURE;
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (md_ctx != NULL)
    {
        /* EVP_DigestVerifyInit defaults to RSASSA-PKCS1-v1_5 padding for RSA
         * keys, which is exactly RS256 when paired with SHA-256. */
        if (EVP_DigestVerifyInit(md_ctx, NULL, EVP_sha256(), NULL, pkey) == 1
            && EVP_DigestVerify(
                   md_ctx, signature, signature_len, signed_data, signed_data_len)
                == 1)
        {
            result = AZ_IOT_ADU_RESULT_SUCCESS;
        }
        EVP_MD_CTX_free(md_ctx);
    }

    EVP_PKEY_free(pkey);
    return result;
}

static int32_t openssl_sha256(
    const uint8_t* data, size_t data_len, uint8_t hash_out[32], void* user_ctx)
{
    (void)user_ctx;
    if (hash_out == NULL || (data == NULL && data_len != 0))
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }

    unsigned int out_len = 0;
    if (EVP_Digest(data, data_len, hash_out, &out_len, EVP_sha256(), NULL) != 1
        || out_len != 32)
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t openssl_sha256_init(void** ctx_out, void* user_ctx)
{
    (void)user_ctx;
    if (ctx_out == NULL)
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (md_ctx == NULL || EVP_DigestInit_ex(md_ctx, EVP_sha256(), NULL) != 1)
    {
        if (md_ctx != NULL) EVP_MD_CTX_free(md_ctx);
        *ctx_out = NULL;
        return AZ_IOT_ADU_RESULT_FAILURE;
    }
    *ctx_out = md_ctx;
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t openssl_sha256_update(
    void* ctx, const uint8_t* data, size_t len, void* user_ctx)
{
    (void)user_ctx;
    if (ctx == NULL || (data == NULL && len != 0))
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }
    if (EVP_DigestUpdate((EVP_MD_CTX*)ctx, data, len) != 1)
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t openssl_sha256_final(void* ctx, uint8_t hash_out[32], void* user_ctx)
{
    (void)user_ctx;
    if (ctx == NULL || hash_out == NULL)
    {
        /* Still free the context if present to avoid a leak on misuse. */
        if (ctx != NULL) EVP_MD_CTX_free((EVP_MD_CTX*)ctx);
        return AZ_IOT_ADU_RESULT_FAILURE;
    }

    unsigned int out_len = 0;
    int ok = EVP_DigestFinal_ex((EVP_MD_CTX*)ctx, hash_out, &out_len);
    EVP_MD_CTX_free((EVP_MD_CTX*)ctx);
    if (ok != 1 || out_len != 32)
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

az_iot_adu_crypto_hooks az_iot_adu_crypto_openssl_hooks(void)
{
    az_iot_adu_crypto_hooks hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.verify_rs256_fn = openssl_verify_rs256;
    hooks.sha256_fn = openssl_sha256;
    hooks.sha256_init_fn = openssl_sha256_init;
    hooks.sha256_update_fn = openssl_sha256_update;
    hooks.sha256_final_fn = openssl_sha256_final;
    hooks.user_ctx = NULL;
    return hooks;
}
