// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* PSA-Crypto implementation of the ADU crypto primitive hooks.
 *
 * ESP-IDF v6.0 ships mbedTLS 4.x (TF-PSA-Crypto), where the legacy
 * mbedtls_rsa_* / mbedtls_sha256_* APIs are private. The supported native
 * primitive API is PSA Crypto (psa/crypto.h), so the hooks below are
 * implemented with it. See the header for the design rationale (hooks-only:
 * no JWS/base64/key resolution here).
 *
 * RS256 verification: ADU core hands us the signer's RSA public key as raw
 * big-endian (modulus, exponent). PSA imports an RSA public key from its PKCS#1
 * RSAPublicKey DER encoding (RFC 8017): SEQUENCE { INTEGER n, INTEGER e }, so
 * we DER-wrap the two integers before psa_import_key().
 */
#include "az_iot_adu_crypto_mbedtls.h"

#include <stdlib.h>
#include <string.h>

#include "psa/crypto.h"

/* ------------------------------------------------------------------------- */
/* minimal DER encoder for an RSA public key (SEQUENCE { INTEGER n, e })      */
/* ------------------------------------------------------------------------- */

/* Largest key we accommodate: 4096-bit modulus (512 bytes) + slack. */
#define DER_BUFFER_SIZE 600

/* Encode a DER length field; returns the number of bytes written (1..3). */
static size_t der_enc_len(uint8_t* out, size_t len)
{
  if (len < 0x80)
  {
    out[0] = (uint8_t)len;
    return 1;
  }
  if (len <= 0xFF)
  {
    out[0] = 0x81;
    out[1] = (uint8_t)len;
    return 2;
  }
  out[0] = 0x82;
  out[1] = (uint8_t)(len >> 8);
  out[2] = (uint8_t)(len & 0xFF);
  return 3;
}

/* Encode a positive DER INTEGER from big-endian bytes; returns bytes written,
 * or 0 on overflow of @p cap. */
static size_t der_enc_int(uint8_t* out, size_t cap, const uint8_t* val, size_t val_len)
{
  /* Strip leading zero bytes (keep at least one). */
  while (val_len > 1 && val[0] == 0x00)
  {
    val++;
    val_len--;
  }

  /* DER integers are signed: prepend 0x00 if the MSB is set so the value
   * stays positive. */
  size_t pad = (val_len > 0 && (val[0] & 0x80)) ? 1 : 0;
  size_t content_len = val_len + pad;

  size_t o = 0;
  if (o + 1 > cap)
  {
    return 0;
  }
  out[o++] = 0x02; /* INTEGER */
  o += der_enc_len(out + o, content_len);
  if (o + content_len > cap)
  {
    return 0;
  }
  if (pad)
  {
    out[o++] = 0x00;
  }
  memcpy(out + o, val, val_len);
  o += val_len;
  return o;
}

/* Build the PKCS#1 RSAPublicKey DER into @p der; returns its length or 0. */
static size_t build_rsa_public_der(
    uint8_t* der,
    size_t der_cap,
    const uint8_t* modulus,
    size_t modulus_len,
    const uint8_t* exponent,
    size_t exponent_len)
{
  uint8_t body[DER_BUFFER_SIZE];
  size_t bo = 0;
  size_t n;

  n = der_enc_int(body + bo, sizeof(body) - bo, modulus, modulus_len);
  if (n == 0)
  {
    return 0;
  }
  bo += n;
  n = der_enc_int(body + bo, sizeof(body) - bo, exponent, exponent_len);
  if (n == 0)
  {
    return 0;
  }
  bo += n;

  size_t o = 0;
  if (o + 1 > der_cap)
  {
    return 0;
  }
  der[o++] = 0x30; /* SEQUENCE */
  o += der_enc_len(der + o, bo);
  if (o + bo > der_cap)
  {
    return 0;
  }
  memcpy(der + o, body, bo);
  o += bo;
  return o;
}

/* ------------------------------------------------------------------------- */
/* crypto hooks                                                              */
/* ------------------------------------------------------------------------- */

static int32_t psa_verify_rs256(
    const uint8_t* modulus,
    size_t modulus_len,
    const uint8_t* exponent,
    size_t exponent_len,
    const uint8_t* signed_data,
    size_t signed_data_len,
    const uint8_t* signature,
    size_t signature_len,
    void* user_ctx)
{
  (void)user_ctx;
  if (!modulus || !modulus_len || !exponent || !exponent_len || !signed_data || !signature
      || !signature_len)
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }

  if (psa_crypto_init() != PSA_SUCCESS)
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }

  uint8_t der[DER_BUFFER_SIZE];
  size_t der_len
      = build_rsa_public_der(der, sizeof(der), modulus, modulus_len, exponent, exponent_len);
  if (der_len == 0)
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }

  psa_algorithm_t alg = PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256);
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
  psa_set_key_algorithm(&attr, alg);

  psa_key_id_t key = 0;
  if (psa_import_key(&attr, der, der_len, &key) != PSA_SUCCESS)
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }

  int32_t result = AZ_IOT_ADU_RESULT_FAILURE;
  uint8_t hash[32];
  size_t hash_len = 0;
  if (psa_hash_compute(PSA_ALG_SHA_256, signed_data, signed_data_len, hash, sizeof(hash), &hash_len)
          == PSA_SUCCESS
      && psa_verify_hash(key, alg, hash, hash_len, signature, signature_len) == PSA_SUCCESS)
  {
    result = AZ_IOT_ADU_RESULT_SUCCESS;
  }

  psa_destroy_key(key);
  return result;
}

static int32_t psa_sha256_oneshot(
    const uint8_t* data,
    size_t data_len,
    uint8_t hash_out[32],
    void* user_ctx)
{
  (void)user_ctx;
  if (!hash_out || (!data && data_len != 0))
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }
  if (psa_crypto_init() != PSA_SUCCESS)
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }

  size_t hash_len = 0;
  return (psa_hash_compute(PSA_ALG_SHA_256, data, data_len, hash_out, 32, &hash_len) == PSA_SUCCESS)
      ? AZ_IOT_ADU_RESULT_SUCCESS
      : AZ_IOT_ADU_RESULT_FAILURE;
}

static int32_t psa_sha256_begin(void** ctx_out, void* user_ctx)
{
  (void)user_ctx;
  if (!ctx_out)
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }
  if (psa_crypto_init() != PSA_SUCCESS)
  {
    *ctx_out = NULL;
    return AZ_IOT_ADU_RESULT_FAILURE;
  }

  psa_hash_operation_t* op = (psa_hash_operation_t*)malloc(sizeof(psa_hash_operation_t));
  if (!op)
  {
    *ctx_out = NULL;
    return AZ_IOT_ADU_RESULT_FAILURE;
  }
  memset(op, 0, sizeof(*op)); /* equivalent to PSA_HASH_OPERATION_INIT */

  if (psa_hash_setup(op, PSA_ALG_SHA_256) != PSA_SUCCESS)
  {
    free(op);
    *ctx_out = NULL;
    return AZ_IOT_ADU_RESULT_FAILURE;
  }
  *ctx_out = op;
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t psa_sha256_feed(void* ctx, const uint8_t* data, size_t len, void* user_ctx)
{
  (void)user_ctx;
  if (!ctx || (!data && len != 0))
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }
  return (psa_hash_update((psa_hash_operation_t*)ctx, data, len) == PSA_SUCCESS)
      ? AZ_IOT_ADU_RESULT_SUCCESS
      : AZ_IOT_ADU_RESULT_FAILURE;
}

static int32_t psa_sha256_end(void* ctx, uint8_t hash_out[32], void* user_ctx)
{
  (void)user_ctx;
  if (!ctx || !hash_out)
  {
    if (ctx)
    {
      psa_hash_abort((psa_hash_operation_t*)ctx);
      free(ctx);
    }
    return AZ_IOT_ADU_RESULT_FAILURE;
  }
  size_t hash_len = 0;
  psa_status_t st = psa_hash_finish((psa_hash_operation_t*)ctx, hash_out, 32, &hash_len);
  free(ctx);
  return (st == PSA_SUCCESS) ? AZ_IOT_ADU_RESULT_SUCCESS : AZ_IOT_ADU_RESULT_FAILURE;
}

az_iot_adu_crypto_hooks az_iot_adu_crypto_mbedtls_hooks(void)
{
  az_iot_adu_crypto_hooks hooks;
  memset(&hooks, 0, sizeof(hooks));
  hooks.verify_rs256_fn = psa_verify_rs256;
  hooks.sha256_fn = psa_sha256_oneshot;
  hooks.sha256_init_fn = psa_sha256_begin;
  hooks.sha256_update_fn = psa_sha256_feed;
  hooks.sha256_final_fn = psa_sha256_end;
  hooks.user_ctx = NULL;
  return hooks;
}
