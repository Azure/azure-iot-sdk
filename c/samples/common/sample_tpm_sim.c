// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file sample_tpm_sim.c
 * @brief TPM 2.0 simulated with OpenSSL: the EK and SRK, TPM2_ActivateCredential,
 * TPM2_Import and TPM2_HMAC, per TPM 2.0 Library Part 1 (sections 11.4, 23-24)
 * and the TCG EK Credential Profile (RSA template L-1).
 */

#include "sample_tpm_sim.h"
#include "sample_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>

/** @brief TPM_ALG_ID values used here. */
#define TPM_ALG_RSA 0x0001u
#define TPM_ALG_SHA256 0x000Bu
#define TPM_ALG_AES 0x0006u
#define TPM_ALG_KEYEDHASH 0x0008u
#define TPM_ALG_NULL 0x0010u
#define TPM_ALG_CFB 0x0043u

/** @brief Object attributes of the EK (template L-1) and of a typical SRK. */
#define SIM_EK_ATTRIBUTES 0x000300B2u
#define SIM_SRK_ATTRIBUTES 0x00030472u

#define SIM_RSA_BITS 2048
#define SIM_RSA_BYTES (SIM_RSA_BITS / 8)
/** @brief Marshalled TPM2B_PUBLIC of an RSA 2048 key with a 32-byte policy. */
#define SIM_PUBLIC_MAX (2 + 2 + 2 + 4 + 2 + 32 + 6 + 2 + 2 + 4 + 2 + SIM_RSA_BYTES)
/** @brief Largest imported HMAC key. */
#define SIM_HMAC_KEY_MAX 128
#define SIM_NAME_SIZE (2 + AZ_IOT_SHA256_SIZE)

/** @brief PolicySecret(TPM_RH_ENDORSEMENT): the EK template authPolicy. */
static const uint8_t ek_policy[AZ_IOT_SHA256_SIZE]
    = { 0x83, 0x71, 0x97, 0x67, 0x44, 0x84, 0xB3, 0xF8, 0x1A, 0x90, 0xCC,
        0x8D, 0x46, 0xA5, 0xD7, 0x24, 0xFD, 0x52, 0xD7, 0x6E, 0x06, 0x52,
        0x0B, 0x64, 0xF2, 0xA1, 0xDA, 0x1B, 0x33, 0x14, 0x69, 0xAA };

/** @brief One RSA key with its marshalled public area and Name. */
typedef struct sim_key
{
  EVP_PKEY* pkey;
  uint8_t pub[SIM_PUBLIC_MAX];
  size_t pub_len;
  uint8_t name[SIM_NAME_SIZE];
} sim_key;

struct sample_tpm_sim
{
  sim_key ek;
  sim_key srk;
  uint8_t hmac_key[SIM_HMAC_KEY_MAX];
  size_t hmac_key_len;
};

/** @brief Bounded reader over a byte buffer. */
typedef struct reader
{
  const uint8_t* p;
  size_t len;
  size_t pos;
} reader;

static bool read_u16(reader* r, uint16_t* out)
{
  if (r->len - r->pos < 2)
  {
    return false;
  }
  *out = (uint16_t)((r->p[r->pos] << 8) | r->p[r->pos + 1]);
  r->pos += 2;
  return true;
}

/** @brief Reads a TPM2B: its size, then that many bytes into @p data. */
static bool read_2b(reader* r, const uint8_t** data, size_t* len)
{
  uint16_t n = 0;
  if (!read_u16(r, &n) || r->len - r->pos < n)
  {
    return false;
  }
  *data = r->p + r->pos;
  *len = n;
  r->pos += n;
  return true;
}

static void put_u16(uint8_t** p, uint16_t v)
{
  (*p)[0] = (uint8_t)(v >> 8);
  (*p)[1] = (uint8_t)v;
  *p += 2;
}

static void put_u32(uint8_t** p, uint32_t v)
{
  (*p)[0] = (uint8_t)(v >> 24);
  (*p)[1] = (uint8_t)(v >> 16);
  (*p)[2] = (uint8_t)(v >> 8);
  (*p)[3] = (uint8_t)v;
  *p += 4;
}

static bool sha256(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len, uint8_t* out)
{
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  bool ok = md != NULL && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1
      && EVP_DigestUpdate(md, a, a_len) == 1 && (b_len == 0 || EVP_DigestUpdate(md, b, b_len) == 1)
      && EVP_DigestFinal_ex(md, out, NULL) == 1;
  EVP_MD_CTX_free(md);
  return ok;
}

/** @brief HMAC-SHA256 of @p a || @p b. */
static bool hmac_sha256(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* a,
    size_t a_len,
    const uint8_t* b,
    size_t b_len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  uint8_t empty = 0;
  EVP_MAC* mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
  EVP_MAC_CTX* ctx = mac != NULL ? EVP_MAC_CTX_new(mac) : NULL;
  OSSL_PARAM params[] = { OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, "SHA256", 0),
                          OSSL_PARAM_construct_end() };
  size_t out_len = 0;
  bool ok = ctx != NULL && EVP_MAC_init(ctx, key_len > 0 ? key : &empty, key_len, params) == 1
      && (a_len == 0 || EVP_MAC_update(ctx, a, a_len) == 1)
      && (b_len == 0 || EVP_MAC_update(ctx, b, b_len) == 1)
      && EVP_MAC_final(ctx, out, &out_len, AZ_IOT_SHA256_SIZE) == 1
      && out_len == AZ_IOT_SHA256_SIZE;
  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);
  return ok;
}

/** @brief KDFa (Part 1, 11.4.10.2) with SHA-256; @p label without its NUL. */
static bool kdfa(
    const uint8_t* key,
    size_t key_len,
    const char* label,
    const uint8_t* context_u,
    size_t context_u_len,
    size_t bits,
    uint8_t* out)
{
  uint8_t buf[4 + 16 + 1 + SIM_NAME_SIZE + 4];
  size_t label_len = strlen(label);
  size_t bytes = (bits + 7) / 8;
  if (label_len > 16 || context_u_len > SIM_NAME_SIZE)
  {
    return false;
  }
  for (uint32_t i = 1, done = 0; done < bytes; ++i)
  {
    uint8_t* p = buf;
    put_u32(&p, i);
    memcpy(p, label, label_len + 1);
    p += label_len + 1;
    if (context_u_len > 0)
    {
      memcpy(p, context_u, context_u_len);
      p += context_u_len;
    }
    put_u32(&p, (uint32_t)bits);
    uint8_t block[AZ_IOT_SHA256_SIZE];
    if (!hmac_sha256(key, key_len, buf, (size_t)(p - buf), NULL, 0, block))
    {
      return false;
    }
    size_t n = bytes - done < sizeof(block) ? bytes - done : sizeof(block);
    memcpy(out + done, block, n);
    done += (uint32_t)n;
    OPENSSL_cleanse(block, sizeof(block));
  }
  OPENSSL_cleanse(buf, sizeof(buf));
  return true;
}

/** @brief AES-CFB decryption with a zero IV; @p key_len 16 or 32. */
static bool aes_cfb_decrypt(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* in,
    size_t len,
    uint8_t* out)
{
  static const uint8_t iv[16] = { 0 };
  const EVP_CIPHER* cipher = key_len == 16 ? EVP_aes_128_cfb128()
      : key_len == 32                      ? EVP_aes_256_cfb128()
                                           : NULL;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  int n = 0;
  int fin = 0;
  bool ok = cipher != NULL && ctx != NULL && len <= (size_t)INT32_MAX
      && EVP_DecryptInit_ex(ctx, cipher, NULL, key, iv) == 1
      && EVP_DecryptUpdate(ctx, out, &n, in, (int)len) == 1
      && EVP_DecryptFinal_ex(ctx, out + n, &fin) == 1 && (size_t)n + (size_t)fin == len;
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

/** @brief RSA-OAEP (SHA-256) decryption with @p label, its NUL included, as the TPM does. */
static bool oaep_decrypt(
    EVP_PKEY* pkey,
    const char* label,
    const uint8_t* in,
    size_t in_len,
    uint8_t* out,
    size_t* out_len)
{
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, NULL);
  size_t label_len = strlen(label) + 1;
  void* label_copy = OPENSSL_memdup(label, label_len);
  bool ok = ctx != NULL && label_copy != NULL && EVP_PKEY_decrypt_init(ctx) == 1
      && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) == 1
      && EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) == 1
      && EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) == 1;
  if (ok && EVP_PKEY_CTX_set0_rsa_oaep_label(ctx, label_copy, (int)label_len) == 1)
  {
    label_copy = NULL; /* owned by ctx */
    ok = EVP_PKEY_decrypt(ctx, out, out_len, in, in_len) == 1;
  }
  else
  {
    ok = false;
  }
  OPENSSL_free(label_copy);
  EVP_PKEY_CTX_free(ctx);
  return ok;
}

/** @brief Fills @p key's marshalled public area and Name from its RSA key. */
static bool key_marshal(sim_key* key, uint32_t attributes, const uint8_t* policy, size_t policy_len)
{
  BIGNUM* n = NULL;
  uint8_t modulus[SIM_RSA_BYTES];
  if (EVP_PKEY_get_bn_param(key->pkey, OSSL_PKEY_PARAM_RSA_N, &n) != 1
      || BN_bn2binpad(n, modulus, (int)sizeof(modulus)) != (int)sizeof(modulus))
  {
    BN_free(n);
    return false;
  }
  BN_free(n);

  uint8_t* p = key->pub + 2; /* TPMT_PUBLIC after the TPM2B size */
  put_u16(&p, TPM_ALG_RSA);
  put_u16(&p, TPM_ALG_SHA256);
  put_u32(&p, attributes);
  put_u16(&p, (uint16_t)policy_len);
  if (policy_len > 0)
  {
    memcpy(p, policy, policy_len);
    p += policy_len;
  }
  put_u16(&p, TPM_ALG_AES); /* symmetric */
  put_u16(&p, 128);
  put_u16(&p, TPM_ALG_CFB);
  put_u16(&p, TPM_ALG_NULL); /* scheme */
  put_u16(&p, SIM_RSA_BITS);
  put_u32(&p, 0); /* exponent: 65537 */
  put_u16(&p, SIM_RSA_BYTES);
  memcpy(p, modulus, sizeof(modulus));
  p += sizeof(modulus);

  size_t tpmt_len = (size_t)(p - (key->pub + 2));
  uint8_t* size = key->pub;
  put_u16(&size, (uint16_t)tpmt_len);
  key->pub_len = tpmt_len + 2;

  uint8_t* name = key->name;
  put_u16(&name, TPM_ALG_SHA256);
  return sha256(key->pub + 2, tpmt_len, NULL, 0, name);
}

/** @brief Loads `<dir>/<file>` or, when absent, creates and stores a new RSA key. */
static EVP_PKEY* key_load_or_create(const char* dir, const char* file)
{
  char path[1024];
  if (dir != NULL)
  {
    int n = snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= sizeof(path))
    {
      return NULL;
    }
    BIO* in = BIO_new_file(path, "rb");
    if (in != NULL)
    {
      EVP_PKEY* pkey = PEM_read_bio_PrivateKey(in, NULL, NULL, NULL);
      BIO_free(in);
      return pkey;
    }
  }
  EVP_PKEY* pkey = EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)SIM_RSA_BITS);
  if (pkey != NULL && dir != NULL)
  {
    /* Owner-only: the files hold the device's private keys. */
    FILE* f = sample_file_open_private(path, false);
    BIO* out = f != NULL ? BIO_new_fp(f, BIO_CLOSE) : NULL;
    if (out == NULL && f != NULL)
    {
      (void)fclose(f);
    }
    bool ok = out != NULL && PEM_write_bio_PrivateKey(out, pkey, NULL, NULL, 0, NULL, NULL) == 1;
    ok = BIO_free(out) == 1 && ok;
    if (!ok)
    {
      EVP_PKEY_free(pkey);
      pkey = NULL;
    }
  }
  return pkey;
}

/** @brief Reads a PEM private key from @p pem. */
static EVP_PKEY* key_from_pem(const char* pem)
{
  BIO* in = BIO_new_mem_buf(pem, -1);
  EVP_PKEY* pkey = in != NULL ? PEM_read_bio_PrivateKey(in, NULL, NULL, NULL) : NULL;
  BIO_free(in);
  return pkey;
}

/** @brief Completes @p sim once its keys are set; destroys it on failure. */
static sample_tpm_sim* sim_finish(sample_tpm_sim* sim)
{
  if (sim->ek.pkey == NULL || sim->srk.pkey == NULL
      || !key_marshal(&sim->ek, SIM_EK_ATTRIBUTES, ek_policy, sizeof(ek_policy))
      || !key_marshal(&sim->srk, SIM_SRK_ATTRIBUTES, NULL, 0))
  {
    sample_tpm_sim_destroy(sim);
    return NULL;
  }
  return sim;
}

sample_tpm_sim* sample_tpm_sim_create(const char* state_dir)
{
  sample_tpm_sim* sim = (sample_tpm_sim*)calloc(1, sizeof(*sim));
  if (sim == NULL)
  {
    return NULL;
  }
  sim->ek.pkey = key_load_or_create(state_dir, "ek.pem");
  sim->srk.pkey = key_load_or_create(state_dir, "srk.pem");
  return sim_finish(sim);
}

sample_tpm_sim* sample_tpm_sim_create_from_pem(const char* ek_pem, const char* srk_pem)
{
  sample_tpm_sim* sim = (sample_tpm_sim*)calloc(1, sizeof(*sim));
  if (sim == NULL || ek_pem == NULL || srk_pem == NULL)
  {
    free(sim);
    return NULL;
  }
  sim->ek.pkey = key_from_pem(ek_pem);
  sim->srk.pkey = key_from_pem(srk_pem);
  return sim_finish(sim);
}

void sample_tpm_sim_destroy(sample_tpm_sim* sim)
{
  if (sim != NULL)
  {
    EVP_PKEY_free(sim->ek.pkey);
    EVP_PKEY_free(sim->srk.pkey);
    OPENSSL_cleanse(sim, sizeof(*sim));
    free(sim);
  }
}

void sample_tpm_sim_ek_public(const sample_tpm_sim* sim, const uint8_t** data, size_t* len)
{
  *data = sim->ek.pub;
  *len = sim->ek.pub_len;
}

void sample_tpm_sim_srk_public(const sample_tpm_sim* sim, const uint8_t** data, size_t* len)
{
  *data = sim->srk.pub;
  *len = sim->srk.pub_len;
}

/**
 * @brief Unwraps a credential protected by @p protector for the object
 * @p name: the seed from @p secret (OAEP, @p label), the HMAC check and the
 * CFB decryption, giving the content with its integrity removed.
 *
 * @param[in]  outer   TPM2B: integrity TPM2B_DIGEST, then the encrypted data.
 * @param[out] out     Decrypted data; at least as large as @p outer.
 * @param[out] out_len Bytes in @p out.
 */
static bool unwrap_outer(
    const sim_key* protector,
    const char* label,
    const uint8_t* name,
    size_t name_len,
    const uint8_t* secret,
    size_t secret_len,
    const uint8_t* outer,
    size_t outer_len,
    uint8_t* out,
    size_t* out_len)
{
  reader r = { outer, outer_len, 0 };
  const uint8_t* integrity = NULL;
  size_t integrity_len = 0;
  if (!read_2b(&r, &integrity, &integrity_len) || integrity_len != AZ_IOT_SHA256_SIZE)
  {
    return false;
  }
  const uint8_t* enc = outer + r.pos;
  size_t enc_len = outer_len - r.pos;

  uint8_t seed[SIM_RSA_BYTES];
  size_t seed_len = sizeof(seed);
  uint8_t hmac_key[AZ_IOT_SHA256_SIZE];
  uint8_t sym_key[16];
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  bool ok = oaep_decrypt(protector->pkey, label, secret, secret_len, seed, &seed_len)
      && kdfa(seed, seed_len, "INTEGRITY", NULL, 0, 8u * (size_t)AZ_IOT_SHA256_SIZE, hmac_key)
      && hmac_sha256(hmac_key, sizeof(hmac_key), enc, enc_len, name, name_len, mac)
      && CRYPTO_memcmp(mac, integrity, sizeof(mac)) == 0
      && kdfa(seed, seed_len, "STORAGE", name, name_len, 128, sym_key)
      && aes_cfb_decrypt(sym_key, sizeof(sym_key), enc, enc_len, out);
  *out_len = enc_len;
  OPENSSL_cleanse(seed, sizeof(seed));
  OPENSSL_cleanse(hmac_key, sizeof(hmac_key));
  OPENSSL_cleanse(sym_key, sizeof(sym_key));
  return ok;
}

/** @brief Contents of a marshalled TPM2B, its size prefix checked. */
static bool span_2b(az_span part, const uint8_t** data, size_t* len)
{
  reader r = { az_span_ptr(part), (size_t)az_span_size(part), 0 };
  return read_2b(&r, data, len) && r.pos == r.len;
}

static az_iot_result sim_import_key(void* ctx, const uint8_t* key, size_t key_len)
{
  sample_tpm_sim* sim = (sample_tpm_sim*)ctx;
  az_iot_dps_tpm_key_parts parts;
  if (sim == NULL || az_iot_dps_tpm_parse_key(key, key_len, &parts) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_PROTOCOL;
  }
  const uint8_t *blob, *secret, *dup, *seed, *pub;
  size_t blob_len, secret_len, dup_len, seed_len, pub_len;
  if (!span_2b(parts.credential_blob, &blob, &blob_len)
      || !span_2b(parts.secret, &secret, &secret_len) || !span_2b(parts.duplicate, &dup, &dup_len)
      || !span_2b(parts.in_sym_seed, &seed, &seed_len)
      || !span_2b(parts.object_public, &pub, &pub_len) || pub_len < 2 + 2
      || ((pub[2] << 8) | pub[3]) != TPM_ALG_SHA256)
  {
    return AZ_IOT_ERR_PROTOCOL;
  }

  /* Decrypted data is never longer than what it was decrypted from. */
  uint8_t* cred = (uint8_t*)malloc(blob_len);
  uint8_t* dup_plain = (uint8_t*)malloc(dup_len);
  uint8_t* sensitive = (uint8_t*)malloc(dup_len);
  if (cred == NULL || dup_plain == NULL || sensitive == NULL)
  {
    free(cred);
    free(dup_plain);
    free(sensitive);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }

  /* TPM2_ActivateCredential(SRK, EK): the inner wrapping key. */
  size_t cred_len = 0;
  const uint8_t* inner_key = NULL;
  size_t inner_key_len = 0;
  reader cr = { cred, 0, 0 };
  bool ok = unwrap_outer(
      &sim->ek,
      "IDENTITY",
      sim->srk.name,
      sizeof(sim->srk.name),
      secret,
      secret_len,
      blob,
      blob_len,
      cred,
      &cred_len);
  cr.len = cred_len;
  ok = ok && read_2b(&cr, &inner_key, &inner_key_len) && cr.pos == cr.len;

  /* TPM2_Import under the SRK. */
  uint8_t name[SIM_NAME_SIZE];
  uint8_t* np = name;
  put_u16(&np, TPM_ALG_SHA256);
  ok = ok && sha256(pub, pub_len, NULL, 0, np);
  size_t dup_plain_len = 0;
  ok = ok
      && unwrap_outer(
           &sim->srk,
           "DUPLICATE",
           name,
           sizeof(name),
           seed,
           seed_len,
           dup,
           dup_len,
           dup_plain,
           &dup_plain_len);
  ok = ok && aes_cfb_decrypt(inner_key, inner_key_len, dup_plain, dup_plain_len, sensitive);

  /* innerIntegrity, then TPM2B_SENSITIVE: checked, then the HMAC key taken. */
  reader sr = { sensitive, dup_plain_len, 0 };
  const uint8_t *integrity = NULL, *tpm2b_sensitive = NULL, *auth, *seed_value, *bits = NULL;
  size_t integrity_len = 0, tpm2b_len = 0, auth_len, seed_value_len, bits_len = 0;
  uint16_t type = 0;
  uint8_t digest[AZ_IOT_SHA256_SIZE];
  ok = ok && read_2b(&sr, &integrity, &integrity_len) && integrity_len == sizeof(digest);
  size_t sensitive_start = sr.pos;
  ok = ok && read_2b(&sr, &tpm2b_sensitive, &tpm2b_len) && sr.pos == sr.len
      && sha256(sensitive + sensitive_start, sr.pos - sensitive_start, name, sizeof(name), digest)
      && CRYPTO_memcmp(digest, integrity, sizeof(digest)) == 0;
  reader tr = { tpm2b_sensitive, tpm2b_len, 0 };
  ok = ok && read_u16(&tr, &type) && type == TPM_ALG_KEYEDHASH && read_2b(&tr, &auth, &auth_len)
      && read_2b(&tr, &seed_value, &seed_value_len) && read_2b(&tr, &bits, &bits_len)
      && bits_len > 0 && bits_len <= sizeof(sim->hmac_key);
  if (ok)
  {
    OPENSSL_cleanse(sim->hmac_key, sizeof(sim->hmac_key));
    memcpy(sim->hmac_key, bits, bits_len);
    sim->hmac_key_len = bits_len;
  }
  OPENSSL_cleanse(cred, blob_len);
  OPENSSL_cleanse(dup_plain, dup_len);
  OPENSSL_cleanse(sensitive, dup_len);
  free(cred);
  free(dup_plain);
  free(sensitive);
  return ok ? AZ_IOT_OK : AZ_IOT_ERR_AUTH;
}

bool sample_tpm_sim_hmac(
    sample_tpm_sim* sim,
    const uint8_t* data,
    size_t len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  return sim != NULL && sim->hmac_key_len > 0
      && hmac_sha256(sim->hmac_key, sim->hmac_key_len, data, len, NULL, 0, out);
}

static az_iot_result sim_hmac_op(
    void* ctx,
    const uint8_t* data,
    size_t data_len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  return sample_tpm_sim_hmac((sample_tpm_sim*)ctx, data, data_len, out) ? AZ_IOT_OK
                                                                        : AZ_IOT_ERR_INTERNAL;
}

az_iot_dps_tpm_ops sample_tpm_sim_ops(sample_tpm_sim* sim)
{
  az_iot_dps_tpm_ops ops;
  ops.import_key = sim_import_key;
  ops.hmac_sha256 = sim_hmac_op;
  ops.ctx = sim;
  return ops;
}
