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

#include "crypto_contract.h"
#include "internal/crypto.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/** @brief Backend under test for the running group. */
static const az_iot_crypto* g_crypto;

/* ------------------------------------------------------------------------- */
/* SHA-256                                                                   */
/* ------------------------------------------------------------------------- */

/** @brief One SHA-256 case: @p message repeated @p repeat times hashes to @p digest. */
typedef struct sha256_vector
{
  const char* name;
  const uint8_t* message;
  size_t message_len;
  size_t repeat;
  const uint8_t* digest;
} sha256_vector;

/* clang-format off */
static const uint8_t k_sha256_digest_empty[32] = {
  0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8,
  0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
  0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
};
static const uint8_t k_sha256_digest_abc[32] = {
  0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
  0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
  0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};
static const uint8_t k_sha256_digest_448_bit[32] = {
  0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93,
  0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
  0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1,
};
static const uint8_t k_sha256_digest_896_bit[32] = {
  0xcf, 0x5b, 0x16, 0xa7, 0x78, 0xaf, 0x83, 0x80, 0x03, 0x6c, 0xe5, 0x9e,
  0x7b, 0x04, 0x92, 0x37, 0x0b, 0x24, 0x9b, 0x11, 0xe8, 0xf0, 0x7a, 0x51,
  0xaf, 0xac, 0x45, 0x03, 0x7a, 0xfe, 0xe9, 0xd1,
};
static const uint8_t k_sha256_digest_million_a[32] = {
  0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7, 0xe2,
  0x84, 0xd7, 0x3e, 0x67, 0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97, 0x20, 0x0e,
  0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0,
};
/* clang-format on */

#define SHA256_MESSAGE(text) (const uint8_t*)(text), sizeof(text) - 1

/** @brief SHA-256 known answers (FIPS 180-4). */
static const sha256_vector k_sha256_kat[] = {
  { "empty", SHA256_MESSAGE(""), 1, k_sha256_digest_empty },
  { "abc", SHA256_MESSAGE("abc"), 1, k_sha256_digest_abc },
  { "448_bit",
    SHA256_MESSAGE("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
    1,
    k_sha256_digest_448_bit },
  { "896_bit",
    SHA256_MESSAGE("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                   "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
    1,
    k_sha256_digest_896_bit },
  { "million_a", SHA256_MESSAGE("a"), 1000000, k_sha256_digest_million_a },
};

/** @brief Materialize a vector's full input (message repeated). Caller frees. */
static uint8_t* sha256_input(const sha256_vector* v, size_t* out_len)
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
  for (size_t i = 0; i < ARRAY_LEN(k_sha256_kat); ++i)
  {
    const sha256_vector* v = &k_sha256_kat[i];
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
  for (size_t i = 0; i < ARRAY_LEN(k_sha256_kat); ++i)
  {
    const sha256_vector* v = &k_sha256_kat[i];
    size_t len = 0;
    uint8_t* in = sha256_input(v, &len);
    for (size_t c = 0; c < ARRAY_LEN(k_chunks); ++c)
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
  const sha256_vector* a = &k_sha256_kat[1]; /* abc */
  const sha256_vector* b = &k_sha256_kat[2]; /* 448-bit */
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
  const sha256_vector* abc = &k_sha256_kat[1];
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
  const sha256_vector* abc = &k_sha256_kat[1];
  uint8_t out[32];

  assert_int_equal(az_iot_crypto__sha256(g_crypto, NULL, 0, out), AZ_IOT_OK);
  assert_memory_equal(out, k_sha256_kat[0].digest, 32);
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
  for (size_t i = 0; i < ARRAY_LEN(k_hmac); ++i)
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
  };
  return cmocka_run_group_tests_name(group_name, tests, NULL, NULL);
}
