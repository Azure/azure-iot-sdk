// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Phase 2 - ADU OpenSSL crypto adapter unit tests.
 *
 * Validates the primitive hooks directly: SHA-256 (one-shot + incremental)
 * against a known NIST vector, and RS256 verify via a self-generated RSA
 * keypair sign/verify roundtrip (plus tamper-rejection). The test links OpenSSL
 * only to GENERATE the test vectors; the adapter under test is the only code
 * that performs verification. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/core_names.h>

#include "az_iot_adu_crypto_openssl.h"

/* SHA-256("abc") per NIST FIPS 180-4. */
static const uint8_t k_sha256_abc[32]
    = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };

static void sha256_oneshot_matches_known_vector(void** state)
{
  (void)state;
  az_iot_adu_crypto_hooks h = az_iot_adu_crypto_openssl_hooks();
  uint8_t out[32];
  assert_int_equal(
      h.sha256_fn((const uint8_t*)"abc", 3, out, h.user_ctx), AZ_IOT_ADU_RESULT_SUCCESS);
  assert_memory_equal(out, k_sha256_abc, 32);
}

static void sha256_incremental_matches_known_vector(void** state)
{
  (void)state;
  az_iot_adu_crypto_hooks h = az_iot_adu_crypto_openssl_hooks();
  void* ctx = NULL;
  assert_int_equal(h.sha256_init_fn(&ctx, h.user_ctx), AZ_IOT_ADU_RESULT_SUCCESS);
  assert_non_null(ctx);
  /* Feed "a" then "bc" to exercise the streaming path. */
  assert_int_equal(
      h.sha256_update_fn(ctx, (const uint8_t*)"a", 1, h.user_ctx), AZ_IOT_ADU_RESULT_SUCCESS);
  assert_int_equal(
      h.sha256_update_fn(ctx, (const uint8_t*)"bc", 2, h.user_ctx), AZ_IOT_ADU_RESULT_SUCCESS);
  uint8_t out[32];
  assert_int_equal(h.sha256_final_fn(ctx, out, h.user_ctx), AZ_IOT_ADU_RESULT_SUCCESS);
  assert_memory_equal(out, k_sha256_abc, 32);
}

/* Generate an RSA-2048 keypair, return raw big-endian modulus + exponent and a
 * signature over `data` (RS256). Caller frees the returned buffers. */
static EVP_PKEY* gen_key_and_sign(
    const uint8_t* data,
    size_t data_len,
    uint8_t* modulus,
    size_t* modulus_len,
    uint8_t* exponent,
    size_t* exponent_len,
    uint8_t* sig,
    size_t* sig_len)
{
  EVP_PKEY* pkey = EVP_RSA_gen(2048);
  assert_non_null(pkey);

  BIGNUM* n = NULL;
  BIGNUM* e = NULL;
  assert_int_equal(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n), 1);
  assert_int_equal(EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e), 1);
  *modulus_len = (size_t)BN_bn2bin(n, modulus);
  *exponent_len = (size_t)BN_bn2bin(e, exponent);
  BN_free(n);
  BN_free(e);

  EVP_MD_CTX* md = EVP_MD_CTX_new();
  assert_non_null(md);
  assert_int_equal(EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, pkey), 1);
  size_t needed = 0;
  assert_int_equal(EVP_DigestSign(md, NULL, &needed, data, data_len), 1);
  assert_true(needed <= 512);
  assert_int_equal(EVP_DigestSign(md, sig, &needed, data, data_len), 1);
  *sig_len = needed;
  EVP_MD_CTX_free(md);
  return pkey;
}

static void verify_rs256_accepts_valid_signature(void** state)
{
  (void)state;
  az_iot_adu_crypto_hooks h = az_iot_adu_crypto_openssl_hooks();

  const uint8_t data[] = "the quick brown fox";
  uint8_t modulus[512], exponent[16], sig[512];
  size_t mlen = 0, elen = 0, slen = 0;
  EVP_PKEY* pkey
      = gen_key_and_sign(data, sizeof(data) - 1, modulus, &mlen, exponent, &elen, sig, &slen);

  assert_int_equal(
      h.verify_rs256_fn(
          modulus, mlen, exponent, elen, data, sizeof(data) - 1, sig, slen, h.user_ctx),
      AZ_IOT_ADU_RESULT_SUCCESS);
  EVP_PKEY_free(pkey);
}

static void verify_rs256_rejects_tampered_signature(void** state)
{
  (void)state;
  az_iot_adu_crypto_hooks h = az_iot_adu_crypto_openssl_hooks();

  const uint8_t data[] = "the quick brown fox";
  uint8_t modulus[512], exponent[16], sig[512];
  size_t mlen = 0, elen = 0, slen = 0;
  EVP_PKEY* pkey
      = gen_key_and_sign(data, sizeof(data) - 1, modulus, &mlen, exponent, &elen, sig, &slen);

  sig[slen / 2] ^= 0xFF; /* flip a byte in the signature */
  assert_int_equal(
      h.verify_rs256_fn(
          modulus, mlen, exponent, elen, data, sizeof(data) - 1, sig, slen, h.user_ctx),
      AZ_IOT_ADU_RESULT_FAILURE);
  EVP_PKEY_free(pkey);
}

static void verify_rs256_rejects_modified_data(void** state)
{
  (void)state;
  az_iot_adu_crypto_hooks h = az_iot_adu_crypto_openssl_hooks();

  const uint8_t data[] = "the quick brown fox";
  uint8_t modulus[512], exponent[16], sig[512];
  size_t mlen = 0, elen = 0, slen = 0;
  EVP_PKEY* pkey
      = gen_key_and_sign(data, sizeof(data) - 1, modulus, &mlen, exponent, &elen, sig, &slen);

  const uint8_t other[] = "the quick brown FOX";
  assert_int_equal(
      h.verify_rs256_fn(
          modulus, mlen, exponent, elen, other, sizeof(other) - 1, sig, slen, h.user_ctx),
      AZ_IOT_ADU_RESULT_FAILURE);
  EVP_PKEY_free(pkey);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(sha256_oneshot_matches_known_vector),
    cmocka_unit_test(sha256_incremental_matches_known_vector),
    cmocka_unit_test(verify_rs256_accepts_valid_signature),
    cmocka_unit_test(verify_rs256_rejects_tampered_signature),
    cmocka_unit_test(verify_rs256_rejects_modified_data),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
