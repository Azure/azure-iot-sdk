// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Azure software updates core state machine.
 *
 * Single-threaded, callback-driven. The `updateMetadata` payload the channel
 * delivers is parsed into current_request/current_manifest; the
 * do_work pump then advances the workflow one phase per invocation:
 *
 *   Idle -> ManifestReceived -> VerifyingManifest -> (accept/reject)
 *        -> DownloadStarted -> DownloadComplete -> BackupStarted -> BackupComplete
 *        -> InstallStarted -> InstallComplete -> ApplyStarted
 *        -> (next step | Idle) ; any failure -> RestoreStarted -> Idle(Failed)
 *
 * Platform operations (download/install/apply/backup/restore/is_installed) and
 * crypto primitives (RS256 verify + SHA-256) are reached only through the hook
 * vtables; the core links no platform/OS/crypto code.
 *
 * NOTE (verification, see design doc section 6): full JWS/SJWK chain parsing and
 * root-key `kid` resolution land with the crypto adapters (Phase 2). This core
 * already gates Install on a verification step and on per-file SHA-256 hash
 * checks via the crypto hooks; the manifest-signature primitive call is wired
 * through verify_manifest() so the chain parsing can be completed there without
 * touching the state machine.
 */
#include <stdint.h>
#include <string.h>

#include <azure/core/az_base64.h>
#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_su.h"

#include "internal/su_internal.h"
#include "internal/json_string.h"
#include "internal/mono_time.h"
#include "internal/retry_policy.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

/* The public header reserves opaque storage for the SDK-built channel so the
 * client struct stays caller-allocated with no hidden allocation. If the
 * channel ever outgrows that reservation this fails the build rather than
 * silently corrupting the struct. */
/* Defined below; used from the workflow state machine above it. */
static void set_su_state(az_iot_su_client* client, az_iot_su_state next);
static void arm_pending_fetch_deadline(az_iot_su_client* client, uint32_t timeout_ms);
static bool fetch_in_flight_overdue(const az_iot_su_client* client);
static bool expire_fetch_in_flight(az_iot_su_client* client);
static void raise_refused(az_iot_su_client* client, az_iot_result reason);

/* fetch_in_flight occupies padding after pending_fetch, so adding it left the
 * caller-allocated client's size and offsets unchanged. Fails the build if that
 * padding is ever gone. */
typedef struct
{
  char c;
  uint64_t v;
} su_u64_alignment_probe;
typedef char az_iot_su_fetch_in_flight_uses_padding
    [((offsetof(az_iot_su_client, _internal.pending_fetch) + 1u)
          % offsetof(su_u64_alignment_probe, v)
      != 0u)
         ? 1
         : -1];

typedef char az_iot_su_channel_storage_is_large_enough
    [(sizeof(((az_iot_su_client*)0)->_internal.channel_storage) >= sizeof(az_iot_su_channel_dps))
         ? 1
         : -1];

/* ------------------------------------------------------------------------- */
/* device-properties cache                                                   */
/* ------------------------------------------------------------------------- */
AZ_NODISCARD size_t
az_iot_su_device_properties_buffer_size(const az_iot_su_device_properties* device_properties)
{
  az_iot_su_device_properties_snapshot snapshot;
  return az_iot_su__prepare_device_properties(device_properties, &snapshot) == AZ_IOT_OK
      ? snapshot.strings_size
      : 0;
}

static az_iot_result prepare_device_properties_cache(
    const az_iot_su_client* client,
    const az_iot_su_device_properties* src,
    az_iot_su_device_properties_snapshot* snapshot)
{
  az_iot_result r = az_iot_su__prepare_device_properties(src, snapshot);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  if (SU_I(client).device_properties_buffer == NULL
      || SU_I(client).device_properties_buffer_size < snapshot->strings_size)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  return AZ_IOT_OK;
}

static void commit_device_properties_cache(
    az_iot_su_client* client,
    const az_iot_su_device_properties_snapshot* snapshot)
{
  az_iot_su__commit_device_properties(
      snapshot,
      &SU_I(client).device_properties,
      SU_I(client).custom_properties,
      (char*)SU_I(client).device_properties_buffer);
}

/* ------------------------------------------------------------------------- */
/* result-code accumulation                                                  */
/* ------------------------------------------------------------------------- */

static void result_init_steps(az_iot_su_client* client, int32_t step_count)
{
  az_iot_su_client_install_result* r = &SU_I(client).install_result;
  memset(r, 0, sizeof(*r));
  memset(SU_I(client).step_results, 0, sizeof(SU_I(client).step_results));
  if (step_count < 0)
  {
    step_count = 0;
  }
  if (step_count > _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS)
  {
    step_count = _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS;
  }
  SU_I(client).step_results_count = step_count;
  for (int32_t i = 0; i < step_count; ++i)
  {
    SU_I(client).step_results[i].outcome = AZ_IOT_SU_OUTCOME_SKIPPED;
    SU_I(client).step_results[i].failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE;
  }
}

static void result_step_success(az_iot_su_client* client, uint32_t step)
{
  if ((int32_t)step < SU_I(client).step_results_count)
  {
    SU_I(client).step_results[step].outcome = AZ_IOT_SU_OUTCOME_SUCCEEDED;
    SU_I(client).step_results[step].failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE;
    SU_I(client).step_results[step].result_code = AZ_IOT_SU_AGENT_RESULT_CODE_SUCCESS;
    SU_I(client).step_results[step].extended_result_code = 0;
  }
}

/* Record a phase failure for the given step + overall result. The overall
 * result mirrors the FIRST failing step (root cause); later calls don't
 * overwrite an already-recorded overall failure. */
static void result_step_failure(
    az_iot_su_client* client,
    uint32_t step,
    uint32_t facility,
    int32_t sub_code)
{
  az_iot_su_client_install_result* r = &SU_I(client).install_result;
  int32_t extended = AZ_IOT_SU_EXTENDED_RESULT(facility, (uint32_t)sub_code);
  int32_t code = (int32_t)(700 - facility); /* a non-success software updates code (< 700) */

  if ((int32_t)step < SU_I(client).step_results_count)
  {
    SU_I(client).step_results[step].outcome = AZ_IOT_SU_OUTCOME_FAILED;
    SU_I(client).step_results[step].failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_AGENT_CORE;
    SU_I(client).step_results[step].result_code = code;
    SU_I(client).step_results[step].extended_result_code = extended;
  }
  if (r->result_code == 0 || r->result_code == AZ_IOT_SU_AGENT_RESULT_CODE_SUCCESS)
  {
    r->result_code = code;
    r->extended_result_code = extended;
  }
}

static void result_step_canceled(az_iot_su_client* client)
{
  int32_t step_count = SU_I(client).step_results_count;
  uint32_t step = SU_I(client).current_step;
  if ((int32_t)step >= step_count)
  {
    return;
  }

  az_iot_su_step_result* result = &SU_I(client).step_results[step];
  if (result->outcome == AZ_IOT_SU_OUTCOME_SKIPPED)
  {
    result->outcome = AZ_IOT_SU_OUTCOME_CANCELED;
    result->failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE;
    result->result_code = AZ_IOT_SU_RESULT_FAILURE;
    result->extended_result_code = 0;
  }
}

static void result_overall_success(az_iot_su_client* client)
{
  az_iot_su_client_install_result* r = &SU_I(client).install_result;
  r->result_code = AZ_IOT_SU_AGENT_RESULT_CODE_SUCCESS;
  r->extended_result_code = 0;

  /* Latch the outcome and the applied update id while the manifest is still
   * live: reset_to_idle() clears it before the report is emitted. */
  SU_I(client).pending_outcome = AZ_IOT_SU_OUTCOME_SUCCEEDED;
  SU_I(client).applied_update_id_valid = false;

  az_span parts[3] = { SU_I(client).current_manifest.update_id.provider,
                       SU_I(client).current_manifest.update_id.name,
                       SU_I(client).current_manifest.update_id.version };
  const char* out[3] = { NULL, NULL, NULL };
  size_t used = 0;
  for (int i = 0; i < 3; ++i)
  {
    int32_t n = az_span_size(parts[i]);
    if (n <= 0 || (size_t)n + 1 > sizeof(SU_I(client).applied_update_id_buf) - used)
    {
      return;
    }
    char* dst = SU_I(client).applied_update_id_buf + used;
    memcpy(dst, az_span_ptr(parts[i]), (size_t)n);
    dst[n] = '\0';
    out[i] = dst;
    used += (size_t)n + 1;
  }

  SU_I(client).applied_update_id.provider = out[0];
  SU_I(client).applied_update_id.name = out[1];
  SU_I(client).applied_update_id.version = out[2];
  SU_I(client).applied_update_id_valid = true;
}

/* ------------------------------------------------------------------------- */
/* manifest verification (JWS / SJWK two-level signature chain)              */
/* ------------------------------------------------------------------------- */
/*
 * Stage 1 gates the whole workflow: the manifest's JWS must verify before any
 * field (including the per-file hashes) is trusted. Core performs every parsing
 * and resolution step (compact JWS split, base64url decode, JSON parse, root-key
 * `kid` resolution, revocation, `alg=RS256` enforcement) and calls the crypto
 * hooks only for the two RSA signature checks and the binding SHA-256 (see
 * design doc section 6). The hooks remain pure primitives.
 *
 * Stack note: the chain decodes several base64url segments into local scratch
 * buffers (the largest is the manifest protected header, which embeds the whole
 * SJWK). The frame is a few KiB; acceptable for the Linux/desktop OpenSSL target.
 * Constrained backends should mind the per-task stack budget.
 */

/* Split a compact JWS "header.payload.signature" (exactly two '.'). On success
 * fills the three base64url segments plus `signed_bytes` = "header.payload" (the
 * exact ASCII bytes an RS256 signature covers). Returns false if malformed. */
static bool jws_split(
    az_span jws,
    az_span* header,
    az_span* payload,
    az_span* sig,
    az_span* signed_bytes)
{
  az_span dot = AZ_SPAN_FROM_STR(".");
  int32_t n = az_span_size(jws);

  /* Locate the first '.' and, within the remainder, the second one. */
  int32_t d1 = az_span_find(jws, dot);
  if (d1 <= 0)
  {
    return false;
  }
  int32_t d2_rel = az_span_find(az_span_slice_to_end(jws, d1 + 1), dot);
  if (d2_rel < 0)
  {
    return false;
  }
  int32_t d2 = d1 + 1 + d2_rel;

  /* Need all three parts non-empty, and the signature must not itself contain a
   * further '.' (a 4th segment ⇒ not a valid compact JWS). */
  if (d2 <= d1 + 1 || d2 >= n - 1)
  {
    return false;
  }
  if (az_span_find(az_span_slice_to_end(jws, d2 + 1), dot) >= 0)
  {
    return false;
  }
  *header = az_span_slice(jws, 0, d1);
  *payload = az_span_slice(jws, d1 + 1, d2);
  *sig = az_span_slice(jws, d2 + 1, n);
  *signed_bytes = az_span_slice(jws, 0, d2);
  return true;
}

/* base64url-decode `src` into `buf`; returns the decoded span, or AZ_SPAN_EMPTY
 * on any error (including insufficient capacity). */
static az_span jws_b64url(az_span src, uint8_t* buf, int32_t cap)
{
  int32_t written = 0;
  if (az_span_size(src) <= 0
      || az_result_failed(az_base64_url_decode(az_span_create(buf, cap), src, &written))
      || written <= 0)
  {
    return AZ_SPAN_EMPTY;
  }
  return az_span_create(buf, written);
}

/* Decode a JWK field that may be encoded as either base64url (RFC 7515) or
 * standard base64. The software updates service emits the signing-key modulus (n) as
 * standard base64 (with '+'/'/'), while the surrounding JWS segments are
 * base64url; az_base64_url_decode rejects '+'/'/' outright, so try base64url
 * first and fall back to standard base64. */
static az_span jws_b64_any(az_span src, uint8_t* buf, int32_t cap)
{
  int32_t written = 0;
  if (az_span_size(src) <= 0)
  {
    return AZ_SPAN_EMPTY;
  }
  if (az_result_succeeded(az_base64_url_decode(az_span_create(buf, cap), src, &written))
      && written > 0)
  {
    return az_span_create(buf, written);
  }
  if (az_result_succeeded(az_base64_decode(az_span_create(buf, cap), src, &written)) && written > 0)
  {
    return az_span_create(buf, written);
  }
  return AZ_SPAN_EMPTY;
}

/* Read a top-level string property `name` from JSON object `obj`, copying its
 * (unescaped) value into `out_buf`. Returns the value span, or AZ_SPAN_EMPTY if
 * the property is absent or not a string. */
static az_span jws_json_str(az_span obj, az_span name, char* out_buf, int32_t cap)
{
  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, obj, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return AZ_SPAN_EMPTY;
  }
  while (az_result_succeeded(az_json_reader_next_token(&jr))
         && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
  {
    if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      continue;
    }
    bool match = az_json_token_is_text_equal(&jr.token, name);
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return AZ_SPAN_EMPTY;
    }
    if (match)
    {
      if (jr.token.kind != AZ_JSON_TOKEN_STRING)
      {
        return AZ_SPAN_EMPTY;
      }
      int32_t len = 0;
      if (az_result_failed(az_json_token_get_string(&jr.token, out_buf, cap, &len)))
      {
        return AZ_SPAN_EMPTY;
      }
      return az_span_create((uint8_t*)out_buf, len);
    }
    /* Skip a non-matching object/array value so we stay at object scope. */
    if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      if (az_result_failed(az_json_reader_skip_children(&jr)))
      {
        return AZ_SPAN_EMPTY;
      }
    }
  }
  return AZ_SPAN_EMPTY;
}

static const az_span k_alg_rs256 = AZ_SPAN_LITERAL_FROM_STR("RS256");

/* CRC-32 (defined with the persistence helpers below); forward-declared here so
 * the manifest-fingerprint de-duplication path can use it. */
static uint32_t su_crc32(const uint8_t* data, size_t len);

/* CRC-32 fingerprint of a manifest span (empty span hashes to 0). */
static uint32_t manifest_fingerprint(az_span manifest)
{
  int32_t n = az_span_size(manifest);
  if (n <= 0)
  {
    return 0;
  }
  return su_crc32(az_span_ptr(manifest), (size_t)n);
}

/* Resolve a JWS `kid` against a root-key store. Returns the matching key, or
 * NULL if unknown or disabled (revoked). */
static const az_iot_su_root_key* resolve_root_key(
    const az_iot_su_root_key* root_keys,
    size_t root_key_count,
    az_span kid)
{
  for (size_t i = 0; i < root_key_count; ++i)
  {
    const az_iot_su_root_key* rk = &root_keys[i];
    if (rk->kid == NULL)
    {
      continue;
    }
    az_span rk_kid = az_span_create_from_str((char*)(uintptr_t)rk->kid);
    if (az_span_is_content_equal(kid, rk_kid))
    {
      return rk->disabled ? NULL : rk;
    }
  }
  return NULL;
}

/* Log the reason a manifest fails verification, then bail. Verification has
 * many independent failure paths; naming each one makes a Failed deployment
 * diagnosable from the application's log instead of a single opaque "Failed". */
#define SU_VERIFY_FAIL(why)                                           \
  do                                                                  \
  {                                                                   \
    AZ_IOT_LOG_ERRORF("su: manifest verification failed: %s", (why)); \
    return AZ_IOT_SU_RESULT_FAILURE;                                  \
  } while (0)

static int32_t verify_manifest_core(
    const az_iot_su_crypto_hooks* crypto,
    const az_iot_su_root_key* root_keys,
    size_t root_key_count,
    az_span manifest,
    az_span jws)
{
  if (crypto == NULL || crypto->verify_rs256_fn == NULL || crypto->sha256_fn == NULL)
  {
    SU_VERIFY_FAIL("crypto hooks not configured (verify_rs256_fn/sha256_fn)");
  }

  /* `manifest` is the UNESCAPED manifest body. parse_manifest() unescapes the
   * service-supplied manifest IN PLACE (the unescaped form is never longer)
   * and records that span; the service signs the hash of the unescaped body,
   * so step 6 must hash exactly these bytes (not the original escaped span,
   * whose LENGTH still covers trailing leftover bytes). `jws` is the compact
   * update-manifest signature. */
  if (az_span_size(jws) <= 0 || az_span_size(manifest) <= 0)
  {
    SU_VERIFY_FAIL("empty updateManifest or updateManifestSignature");
  }

  /* 1. Split the manifest JWS and decode + parse its protected header. */
  az_span m_hdr_b64, m_pl_b64, m_sig_b64, m_signed;
  if (!jws_split(jws, &m_hdr_b64, &m_pl_b64, &m_sig_b64, &m_signed))
  {
    SU_VERIFY_FAIL("step 1: manifest JWS is not a valid 3-part token");
  }

  uint8_t sjwk_buf[2048]; /* outlives steps 2-6: SJWK segments point into it */
  az_span sjwk;
  {
    uint8_t hdr_buf[2048];
    az_span hdr = jws_b64url(m_hdr_b64, hdr_buf, (int32_t)sizeof(hdr_buf));
    if (az_span_size(hdr) <= 0)
    {
      SU_VERIFY_FAIL("step 1: manifest JWS header is not valid base64url");
    }

    char alg_buf[16];
    az_span alg = jws_json_str(hdr, AZ_SPAN_FROM_STR("alg"), alg_buf, (int32_t)sizeof(alg_buf));
    if (!az_span_is_content_equal(alg, k_alg_rs256))
    {
      SU_VERIFY_FAIL("step 1: manifest JWS alg is not RS256");
    }

    sjwk = jws_json_str(hdr, AZ_SPAN_FROM_STR("sjwk"), (char*)sjwk_buf, (int32_t)sizeof(sjwk_buf));
    if (az_span_size(sjwk) <= 0)
    {
      SU_VERIFY_FAIL("step 1: manifest JWS header has no sjwk");
    }
  }

  /* 2. Split the SJWK and read its header (alg + kid), then resolve the kid. */
  az_span s_hdr_b64, s_pl_b64, s_sig_b64, s_signed;
  if (!jws_split(sjwk, &s_hdr_b64, &s_pl_b64, &s_sig_b64, &s_signed))
  {
    SU_VERIFY_FAIL("step 2: sjwk is not a valid 3-part token");
  }

  const az_iot_su_root_key* root = NULL;
  {
    uint8_t shdr_buf[512];
    az_span shdr = jws_b64url(s_hdr_b64, shdr_buf, (int32_t)sizeof(shdr_buf));
    if (az_span_size(shdr) <= 0)
    {
      SU_VERIFY_FAIL("step 2: sjwk header is not valid base64url");
    }

    char salg_buf[16];
    az_span salg = jws_json_str(shdr, AZ_SPAN_FROM_STR("alg"), salg_buf, (int32_t)sizeof(salg_buf));
    if (!az_span_is_content_equal(salg, k_alg_rs256))
    {
      SU_VERIFY_FAIL("step 2: sjwk alg is not RS256");
    }

    char kid_buf[128];
    az_span kid = jws_json_str(shdr, AZ_SPAN_FROM_STR("kid"), kid_buf, (int32_t)sizeof(kid_buf));
    if (az_span_size(kid) <= 0)
    {
      SU_VERIFY_FAIL("step 2: sjwk header has no kid");
    }

    root = resolve_root_key(root_keys, root_key_count, kid);
    if (root == NULL)
    {
      SU_VERIFY_FAIL("step 2: sjwk kid does not match any known (enabled) root key");
    }
  }

  /* 3. Verify the SJWK signature with the resolved root key. */
  {
    uint8_t s_sig_buf[1024];
    az_span s_sig = jws_b64url(s_sig_b64, s_sig_buf, (int32_t)sizeof(s_sig_buf));
    if (az_span_size(s_sig) <= 0)
    {
      SU_VERIFY_FAIL("step 3: sjwk signature is not valid base64url");
    }
    if (crypto->verify_rs256_fn(
            root->modulus,
            root->modulus_len,
            root->exponent,
            root->exponent_len,
            az_span_ptr(s_signed),
            (size_t)az_span_size(s_signed),
            az_span_ptr(s_sig),
            (size_t)az_span_size(s_sig),
            crypto->user_ctx)
        != AZ_IOT_SU_RESULT_SUCCESS)
    {
      SU_VERIFY_FAIL("step 3: sjwk signature does not verify against the root key");
    }
  }

  /* 4. Parse the now-trusted SJWK payload as a JWK → signing key (n, e).
   * Buffers are sized for up to 4096-bit RSA keys: the base64 modulus of a
   * 3072-bit key is already 512 chars, and az_json_token_get_string needs the
   * destination strictly larger than the string to fit its NUL terminator. */
  uint8_t n_buf[1024];
  uint8_t e_buf[16];
  az_span n_raw;
  az_span e_raw;
  {
    uint8_t spl_buf[2048];
    az_span spl = jws_b64url(s_pl_b64, spl_buf, (int32_t)sizeof(spl_buf));
    if (az_span_size(spl) <= 0)
    {
      SU_VERIFY_FAIL("step 4: sjwk payload is not valid base64url");
    }

    char n_b64[1024];
    char e_b64[64];
    az_span n_field = jws_json_str(spl, AZ_SPAN_FROM_STR("n"), n_b64, (int32_t)sizeof(n_b64));
    az_span e_field = jws_json_str(spl, AZ_SPAN_FROM_STR("e"), e_b64, (int32_t)sizeof(e_b64));
    if (az_span_size(n_field) <= 0 || az_span_size(e_field) <= 0)
    {
      SU_VERIFY_FAIL("step 4: signing JWK is missing modulus (n) or exponent (e)");
    }
    n_raw = jws_b64_any(n_field, n_buf, (int32_t)sizeof(n_buf));
    e_raw = jws_b64_any(e_field, e_buf, (int32_t)sizeof(e_buf));
    if (az_span_size(n_raw) <= 0 || az_span_size(e_raw) <= 0)
    {
      SU_VERIFY_FAIL("step 4: signing JWK n/e are not valid base64url");
    }
  }

  /* 5. Verify the manifest JWS signature with the trusted signing key. */
  {
    uint8_t m_sig_buf[1024];
    az_span m_sig = jws_b64url(m_sig_b64, m_sig_buf, (int32_t)sizeof(m_sig_buf));
    if (az_span_size(m_sig) <= 0)
    {
      SU_VERIFY_FAIL("step 5: manifest signature is not valid base64url");
    }
    if (crypto->verify_rs256_fn(
            az_span_ptr(n_raw),
            (size_t)az_span_size(n_raw),
            az_span_ptr(e_raw),
            (size_t)az_span_size(e_raw),
            az_span_ptr(m_signed),
            (size_t)az_span_size(m_signed),
            az_span_ptr(m_sig),
            (size_t)az_span_size(m_sig),
            crypto->user_ctx)
        != AZ_IOT_SU_RESULT_SUCCESS)
    {
      SU_VERIFY_FAIL("step 5: manifest signature does not verify against the signing key");
    }
  }

  /* 6. Bind the signed manifest to THIS deployment: the manifest JWS payload
   *    carries SHA-256(manifest body); recompute and compare. */
  {
    uint8_t pl_buf[256];
    az_span pl = jws_b64url(m_pl_b64, pl_buf, (int32_t)sizeof(pl_buf));
    if (az_span_size(pl) <= 0)
    {
      SU_VERIFY_FAIL("step 6: manifest JWS payload is not valid base64url");
    }

    char hash_b64[128];
    az_span hash_field
        = jws_json_str(pl, AZ_SPAN_FROM_STR("sha256"), hash_b64, (int32_t)sizeof(hash_b64));
    if (az_span_size(hash_field) <= 0)
    {
      SU_VERIFY_FAIL("step 6: manifest JWS payload has no sha256");
    }

    uint8_t expected[32];
    int32_t exp_written = 0;
    if (az_result_failed(az_base64_decode(
            az_span_create(expected, (int32_t)sizeof(expected)), hash_field, &exp_written))
        || exp_written != 32)
    {
      SU_VERIFY_FAIL("step 6: manifest sha256 field is not a 32-byte base64 hash");
    }

    uint8_t actual[32];
    if (crypto->sha256_fn(
            az_span_ptr(manifest), (size_t)az_span_size(manifest), actual, crypto->user_ctx)
        != AZ_IOT_SU_RESULT_SUCCESS)
    {
      SU_VERIFY_FAIL("step 6: sha256_fn hook failed over the manifest body");
    }
    if (!az_span_is_content_equal(AZ_SPAN_FROM_BUFFER(expected), AZ_SPAN_FROM_BUFFER(actual)))
    {
      SU_VERIFY_FAIL("step 6: computed manifest SHA-256 does not match the signed hash");
    }
  }

  return AZ_IOT_SU_RESULT_SUCCESS;
}

/* Thin wrapper: verify the current deployment's manifest using the client's
 * crypto hooks + root-key store. */
static int32_t verify_manifest(az_iot_su_client* client)
{
  return verify_manifest_core(
      &SU_I(client).crypto,
      SU_I(client).root_keys,
      SU_I(client).root_key_count,
      SU_I(client).manifest_text,
      SU_I(client).current_request.update_manifest_signature);
}

/* Verify a downloaded file's SHA-256 against the signed manifest by streaming
 * the file back through a generic read-chunk callback and the incremental crypto
 * hooks. Returns SUCCESS when the hash matches; FAILURE on any mismatch or
 * hook/read error. Client-independent so the public API and the managed state
 * machine share one implementation. */
static int32_t verify_file_hash_core(
    const az_iot_su_crypto_hooks* crypto,
    const az_iot_su_client_update_manifest_file* file,
    az_iot_su_read_chunk_callback read_chunk,
    void* read_ctx)
{
  if (crypto == NULL || file == NULL || read_chunk == NULL || crypto->sha256_init_fn == NULL
      || crypto->sha256_update_fn == NULL || crypto->sha256_final_fn == NULL)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  /* Locate the SHA-256 hash entry for this file. */
  az_span hash_b64 = AZ_SPAN_EMPTY;
  for (uint32_t i = 0; i < file->hashes_count; ++i)
  {
    if (az_span_is_content_equal(file->hashes[i].hash_type, AZ_SPAN_FROM_STR("sha256")))
    {
      hash_b64 = file->hashes[i].hash_value;
      break;
    }
  }
  if (az_span_size(hash_b64) <= 0)
  {
    return AZ_IOT_SU_RESULT_FAILURE; /* manifest must carry a sha256 hash */
  }

  /* Decode the expected hash (standard base64) into 32 bytes. */
  uint8_t expected[32];
  int32_t exp_written = 0;
  if (az_result_failed(az_base64_decode(
          az_span_create(expected, (int32_t)sizeof(expected)), hash_b64, &exp_written))
      || exp_written != (int32_t)sizeof(expected))
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  /* Stream the file through the incremental SHA-256 hooks. */
  void* ctx = NULL;
  if (crypto->sha256_init_fn(&ctx, crypto->user_ctx) != AZ_IOT_SU_RESULT_SUCCESS)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  uint8_t chunk[256];
  size_t offset = 0;
  for (;;)
  {
    size_t read = 0;
    if (read_chunk(offset, chunk, sizeof(chunk), &read, read_ctx) != AZ_IOT_SU_RESULT_SUCCESS)
    {
      uint8_t scratch[32];
      (void)crypto->sha256_final_fn(ctx, scratch, crypto->user_ctx); /* free ctx */
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    if (read == 0)
    {
      break; /* end of file */
    }
    if (crypto->sha256_update_fn(ctx, chunk, read, crypto->user_ctx) != AZ_IOT_SU_RESULT_SUCCESS)
    {
      uint8_t scratch[32];
      (void)crypto->sha256_final_fn(ctx, scratch, crypto->user_ctx); /* free ctx */
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    offset += read;
  }

  uint8_t actual[32];
  if (crypto->sha256_final_fn(ctx, actual, crypto->user_ctx) != AZ_IOT_SU_RESULT_SUCCESS)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  if (!az_span_is_content_equal(AZ_SPAN_FROM_BUFFER(expected), AZ_SPAN_FROM_BUFFER(actual)))
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/* Adapter: bridge the managed client's read_file_fn (which is keyed by file +
 * file_index) to the generic read-chunk callback verify_file_hash_core expects. */
struct su_read_file_ctx
{
  az_iot_su_platform_hooks* hooks;
  const az_iot_su_client_update_manifest_file* file;
  uint32_t file_index;
};

static int32_t su_read_file_adapter(
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* read_ctx)
{
  struct su_read_file_ctx* a = (struct su_read_file_ctx*)read_ctx;
  return a->hooks->read_file_fn(
      a->file, a->file_index, offset, buffer, buffer_size, out_read, a->hooks->user_ctx);
}

/* Thin wrapper: verify a downloaded file using the client's hooks. */
static int32_t verify_file_hash(
    az_iot_su_client* client,
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index)
{
  struct su_read_file_ctx a = { &SU_I(client).hooks, file, file_index };
  return verify_file_hash_core(&SU_I(client).crypto, file, su_read_file_adapter, &a);
}

/** @brief Delay, in milliseconds, before the first retry of a failed persist_state_fn write. */
#define AZ_IOT_SU_PERSIST_RETRY_BASE_MS 1000u
/** @brief Upper bound, in milliseconds, of the doubling retry delay. */
#define AZ_IOT_SU_PERSIST_RETRY_MAX_MS 60000u

/** @brief Schedule for retrying a failed persist_state_fn write: doubling, no jitter. */
static const az_iot_retry_policy k_persist_retry_policy
    = { .initial_delay_ms = AZ_IOT_SU_PERSIST_RETRY_BASE_MS,
        .max_delay_ms = AZ_IOT_SU_PERSIST_RETRY_MAX_MS,
        .max_attempts = 0,
        .jitter_pct = 0 };

static az_iot_result su_persist(az_iot_su_client* client, bool terminal);
static void dispatch_event(az_iot_su_client* client, const az_iot_su_event* event);

/** @brief Whether persist_max_attempts consecutive writes have failed. */
static bool persist_gave_up(const az_iot_su_client* client)
{
  return az_iot_retry_state__attempts(&SU_I(client).persist_retry)
      >= SU_I(client).persist_max_attempts;
}

/** @brief Whether a failed write may be retried now. */
static bool persist_retry_due(const az_iot_su_client* client)
{
  return !persist_gave_up(client) && !az_iot_retry_state__pending(&SU_I(client).persist_retry);
}

/** @brief Raise AZ_IOT_SU_EVENT_PERSIST_FAILED or AZ_IOT_SU_EVENT_PERSIST_RECOVERED. */
static void raise_persist(az_iot_su_client* client, bool failed, uint32_t attempts)
{
  az_iot_su_event event = {
    .kind = failed ? AZ_IOT_SU_EVENT_PERSIST_FAILED : AZ_IOT_SU_EVENT_PERSIST_RECOVERED,
    .state = SU_I(client).state,
    .previous_state = SU_I(client).state,
    .operation = AZ_IOT_SU_OP_REPORT_STATUS,
    .reason = failed ? AZ_IOT_ERR_INTERNAL : AZ_IOT_OK,
    .service_error = { .code = 0, .message = "", .tracking_id = "", .retry_after_ms = 0 },
    .persist_attempts = attempts,
    .persist_retrying = failed && attempts < SU_I(client).persist_max_attempts,
  };
  dispatch_event(client, &event);
}

/**
 * @brief Call persist_state_fn and account for the result.
 *
 * A failure arms the next retry (doubling from AZ_IOT_SU_PERSIST_RETRY_BASE_MS,
 * capped at AZ_IOT_SU_PERSIST_RETRY_MAX_MS) and raises PERSIST_FAILED on the
 * first failure and on reaching AZ_IOT_SU_PERSIST_MAX_ATTEMPTS. A success
 * after failures raises PERSIST_RECOVERED and resets the count.
 *
 * @return true when the hook succeeded.
 */
static bool persist_write(az_iot_su_client* client, const uint8_t* blob, size_t len)
{
  az_iot_su_platform_hooks* h = &SU_I(client).hooks;
  int32_t rc = h->persist_state_fn(blob, len, h->user_ctx);
  if (rc == 0)
  {
    uint32_t failed = az_iot_retry_state__attempts(&SU_I(client).persist_retry);
    az_iot_retry_state__reset(&SU_I(client).persist_retry);
    if (failed > 0)
    {
      raise_persist(client, false, failed);
    }
    return true;
  }

  SU_I(client).persist_last_error = rc;
  (void)az_iot_retry_state__schedule(
      &SU_I(client).persist_retry, &k_persist_retry_policy, &SU_I(client).retry_rng);
  uint32_t n = az_iot_retry_state__attempts(&SU_I(client).persist_retry);
  AZ_IOT_LOG_ERRORF("su: persist_state_fn failed (%u consecutive)", (unsigned)n);
  if (n == 1u || n == SU_I(client).persist_max_attempts)
  {
    raise_persist(client, true, n);
  }
  return false;
}

/**
 * @brief Retire the stored checkpoint with a zero-length persist_state_fn write.
 *
 * Both shipped loaders re-read the same record on every boot, so without this
 * a finished workflow is reloaded, re-applied and re-reported. Only issued when
 * a record is believed stored, to spare flash endurance; a failed write keeps
 * it believed stored and is retried from do_work() (see persist_write()), or
 * at the next terminal transition.
 */
static void clear_checkpoint(az_iot_su_client* client)
{
  az_iot_su_platform_hooks* h = &SU_I(client).hooks;
  if (!SU_I(client).checkpoint_stored || h->persist_state_fn == NULL)
  {
    return;
  }
  if (persist_write(client, SU_I(client).persist_scratch, 0))
  {
    SU_I(client).checkpoint_stored = false;
    SU_I(client).checkpoint_terminal = false;
    SU_I(client).checkpoint_superseded = false;
  }
}

/**
 * @brief Retire a stale workflow-position record after the terminal write failed.
 *
 * Not counted as a persist attempt and raises no event: it belongs to the
 * failed terminal write already counted, and must not spend the retry budget
 * or report a recovery while the terminal record is still unwritten.
 */
static void clear_stale_checkpoint(az_iot_su_client* client)
{
  az_iot_su_platform_hooks* h = &SU_I(client).hooks;
  if (SU_I(client).checkpoint_stored
      && h->persist_state_fn(SU_I(client).persist_scratch, 0, h->user_ctx) == 0)
  {
    SU_I(client).checkpoint_stored = false;
    SU_I(client).checkpoint_terminal = false;
    SU_I(client).checkpoint_superseded = false;
  }
}

/**
 * @brief Bring storage in line with a finished (or idle) workflow.
 *
 * An owed terminal report is stored as the terminal record; otherwise the
 * record is retired. When the terminal record cannot be written, a stale
 * workflow-position record is retired instead, so a reboot cannot re-apply a
 * finished workflow. Failures are retried from do_work() while Idle.
 */
static void sync_checkpoint(az_iot_su_client* client)
{
  if (SU_I(client).hooks.persist_state_fn == NULL)
  {
    return;
  }
  if (!SU_I(client).report_owed)
  {
    clear_checkpoint(client);
    return;
  }
  if (SU_I(client).checkpoint_terminal)
  {
    return;
  }
  if (SU_I(client).terminal_write_failed && !persist_retry_due(client))
  {
    return;
  }
  az_iot_result pr = su_persist(client, true);
  SU_I(client).terminal_write_failed = (pr == AZ_IOT_ERR_INTERNAL);
  if (pr == AZ_IOT_OK)
  {
    return;
  }
  if (pr == AZ_IOT_ERR_INTERNAL)
  {
    clear_stale_checkpoint(client);
    return;
  }
  /* Not representable (e.g. no workflow id kept): the report stays in memory only. */
  clear_checkpoint(client);
  SU_I(client).report_owed = false;
}

/** @brief Whether storage differs from what sync_checkpoint() would leave there. */
static bool checkpoint_out_of_sync(const az_iot_su_client* client)
{
  if (SU_I(client).hooks.persist_state_fn == NULL)
  {
    return false;
  }
  return SU_I(client).report_owed ? !SU_I(client).checkpoint_terminal
                                  : SU_I(client).checkpoint_stored;
}

/**
 * @brief Steps to roll back for a workflow held at a reboot boundary.
 *
 * Every step before the current one, plus the current one when its install
 * already ran (INSTALL_COMPLETE / APPLY_STARTED).
 */
static uint32_t held_restore_count(const az_iot_su_client* client)
{
  uint32_t step = SU_I(client).current_step;
  bool installed = SU_I(client).state == AZ_IOT_SU_STATE_INSTALL_COMPLETE
      || SU_I(client).state == AZ_IOT_SU_STATE_APPLY_STARTED;
  return installed ? step + 1u : step;
}

/** @brief Latch a terminal outcome whose report must survive a reboot until accepted. */
static void latch_terminal(az_iot_su_client* client, az_iot_su_outcome outcome)
{
  SU_I(client).pending_outcome = outcome;
  SU_I(client).report_owed = true;
  SU_I(client).checkpoint_terminal = false;
  SU_I(client).terminal_write_failed = false;
}

/* Reset the workflow back to Idle, clearing the parsed request. */
static void reset_to_idle(az_iot_su_client* client)
{
  sync_checkpoint(client);
  set_su_state(client, AZ_IOT_SU_STATE_IDLE);
  SU_I(client).have_request = false;
  SU_I(client).current_step = 0;
  SU_I(client).current_file = 0;
  SU_I(client).cancel_requested = false;
  SU_I(client).checkpoint_pending = false;
  memset(&SU_I(client).current_request, 0, sizeof(SU_I(client).current_request));
  memset(&SU_I(client).current_manifest, 0, sizeof(SU_I(client).current_manifest));
  SU_I(client).request_len = 0;
}

/* Parse the manifest body (an escaped JSON string in the request) into
 * current_manifest. Returns AZ_IOT_OK on success. The unescape happens into the
 * tail of a caller-independent scratch buffer owned by the request span; since
 * the upstream manifest parser only stores spans pointing into the unescaped
 * text, that text MUST remain valid as long as current_manifest is used — so we
 * unescape in place within a client-owned scratch buffer. */
static az_iot_result parse_manifest(az_iot_su_client* client)
{
  az_span manifest = SU_I(client).current_request.update_manifest;
  if (az_span_size(manifest) <= 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Decode in place: the decoded form is never longer than the source. */
  az_span unescaped;
  if (az_iot_json_string_decode(manifest, manifest, &unescaped) != AZ_IOT_OK
      || az_span_size(unescaped) <= 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, unescaped, NULL)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (az_result_failed(az_iot_adu_client_parse_update_manifest(
          &SU_I(client).az, &jr, &SU_I(client).current_manifest)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  SU_I(client).current_request.update_manifest = unescaped;
  SU_I(client).manifest_text = unescaped;
  return AZ_IOT_OK;
}

/**
 * @brief Decode one JSON string span in place.
 *
 * @param s The span; replaced by its decoded, non-empty slice.
 * @return true on success.
 */
static bool decode_in_place(az_span* s)
{
  az_span decoded;
  if (az_iot_json_string_decode(*s, *s, &decoded) != AZ_IOT_OK || az_span_size(decoded) <= 0)
  {
    return false;
  }
  *s = decoded;
  return true;
}

/**
 * @brief Decode the JSON escapes in `workflowId` and each `fileUrls` id and
 * URL, in place.
 *
 * @param req Parsed request whose spans point into writable storage.
 * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG on an undecodable or empty value.
 */
static az_iot_result decode_request_strings(az_iot_su_client_update_request* req)
{
  if (!decode_in_place(&req->workflow.id))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  for (uint32_t i = 0; i < req->file_urls_count; ++i)
  {
    az_iot_su_client_file_url* f = &req->file_urls[i];
    if (!decode_in_place(&f->id) || !decode_in_place(&f->url))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }
  return AZ_IOT_OK;
}

/** @brief True if @p s decodes to a nonempty string; @p scratch receives it. */
static bool decodes_nonempty(az_span s, az_span scratch)
{
  az_span decoded;
  return az_iot_json_string_decode(s, scratch, &decoded) == AZ_IOT_OK && az_span_size(decoded) > 0;
}

/**
 * @brief Check that decode_request_strings() will accept @p req, without
 * changing it. Lets a bad payload be refused before it disturbs the active
 * workflow or its checkpoint.
 *
 * @param client The client; its persist_scratch is used as decode scratch.
 * @param req    Parsed request; each string is no longer than the payload.
 * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG on an undecodable or empty value.
 */
static az_iot_result check_request_strings(
    az_iot_su_client* client,
    const az_iot_su_client_update_request* req)
{
  az_span scratch = AZ_SPAN_FROM_BUFFER(SU_I(client).persist_scratch);
  if (!decodes_nonempty(req->workflow.id, scratch))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  for (uint32_t i = 0; i < req->file_urls_count; ++i)
  {
    if (!decodes_nonempty(req->file_urls[i].id, scratch)
        || !decodes_nonempty(req->file_urls[i].url, scratch))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }
  return AZ_IOT_OK;
}

/**
 * @brief Parse a software updates `updateMetadata` object into @p out_req.
 *
 * Shape: `{ workflowId, updateManifest, updateManifestSignature, fileUrls }`.
 * It carries no action, so it is an apply; unknown properties are skipped.
 *
 * @param doc     The object. MUST outlive @p out_req, whose spans point into it.
 * @param out_req Zeroed, then filled.
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_FOUND when @p doc has no `workflowId`;
 * AZ_IOT_ERR_INVALID_ARG when malformed or without a manifest.
 */
static az_iot_result parse_update_metadata(az_span doc, az_iot_su_client_update_request* out_req)
{
  memset(out_req, 0, sizeof(*out_req));

  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, doc, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  bool closed = false;
  while (az_result_succeeded(az_json_reader_next_token(&jr)))
  {
    if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      closed = true;
      break;
    }
    if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    az_json_token name = jr.token;
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }

    if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("workflowId")))
    {
      if (jr.token.kind != AZ_JSON_TOKEN_STRING)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
      out_req->workflow.id = jr.token.slice;
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("updateManifest")))
    {
      if (jr.token.kind != AZ_JSON_TOKEN_STRING)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
      out_req->update_manifest = jr.token.slice;
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("updateManifestSignature")))
    {
      if (jr.token.kind != AZ_JSON_TOKEN_STRING)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
      out_req->update_manifest_signature = jr.token.slice;
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("fileUrls")))
    {
      if (jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
      while (az_result_succeeded(az_json_reader_next_token(&jr))
             && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
      {
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME
            || out_req->file_urls_count == _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT)
        {
          return AZ_IOT_ERR_INVALID_ARG;
        }
        az_span id = jr.token.slice;
        if (az_result_failed(az_json_reader_next_token(&jr))
            || jr.token.kind != AZ_JSON_TOKEN_STRING)
        {
          return AZ_IOT_ERR_INVALID_ARG;
        }
        out_req->file_urls[out_req->file_urls_count].id = id;
        out_req->file_urls[out_req->file_urls_count].url = jr.token.slice;
        out_req->file_urls_count++;
      }
      if (jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
    }
    else if (az_result_failed(az_json_reader_skip_children(&jr)))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }

  if (!closed || az_json_reader_next_token(&jr) != AZ_ERROR_JSON_READER_DONE)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (az_span_size(out_req->workflow.id) <= 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  if (az_span_size(out_req->update_manifest) <= 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  out_req->workflow.action = AZ_IOT_SU_CLIENT_SERVICE_ACTION_APPLY_DEPLOYMENT;
  return AZ_IOT_OK;
}

/* --- Duplicate detection -------------------------------------------------- */

/* Copy the deployment identity (workflow `id`) out of the request so it
 * survives request_buffer being overwritten by a later payload. `retry` and
 * `manifest_crc` are kept only for the persisted snapshot. New deployments
 * whose id does not fit are refused in process_update_metadata(); one that
 * still does not fit (a record persisted by an earlier build) leaves
 * active_workflow_valid false, so its progress cannot be reported. */
static void set_active_workflow(
    az_iot_su_client* client,
    az_span id,
    az_span retry,
    uint32_t manifest_crc)
{
  int32_t id_len = az_span_size(id);
  if (id_len <= 0 || (size_t)id_len > sizeof(SU_I(client).active_workflow_id))
  {
    AZ_IOT_LOG_ERRORF(
        "su: workflowId of %d bytes does not fit AZ_IOT_SU_WORKFLOW_ID_SIZE (%u); not reported",
        (int)id_len,
        (unsigned)sizeof(SU_I(client).active_workflow_id));
    SU_I(client).active_workflow_valid = false;
    SU_I(client).active_workflow_id_len = 0;
    SU_I(client).active_retry_timestamp_len = 0;
    SU_I(client).active_manifest_crc = 0;
    return;
  }
  memcpy(SU_I(client).active_workflow_id, az_span_ptr(id), (size_t)id_len);
  SU_I(client).active_workflow_id_len = (size_t)id_len;

  int32_t rt_len = az_span_size(retry);
  if (rt_len > 0 && (size_t)rt_len <= sizeof(SU_I(client).active_retry_timestamp))
  {
    memcpy(SU_I(client).active_retry_timestamp, az_span_ptr(retry), (size_t)rt_len);
    SU_I(client).active_retry_timestamp_len = (size_t)rt_len;
  }
  else
  {
    /* Absent (or, defensively, oversized) retryTimestamp is treated as empty. */
    SU_I(client).active_retry_timestamp_len = 0;
  }
  SU_I(client).active_manifest_crc = manifest_crc;
  SU_I(client).active_workflow_valid = true;
}

/* True if `id` matches the active deployment's workflow id. */
static bool same_workflow_id(az_iot_su_client* client, az_span id)
{
  if (!SU_I(client).active_workflow_valid)
  {
    return false;
  }
  return az_span_is_content_equal(
      id,
      az_span_create(
          SU_I(client).active_workflow_id, (int32_t)SU_I(client).active_workflow_id_len));
}

/**
 * @brief Stage and start the deployment in an `updateMetadata` payload.
 *
 * The payload is valid only for this call, and the parsed spans must outlive
 * it, so it is copied into request_buffer and parsed from there.
 *
 * `workflowId` is the sole deployment identity: a payload with the active id
 * is a redelivery and is ignored before request_buffer is touched, so the
 * running workflow is undisturbed. A new id (re)starts the workflow.
 *
 * @param client    The client.
 * @param patch     The payload.
 * @param patch_len Length of @p patch.
 */
static void process_update_metadata(
    az_iot_su_client* client,
    const uint8_t* patch,
    size_t patch_len)
{
  if (client == NULL || patch == NULL || patch_len == 0)
  {
    return;
  }
  if (SU_I(client).detached)
  {
    return;
  }
  if (patch_len > sizeof(SU_I(client).request_buffer))
  {
    AZ_IOT_LOG_ERRORF(
        "su: update payload of %u bytes exceeds AZ_IOT_SU_REQUEST_BUFFER_SIZE (%u); ignored",
        (unsigned)patch_len,
        (unsigned)sizeof(SU_I(client).request_buffer));
    raise_refused(client, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    return;
  }

  AZ_IOT_LOG_DEBUGF(
      "su: update payload received (%u bytes): %.*s",
      (unsigned)patch_len,
      (int)patch_len,
      (const char*)patch);

  /* Probe the transient buffer for the workflow identity. These spans are only
   * valid for the duration of this call, which is enough to decide what to do. */
  az_span transient = az_span_create((uint8_t*)(uintptr_t)patch, (int32_t)patch_len);
  az_iot_su_client_update_request probe;
  az_iot_result pr = parse_update_metadata(transient, &probe);
  if (pr != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF("su: update payload not understood (%d); ignored", (int)pr);
    return;
  }

  /* Compare the decoded id: an escaped spelling of the active id is the same
   * workflow. An id too long for the scratch can be neither kept nor reported,
   * so the deployment is refused rather than run unreported. Not truncated:
   * the service correlates on the exact id. */
  uint8_t id_scratch[AZ_IOT_SU_WORKFLOW_ID_SIZE];
  az_span probe_id;
  az_iot_result dr
      = az_iot_json_string_decode(probe.workflow.id, AZ_SPAN_FROM_BUFFER(id_scratch), &probe_id);
  if (dr == AZ_IOT_ERR_INVALID_ARG)
  {
    AZ_IOT_LOG_ERROR("su: update payload has an undecodable workflowId; ignored");
    return;
  }
  if (dr == AZ_IOT_ERR_NOT_ENOUGH_SPACE)
  {
    AZ_IOT_LOG_ERRORF(
        "su: decoded workflowId exceeds AZ_IOT_SU_WORKFLOW_ID_SIZE (%u; %d bytes encoded); "
        "update refused",
        (unsigned)sizeof(id_scratch),
        (int)az_span_size(probe.workflow.id));
    raise_refused(client, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    return;
  }
  if (dr == AZ_IOT_OK && same_workflow_id(client, probe_id))
  {
    return;
  }

  if (check_request_strings(client, &probe) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("su: update payload has an undecodable string; ignored");
    return;
  }

  /* A workflow held at a reboot boundary has installed a step the device has
   * not rebooted into. It is not replaced until its checkpoint lands or the
   * client gives up and rolls it back; the service offers the new workflow
   * again on a later update check. */
  if (SU_I(client).checkpoint_pending && SU_I(client).state != AZ_IOT_SU_STATE_IDLE
      && SU_I(client).state != AZ_IOT_SU_STATE_FAILED)
  {
    AZ_IOT_LOG_ERROR("su: new workflow ignored while one is held at a reboot boundary");
    return;
  }

  /* A new workflow supersedes whatever the stored checkpoint belongs to,
   * including an unsent terminal report of the previous one. */
  SU_I(client).report_owed = false;
  SU_I(client).terminal_report_in_flight = false;
  clear_checkpoint(client);
  SU_I(client).checkpoint_superseded = SU_I(client).checkpoint_stored;

  /* Stage the patch into client-owned storage and re-parse so current_request
   * / current_manifest reference stable memory. */
  memcpy(SU_I(client).request_buffer, patch, patch_len);
  SU_I(client).request_len = patch_len;
  az_span buf = az_span_create(SU_I(client).request_buffer, (int32_t)patch_len);

  az_iot_su_client_update_request req;
  if (parse_update_metadata(buf, &req) != AZ_IOT_OK || decode_request_strings(&req) != AZ_IOT_OK)
  {
    SU_I(client).request_len = 0;
    return;
  }

  SU_I(client).current_request = req;
  SU_I(client).have_request = true;
  SU_I(client).cancel_requested = false;
  SU_I(client).checkpoint_pending = false;
  SU_I(client).current_step = 0;
  SU_I(client).current_file = 0;
  result_init_steps(client, 0);
  set_su_state(client, AZ_IOT_SU_STATE_MANIFEST_RECEIVED);
  set_active_workflow(
      client,
      req.workflow.id,
      req.workflow.retry_timestamp,
      manifest_fingerprint(req.update_manifest));
}

/* Channel delivery callback: the channel hands us a raw update payload and the
 * engine parses, verifies and drives it. The engine does not know, and must not
 * know, whether that payload was pushed or pulled, nor what carried it. */
static void on_channel_update(
    const uint8_t* update_payload,
    size_t update_payload_len,
    void* engine_ctx)
{
  az_iot_su_client* client = (az_iot_su_client*)engine_ctx;
  if (client == NULL || SU_I(client).detached)
  {
    return;
  }
  if (update_payload == NULL || update_payload_len == 0)
  {
    return;
  }
  /* Past the check's deadline: the caller was promised an answer by then, so a
   * late one is ignored; its verdict abandons the check. */
  if (fetch_in_flight_overdue(client))
  {
    AZ_IOT_LOG_ERROR("su: ignoring an update that arrived after its check's deadline");
    return;
  }
  process_update_metadata(client, update_payload, update_payload_len);
}

/* Deliver an event to every observer.
 *
 * The guard is save/restore, not a plain set/clear: an observer may raise
 * another event indirectly (request_update() is permitted), so dispatches can
 * nest, and clearing on the way out of the inner one would drop the guard
 * while the outer pass was still walking the array.
 *
 * Each slot is re-read and a NULL callback skipped, which is what makes
 * withdrawing from inside a callback safe. */
static void dispatch_event(az_iot_su_client* client, const az_iot_su_event* event)
{
  bool was_dispatching = SU_I(client).dispatching;
  SU_I(client).dispatching = true;
  for (size_t i = 0; i < AZ_IOT_MAX_SU_OBSERVERS; ++i)
  {
    if (SU_I(client).observers[i].cb != NULL)
    {
      SU_I(client).observers[i].cb(event, SU_I(client).observers[i].user_ctx);
    }
  }
  SU_I(client).dispatching = was_dispatching;
}

/* Raise OPERATION_ABANDONED. `service_error` may be NULL when the verdict did
 * not come from a service response; the event then carries an empty diagnosis
 * rather than a NULL pointer, so an observer never has to null-check it. */
/* "The service said nothing." A value rather than an absent pointer, so no
 * caller -- and no application callback -- has to test for NULL. */
static const az_iot_su_service_error k_no_service_error
    = { .code = 0, .message = "", .tracking_id = "", .retry_after_ms = 0 };

static void raise_abandoned(
    az_iot_su_client* client,
    az_iot_su_operation operation,
    az_iot_result reason,
    const az_iot_su_service_error* service_error)
{
  az_iot_su_event event = {
    .kind = AZ_IOT_SU_EVENT_OPERATION_ABANDONED,
    .state = SU_I(client).state,
    .previous_state = SU_I(client).state,
    .operation = operation,
    .reason = reason,
    .service_error = k_no_service_error,
  };
  if (service_error != NULL)
  {
    event.service_error = *service_error;
    /* The event's string fields are documented as never NULL, and an
     * application is entitled to print them without checking. A channel that
     * left one unset must not turn that contract into a crash. */
    if (event.service_error.message == NULL)
    {
      event.service_error.message = "";
    }
    if (event.service_error.tracking_id == NULL)
    {
      event.service_error.tracking_id = "";
    }
  }
  dispatch_event(client, &event);
}

/** @brief Raise AZ_IOT_SU_EVENT_UPDATE_REFUSED for an update not processed. */
static void raise_refused(az_iot_su_client* client, az_iot_result reason)
{
  az_iot_su_event event = {
    .kind = AZ_IOT_SU_EVENT_UPDATE_REFUSED,
    .state = SU_I(client).state,
    .previous_state = SU_I(client).state,
    .reason = reason,
    .service_error = k_no_service_error,
  };
  dispatch_event(client, &event);
}

/* Move the workflow, telling anyone watching. Centralised so every transition
 * is reported: eighteen assignment sites cannot each be trusted to remember,
 * and a state change nobody hears about is the gap this closes. */
static void set_su_state(az_iot_su_client* client, az_iot_su_state next)
{
  az_iot_su_state previous = SU_I(client).state;
  if (previous == next)
  {
    return;
  }
  SU_I(client).state = next;

  az_iot_su_event event = {
    .kind = AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED,
    .state = next,
    .previous_state = previous,
    .operation = AZ_IOT_SU_OP_GET_UPDATE,
    .reason = AZ_IOT_OK,
    .service_error = { .code = 0, .message = "", .tracking_id = "" },
  };
  dispatch_event(client, &event);
}

/**
 * @brief Retire the owed terminal report once the channel's verdict makes it final.
 *
 * A report abandoned for want of a session (AZ_IOT_ERR_NOT_CONNECTED) never
 * reached the service, so its record is kept for the next boot.
 */
static void settle_terminal_report(az_iot_su_client* client, az_iot_result result)
{
  if (!SU_I(client).terminal_report_in_flight)
  {
    return;
  }
  SU_I(client).terminal_report_in_flight = false;
  if (result == AZ_IOT_ERR_NOT_CONNECTED)
  {
    return;
  }
  SU_I(client).report_owed = false;
  if (SU_I(client).state == AZ_IOT_SU_STATE_IDLE || SU_I(client).state == AZ_IOT_SU_STATE_FAILED)
  {
    sync_checkpoint(client);
  }
}

/** @brief First fallback delay, in milliseconds, for a retryable verdict naming no delay. */
#define AZ_IOT_SU_RETRY_BASE_MS 1000u
/** @brief Upper bound, in milliseconds, of the doubling fallback delay (before jitter). */
#define AZ_IOT_SU_RETRY_MAX_MS 60000u
/** @brief Jitter, in percent, applied around the fallback delay. */
#define AZ_IOT_SU_RETRY_JITTER_PCT 20u

/**
 * @brief Pace the next fetch/report after a retryable verdict that named no delay.
 *
 * Exponential backoff with jitter. Service verdicts only: a lost session is
 * paced by the connection client, and a service-named delay by the channel.
 * Never moves a fetch deadline.
 *
 * @return true if this verdict armed the backoff.
 */
static bool arm_retry_backoff(
    az_iot_su_client* client,
    az_iot_result result,
    az_iot_su_error_action action,
    const az_iot_su_service_error* service_error)
{
  if (result != AZ_IOT_ERR_DPS
      || (action != AZ_IOT_SU_ERROR_ACTION_RETRY && action != AZ_IOT_SU_ERROR_ACTION_RETRY_AFTER)
      || (service_error != NULL && service_error->retry_after_ms != 0))
  {
    return false;
  }
  static const az_iot_retry_policy policy = { .initial_delay_ms = AZ_IOT_SU_RETRY_BASE_MS,
                                              .max_delay_ms = AZ_IOT_SU_RETRY_MAX_MS,
                                              .max_attempts = 0,
                                              .jitter_pct = AZ_IOT_SU_RETRY_JITTER_PCT };
  if (SU_I(client).retry_rng == 0)
  {
    SU_I(client).retry_rng = az_iot_time_mono_ms() ^ (uint64_t)(uintptr_t)client;
  }
  (void)az_iot_retry_state__schedule(&SU_I(client).retry, &policy, &SU_I(client).retry_rng);
  return true;
}

/* The channel's verdict on an operation it accepted earlier.
 *
 * An asynchronous channel returns AZ_IOT_OK from request_update()/report() to
 * mean "sent". The pending flag is cleared at that point, so without this the
 * engine would never learn the service rejected it and the fetch or report
 * would be lost. Re-arming the flag puts it back in the do_work queue.
 *
 * FATAL is not re-armed: the request is malformed or the device is not
 * entitled, so resending it every tick would spin against the service.
 *
 * ALREADY_REPORTED is not re-armed: a terminal result is already recorded for
 * this workflow, so the report HAS been delivered and reporting is idempotent
 * on workflowId. Re-arming it would retry forever, and during the held
 * bootstrap session that starves the update check until the hold expires.
 *
 * PROCEED is not re-armed either. It is a terminal answer, not a failure: the
 * service is telling the device there is no update service configured for it,
 * and the classifier defines it as "carry on, do not retry". Re-arming it would
 * also fire a fresh check just as the hold is released and registration goes
 * out -- onto a session that is about to be torn down -- which is the race the
 * hold exists to prevent. */
static void on_channel_result(
    az_iot_su_operation operation,
    az_iot_result result,
    az_iot_su_error_action action,
    const az_iot_su_service_error* service_error,
    void* engine_ctx)
{
  az_iot_su_client* client = (az_iot_su_client*)engine_ctx;
  if (client == NULL || SU_I(client).detached)
  {
    return;
  }
  if (operation != AZ_IOT_SU_OP_REPORT_STATUS)
  {
    /* A verdict after the deadline is ignored and the check abandoned, as the
     * tick would have done had it run first. With a newer request queued, that
     * request's expiry (same deadline) reports it instead. */
    if (fetch_in_flight_overdue(client))
    {
      if (!expire_fetch_in_flight(client))
      {
        SU_I(client).fetch_in_flight = SU_FETCH_NONE;
      }
      return;
    }
    /* The accepted fetch has its verdict; nothing is awaited any more. */
    SU_I(client).fetch_in_flight = SU_FETCH_NONE;
  }
  if (result == AZ_IOT_OK || action == AZ_IOT_SU_ERROR_ACTION_FATAL
      || action == AZ_IOT_SU_ERROR_ACTION_PROCEED
      || action == AZ_IOT_SU_ERROR_ACTION_ALREADY_REPORTED || action == AZ_IOT_SU_ERROR_ACTION_NONE)
  {
    if (result == AZ_IOT_OK)
    {
      az_iot_retry_state__reset(&SU_I(client).retry);
    }
    /* This branch IS the definition of "the client will not re-arm it", so it
     * is also where the application is told. Deriving the two from one
     * condition is the point: a separate list elsewhere would be free to drift.
     *
     * Not every verdict here is an abandonment:
     *   - AZ_IOT_OK is success, and NONE accompanies it.
     *   - ALREADY_REPORTED means the service already has a terminal result for
     *     this workflow, so the report was not lost -- nothing to report.
     * What remains -- FATAL and PROCEED -- are requests that were dropped. Both
     * are reported. PROCEED in particular is UPDATE_ACCOUNT_NOT_LINKED on a
     * fetch: the device asked, was refused permanently, and without this reads
     * exactly like "no update available". */
    if (result != AZ_IOT_OK
        && (action == AZ_IOT_SU_ERROR_ACTION_FATAL || action == AZ_IOT_SU_ERROR_ACTION_PROCEED))
    {
      raise_abandoned(client, operation, result, service_error);
    }
    if (operation == AZ_IOT_SU_OP_REPORT_STATUS)
    {
      settle_terminal_report(client, result);
    }
    /* The request is over -- but only ITS deadline goes with it. A newer
     * request may already be queued, holding the slot with its own deadline;
     * clearing unconditionally would strip that and leave the newer request
     * retrying for ever. */
    if (operation != AZ_IOT_SU_OP_REPORT_STATUS && SU_I(client).pending_fetch == SU_FETCH_NONE)
    {
      SU_I(client).pending_fetch_deadline_ms = 0;
    }
    return;
  }

  bool paced = arm_retry_backoff(client, result, action, service_error);
  switch (operation)
  {
    case AZ_IOT_SU_OP_REPORT_STATUS:
      SU_I(client).terminal_report_in_flight = false;
      /* A report queued since keeps its own pacing; one send covers both. */
      if (!SU_I(client).device_properties_report_pending)
      {
        SU_I(client).device_properties_report_pending = true;
        SU_I(client).report_paced = paced;
      }
      break;
    case AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE:
    case AZ_IOT_SU_OP_GET_UPDATE:
      /* A service-requested delay that cannot fit inside the caller's deadline
       * ends the request NOW rather than at the deadline. Retrying until then
       * would publish nothing -- the channel refuses for the whole delay -- so
       * the wait buys nothing and only postpones the same answer.
       *
       * The delay is reported on the event, so the application learns WHEN the
       * service is willing to be asked again and can schedule its next attempt
       * instead of guessing. Deliberately not waited out on its behalf: that
       * would spend a budget the application set. */
      if (service_error != NULL && service_error->retry_after_ms != 0
          && SU_I(client).pending_fetch_deadline_ms != 0
          && az_iot_time_mono_ms() + (uint64_t)service_error->retry_after_ms
              > SU_I(client).pending_fetch_deadline_ms)
      {
        AZ_IOT_LOG_ERRORF(
            "su: service asked for %u ms, which does not fit the request timeout; giving up",
            (unsigned)service_error->retry_after_ms);
        SU_I(client).pending_fetch = SU_FETCH_NONE;
        SU_I(client).pending_fetch_deadline_ms = 0;
        raise_abandoned(client, operation, AZ_IOT_ERR_TIMEOUT, service_error);
        break;
      }
      /* Re-arm the route that failed, not a default: the application asked for
       * this one and a retry on the other would query the wrong thing.
       *
       * Unless it already asked for something newer. This verdict belongs to a
       * request the channel accepted earlier, so the application has had time
       * to queue another one in between; overwriting it here would silently
       * discard the newer request and retry a route nobody currently wants.
       * The newest request wins, which is what a second call to either request
       * function does as well. */
      if (SU_I(client).pending_fetch == SU_FETCH_NONE)
      {
        SU_I(client).pending_fetch = (operation == AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE)
            ? SU_FETCH_ONBOARDING
            : SU_FETCH_REGULAR;
        SU_I(client).pending_fetch_paced = paced;
        /* A re-armed request is always bounded. Without this a LATE verdict
         * could resurrect an operation after the slot had been abandoned: the
         * abandonment cleared the deadline, this puts the request back, and it
         * would then retry for ever.
         *
         * Re-armed with the timeout the CALLER gave, not a default: it is the
         * same request, so it keeps the same policy. */
        if (SU_I(client).pending_fetch_deadline_ms == 0)
        {
          arm_pending_fetch_deadline(client, SU_I(client).pending_fetch_timeout_ms);
        }
      }
      break;
  }
}

/* Ask the channel to check for an update on `operation`. Returns the channel's
 * result so the caller can leave pending_fetch set and retry on a later tick. */
static az_iot_result channel_request_update(az_iot_su_client* client, az_iot_su_operation operation)
{
  if (SU_I(client).channel.vtable == NULL || SU_I(client).channel.vtable->request_update == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return SU_I(client).channel.vtable->request_update(SU_I(client).channel.ctx, operation);
}

/* Issue whatever fetch is pending. No-op when nothing was requested.
 *
 * The slot is cleared BEFORE the channel is called, not after. A synchronous
 * channel is allowed to deliver its verdict from inside request_update(), and
 * that verdict re-arms the slot; clearing afterwards would wipe the re-armed
 * retry. Clearing first also means the re-arm logic sees an empty slot, which
 * is what tells it this is its own request rather than a newer one.
 *
 * On rejection the request is put back -- unless something already refilled the
 * slot while the channel had control, because that value is newer. A channel
 * that honours the contract cannot hit that case (the verdict callback fires
 * only for an ACCEPTED operation), so the check is defensive: it keeps a
 * misbehaving channel from turning a fresh request into a stale retry. */
/* A fresh request gets a fresh clock. Asking again is how an application
 * responds to an abandonment, so inheriting the old deadline would abandon the
 * new request immediately. */
static void arm_pending_fetch_deadline(az_iot_su_client* client, uint32_t timeout_ms)
{
  SU_I(client).pending_fetch_timeout_ms = timeout_ms;
  SU_I(client).pending_fetch_deadline_ms
      = (timeout_ms == 0u) ? 0u : az_iot_time_mono_ms() + (uint64_t)timeout_ms;
}

/** @brief True when the awaited fetch's deadline has passed. */
static bool fetch_in_flight_overdue(const az_iot_su_client* client)
{
  return SU_I(client).fetch_in_flight != SU_FETCH_NONE
      && SU_I(client).pending_fetch_deadline_ms != 0
      && az_iot_time_mono_ms() >= SU_I(client).pending_fetch_deadline_ms;
}

/** @brief The operation a SU_FETCH_* value stands for. */
static az_iot_su_operation fetch_operation(uint8_t fetch)
{
  return (fetch == SU_FETCH_ONBOARDING) ? AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE
                                        : AZ_IOT_SU_OP_GET_UPDATE;
}

/**
 * @brief Abandon an accepted fetch whose answer has not arrived by its deadline.
 *
 * The timeout bounds the whole wait, so being accepted by the channel does not
 * stop the clock. The channel is told to stop waiting, which frees its slot
 * and makes a late answer be ignored.
 *
 * @param client The software updates client.
 * @return true if the fetch was abandoned.
 */
static bool expire_fetch_in_flight(az_iot_su_client* client)
{
  uint8_t in_flight = SU_I(client).fetch_in_flight;
  if (in_flight == SU_FETCH_NONE || SU_I(client).pending_fetch != SU_FETCH_NONE
      || SU_I(client).pending_fetch_deadline_ms == 0
      || az_iot_time_mono_ms() < SU_I(client).pending_fetch_deadline_ms)
  {
    return false;
  }
  AZ_IOT_LOG_ERROR("su: giving up on an update check; no answer before its deadline");
  SU_I(client).fetch_in_flight = SU_FETCH_NONE;
  SU_I(client).pending_fetch_deadline_ms = 0;
  if (SU_I(client).channel.vtable != NULL && SU_I(client).channel.vtable->cancel_update != NULL)
  {
    SU_I(client).channel.vtable->cancel_update(
        SU_I(client).channel.ctx, fetch_operation(in_flight));
  }
  raise_abandoned(client, fetch_operation(in_flight), AZ_IOT_ERR_TIMEOUT, &k_no_service_error);
  return true;
}

static void drive_pending_fetch(az_iot_su_client* client)
{
  if (expire_fetch_in_flight(client))
  {
    return;
  }
  uint8_t requested = SU_I(client).pending_fetch;
  if (requested == SU_FETCH_NONE)
  {
    return;
  }
  az_iot_su_operation operation = fetch_operation(requested);

  /* Give up on a request that has gone unaccepted for too long -- refused, or
   * accepted and returned by a retryable verdict. Checked BEFORE the attempt,
   * so the deadline bounds how long the slot is retried rather than how long it
   * is held: an attempt that is about to succeed still does.
   *
   * WALL-CLOCK: time spent obeying a service-requested delay counts like any
   * other. The caller asked for an answer within N ms, and quietly moving its
   * deadline to exclude the wait would take away the very thing it was
   * planning around. A delay that CANNOT fit is answered immediately instead
   * -- see on_channel_result() -- rather than leaving the request to sit until
   * it expires. */
  if (SU_I(client).pending_fetch_deadline_ms != 0
      && az_iot_time_mono_ms() >= SU_I(client).pending_fetch_deadline_ms)
  {
    AZ_IOT_LOG_ERROR("su: giving up on a pending update check; its deadline expired");
    SU_I(client).pending_fetch = SU_FETCH_NONE;
    SU_I(client).pending_fetch_deadline_ms = 0;
    /* A superseded fetch still awaiting its answer shares the deadline and
     * goes with it; otherwise it would hold the channel with no bound. */
    uint8_t in_flight = SU_I(client).fetch_in_flight;
    SU_I(client).fetch_in_flight = SU_FETCH_NONE;
    if (in_flight != SU_FETCH_NONE && SU_I(client).channel.vtable != NULL
        && SU_I(client).channel.vtable->cancel_update != NULL)
    {
      SU_I(client).channel.vtable->cancel_update(
          SU_I(client).channel.ctx, fetch_operation(in_flight));
    }
    raise_abandoned(client, operation, AZ_IOT_ERR_TIMEOUT, &k_no_service_error);
    return;
  }
  /* After the deadline check: pacing must not postpone an abandonment. A newer
   * application request is not paced: the application owns its cadence. */
  if (SU_I(client).pending_fetch_paced && !az_iot_retry_state__due(&SU_I(client).retry))
  {
    return;
  }

  SU_I(client).pending_fetch = SU_FETCH_NONE;
  /* Set before the call: a synchronous channel may deliver the verdict from
   * inside it, which clears this again. */
  uint8_t previous_in_flight = SU_I(client).fetch_in_flight;
  SU_I(client).fetch_in_flight = requested;
  az_iot_result r = channel_request_update(client, operation);
  if (r != AZ_IOT_OK)
  {
    /* Refused, so not sent; whatever was in flight before still is. */
    if (SU_I(client).fetch_in_flight == requested)
    {
      SU_I(client).fetch_in_flight = previous_in_flight;
    }
    if (SU_I(client).pending_fetch == SU_FETCH_NONE)
    {
      SU_I(client).pending_fetch = requested;
    }
  }
  /* No refresh on a refusal: the deadline is the caller's, and a request whose
   * verdict never arrives has to stay bounded. */
  (void)r;
  /* The deadline otherwise SURVIVES the channel accepting the request. A
   * retryable verdict puts the same request straight back in the slot, so
   * clearing it here would restart the clock on every accepted-then-retried
   * round and the bound would never be reached. */
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

AZ_NODISCARD az_iot_su_client_config_options az_iot_su_client_config_options_default(void)
{
  az_iot_su_client_config_options opts = { 0 };
  return opts;
}

/* Core initialization. Assumes the caller has ALREADY zeroed the client: the
 * public path builds its channel into storage inside that client, so a memset
 * here would wipe it. */
static az_iot_result su_client_init_core(
    az_iot_su_client* client,
    const az_iot_su_channel* channel,
    const az_iot_su_client_config_options* options)
{
  if (client == NULL || channel == NULL || channel->vtable == NULL || options == NULL
      || options->hooks == NULL || options->crypto == NULL || options->device_properties == NULL
      || options->device_properties_buffer == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* open/close/request_update/report/cancel_update are required; do_work and
   * set_device_properties are optional. */
  if (channel->vtable->open == NULL || channel->vtable->close == NULL
      || channel->vtable->request_update == NULL || channel->vtable->report == NULL
      || channel->vtable->cancel_update == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (options->root_key_count > AZ_IOT_SU_MAX_ROOT_KEYS)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  SU_I(client).channel.vtable = channel->vtable;
  SU_I(client).channel.ctx = channel->ctx;
  SU_I(client).hooks = *options->hooks;
  SU_I(client).crypto = *options->crypto;
  SU_I(client).persist_max_attempts = (uint32_t)AZ_IOT_SU_PERSIST_MAX_ATTEMPTS;
  SU_I(client).device_properties_buffer = options->device_properties_buffer;
  SU_I(client).device_properties_buffer_size = options->device_properties_buffer_size;
  set_su_state(client, AZ_IOT_SU_STATE_IDLE);

  if (options->root_keys != NULL && options->root_key_count > 0)
  {
    for (size_t i = 0; i < options->root_key_count; ++i)
    {
      SU_I(client).root_keys[i] = options->root_keys[i];
    }
    SU_I(client).root_key_count = options->root_key_count;
  }

  if (az_result_failed(az_iot_adu_client_init(&SU_I(client).az, NULL)))
  {
    memset(client, 0, sizeof(*client));
    return AZ_IOT_ERR_INTERNAL;
  }

  az_iot_su_device_properties_snapshot snapshot;
  az_iot_result r = prepare_device_properties_cache(client, options->device_properties, &snapshot);
  if (r != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return r;
  }
  commit_device_properties_cache(client, &snapshot);

  r = channel->vtable->open(channel->ctx, on_channel_update, on_channel_result, client);
  if (r != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return r;
  }

  /* Report the initial Idle agent state + installed update id on startup. */
  SU_I(client).device_properties_report_pending = true;
  /* No update check is issued here. Only the application knows which route it
   * needs -- onboarding before it has a device record, regular after -- so it
   * asks, with az_iot_su_client_request_onboarding_update() or
   * az_iot_su_client_request_update(). */
  SU_I(client).pending_fetch = SU_FETCH_NONE;
  SU_I(client).fetch_in_flight = SU_FETCH_NONE;
  return AZ_IOT_OK;
}

az_iot_result az_iot_su_client__initialize_with_channel(
    az_iot_su_client* client,
    const az_iot_su_channel* channel,
    const az_iot_su_client_config_options* options)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(client, 0, sizeof(*client));
  return su_client_init_core(client, channel, options);
}

AZ_NODISCARD az_iot_result az_iot_su_client_init(
    az_iot_su_client* client,
    az_iot_connection_client* connection,
    const az_iot_su_client_config_options* options)
{
  if (client == NULL || connection == NULL || options == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* The application hands us a connection, not a transport implementation: the
   * SDK owns the device-update protocol. Build the shipping channel here.
   *
   * Order matters: the channel state lives INSIDE the client, so the client is
   * zeroed first and the core initializer must not zero it again. */
  memset(client, 0, sizeof(*client));

  az_iot_su_channel channel;
  az_iot_su_channel_dps* channel_state
      = (az_iot_su_channel_dps*)(void*)&SU_I(client).channel_storage;

  az_iot_result r
      = az_iot_su_channel_dps_init(channel_state, connection, options->device_properties, &channel);
  if (r != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return r;
  }

  r = su_client_init_core(client, &channel, options);
  if (r != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
  }
  return r;
}

void az_iot_su_client_deinit(az_iot_su_client* client)
{
  if (client == NULL)
  {
    return;
  }
  if (SU_I(client).channel.vtable != NULL && SU_I(client).channel.vtable->close != NULL)
  {
    /* Best-effort unbind during teardown. */
    SU_I(client).channel.vtable->close(SU_I(client).channel.ctx);
  }
  memset(client, 0, sizeof(*client));
}

/* az_iot_su_microsoft_root_keys() — Microsoft's compiled-in software updates production
 * root public keys — is defined in su_root_keys_microsoft.c (generated from the
 * official agent's hardcoded key list). Kept in a separate translation unit so
 * the large key blobs live apart from the state machine. */

/* ------------------------------------------------------------------------- */
/* persistence & resume (Phase 5)                                            */
/* ------------------------------------------------------------------------- */

/* Versioned, integrity-checked workflow snapshot. Layout (all little-endian):
 *   [0]  magic[4]   = 'S','U','C','P'
 *   [4]  u16 version (= AZ_IOT_SU_PERSIST_VERSION)
 *   [6]  u16 flags   (bit0 cancel_requested, bit1 have_request, bit2 report_owed)
 *   [8]  u32 state
 *   [12] u32 current_step
 *   [16] u32 current_file
 *   [20] u32 workflow_id_off   (offset into request_buffer)
 *   [24] u32 workflow_id_len
 *   [28] u32 manifest_off      (offset into request_buffer)
 *   [32] u32 manifest_len
 *   [36] u32 request_len
 *   [40] request_buffer[request_len]
 *   --- trailer (T = 40 + request_len) ---
 *   [T+0]  u32 outcome            (latched terminal outcome)
 *   [T+4]  i32 result_code        (accumulated install_result)
 *   [T+8]  i32 extended_result_code
 *   [T+12] u32 step_results_count (clamped to MAX_INSTRUCTIONS_STEPS)
 *   [T+16] step records { u32 outcome, u32 failure_origin, i32 result_code,
 *                         i32 extended_result_code }
 *          u32 url_count, then url_count *
 *            { u32 id_off, u32 id_len, u32 url_off, u32 url_len }
 *            (offsets into request_buffer)
 *          u32 len + active workflow id
 *          u32 len + applied update id ("provider\0name\0version\0"; 0 = none)
 *          u32 len + opaque channel state (save_state())
 *   [end] u32 crc32 (over bytes [0 .. end))
 *
 * A record with report_owed set is a terminal record: state Idle, no request.
 * It carries only the unsent terminal report, so resume re-sends it and runs
 * nothing. A record of any other magic or version is ignored. */
#define AZ_IOT_SU_PERSIST_MAGIC0 'S'
#define AZ_IOT_SU_PERSIST_MAGIC1 'U'
#define AZ_IOT_SU_PERSIST_MAGIC2 'C'
#define AZ_IOT_SU_PERSIST_MAGIC3 'P'
#define AZ_IOT_SU_PERSIST_VERSION 1u
#define AZ_IOT_SU_PERSIST_HEADER_SIZE 40u
#define AZ_IOT_SU_PERSIST_FLAG_CANCEL 0x1u
#define AZ_IOT_SU_PERSIST_FLAG_HAVE_REQUEST 0x2u
#define AZ_IOT_SU_PERSIST_FLAG_REPORT_OWED 0x4u
/* Fixed part of the trailer (outcome + 3 result ints). */
#define AZ_IOT_SU_PERSIST_TRAILER_FIXED 16u
#define AZ_IOT_SU_PERSIST_STEP_RECORD 16u
#define AZ_IOT_SU_PERSIST_URL_RECORD 16u
/* Upper bound on the whole trailer + crc, used to size the persist scratch. */
#define AZ_IOT_SU_PERSIST_TRAILER_MAX                                                             \
  (AZ_IOT_SU_PERSIST_TRAILER_FIXED                                                                \
   + ((uint32_t)(_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS) * AZ_IOT_SU_PERSIST_STEP_RECORD) + 4u \
   + ((uint32_t)(_az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT) * AZ_IOT_SU_PERSIST_URL_RECORD) + 4u    \
   + (uint32_t)(AZ_IOT_SU_WORKFLOW_ID_SIZE) + 4u + (uint32_t)(AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE)   \
   + 4u + (uint32_t)(AZ_IOT_SU_CHANNEL_STATE_MAX_SIZE) + 4u)

/* The per-instance persist_scratch (AZ_IOT_SU_PERSIST_BLOB_SIZE, in the client
 * struct) must hold the largest serialized blob: header + full request buffer +
 * the maximum trailer. C99-portable compile-time check (negative array size
 * on failure) so bumping the step count without growing the overhead is caught
 * at build time rather than overflowing at run time. */
typedef char az_iot_su_persist_blob_fits
    [(AZ_IOT_SU_PERSIST_HEADER_SIZE + AZ_IOT_SU_REQUEST_BUFFER_SIZE + AZ_IOT_SU_PERSIST_TRAILER_MAX
      <= AZ_IOT_SU_PERSIST_BLOB_SIZE)
         ? 1
         : -1];

static void wr_u16le(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr_u32le(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t rd_u16le(const uint8_t* p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), computed bytewise. */
static uint32_t su_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i)
  {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b)
    {
      uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

/* Offset of an az_span's data within request_buffer, or 0 if not contained. */
static uint32_t request_offset(const az_iot_su_client* client, az_span s)
{
  const uint8_t* base = SU_I(client).request_buffer;
  const uint8_t* p = az_span_ptr(s);
  if (p < base || p > base + SU_I(client).request_len)
  {
    return 0;
  }
  return (uint32_t)(p - base);
}

/** @brief Whether @p v lies entirely inside the staged request buffer. */
static bool span_in_request(const az_iot_su_client* client, az_span v)
{
  const uint8_t* base = SU_I(client).request_buffer;
  const uint8_t* ptr = az_span_ptr(v);
  size_t len = (size_t)az_span_size(v);
  return len > 0 && ptr >= base && (size_t)(ptr - base) <= SU_I(client).request_len
      && len <= SU_I(client).request_len - (size_t)(ptr - base);
}

/** @brief Whether every file of steps [from, steps_count) has a download URL. */
static bool remaining_files_have_urls(
    const az_iot_su_client_update_manifest* m,
    uint32_t from,
    const az_iot_su_client_file_url* urls,
    uint32_t url_count)
{
  for (uint32_t i = from; i < m->instructions.steps_count; ++i)
  {
    const az_iot_su_client_update_manifest_instructions_step* st = &m->instructions.steps[i];
    for (uint32_t j = 0; j < st->files_count; ++j)
    {
      bool found = false;
      for (uint32_t k = 0; k < url_count && !found; ++k)
      {
        found = az_span_is_content_equal(st->files[j], urls[k].id);
      }
      if (!found)
      {
        return false;
      }
    }
  }
  return true;
}

/** @brief Length of the packed applied update id, or 0 when none is held. */
static size_t applied_update_id_len(const az_iot_su_client* client)
{
  if (!SU_I(client).applied_update_id_valid)
  {
    return 0;
  }
  const char* base = SU_I(client).applied_update_id_buf;
  const char* v = SU_I(client).applied_update_id.version;
  return (size_t)(v - base) + strlen(v) + 1u;
}

/**
 * @brief Serialize the workflow and hand it to persist_state_fn.
 *
 * @param terminal Write the terminal record (unsent terminal report only)
 *                 instead of the workflow position.
 * @return AZ_IOT_OK when written; AZ_IOT_ERR_NOT_SUPPORTED when no
 * persist_state_fn is set; AZ_IOT_ERR_INVALID_ARG when the state cannot be
 * represented; AZ_IOT_ERR_INTERNAL when the hook fails.
 */
static az_iot_result su_persist(az_iot_su_client* client, bool terminal)
{
  az_iot_su_platform_hooks* h = &SU_I(client).hooks;
  if (h->persist_state_fn == NULL)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  const az_iot_su_client_update_request* req = &SU_I(client).current_request;
  uint32_t request_len = terminal ? 0u : (uint32_t)SU_I(client).request_len;
  uint32_t url_count = terminal ? 0u : req->file_urls_count;
  if (request_len > AZ_IOT_SU_REQUEST_BUFFER_SIZE
      || url_count > _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  for (uint32_t i = 0; i < url_count; ++i)
  {
    if (!span_in_request(client, req->file_urls[i].id)
        || !span_in_request(client, req->file_urls[i].url))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }
  int32_t step_count = SU_I(client).step_results_count;
  if (step_count < 0 || step_count > _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t wf_len = SU_I(client).active_workflow_valid ? SU_I(client).active_workflow_id_len : 0u;
  if (terminal && wf_len == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG; /* nothing the service could attribute it to */
  }

  uint8_t* blob = SU_I(client).persist_scratch;
  uint16_t flags = 0;
  if (terminal)
  {
    flags = AZ_IOT_SU_PERSIST_FLAG_REPORT_OWED;
  }
  else
  {
    if (SU_I(client).cancel_requested)
    {
      flags |= AZ_IOT_SU_PERSIST_FLAG_CANCEL;
    }
    if (SU_I(client).have_request)
    {
      flags |= AZ_IOT_SU_PERSIST_FLAG_HAVE_REQUEST;
    }
  }

  blob[0] = AZ_IOT_SU_PERSIST_MAGIC0;
  blob[1] = AZ_IOT_SU_PERSIST_MAGIC1;
  blob[2] = AZ_IOT_SU_PERSIST_MAGIC2;
  blob[3] = AZ_IOT_SU_PERSIST_MAGIC3;
  wr_u16le(&blob[4], (uint16_t)AZ_IOT_SU_PERSIST_VERSION);
  wr_u16le(&blob[6], flags);
  wr_u32le(&blob[8], terminal ? (uint32_t)AZ_IOT_SU_STATE_IDLE : (uint32_t)SU_I(client).state);
  wr_u32le(&blob[12], terminal ? 0u : SU_I(client).current_step);
  wr_u32le(&blob[16], terminal ? 0u : SU_I(client).current_file);
  wr_u32le(&blob[20], terminal ? 0u : request_offset(client, req->workflow.id));
  wr_u32le(&blob[24], terminal ? 0u : (uint32_t)az_span_size(req->workflow.id));
  wr_u32le(&blob[28], terminal ? 0u : request_offset(client, SU_I(client).manifest_text));
  wr_u32le(&blob[32], terminal ? 0u : (uint32_t)az_span_size(SU_I(client).manifest_text));
  wr_u32le(&blob[36], request_len);
  memcpy(&blob[AZ_IOT_SU_PERSIST_HEADER_SIZE], SU_I(client).request_buffer, request_len);

  uint32_t t = AZ_IOT_SU_PERSIST_HEADER_SIZE + request_len;
  const az_iot_su_client_install_result* r = &SU_I(client).install_result;
  wr_u32le(&blob[t + 0], (uint32_t)SU_I(client).pending_outcome);
  wr_u32le(&blob[t + 4], (uint32_t)r->result_code);
  wr_u32le(&blob[t + 8], (uint32_t)r->extended_result_code);
  wr_u32le(&blob[t + 12], (uint32_t)step_count);
  uint32_t p = t + AZ_IOT_SU_PERSIST_TRAILER_FIXED;
  for (int32_t i = 0; i < step_count; ++i)
  {
    const az_iot_su_step_result* sr = &SU_I(client).step_results[i];
    wr_u32le(&blob[p], (uint32_t)sr->outcome);
    wr_u32le(&blob[p + 4], (uint32_t)sr->failure_origin);
    wr_u32le(&blob[p + 8], (uint32_t)sr->result_code);
    wr_u32le(&blob[p + 12], (uint32_t)sr->extended_result_code);
    p += AZ_IOT_SU_PERSIST_STEP_RECORD;
  }

  wr_u32le(&blob[p], url_count);
  p += 4u;
  for (uint32_t i = 0; i < url_count; ++i)
  {
    wr_u32le(&blob[p], request_offset(client, req->file_urls[i].id));
    wr_u32le(&blob[p + 4], (uint32_t)az_span_size(req->file_urls[i].id));
    wr_u32le(&blob[p + 8], request_offset(client, req->file_urls[i].url));
    wr_u32le(&blob[p + 12], (uint32_t)az_span_size(req->file_urls[i].url));
    p += AZ_IOT_SU_PERSIST_URL_RECORD;
  }

  wr_u32le(&blob[p], (uint32_t)wf_len);
  memcpy(&blob[p + 4u], SU_I(client).active_workflow_id, wf_len);
  p += 4u + (uint32_t)wf_len;

  size_t applied_len = applied_update_id_len(client);
  wr_u32le(&blob[p], (uint32_t)applied_len);
  memcpy(&blob[p + 4u], SU_I(client).applied_update_id_buf, applied_len);
  p += 4u + (uint32_t)applied_len;

  size_t ch_len = 0;
  const az_iot_su_channel_vtable* vt = SU_I(client).channel.vtable;
  if (vt != NULL && vt->save_state != NULL
      && (vt->save_state(
              SU_I(client).channel.ctx, &blob[p + 4u], AZ_IOT_SU_CHANNEL_STATE_MAX_SIZE, &ch_len)
              != AZ_IOT_OK
          || ch_len > AZ_IOT_SU_CHANNEL_STATE_MAX_SIZE))
  {
    ch_len = 0;
  }
  wr_u32le(&blob[p], (uint32_t)ch_len);
  p += 4u + (uint32_t)ch_len;

  wr_u32le(&blob[p], su_crc32(blob, p));
  if (!persist_write(client, blob, (size_t)p + 4u))
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  SU_I(client).checkpoint_stored = true;
  SU_I(client).checkpoint_terminal = terminal;
  SU_I(client).checkpoint_superseded = false;
  return AZ_IOT_OK;
}

/**
 * @brief Take one u32-length-prefixed field at @p *pos.
 *
 * @return false when the field exceeds @p max or the blob; *pos then is unchanged.
 */
static bool take_field(
    const uint8_t* blob,
    size_t blen,
    uint32_t* pos,
    uint32_t max,
    uint32_t* out_at,
    uint32_t* out_len)
{
  if ((size_t)*pos + 4u > blen)
  {
    return false;
  }
  uint32_t len = rd_u32le(&blob[*pos]);
  if (len > max || (size_t)*pos + 4u + len > blen)
  {
    return false;
  }
  *out_at = *pos + 4u;
  *out_len = len;
  *pos = *out_at + len;
  return true;
}

/**
 * @brief Restore a packed applied update id ("provider\0name\0version\0").
 *
 * @return false, leaving the client unchanged, when @p src is not exactly three
 * non-empty NUL-terminated strings.
 */
static bool restore_applied_update_id(az_iot_su_client* client, const uint8_t* src, uint32_t len)
{
  if (len == 0)
  {
    SU_I(client).applied_update_id_valid = false;
    return true;
  }
  if (len > sizeof(SU_I(client).applied_update_id_buf) || src[len - 1u] != '\0')
  {
    return false;
  }
  const char* parts[3] = { NULL, NULL, NULL };
  uint32_t start = 0;
  int n = 0;
  for (uint32_t i = 0; i < len; ++i)
  {
    if (src[i] == '\0')
    {
      if (i == start || n == 3)
      {
        return false;
      }
      parts[n++] = SU_I(client).applied_update_id_buf + start;
      start = i + 1u;
    }
  }
  if (n != 3)
  {
    return false;
  }
  memcpy(SU_I(client).applied_update_id_buf, src, len);
  SU_I(client).applied_update_id.provider = parts[0];
  SU_I(client).applied_update_id.name = parts[1];
  SU_I(client).applied_update_id.version = parts[2];
  SU_I(client).applied_update_id_valid = true;
  return true;
}

/** @brief Whether @p o is an outcome that ends a workflow. */
static bool is_terminal_outcome(uint32_t o)
{
  return o == (uint32_t)AZ_IOT_SU_OUTCOME_SUCCEEDED || o == (uint32_t)AZ_IOT_SU_OUTCOME_FAILED
      || o == (uint32_t)AZ_IOT_SU_OUTCOME_CANCELED || o == (uint32_t)AZ_IOT_SU_OUTCOME_SKIPPED;
}

/** @brief Restore persisted step results and the accumulated result. */
static void restore_step_results(
    az_iot_su_client* client,
    const uint8_t* steps,
    int32_t step_count,
    int32_t res_code,
    int32_t res_ext)
{
  memset(&SU_I(client).install_result, 0, sizeof(SU_I(client).install_result));
  memset(SU_I(client).step_results, 0, sizeof(SU_I(client).step_results));
  SU_I(client).install_result.result_code = res_code;
  SU_I(client).install_result.extended_result_code = res_ext;
  SU_I(client).step_results_count = step_count;
  for (int32_t i = 0; i < step_count; ++i)
  {
    const uint8_t* rec = &steps[(size_t)i * AZ_IOT_SU_PERSIST_STEP_RECORD];
    az_iot_su_step_result* sr = &SU_I(client).step_results[i];
    sr->outcome = (az_iot_su_outcome)rd_u32le(rec);
    sr->failure_origin = (az_iot_su_failure_origin)rd_u32le(rec + 4);
    sr->result_code = (int32_t)rd_u32le(rec + 8);
    sr->extended_result_code = (int32_t)rd_u32le(rec + 12);
  }
}

/** @brief Hand persisted channel state back to the channel; best effort. */
static void restore_channel_state(az_iot_su_client* client, const uint8_t* src, uint32_t len)
{
  const az_iot_su_channel_vtable* vt = SU_I(client).channel.vtable;
  if (len > 0 && vt != NULL && vt->restore_state != NULL)
  {
    (void)vt->restore_state(SU_I(client).channel.ctx, src, len);
  }
}

AZ_NODISCARD az_iot_result az_iot_su_client_resume(az_iot_su_client* client)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (SU_I(client).detached)
  {
    return AZ_IOT_ERR_DETACHED;
  }
  if (SU_I(client).hooks.load_state_fn == NULL)
  {
    return AZ_IOT_OK;
  }

  uint8_t* blob = SU_I(client).persist_scratch;
  size_t blen = 0;
  if (SU_I(client).hooks.load_state_fn(
          blob, sizeof(SU_I(client).persist_scratch), &blen, SU_I(client).hooks.user_ctx)
      != 0)
  {
    return AZ_IOT_OK; /* nothing persisted */
  }
  if (blen > sizeof(SU_I(client).persist_scratch) || blen < AZ_IOT_SU_PERSIST_HEADER_SIZE + 4u)
  {
    return AZ_IOT_OK; /* too small, including an invalidated (empty) record */
  }
  if (blob[0] != AZ_IOT_SU_PERSIST_MAGIC0 || blob[1] != AZ_IOT_SU_PERSIST_MAGIC1
      || blob[2] != AZ_IOT_SU_PERSIST_MAGIC2 || blob[3] != AZ_IOT_SU_PERSIST_MAGIC3)
  {
    return AZ_IOT_OK; /* not our blob */
  }
  if (rd_u16le(&blob[4]) != AZ_IOT_SU_PERSIST_VERSION)
  {
    return AZ_IOT_OK;
  }

  uint32_t request_len = rd_u32le(&blob[36]);
  if (request_len > AZ_IOT_SU_REQUEST_BUFFER_SIZE)
  {
    return AZ_IOT_OK;
  }

  /* Locate and bounds-check the trailer. Every length below is bounded by
   * blen or a compile-time limit, so the sums cannot wrap. */
  uint32_t t = AZ_IOT_SU_PERSIST_HEADER_SIZE + request_len;
  if ((size_t)t + AZ_IOT_SU_PERSIST_TRAILER_FIXED > blen)
  {
    return AZ_IOT_OK;
  }
  uint32_t outcome = rd_u32le(&blob[t]);
  int32_t res_code = (int32_t)rd_u32le(&blob[t + 4u]);
  int32_t res_ext = (int32_t)rd_u32le(&blob[t + 8u]);
  int32_t step_count = (int32_t)rd_u32le(&blob[t + 12u]);
  if (step_count < 0 || step_count > _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS)
  {
    return AZ_IOT_OK;
  }
  uint32_t steps_at = t + AZ_IOT_SU_PERSIST_TRAILER_FIXED;
  uint32_t end = steps_at + (uint32_t)step_count * AZ_IOT_SU_PERSIST_STEP_RECORD;
  if ((size_t)end + 4u > blen)
  {
    return AZ_IOT_OK;
  }
  uint32_t url_count = rd_u32le(&blob[end]);
  if (url_count > _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT)
  {
    return AZ_IOT_OK;
  }
  uint32_t urls_at = end + 4u;
  end = urls_at + url_count * AZ_IOT_SU_PERSIST_URL_RECORD;
  uint32_t wf_at = 0;
  uint32_t wf_id_len = 0;
  uint32_t applied_at = 0;
  uint32_t applied_len = 0;
  uint32_t ch_at = 0;
  uint32_t ch_len = 0;
  if (!take_field(blob, blen, &end, AZ_IOT_SU_WORKFLOW_ID_SIZE, &wf_at, &wf_id_len)
      || !take_field(blob, blen, &end, AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE, &applied_at, &applied_len)
      || !take_field(blob, blen, &end, AZ_IOT_SU_CHANNEL_STATE_MAX_SIZE, &ch_at, &ch_len))
  {
    return AZ_IOT_OK;
  }
  if ((size_t)end + 4u != blen)
  {
    return AZ_IOT_OK; /* the CRC must be the last four bytes */
  }
  if (su_crc32(blob, end) != rd_u32le(&blob[end]))
  {
    return AZ_IOT_OK; /* corrupt */
  }
  if (SU_I(client).hooks.persist_state_fn == NULL)
  {
    /* It could never be invalidated, so every later boot would re-apply it. */
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  uint16_t flags = rd_u16le(&blob[6]);
  if ((flags & AZ_IOT_SU_PERSIST_FLAG_REPORT_OWED) != 0)
  {
    /* Terminal record: restore the unsent report and stay Idle. */
    if (!is_terminal_outcome(outcome) || wf_id_len == 0
        || !restore_applied_update_id(client, &blob[applied_at], applied_len))
    {
      return AZ_IOT_OK;
    }
    memset(&SU_I(client).current_request, 0, sizeof(SU_I(client).current_request));
    memset(&SU_I(client).current_manifest, 0, sizeof(SU_I(client).current_manifest));
    SU_I(client).request_len = 0;
    SU_I(client).have_request = false;
    SU_I(client).cancel_requested = false;
    SU_I(client).current_step = 0;
    SU_I(client).current_file = 0;
    SU_I(client).checkpoint_pending = false;
    restore_step_results(client, &blob[steps_at], step_count, res_code, res_ext);
    set_active_workflow(client, az_span_create(&blob[wf_at], (int32_t)wf_id_len), AZ_SPAN_EMPTY, 0);
    SU_I(client).pending_outcome = (az_iot_su_outcome)outcome;
    SU_I(client).report_owed = true;
    SU_I(client).terminal_report_in_flight = false;
    SU_I(client).checkpoint_stored = true;
    SU_I(client).checkpoint_terminal = true;
    SU_I(client).checkpoint_superseded = false;
    SU_I(client).device_properties_report_pending = true;
    SU_I(client).report_paced = false;
    restore_channel_state(client, &blob[ch_at], ch_len);
    set_su_state(client, AZ_IOT_SU_STATE_IDLE);
    return AZ_IOT_OK;
  }

  uint32_t state = rd_u32le(&blob[8]);
  uint32_t step = rd_u32le(&blob[12]);
  uint32_t file = rd_u32le(&blob[16]);
  uint32_t wf_off = rd_u32le(&blob[20]);
  uint32_t wf_len = rd_u32le(&blob[24]);
  uint32_t mf_off = rd_u32le(&blob[28]);
  uint32_t mf_len = rd_u32le(&blob[32]);
  if (mf_off > request_len || mf_len > request_len - mf_off || wf_off > request_len
      || wf_len > request_len - wf_off)
  {
    return AZ_IOT_OK;
  }
  for (uint32_t i = 0; i < url_count; ++i)
  {
    const uint8_t* u = &blob[urls_at + i * AZ_IOT_SU_PERSIST_URL_RECORD];
    uint32_t id_off = rd_u32le(u);
    uint32_t id_len = rd_u32le(u + 4);
    uint32_t url_off = rd_u32le(u + 8);
    uint32_t url_len = rd_u32le(u + 12);
    if (id_len == 0 || url_len == 0 || id_off > request_len || id_len > request_len - id_off
        || url_off > request_len || url_len > request_len - url_off)
    {
      return AZ_IOT_OK;
    }
  }

  /* Restore the request payload and re-derive the manifest from it. */
  SU_I(client).report_owed = false;
  SU_I(client).terminal_report_in_flight = false;
  memset(&SU_I(client).current_request, 0, sizeof(SU_I(client).current_request));
  memset(&SU_I(client).current_manifest, 0, sizeof(SU_I(client).current_manifest));
  memcpy(SU_I(client).request_buffer, &blob[AZ_IOT_SU_PERSIST_HEADER_SIZE], request_len);
  SU_I(client).request_len = request_len;

  az_span manifest_text = az_span_create(SU_I(client).request_buffer + mf_off, (int32_t)mf_len);
  SU_I(client).manifest_text = manifest_text;

  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, manifest_text, NULL))
      || az_result_failed(az_iot_adu_client_parse_update_manifest(
          &SU_I(client).az, &jr, &SU_I(client).current_manifest)))
  {
    /* Corrupt manifest text: discard the snapshot and stay Idle. */
    reset_to_idle(client);
    return AZ_IOT_OK;
  }

  az_iot_su_client_update_request* req = &SU_I(client).current_request;
  req->file_urls_count = url_count;
  for (uint32_t i = 0; i < url_count; ++i)
  {
    const uint8_t* u = &blob[urls_at + i * AZ_IOT_SU_PERSIST_URL_RECORD];
    req->file_urls[i].id
        = az_span_create(SU_I(client).request_buffer + rd_u32le(u), (int32_t)rd_u32le(u + 4));
    req->file_urls[i].url
        = az_span_create(SU_I(client).request_buffer + rd_u32le(u + 8), (int32_t)rd_u32le(u + 12));
  }

  /* Any step still to run must be able to download its files. A record taken
   * after the current step's install needs URLs only for the later steps. */
  bool step_installed = state == (uint32_t)AZ_IOT_SU_STATE_INSTALL_COMPLETE
      || state == (uint32_t)AZ_IOT_SU_STATE_APPLY_STARTED;
  uint32_t from = step_installed ? step + 1u : step;
  if (!remaining_files_have_urls(&SU_I(client).current_manifest, from, req->file_urls, url_count))
  {
    /* It can never be resumed, so retire it rather than refuse it every boot. */
    SU_I(client).checkpoint_stored = true;
    reset_to_idle(client);
    return AZ_IOT_ERR_INVALID_ARG;
  }

  req->workflow.id = az_span_create(SU_I(client).request_buffer + wf_off, (int32_t)wf_len);
  req->workflow.retry_timestamp = AZ_SPAN_EMPTY;
  req->update_manifest = manifest_text;

  SU_I(client).current_step = step;
  SU_I(client).current_file = file;
  SU_I(client).cancel_requested = (flags & AZ_IOT_SU_PERSIST_FLAG_CANCEL) != 0;
  SU_I(client).have_request = (flags & AZ_IOT_SU_PERSIST_FLAG_HAVE_REQUEST) != 0;
  SU_I(client).checkpoint_pending = false;
  SU_I(client).checkpoint_stored = true;
  SU_I(client).checkpoint_terminal = false;
  SU_I(client).checkpoint_superseded = false;

  /* Restore the accumulated result so completed step results of a multi-step
   * deployment survive a mid-deployment reboot. */
  restore_step_results(client, &blob[steps_at], step_count, res_code, res_ext);
  restore_channel_state(client, &blob[ch_at], ch_len);

  /* Re-establish the active workflow id so a redelivery after the reboot is
   * recognized as a duplicate and does NOT restart the resumed workflow. */
  set_active_workflow(client, req->workflow.id, AZ_SPAN_EMPTY, 0);
  set_su_state(client, (az_iot_su_state)state);
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* runtime: state machine                                                    */
/* ------------------------------------------------------------------------- */

static uint32_t step_file_count(const az_iot_su_client* client, uint32_t step)
{
  const az_iot_su_client_update_manifest* m = &SU_I(client).current_manifest;
  if (step >= m->instructions.steps_count)
  {
    return 0;
  }
  return m->instructions.steps[step].files_count;
}

/** @brief Resolve a step-local file slot to its manifest file entry.
 *
 * `instructions.steps[].files[]` holds file ids, not indices into
 * `manifest.files[]`; the two lists are ordered independently.
 *
 * @return The entry, or NULL when the manifest does not describe the id.
 */
static const az_iot_su_client_update_manifest_file* step_file_entry(
    const az_iot_su_client* client,
    uint32_t step,
    uint32_t file_slot)
{
  const az_iot_su_client_update_manifest* m = &SU_I(client).current_manifest;
  if (step >= m->instructions.steps_count || file_slot >= m->instructions.steps[step].files_count)
  {
    return NULL;
  }
  az_span id = m->instructions.steps[step].files[file_slot];
  for (uint32_t i = 0; i < m->files_count; ++i)
  {
    if (az_span_is_content_equal(m->files[i].id, id))
    {
      return &m->files[i];
    }
  }
  return NULL;
}

/* Begin a best-effort reverse-order rollback and finish in Failed/Idle.
 * restore_count is the number of leading steps that have a backup to undo;
 * steps [restore_count-1 .. 0] are restored in reverse order. A failure during
 * download or backup of step N leaves step N without a backup (restore_count =
 * N); a failure during install or apply of step N means step N was backed up
 * (restore_count = N + 1). */
/** @brief Enter FAILED and store the terminal report before it is sent. */
static void fail_workflow(az_iot_su_client* client)
{
  set_su_state(client, AZ_IOT_SU_STATE_FAILED);
  latch_terminal(client, AZ_IOT_SU_OUTCOME_FAILED);
  sync_checkpoint(client);
}

/**
 * @brief Call restore_fn for steps [end-1 .. first], in reverse order.
 *
 * Best effort: a failed restore is recorded (facility AZ_IOT_SU_FACILITY_RESTORE)
 * and earlier steps are still restored.
 */
static void restore_step_range(az_iot_su_client* client, uint32_t first, uint32_t end)
{
  /* Since backup/restore are non-blocking in practice (simulated or fast OTA
   * slot swaps), this performs the rollback synchronously. */
  if (SU_I(client).hooks.restore_fn == NULL)
  {
    return;
  }
  for (int32_t s = (int32_t)end - 1; s >= (int32_t)first; --s)
  {
    int32_t rr = SU_I(client).hooks.restore_fn(
        &SU_I(client).current_manifest, (uint32_t)s, SU_I(client).hooks.user_ctx);
    if (rr != AZ_IOT_SU_RESULT_SUCCESS)
    {
      az_iot_su_client_install_result* r = &SU_I(client).install_result;
      r->extended_result_code = AZ_IOT_SU_EXTENDED_RESULT(AZ_IOT_SU_FACILITY_RESTORE, (uint32_t)rr);
    }
  }
}

static void begin_rollback(az_iot_su_client* client, uint32_t restore_count)
{
  restore_step_range(client, 0u, restore_count);
  fail_workflow(client);
}

/**
 * @brief Roll back steps [first, end) whose undo is required, reporting when it cannot happen.
 *
 * Like restore_step_range(), but with no restore_fn the overall extended result
 * is set to AZ_IOT_SU_FACILITY_RESTORE (sub-code 0) rather than implying a rollback.
 */
static void roll_back_installed(az_iot_su_client* client, uint32_t first, uint32_t end)
{
  if (end > first && SU_I(client).hooks.restore_fn == NULL)
  {
    AZ_IOT_LOG_ERROR("su: no restore_fn; the installed step is not rolled back");
    SU_I(client).install_result.extended_result_code
        = AZ_IOT_SU_EXTENDED_RESULT(AZ_IOT_SU_FACILITY_RESTORE, 0u);
  }
  restore_step_range(client, first, end);
}

/**
 * @brief Fail the workflow held at a reboot boundary whose checkpoint cannot be written.
 *
 * Rolls back every step that was backed up (see held_restore_count()). When no
 * restore_fn is set or a restore fails, the overall extended result carries
 * AZ_IOT_SU_FACILITY_RESTORE (sub-code 0: no restore_fn).
 */
static void fail_on_persist(az_iot_su_client* client)
{
  uint32_t step = SU_I(client).current_step;
  uint32_t count = held_restore_count(client);
  SU_I(client).checkpoint_pending = false;
  result_step_failure(client, step, AZ_IOT_SU_FACILITY_PERSIST, SU_I(client).persist_last_error);
  roll_back_installed(client, 0u, count);
  fail_workflow(client);
  (void)az_iot_su__report_state(client);
}

az_iot_result az_iot_su_client_do_work(az_iot_su_client* client)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (SU_I(client).detached)
  {
    return AZ_IOT_ERR_DETACHED;
  }

  /* Give the channel its tick first. A channel with asynchronous work of its
   * own reports lost operations here, and the engine's own pending flags are
   * re-armed from that, so this has to run before they are read below. */
  if (SU_I(client).channel.vtable != NULL && SU_I(client).channel.vtable->do_work != NULL)
  {
    (void)SU_I(client).channel.vtable->do_work(SU_I(client).channel.ctx);
  }

  /* Storage out of step while Idle is a failed write: a reboot now would
   * resume the finished workflow or lose its report, so retry it rather than
   * wait for the next workflow. */
  if (SU_I(client).state == AZ_IOT_SU_STATE_IDLE && checkpoint_out_of_sync(client)
      && persist_retry_due(client))
  {
    sync_checkpoint(client);
  }

  /* Retry failed checkpoint writes ahead of anything that needs the network,
   * so storage recovery does not wait on connectivity. Retries back off (see
   * persist_write()) to spare flash endurance. */
  if (SU_I(client).checkpoint_superseded && persist_retry_due(client))
  {
    clear_checkpoint(client);
  }
  bool running
      = SU_I(client).state != AZ_IOT_SU_STATE_IDLE && SU_I(client).state != AZ_IOT_SU_STATE_FAILED;
  if (SU_I(client).checkpoint_pending && running && persist_retry_due(client)
      && su_persist(client, false) != AZ_IOT_ERR_INTERNAL)
  {
    SU_I(client).checkpoint_pending = false;
  }
  if (persist_gave_up(client))
  {
    /* The new workflow proceeds; a reboot before it ends may resume the old
     * record, which the new workflowId supersedes again. */
    SU_I(client).checkpoint_superseded = false;
    if (SU_I(client).checkpoint_pending && running)
    {
      /* Reported there; a re-armed report goes out on the next tick. */
      fail_on_persist(client);
      return AZ_IOT_OK;
    }
  }

  /* A pending device-properties / startup report takes priority. */
  if (SU_I(client).device_properties_report_pending)
  {
    /* Cleared BEFORE the send, as for the update check: a synchronous channel
     * may re-arm it from inside report(). Restored on refusal, so a status
     * report -- the service's only record of what this device did -- is not
     * dropped on a transient channel failure. */
    if (!SU_I(client).report_paced || az_iot_retry_state__due(&SU_I(client).retry))
    {
      SU_I(client).device_properties_report_pending = false;
      if (az_iot_su__report_state(client) != AZ_IOT_OK)
      {
        SU_I(client).device_properties_report_pending = true;
      }
    }
    /* Piggyback a requested update check on the same startup tick so a
     * deployment already waiting is consumed without needing a fresh
     * delivery. Cleared only once the request is actually issued (the channel
     * may not be ready yet). */
    drive_pending_fetch(client);
    return AZ_IOT_OK;
  }

  /* Retry a requested update check that could not be issued earlier (e.g. the
   * channel was not ready, or the service rejected it retryably). Self-heals
   * across do_work iterations; does not preempt state-machine progress. */
  drive_pending_fetch(client);

  /* Hold while a checkpoint write is outstanding. A superseded workflow's
   * record would make a reboot resume it or re-send its report; a failed
   * boundary write would let the workflow run a phase it could not resume.
   * Checked before cancellation, which would otherwise abandon an installed
   * step without rolling it back. */
  if (SU_I(client).checkpoint_superseded
      || (SU_I(client).checkpoint_pending && SU_I(client).state != AZ_IOT_SU_STATE_IDLE
          && SU_I(client).state != AZ_IOT_SU_STATE_FAILED))
  {
    return AZ_IOT_OK;
  }

  /* Cancellation at a phase boundary returns immediately to Idle. FAILED is
   * excluded: its terminal outcome is already reported, and a late cancel would
   * report a second, conflicting one for the same workflow. */
  if (SU_I(client).cancel_requested && SU_I(client).state != AZ_IOT_SU_STATE_IDLE
      && SU_I(client).state != AZ_IOT_SU_STATE_FAILED)
  {
    /* The installed step not yet applied is undone: a reboot must not activate
     * a canceled update. Completed steps are kept, as the report says. */
    if (SU_I(client).state == AZ_IOT_SU_STATE_INSTALL_COMPLETE
        || SU_I(client).state == AZ_IOT_SU_STATE_APPLY_STARTED)
    {
      roll_back_installed(client, SU_I(client).current_step, SU_I(client).current_step + 1u);
    }
    result_step_canceled(client);
    latch_terminal(client, AZ_IOT_SU_OUTCOME_CANCELED);
    reset_to_idle(client);
    (void)az_iot_su__report_state(client);
    return AZ_IOT_OK;
  }

  az_iot_su_platform_hooks* h = &SU_I(client).hooks;

  switch (SU_I(client).state)
  {
    case AZ_IOT_SU_STATE_IDLE:
      /* Nothing to do until a deployment arrives. */
      break;

    case AZ_IOT_SU_STATE_MANIFEST_RECEIVED:
    {
      if (parse_manifest(client) != AZ_IOT_OK)
      {
        result_init_steps(client, 1);
        result_step_failure(client, 0, AZ_IOT_SU_FACILITY_INTERNAL, 0);
        fail_workflow(client);
        (void)az_iot_su__report_state(client);
        break;
      }
      result_init_steps(client, (int32_t)SU_I(client).current_manifest.instructions.steps_count);
      set_su_state(client, AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
      (void)az_iot_su__report_state(client);
      break;
    }

    case AZ_IOT_SU_STATE_VERIFYING_MANIFEST:
    {
      if (verify_manifest(client) != AZ_IOT_SU_RESULT_SUCCESS)
      {
        result_step_failure(client, 0, AZ_IOT_SU_FACILITY_MANIFEST, 0);
        fail_workflow(client);
        (void)az_iot_su__report_state(client);
        break;
      }
      /* Already installed. There is no accept/reject acknowledgement, so it is
       * reported as a SKIPPED outcome. */
      int32_t inst = (h->is_installed_fn != NULL)
          ? h->is_installed_fn(&SU_I(client).current_manifest, h->user_ctx)
          : AZ_IOT_SU_RESULT_SUCCESS;
      if (inst == AZ_IOT_SU_RESULT_ALREADY_INSTALLED)
      {
        latch_terminal(client, AZ_IOT_SU_OUTCOME_SKIPPED);
        reset_to_idle(client);
        (void)az_iot_su__report_state(client);
        break;
      }
      SU_I(client).current_step = 0;
      SU_I(client).current_file = 0;
      set_su_state(client, AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
      (void)az_iot_su__report_state(client);
      break;
    }

    case AZ_IOT_SU_STATE_DOWNLOAD_STARTED:
    {
      uint32_t step = SU_I(client).current_step;
      uint32_t fcount = step_file_count(client, step);
      if (SU_I(client).current_file >= fcount)
      {
        set_su_state(client, AZ_IOT_SU_STATE_DOWNLOAD_COMPLETE);
        break;
      }
      /* Resolve the file + its download url, then drive download_fn. */
      uint32_t fidx = SU_I(client).current_file;
      const az_iot_su_client_update_manifest_file* file = step_file_entry(client, step, fidx);
      if (file == NULL)
      {
        /* The step names a file id the manifest does not describe. */
        result_step_failure(client, step, AZ_IOT_SU_FACILITY_DOWNLOAD, AZ_IOT_SU_RESULT_FAILURE);
        begin_rollback(client, step);
        (void)az_iot_su__report_state(client);
        break;
      }
      az_span url = AZ_SPAN_EMPTY;
      for (uint32_t i = 0; i < SU_I(client).current_request.file_urls_count; ++i)
      {
        if (az_span_is_content_equal(SU_I(client).current_request.file_urls[i].id, file->id))
        {
          url = SU_I(client).current_request.file_urls[i].url;
          break;
        }
      }
      int32_t dr = (h->download_fn != NULL) ? h->download_fn(file, url, fidx, fcount, h->user_ctx)
                                            : AZ_IOT_SU_RESULT_FAILURE;
      if (dr == AZ_IOT_SU_RESULT_IN_PROGRESS)
      {
        break; /* re-enter on next do_work */
      }
      if (dr != AZ_IOT_SU_RESULT_SUCCESS)
      {
        result_step_failure(client, step, AZ_IOT_SU_FACILITY_DOWNLOAD, dr);
        begin_rollback(client, step);
        (void)az_iot_su__report_state(client);
        break;
      }
      /* Streaming per-file SHA-256 verification (opt-in: requires a
       * read-back hook plus the incremental crypto hooks). */
      if (h->read_file_fn != NULL && SU_I(client).crypto.sha256_init_fn != NULL
          && SU_I(client).crypto.sha256_update_fn != NULL
          && SU_I(client).crypto.sha256_final_fn != NULL)
      {
        int32_t hr = verify_file_hash(client, file, fidx);
        if (hr != AZ_IOT_SU_RESULT_SUCCESS)
        {
          result_step_failure(client, step, AZ_IOT_SU_FACILITY_HASH, hr);
          begin_rollback(client, step);
          (void)az_iot_su__report_state(client);
          break;
        }
      }
      SU_I(client).current_file++;
      break;
    }

    case AZ_IOT_SU_STATE_DOWNLOAD_COMPLETE:
      set_su_state(client, AZ_IOT_SU_STATE_BACKUP_STARTED);
      break;

    case AZ_IOT_SU_STATE_BACKUP_STARTED:
    {
      uint32_t step = SU_I(client).current_step;
      int32_t br = (h->backup_fn != NULL)
          ? h->backup_fn(&SU_I(client).current_manifest, step, h->user_ctx)
          : AZ_IOT_SU_RESULT_SUCCESS;
      if (br == AZ_IOT_SU_RESULT_IN_PROGRESS)
      {
        break;
      }
      if (br != AZ_IOT_SU_RESULT_SUCCESS)
      {
        result_step_failure(client, step, AZ_IOT_SU_FACILITY_BACKUP, br);
        begin_rollback(client, step);
        (void)az_iot_su__report_state(client);
        break;
      }
      set_su_state(client, AZ_IOT_SU_STATE_BACKUP_COMPLETE);
      break;
    }

    case AZ_IOT_SU_STATE_BACKUP_COMPLETE:
      set_su_state(client, AZ_IOT_SU_STATE_INSTALL_STARTED);
      break;

    case AZ_IOT_SU_STATE_INSTALL_STARTED:
    {
      uint32_t step = SU_I(client).current_step;
      int32_t ir = (h->install_fn != NULL)
          ? h->install_fn(&SU_I(client).current_manifest, step, h->user_ctx)
          : AZ_IOT_SU_RESULT_FAILURE;
      if (ir == AZ_IOT_SU_RESULT_IN_PROGRESS)
      {
        break;
      }
      if (ir == AZ_IOT_SU_RESULT_REBOOT_REQUIRED)
      {
        /* The install needs a reboot to take effect: snapshot the
         * workflow so az_iot_su_client_resume() can continue at Apply
         * on the next boot, report, and advance (a real device reboots
         * here; the loop simply continues if it does not). */
        set_su_state(client, AZ_IOT_SU_STATE_INSTALL_COMPLETE);
        /* A failed write blocks Apply until a retry succeeds; with no
         * persist_state_fn there is nothing to retry. */
        SU_I(client).checkpoint_pending = (su_persist(client, false) == AZ_IOT_ERR_INTERNAL);
        (void)az_iot_su__report_state(client);
        break;
      }
      if (ir != AZ_IOT_SU_RESULT_SUCCESS)
      {
        result_step_failure(client, step, AZ_IOT_SU_FACILITY_INSTALL, ir);
        begin_rollback(client, step + 1); /* step was backed up */
        (void)az_iot_su__report_state(client);
        break;
      }
      set_su_state(client, AZ_IOT_SU_STATE_INSTALL_COMPLETE);
      break;
    }

    case AZ_IOT_SU_STATE_INSTALL_COMPLETE:
      set_su_state(client, AZ_IOT_SU_STATE_APPLY_STARTED);
      break;

    case AZ_IOT_SU_STATE_APPLY_STARTED:
    {
      uint32_t step = SU_I(client).current_step;
      int32_t ar = (h->apply_fn != NULL)
          ? h->apply_fn(&SU_I(client).current_manifest, step, h->user_ctx)
          : AZ_IOT_SU_RESULT_SUCCESS;
      if (ar == AZ_IOT_SU_RESULT_IN_PROGRESS)
      {
        break;
      }
      if (ar != AZ_IOT_SU_RESULT_SUCCESS && ar != AZ_IOT_SU_RESULT_REBOOT_REQUIRED)
      {
        result_step_failure(client, step, AZ_IOT_SU_FACILITY_APPLY, ar);
        begin_rollback(client, step + 1); /* step was backed up */
        (void)az_iot_su__report_state(client);
        break;
      }
      result_step_success(client, step);

      /* Advance to the next step, or finish. */
      uint32_t steps = SU_I(client).current_manifest.instructions.steps_count;
      if (step + 1 < steps)
      {
        SU_I(client).current_step = step + 1;
        SU_I(client).current_file = 0;
        set_su_state(client, AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
        /* An apply-requested reboot resumes at the next step. A stored record
         * is refreshed too: left at this step, a reboot would re-apply it. */
        if (ar == AZ_IOT_SU_RESULT_REBOOT_REQUIRED || SU_I(client).checkpoint_stored)
        {
          SU_I(client).checkpoint_pending = (su_persist(client, false) == AZ_IOT_ERR_INTERNAL);
        }
        (void)az_iot_su__report_state(client);
        break;
      }
      /* The last step: the terminal record replaces any stored position. */
      result_overall_success(client);
      latch_terminal(client, AZ_IOT_SU_OUTCOME_SUCCEEDED);
      reset_to_idle(client);
      (void)az_iot_su__report_state(client);
      break;
    }

    case AZ_IOT_SU_STATE_RESTORE_STARTED:
      /* begin_rollback() performs restore synchronously then sets Failed;
       * this state is reserved for a future chunked rollback. */
      fail_workflow(client);
      break;

    case AZ_IOT_SU_STATE_FAILED:
      /* Retain failure for later reports after returning to Idle. */
      SU_I(client).pending_outcome = AZ_IOT_SU_OUTCOME_FAILED;
      reset_to_idle(client);
      break;
  }

  return AZ_IOT_OK;
}

bool az_iot_su_is_cancelled(const az_iot_su_client* client)
{
  if (client == NULL)
  {
    return false;
  }
  return SU_I(client).cancel_requested;
}

az_iot_su_state az_iot_su_client_get_state(const az_iot_su_client* client)
{
  if (client == NULL)
  {
    return AZ_IOT_SU_STATE_IDLE;
  }
  return SU_I(client).state;
}

az_iot_result az_iot_su_client_add_observer(
    az_iot_su_client* client,
    az_iot_su_observer_callback cb,
    void* user_ctx)
{
  if (client == NULL || cb == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Adding from inside a dispatch would hand the new subscriber the event in
   * flight -- one it was not watching for -- and mutate the array being
   * walked. Removing is allowed; see the remove function. */
  if (SU_I(client).dispatching)
  {
    AZ_IOT_LOG_ERROR("su: cannot add an observer from inside one");
    return AZ_IOT_ERR_BUSY;
  }

  size_t free_slot = AZ_IOT_MAX_SU_OBSERVERS;
  for (size_t i = 0; i < AZ_IOT_MAX_SU_OBSERVERS; ++i)
  {
    /* Idempotent on the (cb, user_ctx) PAIR, not on cb alone: one callback
     * shared by two owners is two subscriptions and must be delivered twice. */
    if (SU_I(client).observers[i].cb == cb && SU_I(client).observers[i].user_ctx == user_ctx)
    {
      return AZ_IOT_OK;
    }
    if (SU_I(client).observers[i].cb == NULL && free_slot == AZ_IOT_MAX_SU_OBSERVERS)
    {
      free_slot = i;
    }
  }
  if (free_slot == AZ_IOT_MAX_SU_OBSERVERS)
  {
    AZ_IOT_LOG_ERROR("su: no free observer slot");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  SU_I(client).observers[free_slot].cb = cb;
  SU_I(client).observers[free_slot].user_ctx = user_ctx;
  return AZ_IOT_OK;
}

az_iot_result az_iot_su_client_remove_observer(
    az_iot_su_client* client,
    az_iot_su_observer_callback cb,
    void* user_ctx)
{
  if (client == NULL || cb == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Deliberately NOT refused during a dispatch, unlike adding. An owner torn
   * down in reaction to an event runs its teardown from inside the callback,
   * and the entry holds a raw pointer to storage it is about to release, so it
   * must be able to withdraw. Safe against the walk in dispatch_event(): that
   * loop re-reads each slot and skips a NULL callback, and nothing is
   * compacted, so clearing a slot only means that observer is not called --
   * which is what withdrawing asks for. */
  for (size_t i = 0; i < AZ_IOT_MAX_SU_OBSERVERS; ++i)
  {
    if (SU_I(client).observers[i].cb == cb && SU_I(client).observers[i].user_ctx == user_ctx)
    {
      SU_I(client).observers[i].cb = NULL;
      SU_I(client).observers[i].user_ctx = NULL;
      return AZ_IOT_OK;
    }
  }
  return AZ_IOT_ERR_NOT_FOUND;
}

AZ_NODISCARD az_iot_result
az_iot_su_client_request_onboarding_update(az_iot_su_client* client, uint32_t timeout_ms)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  SU_I(client).pending_fetch = SU_FETCH_ONBOARDING;
  SU_I(client).pending_fetch_paced = false;
  arm_pending_fetch_deadline(client, timeout_ms);
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result
az_iot_su_client_request_update(az_iot_su_client* client, uint32_t timeout_ms)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  SU_I(client).pending_fetch = SU_FETCH_REGULAR;
  SU_I(client).pending_fetch_paced = false;
  arm_pending_fetch_deadline(client, timeout_ms);
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_su_client_update_device_properties(
    az_iot_su_client* client,
    const az_iot_su_device_properties* device_properties)
{
  if (client == NULL || device_properties == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (SU_I(client).detached)
  {
    return AZ_IOT_ERR_DETACHED;
  }

  az_iot_su_device_properties_snapshot snapshot;
  az_iot_result r = prepare_device_properties_cache(client, device_properties, &snapshot);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* The channel may hold its own copy (compatibility properties, installed
   * update id). Refresh it, or fetches keep carrying the startup identity. */
  if (SU_I(client).channel.vtable != NULL
      && SU_I(client).channel.vtable->set_device_properties != NULL)
  {
    r = SU_I(client).channel.vtable->set_device_properties(
        SU_I(client).channel.ctx, &snapshot.properties);
    if (r != AZ_IOT_OK)
    {
      return r;
    }
  }

  commit_device_properties_cache(client, &snapshot);
  SU_I(client).device_properties_report_pending = true;
  SU_I(client).report_paced = false;
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* agent core-library API (library mode / bring-your-own state machine)      */
/* ------------------------------------------------------------------------- */

AZ_NODISCARD az_iot_result az_iot_su_parse_update_request(
    az_span request_json,
    const az_iot_su_crypto_hooks* crypto,
    const az_iot_su_root_key* root_keys,
    size_t root_key_count,
    az_iot_su_client_update_request* out_request,
    az_iot_su_client_update_manifest* out_manifest)
{
  if (crypto == NULL || out_request == NULL || out_manifest == NULL
      || az_span_size(request_json) <= 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Fail-closed: outputs stay zeroed unless every stage succeeds. */
  memset(out_request, 0, sizeof(*out_request));
  memset(out_manifest, 0, sizeof(*out_manifest));

  az_iot_adu_client az;
  if (az_result_failed(az_iot_adu_client_init(&az, NULL)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_iot_su_client_update_request req;
  az_iot_result r = parse_update_metadata(request_json, &req);
  if (r == AZ_IOT_OK)
  {
    r = decode_request_strings(&req);
  }
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* Decode the manifest in place (never longer); the JWS is signed over the
   * decoded text. */
  az_span manifest_text;
  if (az_iot_json_string_decode(req.update_manifest, req.update_manifest, &manifest_text)
          != AZ_IOT_OK
      || az_span_size(manifest_text) <= 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  req.update_manifest = manifest_text;

  /* Trust gate before the manifest is parsed. */
  if (verify_manifest_core(
          crypto, root_keys, root_key_count, manifest_text, req.update_manifest_signature)
      != AZ_IOT_SU_RESULT_SUCCESS)
  {
    return AZ_IOT_ERR_AUTH;
  }

  az_json_reader jr;
  az_iot_su_client_update_manifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  if (az_result_failed(az_json_reader_init(&jr, manifest_text, NULL))
      || az_result_failed(az_iot_adu_client_parse_update_manifest(&az, &jr, &manifest)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  *out_request = req;
  *out_manifest = manifest;
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_su_verify_file_hash(
    const az_iot_su_client_update_manifest_file* file,
    const az_iot_su_crypto_hooks* crypto,
    az_iot_su_read_chunk_callback read_chunk,
    void* read_ctx)
{
  if (file == NULL || crypto == NULL || read_chunk == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return (verify_file_hash_core(crypto, file, read_chunk, read_ctx) == AZ_IOT_SU_RESULT_SUCCESS)
      ? AZ_IOT_OK
      : AZ_IOT_ERR_AUTH;
}
