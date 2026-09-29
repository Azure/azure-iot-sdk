// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file su_crypto_contract.c
 * @brief Known-answer contract suite shared by every software updates crypto adapter.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "su_crypto_contract.h"
#include "su_crypto_vectors.h"

#define SU_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/** @brief Adapter under test for the running group. */
static const az_iot_su_crypto_hooks* g_hooks;

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
    if (g_hooks->sha256_fn(in, len, out, g_hooks->user_ctx) != AZ_IOT_SU_RESULT_SUCCESS
        || memcmp(out, v->digest, sizeof(out)) != 0)
    {
      free(in);
      fail_msg("sha256_fn: vector '%s' did not match", v->name);
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
      void* ctx = NULL;
      assert_int_equal(g_hooks->sha256_init_fn(&ctx, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
      assert_non_null(ctx);
      /* An empty update is legal anywhere in the stream and must not change the digest. */
      assert_int_equal(
          g_hooks->sha256_update_fn(ctx, NULL, 0, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
      for (size_t off = 0; off < len; off += chunk)
      {
        size_t n = (len - off < chunk) ? len - off : chunk;
        assert_int_equal(
            g_hooks->sha256_update_fn(ctx, in + off, n, g_hooks->user_ctx),
            AZ_IOT_SU_RESULT_SUCCESS);
      }
      assert_int_equal(
          g_hooks->sha256_update_fn(ctx, in, 0, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
      uint8_t out[32];
      memset(out, 0, sizeof(out));
      assert_int_equal(
          g_hooks->sha256_final_fn(ctx, out, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
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

  void* ca = NULL;
  void* cb = NULL;
  assert_int_equal(g_hooks->sha256_init_fn(&ca, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(g_hooks->sha256_init_fn(&cb, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
  assert_ptr_not_equal(ca, cb);
  for (size_t i = 0; i < b->message_len; ++i)
  {
    if (i < a->message_len)
    {
      assert_int_equal(
          g_hooks->sha256_update_fn(ca, a->message + i, 1, g_hooks->user_ctx),
          AZ_IOT_SU_RESULT_SUCCESS);
    }
    assert_int_equal(
        g_hooks->sha256_update_fn(cb, b->message + i, 1, g_hooks->user_ctx),
        AZ_IOT_SU_RESULT_SUCCESS);
  }
  uint8_t out_a[32];
  uint8_t out_b[32];
  assert_int_equal(
      g_hooks->sha256_final_fn(cb, out_b, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(
      g_hooks->sha256_final_fn(ca, out_a, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
  assert_memory_equal(out_a, a->digest, 32);
  assert_memory_equal(out_b, b->digest, 32);
}

/* Run under valgrind in CI: every failing final must still release the context. */
static void sha256_rejects_bad_arguments_without_leaking(void** state)
{
  (void)state;
  const su_sha256_vector* abc = &k_su_sha256_kat[1];
  uint8_t out[32];

  assert_int_equal(g_hooks->sha256_fn(NULL, 0, out, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
  assert_memory_equal(out, k_su_sha256_kat[0].digest, 32);
  assert_int_equal(g_hooks->sha256_fn(NULL, 1, out, g_hooks->user_ctx), AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(
      g_hooks->sha256_fn(abc->message, abc->message_len, NULL, g_hooks->user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);

  assert_int_equal(g_hooks->sha256_init_fn(NULL, g_hooks->user_ctx), AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(
      g_hooks->sha256_update_fn(NULL, abc->message, 1, g_hooks->user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(
      g_hooks->sha256_final_fn(NULL, out, g_hooks->user_ctx), AZ_IOT_SU_RESULT_FAILURE);

  /* A rejected update leaves the stream untouched. */
  void* ctx = NULL;
  assert_int_equal(g_hooks->sha256_init_fn(&ctx, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(
      g_hooks->sha256_update_fn(ctx, abc->message, abc->message_len, g_hooks->user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(
      g_hooks->sha256_update_fn(ctx, NULL, 5, g_hooks->user_ctx), AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(g_hooks->sha256_final_fn(ctx, out, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
  assert_memory_equal(out, abc->digest, 32);

  /* final() with no output buffer fails and still frees the context. */
  ctx = NULL;
  assert_int_equal(g_hooks->sha256_init_fn(&ctx, g_hooks->user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(
      g_hooks->sha256_final_fn(ctx, NULL, g_hooks->user_ctx), AZ_IOT_SU_RESULT_FAILURE);
}

/* ------------------------------------------------------------------------- */
/* RS256                                                                     */
/* ------------------------------------------------------------------------- */

static int32_t verify_vector(const su_rs256_vector* v)
{
  return g_hooks->verify_rs256_fn(
      v->modulus,
      v->modulus_len,
      v->exponent,
      v->exponent_len,
      v->message,
      v->message_len,
      v->signature,
      v->signature_len,
      g_hooks->user_ctx);
}

static void rs256_accepts_known_good_vectors(void** state)
{
  (void)state;
  for (size_t i = 0; i < SU_ARRAY_LEN(k_su_rs256_good); ++i)
  {
    if (verify_vector(&k_su_rs256_good[i]) != AZ_IOT_SU_RESULT_SUCCESS)
    {
      fail_msg("verify_rs256_fn rejected good vector '%s'", k_su_rs256_good[i].name);
    }
  }
}

static void rs256_rejects_known_bad_vectors(void** state)
{
  (void)state;
  size_t accepted = 0;
  for (size_t i = 0; i < SU_ARRAY_LEN(k_su_rs256_bad); ++i)
  {
    int32_t r = verify_vector(&k_su_rs256_bad[i]);
    if (r != AZ_IOT_SU_RESULT_FAILURE)
    {
      print_error("verify_rs256_fn returned %d for bad vector '%s'\n", r, k_su_rs256_bad[i].name);
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
    if (verify_vector(&v) != AZ_IOT_SU_RESULT_FAILURE)
    {
      fail_msg("verify_rs256_fn accepted %s", k_cases[i].name);
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Through core: the adapter only ever sees primitive inputs                 */
/* ------------------------------------------------------------------------- */

#define SU_MAX_RECORDED 4

/** @brief One recorded verify_rs256_fn call. */
typedef struct recorded_verify
{
  uint8_t modulus[1024];
  size_t modulus_len;
  uint8_t data[4096];
  size_t data_len;
  size_t signature_len;
} recorded_verify;

/** @brief Calls observed by the recording hooks. */
static struct
{
  recorded_verify verify[SU_MAX_RECORDED];
  size_t verify_count;
  uint8_t sha256_data[4096];
  size_t sha256_len;
  size_t sha256_count;
} g_rec;

static int32_t rec_verify_rs256(
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
  return g_hooks->verify_rs256_fn(
      modulus,
      modulus_len,
      exponent,
      exponent_len,
      signed_data,
      signed_data_len,
      signature,
      signature_len,
      g_hooks->user_ctx);
}

static int32_t rec_sha256(
    const uint8_t* data,
    size_t data_len,
    uint8_t hash_out[32],
    void* user_ctx)
{
  (void)user_ctx;
  if (g_rec.sha256_count == 0 && data_len <= sizeof(g_rec.sha256_data))
  {
    memcpy(g_rec.sha256_data, data, data_len);
    g_rec.sha256_len = data_len;
  }
  ++g_rec.sha256_count;
  return g_hooks->sha256_fn(data, data_len, hash_out, g_hooks->user_ctx);
}

static az_iot_su_crypto_hooks recording_hooks(void)
{
  az_iot_su_crypto_hooks h = *g_hooks;
  h.verify_rs256_fn = rec_verify_rs256;
  h.sha256_fn = rec_sha256;
  memset(&g_rec, 0, sizeof(g_rec));
  return h;
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
    const az_iot_su_crypto_hooks* hooks,
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
      az_span_create((uint8_t*)g_request, (int32_t)len), hooks, root, 1, out_req, out_manifest);
}

static void chain_verifies_and_hooks_see_only_primitive_inputs(void** state)
{
  (void)state;
  az_iot_su_crypto_hooks hooks = recording_hooks();
  az_iot_su_root_key root = chain_root(false);
  az_iot_su_client_update_request req;
  az_iot_su_client_update_manifest manifest;

  assert_int_equal(parse_chain(k_su_vec_chain_request, &hooks, &root, &req, &manifest), AZ_IOT_OK);
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
    az_iot_su_crypto_hooks hooks = recording_hooks();
    az_iot_result r = parse_chain(v->request, &hooks, &root, &req, &manifest);
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
      parse_chain(k_su_vec_chain_request, g_hooks, &root, &req, &manifest), AZ_IOT_ERR_AUTH);
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
  assert_int_equal(parse_chain(k_su_vec_chain_request, g_hooks, &root, &req, &manifest), AZ_IOT_OK);
  assert_int_equal(manifest.files_count, 1);

  payload_reader intact = { SIZE_MAX };
  assert_int_equal(
      az_iot_su_verify_file_hash(&manifest.files[0], g_hooks, read_payload, &intact), AZ_IOT_OK);

  payload_reader flipped = { SU_VEC_FILE_PAYLOAD_LEN - 1 };
  assert_int_equal(
      az_iot_su_verify_file_hash(&manifest.files[0], g_hooks, read_payload, &flipped),
      AZ_IOT_ERR_AUTH);
}

int su_crypto_contract_run(const char* group_name, const az_iot_su_crypto_hooks* hooks)
{
  g_hooks = hooks;
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(sha256_oneshot_matches_known_answers),
    cmocka_unit_test(sha256_incremental_matches_known_answers_for_any_chunking),
    cmocka_unit_test(sha256_contexts_are_independent),
    cmocka_unit_test(sha256_rejects_bad_arguments_without_leaking),
    cmocka_unit_test(rs256_accepts_known_good_vectors),
    cmocka_unit_test(rs256_rejects_known_bad_vectors),
    cmocka_unit_test(rs256_rejects_missing_inputs),
    cmocka_unit_test(chain_verifies_and_hooks_see_only_primitive_inputs),
    cmocka_unit_test(chain_is_rejected_when_tampered_or_root_disabled),
    cmocka_unit_test(file_hash_matches_signed_manifest),
  };
  return cmocka_run_group_tests_name(group_name, tests, NULL, NULL);
}
