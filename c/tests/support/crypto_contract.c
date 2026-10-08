// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file crypto_contract.c
 * @brief Known-answer contract suite shared by every az_iot_crypto backend.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_su.h"
#include "crypto_contract.h"
#include "internal/crypto.h"
#include "su_crypto_vectors.h"

#define SU_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/** @brief Backend under test for the running group. */
static const az_iot_crypto* g_crypto;

/* ------------------------------------------------------------------------- */
/* SHA-256                                                                   */
/* ------------------------------------------------------------------------- */

/** @brief Materialize a vector's full input (message repeated). Caller frees. */
static uint8_t* sha256_input(const su_sha256_vector* v, size_t* out_len)
{
  size_t len = v->message_len * v->repeat;
  uint8_t* buf = (uint8_t*)malloc(len == 0 ? 1 : len);
  assert_non_null(buf);
  for (size_t i = 0; i < v->repeat; ++i)
  {
    memcpy(buf + i * v->message_len, v->message, v->message_len);
  }
  *out_len = len;
  return buf;
}

static void sha256_oneshot_matches_known_answers(void** state)
{
  (void)state;
  for (size_t i = 0; i < SU_ARRAY_LEN(k_su_sha256_kat); ++i)
  {
    const su_sha256_vector* v = &k_su_sha256_kat[i];
    size_t len = 0;
    uint8_t* in = sha256_input(v, &len);
    uint8_t out[32];
    memset(out, 0, sizeof(out));
    if (az_iot_crypto__sha256(g_crypto, in, len, out) != AZ_IOT_OK
        || memcmp(out, v->digest, sizeof(out)) != 0)
    {
      free(in);
      fail_msg("sha256: vector '%s' did not match", v->name);
    }
    free(in);
  }
}

static void sha256_incremental_matches_known_answers_for_any_chunking(void** state)
{
  (void)state;
  static const size_t k_chunks[] = { 1, 63, 64, 65, 4096 };
  for (size_t i = 0; i < SU_ARRAY_LEN(k_su_sha256_kat); ++i)
  {
    const su_sha256_vector* v = &k_su_sha256_kat[i];
    size_t len = 0;
    uint8_t* in = sha256_input(v, &len);
    for (size_t c = 0; c < SU_ARRAY_LEN(k_chunks); ++c)
    {
      size_t chunk = k_chunks[c];
      if (chunk == 1 && len > 4096)
      {
        continue; /* a million one-byte updates adds time, not coverage */
      }
      az_iot_sha256_ctx ctx;
      assert_int_equal(g_crypto->sha256_init(g_crypto, &ctx), AZ_IOT_OK);
      /* An empty update is legal anywhere in the stream and must not change the digest. */
      assert_int_equal(g_crypto->sha256_update(g_crypto, &ctx, NULL, 0), AZ_IOT_OK);
      for (size_t off = 0; off < len; off += chunk)
      {
        size_t n = (len - off < chunk) ? len - off : chunk;
        assert_int_equal(g_crypto->sha256_update(g_crypto, &ctx, in + off, n), AZ_IOT_OK);
      }
      assert_int_equal(g_crypto->sha256_update(g_crypto, &ctx, in, 0), AZ_IOT_OK);
      uint8_t out[32];
      memset(out, 0, sizeof(out));
      assert_int_equal(g_crypto->sha256_final(g_crypto, &ctx, out), AZ_IOT_OK);
      if (memcmp(out, v->digest, sizeof(out)) != 0)
      {
        free(in);
        fail_msg("incremental sha256: vector '%s' chunk %zu did not match", v->name, chunk);
      }
    }
    free(in);
  }
}

static void sha256_contexts_are_independent(void** state)
{
  (void)state;
  const su_sha256_vector* a = &k_su_sha256_kat[1]; /* abc */
  const su_sha256_vector* b = &k_su_sha256_kat[2]; /* 448-bit */
  assert_int_equal(a->repeat, 1);
  assert_int_equal(b->repeat, 1);

  az_iot_sha256_ctx ca;
  az_iot_sha256_ctx cb;
  assert_int_equal(g_crypto->sha256_init(g_crypto, &ca), AZ_IOT_OK);
  assert_int_equal(g_crypto->sha256_init(g_crypto, &cb), AZ_IOT_OK);
  for (size_t i = 0; i < b->message_len; ++i)
  {
    if (i < a->message_len)
    {
      assert_int_equal(g_crypto->sha256_update(g_crypto, &ca, a->message + i, 1), AZ_IOT_OK);
    }
    assert_int_equal(g_crypto->sha256_update(g_crypto, &cb, b->message + i, 1), AZ_IOT_OK);
  }
  uint8_t out_a[32];
  uint8_t out_b[32];
  assert_int_equal(g_crypto->sha256_final(g_crypto, &cb, out_b), AZ_IOT_OK);
  assert_int_equal(g_crypto->sha256_final(g_crypto, &ca, out_a), AZ_IOT_OK);
  assert_memory_equal(out_a, a->digest, 32);
  assert_memory_equal(out_b, b->digest, 32);
}

/* init() must not read what the caller's context held before: it is stack
 * memory in practice. Run under ASan in CI. */
static void sha256_init_ignores_prior_context_contents(void** state)
{
  (void)state;
  const su_sha256_vector* abc = &k_su_sha256_kat[1];
  az_iot_sha256_ctx ctx;
  uint8_t out[AZ_IOT_SHA256_SIZE];

  memset(&ctx, 0xA5, sizeof(ctx));
  assert_int_equal(g_crypto->sha256_init(g_crypto, &ctx), AZ_IOT_OK);
  assert_int_equal(
      g_crypto->sha256_update(g_crypto, &ctx, abc->message, abc->message_len), AZ_IOT_OK);
  assert_int_equal(g_crypto->sha256_final(g_crypto, &ctx, out), AZ_IOT_OK);
  assert_memory_equal(out, abc->digest, AZ_IOT_SHA256_SIZE);

  memset(&ctx, 0xA5, sizeof(ctx));
  assert_int_equal(g_crypto->sha256_init(g_crypto, &ctx), AZ_IOT_OK);
  assert_int_equal(g_crypto->sha256_final(g_crypto, &ctx, NULL), AZ_IOT_OK);
}

/* Run under valgrind in CI: every final, with or without output, releases the context. */
static void sha256_rejects_bad_arguments_without_leaking(void** state)
{
  (void)state;
  const su_sha256_vector* abc = &k_su_sha256_kat[1];
  uint8_t out[32];

  assert_int_equal(az_iot_crypto__sha256(g_crypto, NULL, 0, out), AZ_IOT_OK);
  assert_memory_equal(out, k_su_sha256_kat[0].digest, 32);
  assert_int_not_equal(az_iot_crypto__sha256(g_crypto, NULL, 1, out), AZ_IOT_OK);
  assert_int_not_equal(
      az_iot_crypto__sha256(g_crypto, abc->message, abc->message_len, NULL), AZ_IOT_OK);

  assert_int_not_equal(g_crypto->sha256_init(g_crypto, NULL), AZ_IOT_OK);
  assert_int_not_equal(g_crypto->sha256_update(g_crypto, NULL, abc->message, 1), AZ_IOT_OK);
  assert_int_not_equal(g_crypto->sha256_final(g_crypto, NULL, out), AZ_IOT_OK);

  /* A rejected update leaves the stream untouched. */
  az_iot_sha256_ctx ctx;
  assert_int_equal(g_crypto->sha256_init(g_crypto, &ctx), AZ_IOT_OK);
  assert_int_equal(
      g_crypto->sha256_update(g_crypto, &ctx, abc->message, abc->message_len), AZ_IOT_OK);
  assert_int_not_equal(g_crypto->sha256_update(g_crypto, &ctx, NULL, 5), AZ_IOT_OK);
  assert_int_equal(g_crypto->sha256_final(g_crypto, &ctx, out), AZ_IOT_OK);
  assert_memory_equal(out, abc->digest, 32);

  /* final() with no output buffer only releases the context. */
  assert_int_equal(g_crypto->sha256_init(g_crypto, &ctx), AZ_IOT_OK);
  assert_int_equal(g_crypto->sha256_update(g_crypto, &ctx, abc->message, 1), AZ_IOT_OK);
  assert_int_equal(g_crypto->sha256_final(g_crypto, &ctx, NULL), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* HMAC-SHA256, composed by the SDK over the backend's SHA-256               */
/* ------------------------------------------------------------------------- */

/** @brief One HMAC-SHA256 known answer. */
typedef struct hmac_vector
{
  const char* name;
  const char* key_text; /**< Key text, or NULL to build it from the next fields. */
  uint8_t key_byte; /**< Key is key_len copies of this, unless key_from is set. */
  int key_from; /**< Non-zero: key is key_from - 1, key_from, ... (one per byte). */
  size_t key_len;
  const char* data; /**< Data, unless data_byte is non-zero. */
  uint8_t data_byte; /**< Data is data_len copies of this. */
  size_t data_len;
  uint8_t mac[32];
} hmac_vector;

/* RFC 4231 test cases 1-4, 6, 7 (5 is truncated output), plus a key of exactly
 * one block and an empty key and message. */
static const hmac_vector k_hmac[] = {
  { "rfc4231-1", NULL, 0x0b, 0, 20, "Hi There", 0, 8, { 0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38,
                                                        0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b,
                                                        0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9,
                                                        0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c,
                                                        0x2e, 0x32, 0xcf, 0xf7 } },
  { "rfc4231-2",
    "Jefe",
    0,
    0,
    4,
    "what do ya want for nothing?",
    0,
    28,
    { 0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
      0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
      0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43 } },
  { "rfc4231-3", NULL, 0xaa, 0, 20, NULL, 0xdd, 50, { 0x77, 0x3e, 0xa9, 0x1e, 0x36, 0x80, 0x0e,
                                                      0x46, 0x85, 0x4d, 0xb8, 0xeb, 0xd0, 0x91,
                                                      0x81, 0xa7, 0x29, 0x59, 0x09, 0x8b, 0x3e,
                                                      0xf8, 0xc1, 0x22, 0xd9, 0x63, 0x55, 0x14,
                                                      0xce, 0xd5, 0x65, 0xfe } },
  { "rfc4231-4",
    NULL,
    0,
    2,
    25,
    NULL,
    0xcd,
    50,
    { 0x82, 0x55, 0x8a, 0x38, 0x9a, 0x44, 0x3c, 0x0e, 0xa4, 0xcc, 0x81,
      0x98, 0x99, 0xf2, 0x08, 0x3a, 0x85, 0xf0, 0xfa, 0xa3, 0xe5, 0x78,
      0xf8, 0x07, 0x7a, 0x2e, 0x3f, 0xf4, 0x67, 0x29, 0x66, 0x5b } },
  { "rfc4231-6",
    NULL,
    0xaa,
    0,
    131,
    "Test Using Larger Than Block-Size Key - Hash Key First",
    0,
    54,
    { 0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f, 0x0d, 0x8a, 0x26,
      0xaa, 0xcb, 0xf5, 0xb7, 0x7f, 0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28,
      0xc5, 0x14, 0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3, 0x7f, 0x54 } },
  { "rfc4231-7",
    NULL,
    0xaa,
    0,
    131,
    "This is a test using a larger than block-size key and a larger than block-size data. The key "
    "needs to be hashed before being used by the HMAC algorithm.",
    0,
    152,
    { 0x9b, 0x09, 0xff, 0xa7, 0x1b, 0x94, 0x2f, 0xcb, 0x27, 0x63, 0x5f,
      0xbc, 0xd5, 0xb0, 0xe9, 0x44, 0xbf, 0xdc, 0x63, 0x64, 0x4f, 0x07,
      0x13, 0x93, 0x8a, 0x7f, 0x51, 0x53, 0x5c, 0x3a, 0x35, 0xe2 } },
  { "key-of-one-block", NULL, 0, 1, 64, "block", 0, 5, { 0x0c, 0x41, 0x95, 0x06, 0x4d, 0xd4, 0xca,
                                                         0x79, 0x59, 0x9d, 0x2b, 0x85, 0x08, 0xc3,
                                                         0xf4, 0xc5, 0xf3, 0x80, 0x50, 0x80, 0xdd,
                                                         0x6b, 0x45, 0x1c, 0xa3, 0x0e, 0x7d, 0x2d,
                                                         0xa7, 0x3a, 0x2c, 0x3d } },
  { "empty-key-and-data",
    NULL,
    0,
    0,
    0,
    "",
    0,
    0,
    { 0xb6, 0x13, 0x67, 0x9a, 0x08, 0x14, 0xd9, 0xec, 0x77, 0x2f, 0x95,
      0xd7, 0x78, 0xc3, 0x5f, 0xc5, 0xff, 0x16, 0x97, 0xc4, 0x93, 0x71,
      0x56, 0x53, 0xc6, 0xc7, 0x12, 0x14, 0x42, 0x92, 0xc5, 0xad } },
};

static void hmac_sha256_matches_known_answers(void** state)
{
  (void)state;
  for (size_t i = 0; i < SU_ARRAY_LEN(k_hmac); ++i)
  {
    const hmac_vector* v = &k_hmac[i];
    uint8_t key[131];
    uint8_t data[160];
    assert_true(v->key_len <= sizeof(key) && v->data_len <= sizeof(data));
    for (size_t k = 0; k < v->key_len; ++k)
    {
      if (v->key_text != NULL)
      {
        key[k] = (uint8_t)v->key_text[k];
      }
      else if (v->key_from != 0)
      {
        key[k] = (uint8_t)((size_t)v->key_from - 1u + k);
      }
      else
      {
        key[k] = v->key_byte;
      }
    }
    if (v->data_byte != 0)
    {
      memset(data, v->data_byte, v->data_len);
    }
    else
    {
      memcpy(data, v->data, v->data_len);
    }
    uint8_t mac[32];
    memset(mac, 0, sizeof(mac));
    if (az_iot_crypto__hmac_sha256(g_crypto, key, v->key_len, data, v->data_len, mac) != AZ_IOT_OK
        || memcmp(mac, v->mac, sizeof(mac)) != 0)
    {
      fail_msg("hmac-sha256: vector '%s' did not match", v->name);
    }
  }
}

static void hmac_sha256_rejects_bad_arguments(void** state)
{
  (void)state;
  uint8_t mac[32];
  assert_int_equal(
      az_iot_crypto__hmac_sha256(g_crypto, NULL, 1, NULL, 0, mac), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_crypto__hmac_sha256(g_crypto, NULL, 0, NULL, 1, mac), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_crypto__hmac_sha256(g_crypto, NULL, 0, NULL, 0, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_crypto__hmac_sha256(NULL, NULL, 0, NULL, 0, mac), AZ_IOT_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------------- */
/* RS256                                                                     */
/* ------------------------------------------------------------------------- */

static az_iot_result verify_vector(const su_rs256_vector* v)
{
  return g_crypto->verify_rs256(
      g_crypto,
      v->modulus,
      v->modulus_len,
      v->exponent,
      v->exponent_len,
      v->message,
      v->message_len,
      v->signature,
      v->signature_len);
}

static void rs256_accepts_known_good_vectors(void** state)
{
  (void)state;
  for (size_t i = 0; i < SU_ARRAY_LEN(k_su_rs256_good); ++i)
  {
    if (verify_vector(&k_su_rs256_good[i]) != AZ_IOT_OK)
    {
      fail_msg("verify_rs256 rejected good vector '%s'", k_su_rs256_good[i].name);
    }
  }
}

static void rs256_rejects_known_bad_vectors(void** state)
{
  (void)state;
  size_t accepted = 0;
  for (size_t i = 0; i < SU_ARRAY_LEN(k_su_rs256_bad); ++i)
  {
    az_iot_result r = verify_vector(&k_su_rs256_bad[i]);
    if (r == AZ_IOT_OK)
    {
      print_error("verify_rs256 accepted bad vector '%s'\n", k_su_rs256_bad[i].name);
      ++accepted;
    }
  }
  assert_int_equal(accepted, 0);
}

static void rs256_rejects_missing_inputs(void** state)
{
  (void)state;
  const su_rs256_vector* g = &k_su_rs256_good[0];
  su_rs256_vector v;
  const struct
  {
    const char* name;
    size_t field;
  } k_cases[] = {
    { "modulus NULL", 0 },    { "modulus_len 0", 1 }, { "exponent NULL", 2 },
    { "exponent_len 0", 3 },  { "message NULL", 4 },  { "signature NULL", 5 },
    { "signature_len 0", 6 },
  };
  for (size_t i = 0; i < SU_ARRAY_LEN(k_cases); ++i)
  {
    v = *g;
    switch (k_cases[i].field)
    {
      case 0:
        v.modulus = NULL;
        break;
      case 1:
        v.modulus_len = 0;
        break;
      case 2:
        v.exponent = NULL;
        break;
      case 3:
        v.exponent_len = 0;
        break;
      case 4:
        v.message = NULL;
        break;
      case 5:
        v.signature = NULL;
        break;
      default:
        v.signature_len = 0;
        break;
    }
    if (verify_vector(&v) != AZ_IOT_ERR_INVALID_ARG)
    {
      fail_msg("verify_rs256 did not reject %s as an invalid argument", k_cases[i].name);
    }
  }
}

/* Keys sized to fill, then overflow, a fixed-size DER encoding buffer. Every
 * case must be rejected without writing out of bounds (run under ASan in CI). */
static void rs256_rejects_oversized_keys_safely(void** state)
{
  (void)state;
  static const struct
  {
    size_t modulus_len;
    size_t exponent_len;
  } k_cases[] = {
    { 594, 200 }, /* modulus leaves under 4 bytes; exponent header must not overrun */
    { 596, 3 },
    { 4096, 3 },
    { 70000, 3 }, /* beyond a 2-byte DER length */
  };
  static uint8_t modulus[70000];
  static uint8_t exponent[200];
  static uint8_t signature[512];
  memset(modulus, 0x01, sizeof(modulus));
  memset(exponent, 0x01, sizeof(exponent));
  exponent[sizeof(exponent) - 1] = 0x03;
  memset(signature, 0x5A, sizeof(signature));
  static const uint8_t data[] = "data";
  for (size_t i = 0; i < SU_ARRAY_LEN(k_cases); ++i)
  {
    az_iot_result r = g_crypto->verify_rs256(
        g_crypto,
        modulus,
        k_cases[i].modulus_len,
        exponent,
        k_cases[i].exponent_len,
        data,
        sizeof(data) - 1,
        signature,
        sizeof(signature));
    if (r == AZ_IOT_OK)
    {
      fail_msg("verify_rs256 accepted a %zu-byte modulus", k_cases[i].modulus_len);
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Through core: the backend only ever sees primitive inputs                 */
/* ------------------------------------------------------------------------- */

#define SU_MAX_RECORDED 4

/** @brief One recorded verify_rs256 call. */
typedef struct recorded_verify
{
  uint8_t modulus[1024];
  size_t modulus_len;
  uint8_t data[4096];
  size_t data_len;
  size_t signature_len;
} recorded_verify;

/** @brief Calls observed by the recording backend. */
static struct
{
  recorded_verify verify[SU_MAX_RECORDED];
  size_t verify_count;
  uint8_t sha256_data[4096]; /**< Bytes fed to the first SHA-256 stream. */
  size_t sha256_len;
  size_t sha256_count; /**< SHA-256 streams started. */
} g_rec;

static az_iot_result rec_verify_rs256(
    const az_iot_crypto* self,
    const uint8_t* modulus,
    size_t modulus_len,
    const uint8_t* exponent,
    size_t exponent_len,
    const uint8_t* signed_data,
    size_t signed_data_len,
    const uint8_t* signature,
    size_t signature_len)
{
  (void)self;
  if (g_rec.verify_count < SU_MAX_RECORDED && modulus_len <= sizeof(g_rec.verify[0].modulus)
      && signed_data_len <= sizeof(g_rec.verify[0].data))
  {
    recorded_verify* r = &g_rec.verify[g_rec.verify_count];
    memcpy(r->modulus, modulus, modulus_len);
    r->modulus_len = modulus_len;
    memcpy(r->data, signed_data, signed_data_len);
    r->data_len = signed_data_len;
    r->signature_len = signature_len;
  }
  ++g_rec.verify_count;
  return g_crypto->verify_rs256(
      g_crypto,
      modulus,
      modulus_len,
      exponent,
      exponent_len,
      signed_data,
      signed_data_len,
      signature,
      signature_len);
}

static az_iot_result rec_sha256_init(const az_iot_crypto* self, az_iot_sha256_ctx* ctx)
{
  (void)self;
  ++g_rec.sha256_count;
  return g_crypto->sha256_init(g_crypto, ctx);
}

static az_iot_result rec_sha256_update(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    const uint8_t* data,
    size_t len)
{
  (void)self;
  if (g_rec.sha256_count == 1 && g_rec.sha256_len + len <= sizeof(g_rec.sha256_data) && len > 0)
  {
    memcpy(g_rec.sha256_data + g_rec.sha256_len, data, len);
    g_rec.sha256_len += len;
  }
  return g_crypto->sha256_update(g_crypto, ctx, data, len);
}

static az_iot_result rec_sha256_final(
    const az_iot_crypto* self,
    az_iot_sha256_ctx* ctx,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  (void)self;
  return g_crypto->sha256_final(g_crypto, ctx, out);
}

static const az_iot_crypto k_recording = {
  .version = AZ_IOT_CRYPTO_VERSION,
  .sha256_init = rec_sha256_init,
  .sha256_update = rec_sha256_update,
  .sha256_final = rec_sha256_final,
  .verify_rs256 = rec_verify_rs256,
};

/** @brief Resets the recording and returns the recording backend. */
static const az_iot_crypto* recording_crypto(void)
{
  memset(&g_rec, 0, sizeof(g_rec));
  return &k_recording;
}

static az_iot_su_root_key chain_root(bool disabled)
{
  az_iot_su_root_key root;
  root.kid = SU_VEC_CHAIN_ROOT_KID;
  root.modulus = k_su_vec_chain_root_n;
  root.modulus_len = sizeof(k_su_vec_chain_root_n);
  root.exponent = k_su_vec_chain_root_e;
  root.exponent_len = sizeof(k_su_vec_chain_root_e);
  root.disabled = disabled;
  return root;
}

/** @brief Writable copy of a request; the parser decodes strings in place. */
static char g_request[8192];

static az_iot_result parse_chain(
    const char* request,
    const az_iot_crypto* crypto,
    const az_iot_su_root_key* root,
    az_iot_su_client_update_request* out_req,
    az_iot_su_client_update_manifest* out_manifest)
{
  size_t len = strlen(request);
  assert_true(len < sizeof(g_request));
  memcpy(g_request, request, len);
  memset(out_req, 0, sizeof(*out_req));
  memset(out_manifest, 0, sizeof(*out_manifest));
  return az_iot_su_parse_update_request(
      az_span_create((uint8_t*)g_request, (int32_t)len), crypto, root, 1, out_req, out_manifest);
}

static void chain_verifies_and_backend_sees_only_primitive_inputs(void** state)
{
  (void)state;
  const az_iot_crypto* crypto = recording_crypto();
  az_iot_su_root_key root = chain_root(false);
  az_iot_su_client_update_request req;
  az_iot_su_client_update_manifest manifest;

  assert_int_equal(parse_chain(k_su_vec_chain_request, crypto, &root, &req, &manifest), AZ_IOT_OK);
  assert_int_equal(manifest.files_count, 1);

  /* 1: SJWK under the root key; 2: manifest JWS under the SJWK's signing key. Each gets
   * the raw modulus, the ASCII signing input and a decoded, modulus-sized signature. */
  assert_int_equal(g_rec.verify_count, 2);
  const recorded_verify* sj = &g_rec.verify[0];
  assert_int_equal(sj->modulus_len, sizeof(k_su_vec_chain_root_n));
  assert_memory_equal(sj->modulus, k_su_vec_chain_root_n, sj->modulus_len);
  assert_int_equal(sj->data_len, strlen(k_su_vec_chain_sjwk_input));
  assert_memory_equal(sj->data, k_su_vec_chain_sjwk_input, sj->data_len);
  assert_int_equal(sj->signature_len, sizeof(k_su_vec_chain_root_n));

  const recorded_verify* mj = &g_rec.verify[1];
  assert_int_equal(mj->modulus_len, sizeof(k_su_vec_chain_signing_n));
  assert_memory_equal(mj->modulus, k_su_vec_chain_signing_n, mj->modulus_len);
  assert_int_equal(mj->data_len, strlen(k_su_vec_chain_manifest_input));
  assert_memory_equal(mj->data, k_su_vec_chain_manifest_input, mj->data_len);
  assert_int_equal(mj->signature_len, sizeof(k_su_vec_chain_signing_n));

  /* SHA-256 runs once, over the unescaped manifest body. */
  assert_int_equal(g_rec.sha256_count, 1);
  assert_int_equal(g_rec.sha256_len, strlen(k_su_vec_chain_manifest));
  assert_memory_equal(g_rec.sha256_data, k_su_vec_chain_manifest, g_rec.sha256_len);
}

static void chain_is_rejected_when_tampered_or_root_disabled(void** state)
{
  (void)state;
  az_iot_su_root_key root = chain_root(false);
  az_iot_su_client_update_request req;
  az_iot_su_client_update_manifest manifest;
  size_t mismatched = 0;

  for (size_t i = 0; i < SU_ARRAY_LEN(k_su_chain_tampered); ++i)
  {
    const su_chain_vector* v = &k_su_chain_tampered[i];
    const az_iot_crypto* crypto = recording_crypto();
    az_iot_result r = parse_chain(v->request, crypto, &root, &req, &manifest);
    if (r != AZ_IOT_ERR_AUTH || g_rec.verify_count != v->verify_calls
        || g_rec.sha256_count != v->sha256_calls)
    {
      print_error(
          "tampered chain '%s': result %d, %zu verify / %zu sha256 calls (expected %zu / %zu)\n",
          v->name,
          (int)r,
          g_rec.verify_count,
          g_rec.sha256_count,
          v->verify_calls,
          v->sha256_calls);
      ++mismatched;
    }
  }
  assert_int_equal(mismatched, 0);

  root = chain_root(true);
  assert_int_equal(
      parse_chain(k_su_vec_chain_request, g_crypto, &root, &req, &manifest), AZ_IOT_ERR_AUTH);
}

/** @brief Serves the deterministic payload, optionally with one byte flipped. */
typedef struct payload_reader
{
  size_t flip_at;
} payload_reader;

static int32_t read_payload(
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* read_ctx)
{
  const payload_reader* r = (const payload_reader*)read_ctx;
  size_t n = 0;
  while (n < buffer_size && offset + n < SU_VEC_FILE_PAYLOAD_LEN)
  {
    size_t at = offset + n;
    buffer[n] = (uint8_t)(su_crypto_file_byte(at) ^ (at == r->flip_at ? 0x01U : 0x00U));
    ++n;
  }
  *out_read = n;
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static void file_hash_matches_signed_manifest(void** state)
{
  (void)state;
  az_iot_su_root_key root = chain_root(false);
  az_iot_su_client_update_request req;
  az_iot_su_client_update_manifest manifest;
  assert_int_equal(
      parse_chain(k_su_vec_chain_request, g_crypto, &root, &req, &manifest), AZ_IOT_OK);
  assert_int_equal(manifest.files_count, 1);

  payload_reader intact = { SIZE_MAX };
  assert_int_equal(
      az_iot_su_verify_file_hash(&manifest.files[0], g_crypto, read_payload, &intact), AZ_IOT_OK);

  payload_reader flipped = { SU_VEC_FILE_PAYLOAD_LEN - 1 };
  assert_int_equal(
      az_iot_su_verify_file_hash(&manifest.files[0], g_crypto, read_payload, &flipped),
      AZ_IOT_ERR_AUTH);
}

int crypto_contract_run(const char* group_name, const az_iot_crypto* crypto)
{
  g_crypto = crypto;
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(sha256_oneshot_matches_known_answers),
    cmocka_unit_test(sha256_incremental_matches_known_answers_for_any_chunking),
    cmocka_unit_test(sha256_contexts_are_independent),
    cmocka_unit_test(sha256_init_ignores_prior_context_contents),
    cmocka_unit_test(sha256_rejects_bad_arguments_without_leaking),
    cmocka_unit_test(hmac_sha256_matches_known_answers),
    cmocka_unit_test(hmac_sha256_rejects_bad_arguments),
    cmocka_unit_test(rs256_accepts_known_good_vectors),
    cmocka_unit_test(rs256_rejects_known_bad_vectors),
    cmocka_unit_test(rs256_rejects_missing_inputs),
    cmocka_unit_test(rs256_rejects_oversized_keys_safely),
    cmocka_unit_test(chain_verifies_and_backend_sees_only_primitive_inputs),
    cmocka_unit_test(chain_is_rejected_when_tampered_or_root_disabled),
    cmocka_unit_test(file_hash_matches_signed_manifest),
  };
  return cmocka_run_group_tests_name(group_name, tests, NULL, NULL);
}
