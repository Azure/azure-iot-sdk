// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_crypto_mbedtls.c
 * @brief az_iot_crypto on PSA Crypto.
 *
 * mbedTLS 4.x (ESP-IDF 6.0) makes the legacy mbedtls_rsa_* / mbedtls_sha256_*
 * APIs private, so this uses PSA Crypto (psa/crypto.h) only.
 *
 * RS256: PSA imports an RSA public key from its PKCS#1 RSAPublicKey DER
 * (RFC 8017: SEQUENCE { INTEGER n, INTEGER e }), so the raw big-endian
 * modulus and exponent are DER-wrapped before psa_import_key().
 *
 * SHA-256: the psa_hash_operation_t lives inside az_iot_sha256_ctx.
 */
#include "az_iot_crypto_mbedtls.h"

#include <string.h>

#include "psa/crypto.h"

/** @brief Fails to compile when psa_hash_operation_t does not fit az_iot_sha256_ctx. */
typedef char az_iot_psa_hash_fits_ctx
    [(sizeof(psa_hash_operation_t) <= sizeof(((az_iot_sha256_ctx*)0)->opaque)) ? 1 : -1];

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
/* backend                                                                   */
/* ------------------------------------------------------------------------- */

/** @brief The PSA operation stored in @p ctx. */
static psa_hash_operation_t* op_of(az_iot_sha256_ctx* ctx)
{
  return (psa_hash_operation_t*)(void*)ctx->opaque;
}

static az_iot_result psa_verify_rs256(
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
  if (!modulus || !modulus_len || !exponent || !exponent_len || !data || !signature
      || !signature_len)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  if (psa_crypto_init() != PSA_SUCCESS)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  uint8_t der[DER_BUFFER_SIZE];
  size_t der_len
      = build_rsa_public_der(der, sizeof(der), modulus, modulus_len, exponent, exponent_len);
  if (der_len == 0)
  {
    return AZ_IOT_ERR_AUTH;
  }

  psa_algorithm_t alg = PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256);
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
  psa_set_key_algorithm(&attr, alg);

  psa_key_id_t key = 0;
  if (psa_import_key(&attr, der, der_len, &key) != PSA_SUCCESS)
  {
    return AZ_IOT_ERR_AUTH;
  }

  az_iot_result result = AZ_IOT_ERR_AUTH;
  uint8_t hash[AZ_IOT_SHA256_SIZE];
  size_t hash_len = 0;
  if (psa_hash_compute(PSA_ALG_SHA_256, data, data_len, hash, sizeof(hash), &hash_len)
          == PSA_SUCCESS
      && psa_verify_hash(key, alg, hash, hash_len, signature, signature_len) == PSA_SUCCESS)
  {
    result = AZ_IOT_OK;
  }

  psa_destroy_key(key);
  return result;
}

static az_iot_result psa_sha256_init(const az_iot_crypto* self, az_iot_sha256_ctx* ctx)
{
  (void)self;
  if (ctx == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (psa_crypto_init() != PSA_SUCCESS)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  psa_hash_operation_t* op = op_of(ctx);
  memset(op, 0, sizeof(*op)); /* equivalent to PSA_HASH_OPERATION_INIT */
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
    psa_hash_abort(op_of(ctx));
    return AZ_IOT_OK;
  }
  size_t hash_len = 0;
  psa_status_t st = psa_hash_finish(op_of(ctx), out, AZ_IOT_SHA256_SIZE, &hash_len);
  if (st != PSA_SUCCESS)
  {
    psa_hash_abort(op_of(ctx));
    return AZ_IOT_ERR_INTERNAL;
  }
  return AZ_IOT_OK;
}

static const az_iot_crypto k_mbedtls = {
  .version = AZ_IOT_CRYPTO_VERSION,
  .sha256_init = psa_sha256_init,
  .sha256_update = psa_sha256_update,
  .sha256_final = psa_sha256_final,
  .verify_rs256 = psa_verify_rs256,
};

const az_iot_crypto* az_iot_crypto_mbedtls(void) { return &k_mbedtls; }
