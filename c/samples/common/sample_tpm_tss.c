// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file sample_tpm_tss.c
 * @brief az_iot_dps_tpm_ops with tpm2-tss ESAPI: TPM2_ActivateCredential,
 * TPM2_Import, TPM2_Load, TPM2_EvictControl and TPM2_HMAC.
 */

#include "sample_tpm_tss.h"

#include <stdlib.h>
#include <string.h>

#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_tctildr.h>

/** @brief Largest marshalled TPM2B_PUBLIC kept for the EK and SRK. */
#define TSS_PUBLIC_MAX AZ_IOT_DPS_TPM_RSA2048_PUBLIC_MAX

/** @brief PolicySecret(TPM_RH_ENDORSEMENT): the EK template authPolicy. */
static const uint8_t ek_policy[AZ_IOT_SHA256_SIZE]
    = { 0x83, 0x71, 0x97, 0x67, 0x44, 0x84, 0xB3, 0xF8, 0x1A, 0x90, 0xCC,
        0x8D, 0x46, 0xA5, 0xD7, 0x24, 0xFD, 0x52, 0xD7, 0x6E, 0x06, 0x52,
        0x0B, 0x64, 0xF2, 0xA1, 0xDA, 0x1B, 0x33, 0x14, 0x69, 0xAA };

/** @brief An EK or SRK: its ESAPI handle and marshalled public area. */
typedef struct tss_primary
{
  ESYS_TR handle;
  bool transient; /**< Created here, so flushed on release. */
  uint8_t pub[TSS_PUBLIC_MAX];
  size_t pub_len;
} tss_primary;

struct sample_tpm_tss
{
  TSS2_TCTI_CONTEXT* tcti;
  ESYS_CONTEXT* esys;
  tss_primary ek;
  tss_primary srk;
  uint32_t hmac_key_handle;
  ESYS_TR hmac_key; /**< ESYS_TR_NONE until a key is persisted. */
};

/** @brief An RSA 2048 restricted decryption key with AES-128-CFB: the EK and SRK templates. */
static TPM2B_PUBLIC rsa_storage_template(TPMA_OBJECT attributes)
{
  TPM2B_PUBLIC t;
  memset(&t, 0, sizeof(t));
  t.publicArea.type = TPM2_ALG_RSA;
  t.publicArea.nameAlg = TPM2_ALG_SHA256;
  t.publicArea.objectAttributes = attributes;
  t.publicArea.parameters.rsaDetail.symmetric.algorithm = TPM2_ALG_AES;
  t.publicArea.parameters.rsaDetail.symmetric.keyBits.aes = 128;
  t.publicArea.parameters.rsaDetail.symmetric.mode.aes = TPM2_ALG_CFB;
  t.publicArea.parameters.rsaDetail.scheme.scheme = TPM2_ALG_NULL;
  t.publicArea.parameters.rsaDetail.keyBits = 2048;
  t.publicArea.unique.rsa.size = 256;
  return t;
}

/**
 * @brief Sets @p exists to whether the TPM holds an object at persistent
 * @p handle. @return false when the TPM could not be queried.
 */
static bool persistent_lookup(ESYS_CONTEXT* esys, uint32_t handle, bool* exists)
{
  TPMI_YES_NO more = TPM2_NO;
  TPMS_CAPABILITY_DATA* caps = NULL;
  bool queried = Esys_GetCapability(
                     esys,
                     ESYS_TR_NONE,
                     ESYS_TR_NONE,
                     ESYS_TR_NONE,
                     TPM2_CAP_HANDLES,
                     handle,
                     1,
                     &more,
                     &caps)
      == TSS2_RC_SUCCESS;
  *exists = queried && caps->data.handles.count == 1 && caps->data.handles.handle[0] == handle;
  Esys_Free(caps);
  return queried;
}

/**
 * @brief Loads the persistent key at @p persistent_handle or, when absent,
 * creates a primary in @p hierarchy from @p key_template, and keeps its public area.
 */
static bool primary_load(
    ESYS_CONTEXT* esys,
    uint32_t persistent_handle,
    ESYS_TR hierarchy,
    const TPM2B_PUBLIC* key_template,
    tss_primary* out)
{
  TPM2B_PUBLIC* pub = NULL;
  TSS2_RC rc;
  out->transient = false;
  bool exists = false;
  if (!persistent_lookup(esys, persistent_handle, &exists))
  {
    return false;
  }
  if (exists)
  {
    rc = Esys_TR_FromTPMPublic(
        esys, persistent_handle, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &out->handle);
    if (rc == TSS2_RC_SUCCESS)
    {
      rc = Esys_ReadPublic(
          esys, out->handle, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &pub, NULL, NULL);
    }
  }
  else
  {
    TPM2B_SENSITIVE_CREATE sensitive;
    TPM2B_DATA outside_info;
    TPML_PCR_SELECTION pcrs;
    memset(&sensitive, 0, sizeof(sensitive));
    memset(&outside_info, 0, sizeof(outside_info));
    memset(&pcrs, 0, sizeof(pcrs));
    rc = Esys_CreatePrimary(
        esys,
        hierarchy,
        ESYS_TR_PASSWORD,
        ESYS_TR_NONE,
        ESYS_TR_NONE,
        &sensitive,
        key_template,
        &outside_info,
        &pcrs,
        &out->handle,
        &pub,
        NULL,
        NULL,
        NULL);
    out->transient = rc == TSS2_RC_SUCCESS;
  }
  size_t offset = 0;
  bool ok = rc == TSS2_RC_SUCCESS
      && Tss2_MU_TPM2B_PUBLIC_Marshal(pub, out->pub, sizeof(out->pub), &offset) == TSS2_RC_SUCCESS;
  out->pub_len = offset;
  Esys_Free(pub);
  return ok;
}

/** @brief Flushes a transient primary, or releases the handle of a persistent one. */
static void primary_release(ESYS_CONTEXT* esys, tss_primary* primary)
{
  if (primary->handle == ESYS_TR_NONE)
  {
    return;
  }
  if (primary->transient)
  {
    (void)Esys_FlushContext(esys, primary->handle);
  }
  else
  {
    (void)Esys_TR_Close(esys, &primary->handle);
  }
  primary->handle = ESYS_TR_NONE;
}

/** @brief The EK template: L-1 of the TCG EK Credential Profile. */
static TPM2B_PUBLIC ek_template(void)
{
  TPM2B_PUBLIC t = rsa_storage_template(
      TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT | TPMA_OBJECT_SENSITIVEDATAORIGIN
      | TPMA_OBJECT_ADMINWITHPOLICY | TPMA_OBJECT_RESTRICTED | TPMA_OBJECT_DECRYPT);
  t.publicArea.authPolicy.size = sizeof(ek_policy);
  memcpy(t.publicArea.authPolicy.buffer, ek_policy, sizeof(ek_policy));
  return t;
}

/**
 * @brief Loads the EK into @p tpm. TPMs hold few transient objects, so the
 * EK is loaded only while it is used; see primary_release().
 */
static bool ek_load(sample_tpm_tss* tpm)
{
  TPM2B_PUBLIC key_template = ek_template();
  return primary_load(
      tpm->esys, SAMPLE_TPM_TSS_EK_HANDLE, ESYS_TR_RH_ENDORSEMENT, &key_template, &tpm->ek);
}

sample_tpm_tss* sample_tpm_tss_create(const char* tcti_config, uint32_t hmac_key_handle)
{
  /* EvictControl would replace a persistent EK or SRK with the DPS key. */
  if (hmac_key_handle < TPM2_PERSISTENT_FIRST || hmac_key_handle > TPM2_PLATFORM_PERSISTENT - 1
      || hmac_key_handle == SAMPLE_TPM_TSS_EK_HANDLE
      || hmac_key_handle == SAMPLE_TPM_TSS_SRK_HANDLE)
  {
    return NULL;
  }
  sample_tpm_tss* tpm = (sample_tpm_tss*)calloc(1, sizeof(*tpm));
  if (tpm == NULL)
  {
    return NULL;
  }
  tpm->ek.handle = ESYS_TR_NONE;
  tpm->srk.handle = ESYS_TR_NONE;
  tpm->hmac_key = ESYS_TR_NONE;
  tpm->hmac_key_handle = hmac_key_handle;

  TPM2B_PUBLIC srk_template = rsa_storage_template(
      TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT | TPMA_OBJECT_SENSITIVEDATAORIGIN
      | TPMA_OBJECT_USERWITHAUTH | TPMA_OBJECT_NODA | TPMA_OBJECT_RESTRICTED | TPMA_OBJECT_DECRYPT);

  bool ok = Tss2_TctiLdr_Initialize(tcti_config, &tpm->tcti) == TSS2_RC_SUCCESS
      && Esys_Initialize(&tpm->esys, tpm->tcti, NULL) == TSS2_RC_SUCCESS && ek_load(tpm)
      && primary_load(
                tpm->esys, SAMPLE_TPM_TSS_SRK_HANDLE, ESYS_TR_RH_OWNER, &srk_template, &tpm->srk);
  if (ok)
  {
    primary_release(tpm->esys, &tpm->ek);
  }
  bool hmac_key_exists = false;
  ok = ok && persistent_lookup(tpm->esys, hmac_key_handle, &hmac_key_exists);
  if (ok && hmac_key_exists)
  {
    ok = Esys_TR_FromTPMPublic(
             tpm->esys, hmac_key_handle, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &tpm->hmac_key)
        == TSS2_RC_SUCCESS;
  }
  if (!ok)
  {
    sample_tpm_tss_destroy(tpm);
    return NULL;
  }
  return tpm;
}

void sample_tpm_tss_destroy(sample_tpm_tss* tpm)
{
  if (tpm == NULL)
  {
    return;
  }
  if (tpm->esys != NULL)
  {
    primary_release(tpm->esys, &tpm->ek);
    primary_release(tpm->esys, &tpm->srk);
    if (tpm->hmac_key != ESYS_TR_NONE)
    {
      (void)Esys_TR_Close(tpm->esys, &tpm->hmac_key);
    }
    Esys_Finalize(&tpm->esys);
  }
  Tss2_TctiLdr_Finalize(&tpm->tcti);
  free(tpm);
}

void sample_tpm_tss_ek_public(const sample_tpm_tss* tpm, const uint8_t** data, size_t* len)
{
  *data = tpm->ek.pub;
  *len = tpm->ek.pub_len;
}

void sample_tpm_tss_srk_public(const sample_tpm_tss* tpm, const uint8_t** data, size_t* len)
{
  *data = tpm->srk.pub;
  *len = tpm->srk.pub_len;
}

/** @brief The DPS key's TPM2B structures, unmarshalled. */
typedef struct tss_dps_key
{
  TPM2B_ID_OBJECT credential_blob;
  TPM2B_ENCRYPTED_SECRET secret;
  TPM2B_PRIVATE duplicate;
  TPM2B_ENCRYPTED_SECRET in_sym_seed;
  TPM2B_PUBLIC object_public;
} tss_dps_key;

/** @brief Unmarshals @p part with @p unmarshal; true when it took every byte. */
#define TSS_UNMARSHAL_EXACT(unmarshal, part, out, offset)                                        \
  ((offset) = 0,                                                                                 \
   unmarshal(az_span_ptr(part), (size_t)az_span_size(part), &(offset), (out)) == TSS2_RC_SUCCESS \
       && (offset) == (size_t)az_span_size(part))

/** @brief Splits a decoded `authenticationKey` into the TPM command inputs. */
static bool dps_key_unmarshal(const uint8_t* key, size_t key_len, tss_dps_key* out)
{
  az_iot_dps_tpm_key_parts parts;
  size_t offset = 0;
  memset(out, 0, sizeof(*out)); /* Tss2_MU_*_Unmarshal refuses a non-empty destination. */
  return az_iot_dps_tpm_parse_key(key, key_len, &parts) == AZ_IOT_OK
      && TSS_UNMARSHAL_EXACT(
             Tss2_MU_TPM2B_ID_OBJECT_Unmarshal,
             parts.credential_blob,
             &out->credential_blob,
             offset)
      && TSS_UNMARSHAL_EXACT(
             Tss2_MU_TPM2B_ENCRYPTED_SECRET_Unmarshal, parts.secret, &out->secret, offset)
      && TSS_UNMARSHAL_EXACT(
             Tss2_MU_TPM2B_PRIVATE_Unmarshal, parts.duplicate, &out->duplicate, offset)
      && TSS_UNMARSHAL_EXACT(
             Tss2_MU_TPM2B_ENCRYPTED_SECRET_Unmarshal, parts.in_sym_seed, &out->in_sym_seed, offset)
      && TSS_UNMARSHAL_EXACT(
             Tss2_MU_TPM2B_PUBLIC_Unmarshal, parts.object_public, &out->object_public, offset);
}

/**
 * @brief TPM2_ActivateCredential(SRK, EK) in a PolicySecret(TPM_RH_ENDORSEMENT)
 * session: the key that wraps the duplicate.
 */
static bool activate_credential(sample_tpm_tss* tpm, const tss_dps_key* key, TPM2B_DIGEST** out)
{
  TPMT_SYM_DEF no_symmetric;
  memset(&no_symmetric, 0, sizeof(no_symmetric));
  no_symmetric.algorithm = TPM2_ALG_NULL;
  ESYS_TR session = ESYS_TR_NONE;
  if (Esys_StartAuthSession(
          tpm->esys,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          ESYS_TR_NONE,
          NULL,
          TPM2_SE_POLICY,
          &no_symmetric,
          TPM2_ALG_SHA256,
          &session)
      != TSS2_RC_SUCCESS)
  {
    return false;
  }
  TPM2B_NONCE empty_nonce;
  TPM2B_DIGEST empty_cp_hash;
  memset(&empty_nonce, 0, sizeof(empty_nonce));
  memset(&empty_cp_hash, 0, sizeof(empty_cp_hash));
  TPM2B_TIMEOUT* timeout = NULL;
  TPMT_TK_AUTH* ticket = NULL;
  bool ok = ek_load(tpm)
      && Esys_PolicySecret(
             tpm->esys,
             ESYS_TR_RH_ENDORSEMENT,
             session,
             ESYS_TR_PASSWORD,
             ESYS_TR_NONE,
             ESYS_TR_NONE,
             &empty_nonce,
             &empty_cp_hash,
             &empty_nonce,
             0,
             &timeout,
             &ticket)
          == TSS2_RC_SUCCESS
      && Esys_ActivateCredential(
             tpm->esys,
             tpm->srk.handle,
             tpm->ek.handle,
             ESYS_TR_PASSWORD,
             session,
             ESYS_TR_NONE,
             &key->credential_blob,
             &key->secret,
             out)
          == TSS2_RC_SUCCESS;
  Esys_Free(timeout);
  Esys_Free(ticket);
  (void)Esys_FlushContext(tpm->esys, session);
  primary_release(tpm->esys, &tpm->ek);
  return ok;
}

/** @brief TPM2_EvictControl: makes @p loaded persistent, replacing the key held before. */
static bool persist_hmac_key(sample_tpm_tss* tpm, ESYS_TR loaded)
{
  if (tpm->hmac_key != ESYS_TR_NONE)
  {
    ESYS_TR none = ESYS_TR_NONE;
    if (Esys_EvictControl(
            tpm->esys,
            ESYS_TR_RH_OWNER,
            tpm->hmac_key,
            ESYS_TR_PASSWORD,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            tpm->hmac_key_handle,
            &none)
        != TSS2_RC_SUCCESS)
    {
      return false;
    }
    tpm->hmac_key = ESYS_TR_NONE;
  }
  return Esys_EvictControl(
             tpm->esys,
             ESYS_TR_RH_OWNER,
             loaded,
             ESYS_TR_PASSWORD,
             ESYS_TR_NONE,
             ESYS_TR_NONE,
             tpm->hmac_key_handle,
             &tpm->hmac_key)
      == TSS2_RC_SUCCESS;
}

/** @brief TPM2_HMAC (SHA-256) of @p data with the loaded or persistent @p key. */
static bool key_hmac_sha256(
    ESYS_CONTEXT* esys,
    ESYS_TR key,
    const uint8_t* data,
    size_t len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  TPM2B_MAX_BUFFER buffer;
  if (len > sizeof(buffer.buffer))
  {
    return false;
  }
  buffer.size = (UINT16)len;
  memcpy(buffer.buffer, data, len);
  TPM2B_DIGEST* mac = NULL;
  bool ok
      = Esys_HMAC(
            esys, key, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE, &buffer, TPM2_ALG_SHA256, &mac)
          == TSS2_RC_SUCCESS
      && mac->size == AZ_IOT_SHA256_SIZE;
  if (ok)
  {
    memcpy(out, mac->buffer, AZ_IOT_SHA256_SIZE);
  }
  Esys_Free(mac);
  return ok;
}

static az_iot_result tss_import_key(void* ctx, const uint8_t* key, size_t key_len)
{
  sample_tpm_tss* tpm = (sample_tpm_tss*)ctx;
  tss_dps_key* parts = (tss_dps_key*)calloc(1, sizeof(*parts));
  if (parts == NULL)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  if (tpm == NULL || !dps_key_unmarshal(key, key_len, parts))
  {
    free(parts);
    return AZ_IOT_ERR_PROTOCOL;
  }

  /* The credential is the inner wrapping key of the duplicate: AES-128-CFB. */
  TPM2B_DIGEST* credential = NULL;
  TPM2B_PRIVATE* imported = NULL;
  ESYS_TR loaded = ESYS_TR_NONE;
  TPM2B_DATA inner_key;
  memset(&inner_key, 0, sizeof(inner_key));
  TPMT_SYM_DEF_OBJECT inner_symmetric;
  memset(&inner_symmetric, 0, sizeof(inner_symmetric));
  inner_symmetric.algorithm = TPM2_ALG_AES;
  inner_symmetric.keyBits.aes = 128;
  inner_symmetric.mode.aes = TPM2_ALG_CFB;

  bool ok = activate_credential(tpm, parts, &credential)
      && credential->size <= sizeof(inner_key.buffer);
  if (ok)
  {
    inner_key.size = credential->size;
    memcpy(inner_key.buffer, credential->buffer, credential->size);
  }
  ok = ok
      && Esys_Import(
             tpm->esys,
             tpm->srk.handle,
             ESYS_TR_PASSWORD,
             ESYS_TR_NONE,
             ESYS_TR_NONE,
             &inner_key,
             &parts->object_public,
             &parts->duplicate,
             &parts->in_sym_seed,
             &inner_symmetric,
             &imported)
          == TSS2_RC_SUCCESS
      && Esys_Load(
             tpm->esys,
             tpm->srk.handle,
             ESYS_TR_PASSWORD,
             ESYS_TR_NONE,
             ESYS_TR_NONE,
             imported,
             &parts->object_public,
             &loaded)
          == TSS2_RC_SUCCESS;
  az_iot_result result = ok ? AZ_IOT_OK : AZ_IOT_ERR_AUTH;
  /* A key that cannot sign HMAC-SHA256 (e.g. another scheme) must not replace the current one. */
  static const uint8_t probe_data[1] = { 0 };
  uint8_t probe_mac[AZ_IOT_SHA256_SIZE];
  if (ok && !key_hmac_sha256(tpm->esys, loaded, probe_data, sizeof(probe_data), probe_mac))
  {
    result = AZ_IOT_ERR_PROTOCOL;
  }
  else if (ok && !persist_hmac_key(tpm, loaded))
  {
    result = AZ_IOT_ERR_INTERNAL;
  }
  if (loaded != ESYS_TR_NONE)
  {
    (void)Esys_FlushContext(tpm->esys, loaded);
  }
  if (credential != NULL)
  {
    memset(credential, 0, sizeof(*credential));
  }
  memset(&inner_key, 0, sizeof(inner_key));
  Esys_Free(credential);
  Esys_Free(imported);
  free(parts);
  return result;
}

bool sample_tpm_tss_hmac(
    sample_tpm_tss* tpm,
    const uint8_t* data,
    size_t len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  return tpm != NULL && tpm->hmac_key != ESYS_TR_NONE
      && key_hmac_sha256(tpm->esys, tpm->hmac_key, data, len, out);
}

static az_iot_result tss_hmac_op(
    void* ctx,
    const uint8_t* data,
    size_t data_len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  return sample_tpm_tss_hmac((sample_tpm_tss*)ctx, data, data_len, out) ? AZ_IOT_OK
                                                                        : AZ_IOT_ERR_INTERNAL;
}

az_iot_dps_tpm_ops sample_tpm_tss_ops(sample_tpm_tss* tpm)
{
  az_iot_dps_tpm_ops ops;
  ops.import_key = tss_import_key;
  ops.hmac_sha256 = tss_hmac_op;
  ops.ctx = tpm;
  return ops;
}
