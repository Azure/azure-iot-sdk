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
 * Platform operations (download/install/apply/backup/restore/is_installed) are
 * reached through the platform hooks, and RS256 verify + SHA-256 through the
 * connection client's crypto backend; the core links no platform/OS/crypto code.
 *
 * Install is gated on manifest verification (verify_manifest(), design doc
 * section 6) and on per-file SHA-256 checks.
 */
#include <stdint.h>
#include <string.h>

#include <azure/core/az_base64.h>
#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_su.h"

#include "internal/su_internal.h"
#include "internal/base64.h"
#include "internal/crypto.h"
#include "internal/json_string.h"
#include "internal/mono_time.h"
#include "internal/retry_policy.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

/** @brief Bytes of the persisted-record header, which precedes the request. */
#define AZ_IOT_SU_PERSIST_HEADER_SIZE 40u

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
static void raise_deferred(az_iot_su_client* client);
static void raise_retry_wait(az_iot_su_client* client, uint32_t retry_after_ms);
static void raise_simple(
    az_iot_su_client* client,
    az_iot_su_event_kind kind,
    az_iot_su_operation operation,
    bool report_terminal,
    const az_iot_su_report* report);

/* fetch_in_flight, fetch_delivered and report_sent_terminal occupy padding
 * after pending_fetch, so adding them left the caller-allocated client's size and offsets
 * unchanged. Fails the build if that padding is ever gone. */
typedef struct
{
  char c;
  uint64_t v;
} su_u64_alignment_probe;
typedef char az_iot_su_fetch_in_flight_uses_padding
    [(offsetof(az_iot_su_client, _internal.report_sent_terminal)
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

/** @brief Validate @p src, refresh the channel's copy, then the cache; unchanged on failure. */
static az_iot_result store_device_properties(
    az_iot_su_client* client,
    const az_iot_su_device_properties* src)
{
  az_iot_su_device_properties_snapshot snapshot;
  az_iot_result r = prepare_device_properties_cache(client, src, &snapshot);
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
  return AZ_IOT_OK;
}

/**
 * @brief After a SUCCEEDED workflow, advertise the update it applied as installed.
 *
 * The application asserted it when apply_fn succeeded; later update checks
 * carry it without the application having to call
 * az_iot_su_client_update_device_properties(). Kept in its own buffer, so the
 * device-properties buffer need not have room for it.
 */
static void adopt_applied_update_id(az_iot_su_client* client)
{
  if (!SU_I(client).applied_update_id_valid || SU_I(client).device_properties_buffer == NULL)
  {
    return;
  }
  const char* src = SU_I(client).applied_update_id_buf;
  char* dst = SU_I(client).adopted_installed_id_buf;
  memcpy(dst, src, sizeof(SU_I(client).adopted_installed_id_buf));
  az_iot_su_device_properties next = SU_I(client).device_properties;
  next.installed_update_id.provider = dst + (SU_I(client).applied_update_id.provider - src);
  next.installed_update_id.name = dst + (SU_I(client).applied_update_id.name - src);
  next.installed_update_id.version = dst + (SU_I(client).applied_update_id.version - src);
  if (SU_I(client).channel.vtable != NULL
      && SU_I(client).channel.vtable->set_device_properties != NULL
      && SU_I(client).channel.vtable->set_device_properties(SU_I(client).channel.ctx, &next)
          != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_SU,
        "applied update id not adopted as installed; checks keep the previous one");
    return;
  }
  SU_I(client).device_properties.installed_update_id = next.installed_update_id;
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
  int32_t code = -(int32_t)facility; /* non-positive marks a failure; names the failing phase */

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
 * backend only for the two RSA signature checks and the binding SHA-256 (see
 * design doc section 6). The backend remains pure primitives.
 *
 * Buffers: the chain decodes its base64url segments into a caller-supplied
 * scratch, not fixed stack arrays, because signing keys and their headers grow
 * as keys rotate. Only the outer JWS header is decoded into it beside one of:
 * the SJWK header (alg, kid), the manifest signature or the JWS payload; the
 * SJWK signature, payload and signing key are decoded in place within the
 * header. The managed client uses its az_iot_su_limits::scratch_size workspace
 * scratch; az_iot_su_parse_update_request() uses AZ_IOT_SU_VERIFY_SCRATCH_SIZE
 * bytes of stack. Values that do not fit are logged as too large, not as
 * invalid.
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

/** @brief Bump allocator over the scratch manifest verification decodes into. */
typedef struct
{
  uint8_t* buf;
  int32_t cap;
  int32_t used;
  /** Bytes the last scratch_b64_decode() needed; logged when they did not fit. */
  int32_t need;
} su_scratch;

/**
 * @brief Decode @p src (see az_iot_base64_decode()) into the free part of @p s and claim it.
 *
 * @return AZ_IOT_OK with @p out in @p s; AZ_IOT_ERR_NOT_ENOUGH_SPACE if it does not fit;
 *         AZ_IOT_ERR_INVALID_ARG if @p src is empty or malformed.
 */
static az_iot_result scratch_b64_decode(su_scratch* s, az_span src, bool allow_std, az_span* out)
{
  int32_t len = az_span_size(src);
  s->need = (len / 4) * 3 + ((len % 4) * 3) / 4;
  az_iot_result r = az_iot_base64_decode(
      src, allow_std, az_span_create(s->buf + s->used, s->cap - s->used), out);
  if (r == AZ_IOT_OK)
  {
    s->used += az_span_size(*out);
  }
  return r;
}

/**
 * @brief Decode writable @p src (see az_iot_base64_decode()) in place.
 *
 * @return AZ_IOT_OK with @p out at the start of @p src; AZ_IOT_ERR_INVALID_ARG if empty or
 *         malformed.
 */
static az_iot_result b64_decode_in_place(az_span src, bool allow_std, az_span* out)
{
  return az_iot_base64_decode(src, allow_std, src, out);
}

/**
 * @brief Find top-level property @p name of JSON object @p obj.
 *
 * @return true with @p out on its value token; false if absent or @p obj is not an object.
 */
static bool jws_json_find(az_span obj, az_span name, az_json_token* out)
{
  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, obj, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return false;
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
      return false;
    }
    if (match)
    {
      *out = jr.token;
      return true;
    }
    /* Skip a non-matching object/array value so we stay at object scope. */
    if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      if (az_result_failed(az_json_reader_skip_children(&jr)))
      {
        return false;
      }
    }
  }
  return false;
}

/**
 * @brief Copy the unescaped value of top-level string property @p name of @p obj to @p out_buf.
 *
 * @return AZ_IOT_OK with @p out on it; AZ_IOT_ERR_NOT_FOUND if absent, empty or not a string;
 *         AZ_IOT_ERR_NOT_ENOUGH_SPACE if it does not fit @p cap with a terminator;
 *         AZ_IOT_ERR_INVALID_ARG if it cannot be unescaped.
 */
static az_iot_result jws_json_str(
    az_span obj,
    az_span name,
    char* out_buf,
    int32_t cap,
    az_span* out)
{
  az_json_token t;
  if (!jws_json_find(obj, name, &t) || t.kind != AZ_JSON_TOKEN_STRING)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  int32_t len = 0;
  az_result r = az_json_token_get_string(&t, out_buf, cap, &len);
  if (r == AZ_ERROR_NOT_ENOUGH_SPACE)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  if (az_result_failed(r))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (len <= 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  *out = az_span_create((uint8_t*)out_buf, len);
  return AZ_IOT_OK;
}

/**
 * @brief Unescape the value of top-level string property @p name in place in @p obj.
 *
 * Avoids copying a value nearly as large as @p obj. @p obj must be writable; if the value had
 * escapes it is no longer valid JSON afterwards, so read other properties first.
 *
 * @return As jws_json_str(), except AZ_IOT_ERR_NOT_ENOUGH_SPACE.
 */
static az_iot_result jws_json_str_in_place(az_span obj, az_span name, az_span* out)
{
  az_json_token t;
  if (!jws_json_find(obj, name, &t) || t.kind != AZ_JSON_TOKEN_STRING)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  az_span value;
  if (az_iot_json_string_decode(t.slice, t.slice, &value) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (az_span_size(value) <= 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  *out = value;
  return AZ_IOT_OK;
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

/**
 * @brief Map a failed verify_rs256() result to an AZ_IOT_SU_MANIFEST_ERROR_* sub-code.
 *
 * A rejected signature (AZ_IOT_ERR_AUTH) or key (AZ_IOT_ERR_INVALID_ARG) is @p rejected;
 * any other error is a backend fault (AZ_IOT_SU_MANIFEST_ERROR_CRYPTO).
 */
static uint32_t signature_error(az_iot_result r, uint32_t rejected)
{
  return (r == AZ_IOT_ERR_AUTH || r == AZ_IOT_ERR_INVALID_ARG) ? rejected
                                                               : AZ_IOT_SU_MANIFEST_ERROR_CRYPTO;
}

/* Log why a manifest fails verification, store its AZ_IOT_SU_MANIFEST_ERROR_*
 * sub-code in *out_error, then bail. */
#define SU_VERIFY_FAIL(code, why)                                                          \
  do                                                                                       \
  {                                                                                        \
    AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_SU, "manifest verification failed: %s", (why)); \
    *out_error = (code);                                                                   \
    return AZ_IOT_SU_RESULT_FAILURE;                                                       \
  } while (0)

/**
 * @brief Log why a scratch_b64_decode() of @p what failed.
 *
 * @param s    The scratch it decoded into.
 * @param r    Its result, not AZ_IOT_OK.
 * @param what The decoded part, for the log.
 */
static void log_decode_failure(const su_scratch* s, az_iot_result r, const char* what)
{
  if (r == AZ_IOT_ERR_NOT_ENOUGH_SPACE)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "manifest verification failed: %s too large (%ld bytes, %ld of %ld free)",
        what,
        (long)s->need,
        (long)(s->cap - s->used),
        (long)s->cap);
  }
  else
  {
    AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_SU, "manifest verification failed: %s not valid", what);
  }
}

/* Fail verification unless scratch_b64_decode() result `r` is AZ_IOT_OK. */
#define SU_VERIFY_DECODED(r, s, what)                                                        \
  do                                                                                         \
  {                                                                                          \
    az_iot_result r_ = (r);                                                                  \
    if (r_ != AZ_IOT_OK)                                                                     \
    {                                                                                        \
      log_decode_failure((s), r_, (what));                                                   \
      *out_error = (r_ == AZ_IOT_ERR_NOT_ENOUGH_SPACE) ? AZ_IOT_SU_MANIFEST_ERROR_TOO_LARGE  \
                                                       : AZ_IOT_SU_MANIFEST_ERROR_MALFORMED; \
      return AZ_IOT_SU_RESULT_FAILURE;                                                       \
    }                                                                                        \
  } while (0)

/**
 * @brief Verify the manifest signature chain (design doc section 6).
 *
 * @param crypto         Crypto backend; needs verify_rs256.
 * @param root_keys      Trusted root keys.
 * @param root_key_count Entries in @p root_keys.
 * @param manifest       The unescaped manifest body.
 * @param jws            The compact updateManifestSignature.
 * @param scratch        Decoded segments are written here; not read before.
 * @param scratch_size   Size of @p scratch.
 * @param out_error      On failure, the AZ_IOT_SU_MANIFEST_ERROR_* sub-code.
 * @return AZ_IOT_SU_RESULT_SUCCESS; AZ_IOT_SU_RESULT_FAILURE, with the reason logged.
 */
static int32_t verify_manifest_core(
    const az_iot_crypto* crypto,
    const az_iot_su_root_key* root_keys,
    size_t root_key_count,
    az_span manifest,
    az_span jws,
    uint8_t* scratch,
    size_t scratch_size,
    uint32_t* out_error)
{
  if (az_iot_crypto__validate(crypto) != AZ_IOT_OK || crypto->verify_rs256 == NULL)
  {
    SU_VERIFY_FAIL(
        AZ_IOT_SU_MANIFEST_ERROR_CRYPTO, "crypto backend missing or without verify_rs256");
  }

  /* `manifest` is the UNESCAPED manifest body. decode_manifest() unescapes the
   * service-supplied manifest IN PLACE (the unescaped form is never longer)
   * and records that span; the service signs the hash of the unescaped body,
   * so step 6 must hash exactly these bytes (not the original escaped span,
   * whose LENGTH still covers trailing leftover bytes). `jws` is the compact
   * update-manifest signature. */
  if (az_span_size(jws) <= 0 || az_span_size(manifest) <= 0)
  {
    SU_VERIFY_FAIL(
        AZ_IOT_SU_MANIFEST_ERROR_MALFORMED, "empty updateManifest or updateManifestSignature");
  }

  su_scratch s = { scratch, (int32_t)(scratch_size > INT32_MAX ? INT32_MAX : scratch_size), 0, 0 };

  /* 1. Split the manifest JWS and decode + parse its protected header. The
   *    header, and the sjwk unescaped within it, stay in scratch through step 4. */
  az_span m_hdr_b64, m_pl_b64, m_sig_b64, m_signed;
  if (!jws_split(jws, &m_hdr_b64, &m_pl_b64, &m_sig_b64, &m_signed))
  {
    SU_VERIFY_FAIL(
        AZ_IOT_SU_MANIFEST_ERROR_MALFORMED, "step 1: manifest JWS is not a valid 3-part token");
  }

  az_span sjwk;
  {
    az_span hdr;
    SU_VERIFY_DECODED(
        scratch_b64_decode(&s, m_hdr_b64, false, &hdr), &s, "step 1: manifest JWS header");

    char alg_buf[16];
    az_span alg = AZ_SPAN_EMPTY;
    if (jws_json_str(hdr, AZ_SPAN_FROM_STR("alg"), alg_buf, (int32_t)sizeof(alg_buf), &alg)
            != AZ_IOT_OK
        || !az_span_is_content_equal(alg, k_alg_rs256))
    {
      SU_VERIFY_FAIL(AZ_IOT_SU_MANIFEST_ERROR_ALG, "step 1: manifest JWS alg is not RS256");
    }

    if (jws_json_str_in_place(hdr, AZ_SPAN_FROM_STR("sjwk"), &sjwk) != AZ_IOT_OK)
    {
      SU_VERIFY_FAIL(AZ_IOT_SU_MANIFEST_ERROR_MALFORMED, "step 1: manifest JWS header has no sjwk");
    }
  }

  /* 2. Split the SJWK and read its header (alg + kid), then resolve the kid. */
  az_span s_hdr_b64, s_pl_b64, s_sig_b64, s_signed;
  if (!jws_split(sjwk, &s_hdr_b64, &s_pl_b64, &s_sig_b64, &s_signed))
  {
    SU_VERIFY_FAIL(AZ_IOT_SU_MANIFEST_ERROR_MALFORMED, "step 2: sjwk is not a valid 3-part token");
  }

  /* Only the SJWK header is decoded beside the outer header; the SJWK signature
   * and payload, and the signing key in it, are decoded in place within it. */
  int32_t after_hdr = s.used;
  const az_iot_su_root_key* root = NULL;
  {
    az_span shdr;
    SU_VERIFY_DECODED(scratch_b64_decode(&s, s_hdr_b64, false, &shdr), &s, "step 2: sjwk header");

    char salg_buf[16];
    az_span salg = AZ_SPAN_EMPTY;
    if (jws_json_str(shdr, AZ_SPAN_FROM_STR("alg"), salg_buf, (int32_t)sizeof(salg_buf), &salg)
            != AZ_IOT_OK
        || !az_span_is_content_equal(salg, k_alg_rs256))
    {
      SU_VERIFY_FAIL(AZ_IOT_SU_MANIFEST_ERROR_ALG, "step 2: sjwk alg is not RS256");
    }

    char kid_buf[128];
    az_span kid = AZ_SPAN_EMPTY;
    az_iot_result r
        = jws_json_str(shdr, AZ_SPAN_FROM_STR("kid"), kid_buf, (int32_t)sizeof(kid_buf), &kid);
    if (r == AZ_IOT_ERR_NOT_ENOUGH_SPACE)
    {
      SU_VERIFY_FAIL(
          AZ_IOT_SU_MANIFEST_ERROR_TOO_LARGE, "step 2: sjwk kid too large (127 bytes max)");
    }
    if (r != AZ_IOT_OK)
    {
      SU_VERIFY_FAIL(AZ_IOT_SU_MANIFEST_ERROR_MALFORMED, "step 2: sjwk header has no kid");
    }

    root = resolve_root_key(root_keys, root_key_count, kid);
    if (root == NULL)
    {
      SU_VERIFY_FAIL(
          AZ_IOT_SU_MANIFEST_ERROR_UNKNOWN_KID,
          "step 2: sjwk kid does not match any known (enabled) root key");
    }
  }
  s.used = after_hdr;

  /* 3. Verify the SJWK signature with the resolved root key. */
  {
    az_span s_sig;
    SU_VERIFY_DECODED(b64_decode_in_place(s_sig_b64, false, &s_sig), &s, "step 3: sjwk signature");
    az_iot_result vr = crypto->verify_rs256(
        crypto,
        root->modulus,
        root->modulus_len,
        root->exponent,
        root->exponent_len,
        az_span_ptr(s_signed),
        (size_t)az_span_size(s_signed),
        az_span_ptr(s_sig),
        (size_t)az_span_size(s_sig));
    if (vr != AZ_IOT_OK)
    {
      SU_VERIFY_FAIL(
          signature_error(vr, AZ_IOT_SU_MANIFEST_ERROR_ROOT_SIGNATURE),
          "step 3: sjwk signature does not verify against the root key");
    }
  }

  /* 4. Parse the now-trusted SJWK payload as a JWK → signing key (n, e). The
   *    modulus may be standard base64; it is unescaped in place, after `e` is
   *    copied out. The SJWK's signed bytes are no longer needed. */
  az_span n_raw;
  az_span e_raw;
  char e_b64[64];
  {
    az_span spl;
    SU_VERIFY_DECODED(b64_decode_in_place(s_pl_b64, false, &spl), &s, "step 4: sjwk payload");

    az_span e_field = AZ_SPAN_EMPTY;
    az_span n_field = AZ_SPAN_EMPTY;
    az_iot_result r
        = jws_json_str(spl, AZ_SPAN_FROM_STR("e"), e_b64, (int32_t)sizeof(e_b64), &e_field);
    if (r == AZ_IOT_ERR_NOT_ENOUGH_SPACE)
    {
      SU_VERIFY_FAIL(
          AZ_IOT_SU_MANIFEST_ERROR_TOO_LARGE,
          "step 4: signing JWK exponent (e) too large (63 bytes max)");
    }
    if (r != AZ_IOT_OK || jws_json_str_in_place(spl, AZ_SPAN_FROM_STR("n"), &n_field) != AZ_IOT_OK)
    {
      SU_VERIFY_FAIL(
          AZ_IOT_SU_MANIFEST_ERROR_MALFORMED,
          "step 4: signing JWK is missing modulus (n) or exponent (e)");
    }
    SU_VERIFY_DECODED(
        b64_decode_in_place(n_field, true, &n_raw), &s, "step 4: signing JWK modulus (n)");
    SU_VERIFY_DECODED(
        b64_decode_in_place(e_field, true, &e_raw), &s, "step 4: signing JWK exponent (e)");
  }

  /* 5. Verify the manifest JWS signature with the trusted signing key. */
  {
    az_span m_sig;
    SU_VERIFY_DECODED(
        scratch_b64_decode(&s, m_sig_b64, false, &m_sig), &s, "step 5: manifest signature");
    az_iot_result vr = crypto->verify_rs256(
        crypto,
        az_span_ptr(n_raw),
        (size_t)az_span_size(n_raw),
        az_span_ptr(e_raw),
        (size_t)az_span_size(e_raw),
        az_span_ptr(m_signed),
        (size_t)az_span_size(m_signed),
        az_span_ptr(m_sig),
        (size_t)az_span_size(m_sig));
    if (vr != AZ_IOT_OK)
    {
      SU_VERIFY_FAIL(
          signature_error(vr, AZ_IOT_SU_MANIFEST_ERROR_SIGNATURE),
          "step 5: manifest signature does not verify against the signing key");
    }
  }
  s.used = after_hdr;

  /* 6. Bind the signed manifest to THIS deployment: the manifest JWS payload
   *    carries SHA-256(manifest body); recompute and compare. */
  {
    az_span pl;
    SU_VERIFY_DECODED(
        scratch_b64_decode(&s, m_pl_b64, false, &pl), &s, "step 6: manifest JWS payload");

    char hash_b64[128];
    az_span hash_field = AZ_SPAN_EMPTY;
    az_iot_result r = jws_json_str(
        pl, AZ_SPAN_FROM_STR("sha256"), hash_b64, (int32_t)sizeof(hash_b64), &hash_field);
    if (r == AZ_IOT_ERR_NOT_FOUND)
    {
      SU_VERIFY_FAIL(
          AZ_IOT_SU_MANIFEST_ERROR_MALFORMED, "step 6: manifest JWS payload has no sha256");
    }

    az_span expected = AZ_SPAN_EMPTY;
    if (r != AZ_IOT_OK || b64_decode_in_place(hash_field, true, &expected) != AZ_IOT_OK
        || az_span_size(expected) != 32)
    {
      SU_VERIFY_FAIL(
          AZ_IOT_SU_MANIFEST_ERROR_MALFORMED,
          "step 6: manifest sha256 field is not a 32-byte base64 hash");
    }

    uint8_t actual[32];
    if (az_iot_crypto__sha256(crypto, az_span_ptr(manifest), (size_t)az_span_size(manifest), actual)
        != AZ_IOT_OK)
    {
      SU_VERIFY_FAIL(
          AZ_IOT_SU_MANIFEST_ERROR_CRYPTO, "step 6: SHA-256 failed over the manifest body");
    }
    if (!az_span_is_content_equal(expected, AZ_SPAN_FROM_BUFFER(actual)))
    {
      SU_VERIFY_FAIL(
          AZ_IOT_SU_MANIFEST_ERROR_HASH,
          "step 6: computed manifest SHA-256 does not match the signed hash");
    }
  }

  return AZ_IOT_SU_RESULT_SUCCESS;
}

/* Thin wrapper: verify the current deployment's manifest using the client's
 * crypto backend + root-key store, in the workspace scratch. On failure
 * *out_error is the AZ_IOT_SU_MANIFEST_ERROR_* sub-code. */
static int32_t verify_manifest(az_iot_su_client* client, uint32_t* out_error)
{
  return verify_manifest_core(
      SU_I(client).crypto,
      SU_I(client).root_keys,
      SU_I(client).root_key_count,
      SU_I(client).manifest_text,
      SU_I(client).current_request.update_manifest_signature,
      SU_I(client).scratch,
      SU_I(client).scratch_size,
      out_error);
}

/* Verify a downloaded file's SHA-256 against the signed manifest by streaming
 * the file back through a generic read-chunk callback and the crypto backend.
 * Returns SUCCESS when the hash matches; FAILURE on any mismatch or
 * backend/read error. Client-independent so the public API and the managed state
 * machine share one implementation. */
static int32_t verify_file_hash_core(
    const az_iot_crypto* crypto,
    const az_iot_su_client_update_manifest_file* file,
    az_iot_su_read_chunk_callback read_chunk,
    void* read_ctx)
{
  if (az_iot_crypto__validate(crypto) != AZ_IOT_OK || file == NULL || read_chunk == NULL)
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

  /* Stream the file through the backend's SHA-256. */
  az_iot_sha256_ctx ctx;
  if (crypto->sha256_init(crypto, &ctx) != AZ_IOT_OK)
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
      (void)crypto->sha256_final(crypto, &ctx, NULL);
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    if (read == 0)
    {
      break; /* end of file */
    }
    if (crypto->sha256_update(crypto, &ctx, chunk, read) != AZ_IOT_OK)
    {
      (void)crypto->sha256_final(crypto, &ctx, NULL);
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    offset += read;
  }

  uint8_t actual[AZ_IOT_SHA256_SIZE];
  if (crypto->sha256_final(crypto, &ctx, actual) != AZ_IOT_OK)
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
  return verify_file_hash_core(SU_I(client).crypto, file, su_read_file_adapter, &a);
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

/** @brief Which record su_persist() writes. */
typedef enum su_record
{
  SU_RECORD_POSITION, /**< The running workflow's position, to resume it. */
  SU_RECORD_TERMINAL, /**< The owed terminal report only. */
  SU_RECORD_RETRY /**< A FAILED workflow whose report is settled, for its retry. */
} su_record;

static az_iot_result su_persist(az_iot_su_client* client, su_record kind);
static bool retry_record_wanted(const az_iot_su_client* client);
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
  AZ_IOT_LOG_ERRORF(
      AZ_IOT_LOG_COMPONENT_SU, "persist_state_fn failed (%u consecutive)", (unsigned)n);
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
  if (persist_write(client, SU_I(client).terminal_blob, 0))
  {
    SU_I(client).checkpoint_stored = false;
    SU_I(client).checkpoint_terminal = false;
    SU_I(client).checkpoint_superseded = false;
    SU_I(client).checkpoint_retry = false;
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
      && h->persist_state_fn(SU_I(client).terminal_blob, 0, h->user_ctx) == 0)
  {
    SU_I(client).checkpoint_stored = false;
    SU_I(client).checkpoint_terminal = false;
    SU_I(client).checkpoint_superseded = false;
    SU_I(client).checkpoint_retry = false;
  }
}

/**
 * @brief Bring storage in line with a finished (or idle) workflow.
 *
 * An owed terminal report is stored as the terminal record; otherwise a
 * FAILED workflow's retry schedule is stored as the retry record,
 * and any other record is retired. When the terminal record cannot be written, a stale
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
    if (!retry_record_wanted(client))
    {
      clear_checkpoint(client);
    }
    else if (!SU_I(client).checkpoint_retry && persist_retry_due(client))
    {
      (void)su_persist(client, SU_RECORD_RETRY);
    }
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
  az_iot_result pr = su_persist(client, SU_RECORD_TERMINAL);
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
  if (SU_I(client).report_owed)
  {
    return !SU_I(client).checkpoint_terminal;
  }
  return retry_record_wanted(client) ? !SU_I(client).checkpoint_retry
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

/** @brief Whether the last workflow FAILED, so an offer of its id is a retry. */
static bool workflow_retryable(const az_iot_su_client* client)
{
  return SU_I(client).active_workflow_valid && !SU_I(client).active_workflow_running
      && SU_I(client).pending_outcome == AZ_IOT_SU_OUTCOME_FAILED;
}

/** @brief Unix times before 2024-01-01T00:00:00Z are unknown (an unsynced clock). */
#define SU_UNIX_TIME_FLOOR 1704067200u
/** @brief Longest restored workflow retry wait: the 24 h cap plus its largest jitter. */
#define SU_WORKFLOW_RETRY_MAX_WAIT_MS (86400000u + 8640000u)

/** @brief A FAILED workflow's n-th retry waits min(1 h * 2^(n-1), 24 h) plus up to 10%. */
static const az_iot_retry_policy k_workflow_retry_policy = {
  .initial_delay_ms = 3600000u,
  .max_delay_ms = 86400000u,
  .max_attempts = 0u,
  .jitter_pct = 10u,
  .jitter_mode = AZ_IOT_RETRY_JITTER_ADDITIVE,
};

/** @brief Current Unix seconds; 0 when unknown or before SU_UNIX_TIME_FLOOR. */
static uint64_t su_unix_now(const az_iot_su_client* client)
{
  uint64_t now = az_iot_time_unix_s(SU_I(client).unix_time_fn, SU_I(client).unix_time_ctx);
  return now >= SU_UNIX_TIME_FLOOR ? now : 0u;
}

/** @brief Seed the jitter PRNG once, per client. */
static void seed_retry_rng(az_iot_su_client* client)
{
  if (SU_I(client).retry_rng == 0)
  {
    SU_I(client).retry_rng = az_iot_time_mono_ms() ^ (uint64_t)(uintptr_t)client;
  }
}

/** @brief Restore the workflow retry schedule; a stored deadline before the floor is unknown. */
static void workflow_retry_restore(az_iot_su_client* client, const uint8_t* in)
{
  uint8_t rec[AZ_IOT_RETRY_STATE_RECORD_SIZE];
  memcpy(rec, in, sizeof(rec));
  uint64_t due_unix = (uint64_t)rec[8] | ((uint64_t)rec[9] << 8) | ((uint64_t)rec[10] << 16)
      | ((uint64_t)rec[11] << 24) | ((uint64_t)rec[12] << 32) | ((uint64_t)rec[13] << 40)
      | ((uint64_t)rec[14] << 48) | ((uint64_t)rec[15] << 56);
  if (due_unix < SU_UNIX_TIME_FLOOR)
  {
    memset(&rec[8], 0, 8);
  }
  az_iot_retry_state__restore(
      &SU_I(client).workflow_retry, rec, su_unix_now(client), SU_WORKFLOW_RETRY_MAX_WAIT_MS);
}

/** @brief Whether storage should hold the retry record (nothing else being owed). */
static bool retry_record_wanted(const az_iot_su_client* client)
{
  return workflow_retryable(client)
      && az_iot_retry_state__attempts(&SU_I(client).workflow_retry) > 0;
}

/**
 * @brief Latch a terminal outcome whose report must survive a reboot until accepted.
 *
 * Ends the running workflow: FAILED schedules its retry, any other outcome
 * clears the schedule.
 */
static void latch_terminal(az_iot_su_client* client, az_iot_su_outcome outcome)
{
  if (SU_I(client).active_workflow_running)
  {
    if (outcome == AZ_IOT_SU_OUTCOME_FAILED)
    {
      seed_retry_rng(client);
      (void)az_iot_retry_state__schedule_until(
          &SU_I(client).workflow_retry,
          &k_workflow_retry_policy,
          &SU_I(client).retry_rng,
          su_unix_now(client));
      AZ_IOT_LOG_INFOF(
          AZ_IOT_LOG_COMPONENT_SU,
          "workflow failed %u time(s) in a row; a retry waits %u s",
          (unsigned)az_iot_retry_state__attempts(&SU_I(client).workflow_retry),
          (unsigned)(az_iot_retry_state__remaining_ms(&SU_I(client).workflow_retry) / 1000u));
    }
    else
    {
      az_iot_retry_state__reset(&SU_I(client).workflow_retry);
    }
  }
  SU_I(client).active_workflow_running = false;
  SU_I(client).pending_outcome = outcome;
  SU_I(client).report_owed = true;
  SU_I(client).checkpoint_terminal = false;
  SU_I(client).checkpoint_retry = false;
  SU_I(client).terminal_write_failed = false;
  if (outcome == AZ_IOT_SU_OUTCOME_SUCCEEDED)
  {
    adopt_applied_update_id(client);
  }
}

/* Reset the workflow back to Idle, clearing the parsed request. */
static void reset_to_idle(az_iot_su_client* client)
{
  sync_checkpoint(client);
  set_su_state(client, AZ_IOT_SU_STATE_IDLE);
  SU_I(client).have_request = false;
  SU_I(client).current_step = 0;
  SU_I(client).current_file = 0;
  SU_I(client).checkpoint_pending = false;
  memset(&SU_I(client).current_request, 0, sizeof(SU_I(client).current_request));
  memset(&SU_I(client).current_manifest, 0, sizeof(SU_I(client).current_manifest));
  SU_I(client).request_len = 0;
}

/**
 * @brief Unescape the request's `updateManifest` in place into manifest_text.
 *
 * Only removes the JSON string escaping: the signature covers the unescaped
 * bytes. The manifest is not interpreted until verify_manifest() accepts it.
 *
 * @param client The client.
 * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG when empty or undecodable.
 */
static az_iot_result decode_manifest(az_iot_su_client* client)
{
  az_span manifest = SU_I(client).current_request.update_manifest;
  az_span unescaped;
  if (az_span_size(manifest) <= 0
      || az_iot_json_string_decode(manifest, manifest, &unescaped) != AZ_IOT_OK
      || az_span_size(unescaped) <= 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  SU_I(client).current_request.update_manifest = unescaped;
  SU_I(client).manifest_text = unescaped;
  return AZ_IOT_OK;
}

/**
 * @brief Parse the verified manifest_text into current_manifest.
 *
 * current_manifest holds spans into manifest_text (client-owned request_buffer).
 *
 * @param client The client; decode_manifest() and verify_manifest() succeeded.
 * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG when malformed.
 */
static az_iot_result parse_manifest(az_iot_su_client* client)
{
  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, SU_I(client).manifest_text, NULL))
      || az_result_failed(az_iot_adu_client_parse_update_manifest(
          &SU_I(client).az, &jr, &SU_I(client).current_manifest)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
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

/**
 * @brief Check that @p s decodes to a nonempty string; @p scratch receives it.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE when it does not fit @p scratch;
 *   AZ_IOT_ERR_INVALID_ARG when it is undecodable or empty.
 */
static az_iot_result decode_nonempty(az_span s, az_span scratch)
{
  az_span decoded;
  az_iot_result r = az_iot_json_string_decode(s, scratch, &decoded);
  if (r == AZ_IOT_OK && az_span_size(decoded) <= 0)
  {
    r = AZ_IOT_ERR_INVALID_ARG;
  }
  return r;
}

/**
 * @brief Check that decode_request_strings() will accept @p req, without
 * changing it. Lets a bad payload be refused before it disturbs the active
 * workflow or its checkpoint.
 *
 * @param client The client; its workspace scratch is used as decode scratch.
 * @param req    Parsed request.
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE when a value does not fit the
 *   scratch; AZ_IOT_ERR_INVALID_ARG on an undecodable or empty value.
 */
static az_iot_result check_request_strings(
    az_iot_su_client* client,
    const az_iot_su_client_update_request* req)
{
  az_span scratch = az_span_create(SU_I(client).scratch, (int32_t)SU_I(client).scratch_size);
  az_iot_result r = decode_nonempty(req->workflow.id, scratch);
  for (uint32_t i = 0; r == AZ_IOT_OK && i < req->file_urls_count; ++i)
  {
    r = decode_nonempty(req->file_urls[i].id, scratch);
    if (r == AZ_IOT_OK)
    {
      r = decode_nonempty(req->file_urls[i].url, scratch);
    }
  }
  return r;
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
        AZ_IOT_LOG_COMPONENT_SU,
        "workflowId of %d bytes does not fit AZ_IOT_SU_WORKFLOW_ID_SIZE (%u); not reported",
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
 * @brief Whether a new workflow id may replace the current workflow.
 *
 * Not once install_fn has been called for any step, until the workflow ends,
 * nor while a reboot asked for by the last apply is owed: the device then
 * holds a partly applied or not yet activated deployment. This covers a
 * workflow held at a reboot boundary.
 */
static bool workflow_replaceable(const az_iot_su_client* client)
{
  if (SU_I(client).reboot_owed)
  {
    return false;
  }
  switch (SU_I(client).state)
  {
    case AZ_IOT_SU_STATE_IDLE:
    case AZ_IOT_SU_STATE_FAILED:
      return true;
    case AZ_IOT_SU_STATE_INSTALL_COMPLETE:
    case AZ_IOT_SU_STATE_APPLY_STARTED:
    case AZ_IOT_SU_STATE_RESTORE_STARTED:
      return false;
    case AZ_IOT_SU_STATE_MANIFEST_RECEIVED:
    case AZ_IOT_SU_STATE_VERIFYING_MANIFEST:
    case AZ_IOT_SU_STATE_DOWNLOAD_STARTED:
    case AZ_IOT_SU_STATE_DOWNLOAD_COMPLETE:
    case AZ_IOT_SU_STATE_BACKUP_STARTED:
    case AZ_IOT_SU_STATE_BACKUP_COMPLETE:
    case AZ_IOT_SU_STATE_INSTALL_STARTED:
    default:
      return SU_I(client).current_step == 0 && !SU_I(client).install_called;
  }
}

/**
 * @brief Stage and start the deployment in an `updateMetadata` payload.
 *
 * The payload is valid only for this call, and the parsed spans must outlive
 * it, so it is copied into request_buffer and parsed from there.
 *
 * `workflowId` is the sole deployment identity: a payload with the active id
 * is a redelivery and is ignored before request_buffer is touched, so the
 * running workflow is undisturbed. The exception is a FAILED last
 * workflow: once its retry wait has elapsed and its report is settled, the
 * offer starts a new attempt under the same id, keeping the failure count.
 * A new id restarts the workflow while workflow_replaceable() allows it;
 * otherwise it is deferred (AZ_IOT_SU_EVENT_UPDATE_DEFERRED) and nothing is
 * touched.
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
  if (patch_len > SU_I(client).request_buffer_size)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "update payload of %u bytes exceeds az_iot_su_limits::max_update_size (%u); ignored",
        (unsigned)patch_len,
        (unsigned)SU_I(client).request_buffer_size);
    raise_refused(client, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    return;
  }

  /* The body carries service-issued download URLs (fileUrls): TRACE only. */
  AZ_IOT_LOG_DEBUGF(
      AZ_IOT_LOG_COMPONENT_SU, "update payload received (%u bytes)", (unsigned)patch_len);
  AZ_IOT_LOG_TRACEF(
      AZ_IOT_LOG_COMPONENT_SU, "update payload: %.*s", (int)patch_len, (const char*)patch);

  /* Probe the transient buffer for the workflow identity. These spans are only
   * valid for the duration of this call, which is enough to decide what to do. */
  az_span transient = az_span_create((uint8_t*)(uintptr_t)patch, (int32_t)patch_len);
  az_iot_su_client_update_request probe;
  az_iot_result pr = parse_update_metadata(transient, &probe);
  if (pr != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU, "update payload not understood (%d); ignored", (int)pr);
    raise_refused(client, AZ_IOT_ERR_PROTOCOL);
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
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_SU, "update payload has an undecodable workflowId; ignored");
    raise_refused(client, AZ_IOT_ERR_PROTOCOL);
    return;
  }
  if (dr == AZ_IOT_ERR_NOT_ENOUGH_SPACE)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "decoded workflowId exceeds AZ_IOT_SU_WORKFLOW_ID_SIZE (%u; %d bytes encoded); "
        "update refused",
        (unsigned)sizeof(id_scratch),
        (int)az_span_size(probe.workflow.id));
    raise_refused(client, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    return;
  }
  bool retry = dr == AZ_IOT_OK && same_workflow_id(client, probe_id);
  if (retry && !workflow_retryable(client))
  {
    AZ_IOT_LOG_DEBUG(AZ_IOT_LOG_COMPONENT_SU, "update redelivers the current workflowId; ignored");
    raise_simple(client, AZ_IOT_SU_EVENT_UPDATE_DUPLICATE, AZ_IOT_SU_OP_GET_UPDATE, false, NULL);
    return;
  }
  if (retry)
  {
    uint32_t wait_ms = az_iot_retry_state__remaining_ms(&SU_I(client).workflow_retry);
    if (wait_ms > 0)
    {
      AZ_IOT_LOG_DEBUGF(
          AZ_IOT_LOG_COMPONENT_SU,
          "failed workflow offered again; retry waits %u ms more",
          (unsigned)wait_ms);
      raise_retry_wait(client, wait_ms);
      return;
    }
    if (SU_I(client).report_owed)
    {
      AZ_IOT_LOG_WARN(
          AZ_IOT_LOG_COMPONENT_SU,
          "failed workflow retry deferred: its terminal report is not yet settled");
      /* A session lost for good leaves the report owed with no send queued. */
      if (!SU_I(client).terminal_report_in_flight && !SU_I(client).device_properties_report_pending)
      {
        SU_I(client).device_properties_report_pending = true;
      }
      raise_deferred(client);
      return;
    }
  }

  az_iot_result sr = check_request_strings(client, &probe);
  if (sr == AZ_IOT_ERR_NOT_ENOUGH_SPACE)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "update payload has a string over az_iot_su_limits::scratch_size (%u); ignored",
        (unsigned)SU_I(client).scratch_size);
    raise_refused(client, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    return;
  }
  if (sr != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "update payload has an undecodable string; ignored");
    raise_refused(client, AZ_IOT_ERR_PROTOCOL);
    return;
  }

  if (!workflow_replaceable(client))
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "new workflow deferred: the current one has changed the device (state %d, step %u)",
        (int)SU_I(client).state,
        (unsigned)SU_I(client).current_step);
    raise_deferred(client);
    return;
  }

  /* A new workflow supersedes whatever the stored checkpoint belongs to,
   * including an unsent terminal report of the previous one. A retry keeps
   * the retry record until its own progress replaces it. */
  if (!retry)
  {
    SU_I(client).report_owed = false;
    SU_I(client).terminal_report_in_flight = false;
    clear_checkpoint(client);
    SU_I(client).checkpoint_superseded = SU_I(client).checkpoint_stored;
    az_iot_retry_state__reset(&SU_I(client).workflow_retry);
  }

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
  SU_I(client).checkpoint_pending = false;
  SU_I(client).install_called = false;
  SU_I(client).current_step = 0;
  SU_I(client).current_file = 0;
  result_init_steps(client, 0);
  set_su_state(client, AZ_IOT_SU_STATE_MANIFEST_RECEIVED);
  set_active_workflow(
      client,
      req.workflow.id,
      req.workflow.retry_timestamp,
      manifest_fingerprint(req.update_manifest));
  SU_I(client).active_workflow_running = true;
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
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_SU, "ignoring an update that arrived after its check's deadline");
    return;
  }
  SU_I(client).fetch_delivered = true;
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

/** @brief Raise AZ_IOT_SU_EVENT_WORKFLOW_COMPLETED for a settled terminal report. */
static void raise_completed(
    az_iot_su_client* client,
    az_iot_result result,
    const az_iot_su_service_error* service_error,
    const az_iot_su_report* report)
{
  az_iot_su_event event = {
    .kind = AZ_IOT_SU_EVENT_WORKFLOW_COMPLETED,
    .state = SU_I(client).state,
    .previous_state = SU_I(client).state,
    .operation = AZ_IOT_SU_OP_REPORT_STATUS,
    .reason = result,
    .service_error = k_no_service_error,
    .report_terminal = true,
    .report = report,
  };
  if (result != AZ_IOT_OK && service_error != NULL)
  {
    event.service_error = *service_error;
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

/** @brief Raise AZ_IOT_SU_EVENT_UPDATE_DEFERRED for a new workflow not started. */
static void raise_deferred(az_iot_su_client* client)
{
  az_iot_su_event event = {
    .kind = AZ_IOT_SU_EVENT_UPDATE_DEFERRED,
    .state = SU_I(client).state,
    .previous_state = SU_I(client).state,
    .reason = AZ_IOT_ERR_BUSY,
    .service_error = k_no_service_error,
  };
  dispatch_event(client, &event);
}

/** @brief Raise AZ_IOT_SU_EVENT_UPDATE_RETRY_WAIT for a failed workflow offered too early. */
static void raise_retry_wait(az_iot_su_client* client, uint32_t retry_after_ms)
{
  az_iot_su_event event = {
    .kind = AZ_IOT_SU_EVENT_UPDATE_RETRY_WAIT,
    .state = SU_I(client).state,
    .previous_state = SU_I(client).state,
    .reason = AZ_IOT_OK,
    .service_error = k_no_service_error,
    .retry_after_ms = retry_after_ms,
  };
  dispatch_event(client, &event);
}

/**
 * @brief Raise NO_UPDATE, UPDATE_DUPLICATE or REPORT_ACCEPTED with the current `state`.
 *
 * @param client          The client.
 * @param kind            AZ_IOT_SU_EVENT_NO_UPDATE, _UPDATE_DUPLICATE or _REPORT_ACCEPTED.
 * @param operation       The operation the event refers to.
 * @param report_terminal REPORT_ACCEPTED: the accepted report was terminal.
 * @param report          REPORT_ACCEPTED with report_terminal: that report; else NULL.
 */
static void raise_simple(
    az_iot_su_client* client,
    az_iot_su_event_kind kind,
    az_iot_su_operation operation,
    bool report_terminal,
    const az_iot_su_report* report)
{
  az_iot_su_event event = {
    .kind = kind,
    .state = SU_I(client).state,
    .previous_state = SU_I(client).state,
    .operation = operation,
    .reason = AZ_IOT_OK,
    .service_error = k_no_service_error,
    .report_terminal = report_terminal,
    .report = report,
  };
  dispatch_event(client, &event);
}

/**
 * @brief The extended result code that explains why the workflow failed.
 *
 * The first failed step's code, else the overall one: the overall code may
 * have been replaced by a later rollback failure.
 */
static int32_t failure_extended_result(const az_iot_su_client* client)
{
  for (int32_t i = 0; i < SU_I(client).step_results_count; ++i)
  {
    if (SU_I(client).step_results[i].outcome == AZ_IOT_SU_OUTCOME_FAILED
        && SU_I(client).step_results[i].extended_result_code != 0)
    {
      return SU_I(client).step_results[i].extended_result_code;
    }
  }
  return SU_I(client).install_result.extended_result_code;
}

/** @brief Map an extended result code to the FAILED event's `reason`. */
static az_iot_result failure_reason(int32_t extended_result_code)
{
  uint32_t code = (uint32_t)extended_result_code;
  switch ((code >> 28) & 0xFu)
  {
    case AZ_IOT_SU_FACILITY_MANIFEST:
      return ((code & 0x0FFFFFFFu) == AZ_IOT_SU_MANIFEST_ERROR_CRYPTO) ? AZ_IOT_ERR_INTERNAL
                                                                       : AZ_IOT_ERR_AUTH;
    case AZ_IOT_SU_FACILITY_HASH:
      return AZ_IOT_ERR_AUTH;
    case AZ_IOT_SU_FACILITY_INTERNAL:
      return AZ_IOT_ERR_PROTOCOL;
    default:
      return AZ_IOT_ERR_INTERNAL;
  }
}

/* Move the workflow, telling anyone watching. Centralised so every transition
 * is reported: eighteen assignment sites cannot each be trusted to remember,
 * and a state change nobody hears about is the gap this closes. Callers record
 * the failure before entering FAILED, so the event can say why. */
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
  if (next == AZ_IOT_SU_STATE_FAILED)
  {
    event.extended_result_code = failure_extended_result(client);
    event.reason = failure_reason(event.extended_result_code);
  }
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
  seed_retry_rng(client);
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
 * PROCEED is not re-armed either. It is a terminal answer, not a failure: the
 * service is telling the device there is no update service configured for it,
 * and the classifier defines it as "carry on, do not retry". Re-arming it would
 * also fire a fresh check just as the hold is released and registration goes
 * out -- onto a session that is about to be torn down -- which is the race the
 * hold exists to prevent. */
/**
 * @brief Queue an update check after the service rejected a report as conflicting.
 *
 * The service's state differs from the device's, and checking again is how the
 * device learns it. Uses the route the application last asked for (regular if
 * none) and AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS. No-op while a check is
 * queued or in flight: its answer serves the same purpose.
 *
 * @param client The software updates client.
 */
static void queue_conflict_recheck(az_iot_su_client* client)
{
  if (SU_I(client).pending_fetch != SU_FETCH_NONE || SU_I(client).fetch_in_flight != SU_FETCH_NONE)
  {
    return;
  }
  AZ_IOT_LOG_INFO(
      AZ_IOT_LOG_COMPONENT_SU, "report conflicts with the service's state; checking for updates");
  SU_I(client).pending_fetch = (SU_I(client).last_requested_fetch == SU_FETCH_ONBOARDING)
      ? SU_FETCH_ONBOARDING
      : SU_FETCH_REGULAR;
  SU_I(client).pending_fetch_paced = false;
  arm_pending_fetch_deadline(client, AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS);
}

static az_iot_su_operation fetch_operation(uint8_t fetch);

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
      || action == AZ_IOT_SU_ERROR_ACTION_PROCEED || action == AZ_IOT_SU_ERROR_ACTION_RECHECK
      || action == AZ_IOT_SU_ERROR_ACTION_NONE)
  {
    if (result == AZ_IOT_OK)
    {
      az_iot_retry_state__reset(&SU_I(client).retry);
    }
    /* This branch IS the definition of "the client will not re-arm it", so it
     * is also where the application is told. Deriving the two from one
     * condition is the point: a separate list elsewhere would be free to drift.
     *
     * AZ_IOT_OK (with NONE) is success. FATAL and PROCEED are requests that
     * were dropped, and both are reported. PROCEED in particular is an update
     * account the service cannot use: the device asked, was refused, and
     * without this reads exactly like "no update available". */
    /* A check held for an owed terminal report whose session is lost for good
     * goes with it; the report stays owed, and only a new check sends it
     * again. Taken before observers run, so a check they queue stays. */
    uint8_t held_check = SU_FETCH_NONE;
    az_iot_result held_reason = AZ_IOT_ERR_NOT_CONNECTED;
    if (operation == AZ_IOT_SU_OP_REPORT_STATUS && result == AZ_IOT_ERR_NOT_CONNECTED
        && SU_I(client).report_sent_terminal)
    {
      held_check = SU_I(client).pending_fetch;
      SU_I(client).pending_fetch = SU_FETCH_NONE;
      if (held_check != SU_FETCH_NONE)
      {
        /* One already past its deadline is reported as the timeout it is. */
        if (SU_I(client).pending_fetch_deadline_ms != 0
            && az_iot_time_mono_ms() >= SU_I(client).pending_fetch_deadline_ms)
        {
          held_reason = AZ_IOT_ERR_TIMEOUT;
        }
        SU_I(client).pending_fetch_deadline_ms = 0;
      }
    }
    if (result != AZ_IOT_OK
        && (action == AZ_IOT_SU_ERROR_ACTION_FATAL || action == AZ_IOT_SU_ERROR_ACTION_PROCEED
            || action == AZ_IOT_SU_ERROR_ACTION_RECHECK))
    {
      raise_abandoned(client, operation, result, service_error);
    }
    if (operation == AZ_IOT_SU_OP_REPORT_STATUS)
    {
      bool terminal = SU_I(client).report_sent_terminal;
      /* The report as sent: the engine state may have moved on since. */
      az_iot_su_report sent;
      az_iot_su_report_update_id sent_installed;
      bool have_view = terminal && az_iot_su__sent_terminal_report(client, &sent, &sent_installed);
      /* Settled unless kept for a later send (see settle_terminal_report()). */
      bool settled = terminal && SU_I(client).terminal_report_in_flight
          && result != AZ_IOT_ERR_NOT_CONNECTED;
      SU_I(client).sent_terminal[SU_I(client).sent_terminal_slot].valid = false;
      settle_terminal_report(client, result);
      if (result == AZ_IOT_OK)
      {
        raise_simple(
            client, AZ_IOT_SU_EVENT_REPORT_ACCEPTED, operation, terminal, have_view ? &sent : NULL);
      }
      if (settled)
      {
        raise_completed(client, result, service_error, have_view ? &sent : NULL);
      }
      if (result != AZ_IOT_OK && action == AZ_IOT_SU_ERROR_ACTION_RECHECK)
      {
        queue_conflict_recheck(client);
      }
      if (held_check != SU_FETCH_NONE)
      {
        raise_abandoned(client, fetch_operation(held_check), held_reason, &k_no_service_error);
      }
      return;
    }
    /* The request is over -- but only ITS deadline goes with it. A newer
     * request may already be queued, holding the slot with its own deadline;
     * clearing unconditionally would strip that and leave the newer request
     * retrying for ever. */
    if (SU_I(client).pending_fetch == SU_FETCH_NONE)
    {
      SU_I(client).pending_fetch_deadline_ms = 0;
    }
    if (result == AZ_IOT_OK && !SU_I(client).fetch_delivered)
    {
      raise_simple(client, AZ_IOT_SU_EVENT_NO_UPDATE, operation, false, NULL);
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
            AZ_IOT_LOG_COMPONENT_SU,
            "service asked for %u ms, which does not fit the request timeout; giving up",
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
  AZ_IOT_LOG_ERROR(
      AZ_IOT_LOG_COMPONENT_SU, "giving up on an update check; no answer before its deadline");
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
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_SU, "giving up on a pending update check; its deadline expired");
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
  /* An owed terminal report goes first: until the service has the outcome, a
   * check advertises the previous state and may be offered the same update
   * again. Queued here if nothing will send it (e.g. its session was lost). */
  if (SU_I(client).report_owed || SU_I(client).terminal_report_in_flight)
  {
    if (!SU_I(client).terminal_report_in_flight && !SU_I(client).device_properties_report_pending)
    {
      SU_I(client).device_properties_report_pending = true;
    }
    return;
  }

  SU_I(client).pending_fetch = SU_FETCH_NONE;
  /* Set before the call: a synchronous channel may deliver the verdict from
   * inside it, which clears this again. */
  uint8_t previous_in_flight = SU_I(client).fetch_in_flight;
  bool previous_delivered = SU_I(client).fetch_delivered;
  SU_I(client).fetch_in_flight = requested;
  SU_I(client).fetch_delivered = false;
  az_iot_result r = channel_request_update(client, operation);
  if (r != AZ_IOT_OK)
  {
    /* Refused, so not sent; whatever was in flight before still is. */
    if (SU_I(client).fetch_in_flight == requested)
    {
      SU_I(client).fetch_in_flight = previous_in_flight;
      SU_I(client).fetch_delivered = previous_delivered;
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

AZ_NODISCARD az_iot_su_limits az_iot_su_limits_default(void)
{
  az_iot_su_limits limits;
  limits.max_update_size = AZ_IOT_SU_DEFAULT_MAX_UPDATE_SIZE;
  limits.scratch_size = AZ_IOT_SU_VERIFY_SCRATCH_SIZE;
  limits.max_message_size = AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE;
  limits.device_properties_size = AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE;
  limits.result_details_size = AZ_IOT_SU_DEFAULT_RESULT_DETAILS_SIZE;
  return limits;
}

/** @brief Smallest max_update_size and scratch_size accepted. */
#define AZ_IOT_SU_MIN_LIMIT 256u

AZ_NODISCARD size_t az_iot_su_workspace_size(const az_iot_su_limits* limits)
{
  if (limits == NULL || limits->max_update_size < AZ_IOT_SU_MIN_LIMIT
      || limits->scratch_size < AZ_IOT_SU_MIN_LIMIT
      || limits->max_message_size < AZ_IOT_SU_MIN_MESSAGE_SIZE
      || limits->device_properties_size == 0
      || limits->result_details_size > AZ_IOT_SU_MAX_RESULT_DETAILS_SIZE)
  {
    return 0;
  }
  /* Persisted lengths are 32-bit, and each part is used as an az_span. */
  const size_t part_max = (size_t)INT32_MAX - (size_t)AZ_IOT_SU_PERSIST_OVERHEAD;
  if (limits->max_update_size > part_max || limits->scratch_size > part_max
      || limits->max_message_size > part_max || limits->device_properties_size > part_max)
  {
    return 0;
  }
  size_t details = (size_t)_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS * limits->result_details_size;
  size_t parts[7] = { limits->max_update_size + (size_t)AZ_IOT_SU_PERSIST_OVERHEAD + details,
                      (size_t)AZ_IOT_SU_PERSIST_OVERHEAD + details,
                      limits->scratch_size,
                      limits->max_message_size,
                      limits->device_properties_size,
                      details,
                      2u * details };
  size_t total = 0;
  for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i)
  {
    if (parts[i] > SIZE_MAX - total)
    {
      return 0;
    }
    total += parts[i];
  }
  return total;
}

AZ_NODISCARD az_iot_su_client_config_options az_iot_su_client_config_options_default(void)
{
  az_iot_su_client_config_options opts = { 0 };
  opts.limits = az_iot_su_limits_default();
  return opts;
}

/** @brief Where each workspace part starts. */
typedef struct
{
  uint8_t* persist_blob; /**< Workflow record: header, request, trailer. */
  uint8_t* terminal_blob;
  uint8_t* scratch;
  uint8_t* body;
  uint8_t* device_properties;
  uint8_t* step_details;
  uint8_t* sent_details;
} su_workspace_layout;

/**
 * @brief Lay out @p options' workspace.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG on a NULL workspace or a limit out of
 *   range; AZ_IOT_ERR_NOT_ENOUGH_SPACE when the workspace is too small.
 */
static az_iot_result su_workspace_lay_out(
    const az_iot_su_client_config_options* options,
    su_workspace_layout* out)
{
  size_t need = az_iot_su_workspace_size(&options->limits);
  if (options->workspace == NULL || need == 0)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "workspace is NULL or a limit is out of range");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (options->workspace_size < need)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "workspace of %u bytes is smaller than az_iot_su_workspace_size() (%u)",
        (unsigned)options->workspace_size,
        (unsigned)need);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  size_t details
      = (size_t)_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS * options->limits.result_details_size;
  uint8_t* p = options->workspace;
  out->persist_blob = p;
  p += options->limits.max_update_size + (size_t)AZ_IOT_SU_PERSIST_OVERHEAD + details;
  out->terminal_blob = p;
  p += (size_t)AZ_IOT_SU_PERSIST_OVERHEAD + details;
  out->scratch = p;
  p += options->limits.scratch_size;
  out->body = p;
  p += options->limits.max_message_size;
  out->device_properties = p;
  p += options->limits.device_properties_size;
  out->step_details = p;
  p += details;
  out->sent_details = p;
  return AZ_IOT_OK;
}

/* Core initialization. Assumes the caller has ALREADY zeroed the client: the
 * public path builds its channel into storage inside that client, so a memset
 * here would wipe it. */
static az_iot_result su_client_init_core(
    az_iot_su_client* client,
    const az_iot_su_channel* channel,
    const az_iot_crypto* crypto,
    const az_iot_su_client_config_options* options)
{
  if (client == NULL || channel == NULL || channel->vtable == NULL || options == NULL
      || options->hooks == NULL || options->device_properties == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  su_workspace_layout layout;
  az_iot_result lr = su_workspace_lay_out(options, &layout);
  if (lr != AZ_IOT_OK)
  {
    return lr;
  }
  if (az_iot_crypto__validate(crypto) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_SU, "init needs a crypto backend (connection client options.crypto)");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (crypto->verify_rs256 == NULL)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "crypto backend has no verify_rs256");
    return AZ_IOT_ERR_NOT_SUPPORTED;
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
  if (options->replacement_policy == AZ_IOT_SU_REPLACEMENT_ROLLBACK)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "AZ_IOT_SU_REPLACEMENT_ROLLBACK is not supported");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (options->replacement_policy != AZ_IOT_SU_REPLACEMENT_DEFER)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  SU_I(client).channel.vtable = channel->vtable;
  SU_I(client).channel.ctx = channel->ctx;
  SU_I(client).hooks = *options->hooks;
  SU_I(client).crypto = crypto;
  SU_I(client).persist_max_attempts = (uint32_t)AZ_IOT_SU_PERSIST_MAX_ATTEMPTS;
  SU_I(client).persist_blob = layout.persist_blob;
  SU_I(client).persist_blob_size = AZ_IOT_SU_STATE_BLOB_MAX_SIZE(
      options->limits.max_update_size, options->limits.result_details_size);
  SU_I(client).request_buffer = layout.persist_blob + AZ_IOT_SU_PERSIST_HEADER_SIZE;
  SU_I(client).request_buffer_size = options->limits.max_update_size;
  SU_I(client).terminal_blob = layout.terminal_blob;
  SU_I(client).step_details = layout.step_details;
  SU_I(client).sent_details = layout.sent_details;
  SU_I(client).step_details_size = options->limits.result_details_size;
  SU_I(client).scratch = layout.scratch;
  SU_I(client).scratch_size = options->limits.scratch_size;
  SU_I(client).device_properties_buffer = layout.device_properties;
  SU_I(client).device_properties_buffer_size = options->limits.device_properties_size;
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
    const az_iot_crypto* crypto,
    const az_iot_su_client_config_options* options)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(client, 0, sizeof(*client));
  return su_client_init_core(client, channel, crypto, options);
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

  su_workspace_layout layout;
  az_iot_result r = su_workspace_lay_out(options, &layout);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  az_iot_su_channel channel;
  az_iot_su_channel_dps* channel_state
      = (az_iot_su_channel_dps*)(void*)&SU_I(client).channel_storage;

  r = az_iot_su_channel_dps_init(
      channel_state,
      connection,
      options->device_properties,
      layout.body,
      options->limits.max_message_size,
      &channel);
  if (r != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return r;
  }

  r = su_client_init_core(client, &channel, connection->opts.crypto, options);
  if (r != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return r;
  }
  SU_I(client).unix_time_fn = connection->opts.unix_time.get_time;
  SU_I(client).unix_time_ctx = connection->opts.unix_time.user_ctx;
  return AZ_IOT_OK;
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
 * root public keys — is defined in su_root_keys_microsoft.c. Kept in a separate
 * translation unit so the large key blobs live apart from the state machine. */

/* ------------------------------------------------------------------------- */
/* persistence & resume (Phase 5)                                            */
/* ------------------------------------------------------------------------- */

/* Versioned, integrity-checked workflow snapshot. Layout (all little-endian):
 *   [0]  magic[4]   = 'S','U','C','P'
 *   [4]  u16 version (= AZ_IOT_SU_PERSIST_VERSION)
 *   [6]  u16 flags   (bit0 reserved, 0; bit1 have_request, bit2 report_owed,
 *                     bit3 retry record)
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
 *          step_results_count * { u32 len + step result details }
 *          retry schedule { u32 consecutive FAILED attempts,
 *                           u32 seconds left of the wait when written,
 *                           u32 lo, u32 hi of its Unix deadline (0 = unknown) }
 *   [end] u32 crc32 (over bytes [0 .. end))
 *
 * A record with report_owed set is a terminal record: state Idle, no request.
 * It carries only the unsent terminal report, so resume re-sends it and runs
 * nothing. A retry record (bit3) is laid out like a terminal record of a
 * FAILED workflow whose report is already settled: nothing is
 * owed, and it is kept so the retry schedule survives a restart. A record of
 * any other magic or version is ignored. */
#define AZ_IOT_SU_PERSIST_MAGIC0 'S'
#define AZ_IOT_SU_PERSIST_MAGIC1 'U'
#define AZ_IOT_SU_PERSIST_MAGIC2 'C'
#define AZ_IOT_SU_PERSIST_MAGIC3 'P'
#define AZ_IOT_SU_PERSIST_VERSION 3u
#define AZ_IOT_SU_PERSIST_FLAG_HAVE_REQUEST 0x2u
#define AZ_IOT_SU_PERSIST_FLAG_REPORT_OWED 0x4u
#define AZ_IOT_SU_PERSIST_FLAG_RETRY 0x8u
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
   + 4u + (uint32_t)(AZ_IOT_SU_CHANNEL_STATE_MAX_SIZE)                                            \
   + 4u * (uint32_t)(_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS) + AZ_IOT_RETRY_STATE_RECORD_SIZE  \
   + 4u)

/* The workspace reserves AZ_IOT_SU_PERSIST_OVERHEAD beside the request for the
 * workflow record, and as much again for the terminal record: header plus the
 * maximum trailer must fit it. C99-portable compile-time check (negative array
 * size on failure). */
typedef char az_iot_su_persist_overhead_fits
    [(AZ_IOT_SU_PERSIST_HEADER_SIZE + AZ_IOT_SU_PERSIST_TRAILER_MAX <= AZ_IOT_SU_PERSIST_OVERHEAD)
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
 * @param kind The record to write; every kind carries the retry schedule.
 * @return AZ_IOT_OK when written; AZ_IOT_ERR_NOT_SUPPORTED when no
 * persist_state_fn is set; AZ_IOT_ERR_INVALID_ARG when the state cannot be
 * represented; AZ_IOT_ERR_INTERNAL when the hook fails.
 */
static az_iot_result su_persist(az_iot_su_client* client, su_record kind)
{
  az_iot_su_platform_hooks* h = &SU_I(client).hooks;
  if (h->persist_state_fn == NULL)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  bool terminal = kind != SU_RECORD_POSITION;

  const az_iot_su_client_update_request* req = &SU_I(client).current_request;
  uint32_t request_len = terminal ? 0u : (uint32_t)SU_I(client).request_len;
  uint32_t url_count = terminal ? 0u : req->file_urls_count;
  if (request_len > SU_I(client).request_buffer_size
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

  /* A workflow record is written in place: its header sits just before the
   * request it carries, its trailer just after. A terminal record carries no
   * request and is built apart, so a newer request is never overwritten. */
  uint8_t* blob = terminal ? SU_I(client).terminal_blob : SU_I(client).persist_blob;
  uint16_t flags = 0;
  if (terminal)
  {
    flags = (kind == SU_RECORD_RETRY) ? AZ_IOT_SU_PERSIST_FLAG_RETRY
                                      : AZ_IOT_SU_PERSIST_FLAG_REPORT_OWED;
  }
  else
  {
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

  for (int32_t i = 0; i < step_count; ++i)
  {
    az_span d = SU_I(client).step_results[i].result_details;
    uint32_t d_len = (uint32_t)az_span_size(d);
    wr_u32le(&blob[p], d_len);
    if (d_len > 0)
    {
      memcpy(&blob[p + 4u], az_span_ptr(d), d_len);
    }
    p += 4u + d_len;
  }

  az_iot_retry_state__save(&SU_I(client).workflow_retry, &blob[p]);
  p += AZ_IOT_RETRY_STATE_RECORD_SIZE;

  wr_u32le(&blob[p], su_crc32(blob, p));
  if (!persist_write(client, blob, (size_t)p + 4u))
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  SU_I(client).checkpoint_stored = true;
  SU_I(client).checkpoint_terminal = (kind == SU_RECORD_TERMINAL);
  SU_I(client).checkpoint_retry = (kind == SU_RECORD_RETRY);
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

/** @brief Copy each step's persisted details into its workspace slot. */
static void restore_step_details(
    az_iot_su_client* client,
    const uint8_t* blob,
    const uint32_t* at,
    const uint32_t* len,
    int32_t step_count)
{
  for (int32_t i = 0; i < step_count; ++i)
  {
    uint8_t* slot = SU_I(client).step_details + (size_t)i * SU_I(client).step_details_size;
    if (len[i] > 0)
    {
      memcpy(slot, &blob[at[i]], len[i]);
    }
    SU_I(client).step_results[i].result_details = az_span_create(slot, (int32_t)len[i]);
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

  /* The record is loaded in place, over the request buffer. */
  if (SU_I(client).have_request || SU_I(client).state != AZ_IOT_SU_STATE_IDLE)
  {
    return AZ_IOT_ERR_BUSY;
  }

  uint8_t* blob = SU_I(client).persist_blob;
  size_t blen = 0;
  if (SU_I(client).hooks.load_state_fn(
          blob, SU_I(client).persist_blob_size, &blen, SU_I(client).hooks.user_ctx)
      != 0)
  {
    return AZ_IOT_OK; /* nothing persisted */
  }
  if (blen > SU_I(client).persist_blob_size || blen < AZ_IOT_SU_PERSIST_HEADER_SIZE + 4u)
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
    return AZ_IOT_OK; /* includes version 1, which has no retry schedule */
  }

  uint32_t request_len = rd_u32le(&blob[36]);
  if (request_len > SU_I(client).request_buffer_size)
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
  uint32_t details_at[_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS] = { 0 };
  uint32_t details_len[_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS] = { 0 };
  for (int32_t i = 0; i < step_count; ++i)
  {
    if (!take_field(
            blob,
            blen,
            &end,
            (uint32_t)SU_I(client).step_details_size,
            &details_at[i],
            &details_len[i]))
    {
      return AZ_IOT_OK;
    }
  }
  uint32_t retry_at = end;
  end += AZ_IOT_RETRY_STATE_RECORD_SIZE;
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
  bool retry_record = (flags & AZ_IOT_SU_PERSIST_FLAG_RETRY) != 0;
  if (retry_record
      && ((flags & AZ_IOT_SU_PERSIST_FLAG_REPORT_OWED) != 0
          || outcome != (uint32_t)AZ_IOT_SU_OUTCOME_FAILED))
  {
    return AZ_IOT_OK;
  }
  if (retry_record || (flags & AZ_IOT_SU_PERSIST_FLAG_REPORT_OWED) != 0)
  {
    /* Terminal record: restore the unsent report and stay Idle. A retry
     * record restores the same, with nothing owed. */
    if (!is_terminal_outcome(outcome) || wf_id_len == 0
        || !restore_applied_update_id(client, &blob[applied_at], applied_len))
    {
      return AZ_IOT_OK;
    }
    memset(&SU_I(client).current_request, 0, sizeof(SU_I(client).current_request));
    memset(&SU_I(client).current_manifest, 0, sizeof(SU_I(client).current_manifest));
    SU_I(client).request_len = 0;
    SU_I(client).have_request = false;
    SU_I(client).current_step = 0;
    SU_I(client).current_file = 0;
    SU_I(client).checkpoint_pending = false;
    restore_step_results(client, &blob[steps_at], step_count, res_code, res_ext);
    restore_step_details(client, blob, details_at, details_len, step_count);
    set_active_workflow(client, az_span_create(&blob[wf_at], (int32_t)wf_id_len), AZ_SPAN_EMPTY, 0);
    SU_I(client).active_workflow_running = false;
    SU_I(client).pending_outcome = (az_iot_su_outcome)outcome;
    if (outcome == (uint32_t)AZ_IOT_SU_OUTCOME_SUCCEEDED)
    {
      adopt_applied_update_id(client);
    }
    workflow_retry_restore(client, &blob[retry_at]);
    SU_I(client).report_owed = !retry_record;
    SU_I(client).terminal_report_in_flight = false;
    SU_I(client).checkpoint_stored = true;
    SU_I(client).checkpoint_terminal = !retry_record;
    SU_I(client).checkpoint_retry = retry_record;
    SU_I(client).checkpoint_superseded = false;
    /* A retry record's report is settled: re-sending it could replay a rejected one. */
    SU_I(client).device_properties_report_pending = !retry_record;
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
  /* The request is already in place: blob + header is request_buffer. */
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
  SU_I(client).have_request = (flags & AZ_IOT_SU_PERSIST_FLAG_HAVE_REQUEST) != 0;
  SU_I(client).checkpoint_pending = false;
  SU_I(client).checkpoint_stored = true;
  SU_I(client).checkpoint_terminal = false;
  SU_I(client).checkpoint_retry = false;
  SU_I(client).checkpoint_superseded = false;

  /* Restore the accumulated result so completed step results of a multi-step
   * deployment survive a mid-deployment reboot. */
  restore_step_results(client, &blob[steps_at], step_count, res_code, res_ext);
  restore_step_details(client, blob, details_at, details_len, step_count);
  restore_channel_state(client, &blob[ch_at], ch_len);

  /* Re-establish the active workflow id so a redelivery after the reboot is
   * recognized as a duplicate and does NOT restart the resumed workflow. */
  set_active_workflow(client, req->workflow.id, AZ_SPAN_EMPTY, 0);
  SU_I(client).active_workflow_running = true;
  workflow_retry_restore(client, &blob[retry_at]);
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
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_SU, "no restore_fn; the installed step is not rolled back");
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
      && su_persist(client, SU_RECORD_POSITION) != AZ_IOT_ERR_INTERNAL)
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
   */
  if (SU_I(client).checkpoint_superseded
      || (SU_I(client).checkpoint_pending && SU_I(client).state != AZ_IOT_SU_STATE_IDLE
          && SU_I(client).state != AZ_IOT_SU_STATE_FAILED))
  {
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
      if (decode_manifest(client) != AZ_IOT_OK)
      {
        result_init_steps(client, 1);
        result_step_failure(client, 0, AZ_IOT_SU_FACILITY_INTERNAL, 0);
        fail_workflow(client);
        (void)az_iot_su__report_state(client);
        break;
      }
      set_su_state(client, AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
      (void)az_iot_su__report_state(client);
      break;
    }

    case AZ_IOT_SU_STATE_VERIFYING_MANIFEST:
    {
      /* The manifest is parsed only after its signature verifies. */
      uint32_t verify_error = 0;
      if (verify_manifest(client, &verify_error) != AZ_IOT_SU_RESULT_SUCCESS)
      {
        result_init_steps(client, 1);
        result_step_failure(client, 0, AZ_IOT_SU_FACILITY_MANIFEST, (int32_t)verify_error);
        fail_workflow(client);
        (void)az_iot_su__report_state(client);
        break;
      }
      if (parse_manifest(client) != AZ_IOT_OK)
      {
        result_init_steps(client, 1);
        result_step_failure(client, 0, AZ_IOT_SU_FACILITY_INTERNAL, 0);
        fail_workflow(client);
        (void)az_iot_su__report_state(client);
        break;
      }
      result_init_steps(client, (int32_t)SU_I(client).current_manifest.instructions.steps_count);
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
       * read-back hook). */
      if (h->read_file_fn != NULL)
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
      SU_I(client).install_called = true;
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
        SU_I(client).checkpoint_pending
            = (su_persist(client, SU_RECORD_POSITION) == AZ_IOT_ERR_INTERNAL);
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
          SU_I(client).checkpoint_pending
              = (su_persist(client, SU_RECORD_POSITION) == AZ_IOT_ERR_INTERNAL);
        }
        (void)az_iot_su__report_state(client);
        break;
      }
      /* The last step: the terminal record replaces any stored position. */
      result_overall_success(client);
      latch_terminal(client, AZ_IOT_SU_OUTCOME_SUCCEEDED);
      SU_I(client).reboot_owed = (ar == AZ_IOT_SU_RESULT_REBOOT_REQUIRED);
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

az_iot_result az_iot_su_client_set_step_result_details(
    az_iot_su_client* client,
    uint32_t step_index,
    az_span details)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (SU_I(client).detached)
  {
    return AZ_IOT_ERR_DETACHED;
  }
  /* A delivery empties the step results and verification fills them, so a
   * step exists from then (is_installed_fn included) until the workflow ends. */
  az_iot_su_state st = SU_I(client).state;
  if (st == AZ_IOT_SU_STATE_IDLE || st == AZ_IOT_SU_STATE_FAILED
      || step_index >= (uint32_t)_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS
      || (int32_t)step_index >= SU_I(client).step_results_count)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  int32_t len = az_span_size(details);
  if (len < 0 || (size_t)len > SU_I(client).step_details_size)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  /* Unchanged text writes nothing: a hook may set it on every call. */
  if (az_span_is_content_equal(details, SU_I(client).step_results[step_index].result_details))
  {
    return AZ_IOT_OK;
  }
  uint8_t* slot = SU_I(client).step_details + (size_t)step_index * SU_I(client).step_details_size;
  if (len > 0)
  {
    memmove(slot, az_span_ptr(details), (size_t)len);
  }
  SU_I(client).step_results[step_index].result_details = az_span_create(slot, len);
  /* Held at a stored resume point (an install awaiting its reboot, or the next
   * step after an apply that asked for one), no later write would carry the
   * change: rewrite the record on the next do_work(), before the workflow goes on. */
  if (SU_I(client).checkpoint_stored && !SU_I(client).checkpoint_terminal
      && !SU_I(client).checkpoint_retry && !SU_I(client).checkpoint_superseded
      && (st == AZ_IOT_SU_STATE_INSTALL_COMPLETE
          || (st == AZ_IOT_SU_STATE_DOWNLOAD_STARTED && SU_I(client).current_step > 0
              && SU_I(client).current_file == 0)))
  {
    SU_I(client).checkpoint_pending = true;
  }
  return AZ_IOT_OK;
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
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "cannot add an observer from inside one");
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
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "no free observer slot");
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
  SU_I(client).last_requested_fetch = SU_FETCH_ONBOARDING;
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
  SU_I(client).last_requested_fetch = SU_FETCH_REGULAR;
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

  az_iot_result r = store_device_properties(client, device_properties);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  /* A settled terminal report is not sent again: re-sending an accepted one
   * changes nothing, and a rejected one would be replayed. The next update
   * check carries the new properties. */
  bool running
      = SU_I(client).state != AZ_IOT_SU_STATE_IDLE && SU_I(client).state != AZ_IOT_SU_STATE_FAILED;
  if (running || SU_I(client).report_owed || SU_I(client).terminal_report_in_flight)
  {
    SU_I(client).device_properties_report_pending = true;
    SU_I(client).report_paced = false;
  }
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* agent core-library API (library mode / bring-your-own state machine)      */
/* ------------------------------------------------------------------------- */

AZ_NODISCARD az_iot_result az_iot_su_parse_update_request(
    az_span request_json,
    const az_iot_crypto* crypto,
    const az_iot_su_root_key* root_keys,
    size_t root_key_count,
    az_iot_su_client_update_request* out_request,
    az_iot_su_client_update_manifest* out_manifest)
{
  /* Fail-closed: outputs are zeroed first and stay so unless every stage succeeds. */
  if (out_request != NULL)
  {
    memset(out_request, 0, sizeof(*out_request));
  }
  if (out_manifest != NULL)
  {
    memset(out_manifest, 0, sizeof(*out_manifest));
  }
  if (az_iot_crypto__validate(crypto) != AZ_IOT_OK || out_request == NULL || out_manifest == NULL
      || az_span_size(request_json) <= 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (crypto->verify_rs256 == NULL)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

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
  uint8_t scratch[AZ_IOT_SU_VERIFY_SCRATCH_SIZE];
  uint32_t verify_error = 0;
  if (verify_manifest_core(
          crypto,
          root_keys,
          root_key_count,
          manifest_text,
          req.update_manifest_signature,
          scratch,
          sizeof(scratch),
          &verify_error)
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
    const az_iot_crypto* crypto,
    az_iot_su_read_chunk_callback read_chunk,
    void* read_ctx)
{
  if (file == NULL || az_iot_crypto__validate(crypto) != AZ_IOT_OK || read_chunk == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return (verify_file_hash_core(crypto, file, read_chunk, read_ctx) == AZ_IOT_SU_RESULT_SUCCESS)
      ? AZ_IOT_OK
      : AZ_IOT_ERR_AUTH;
}
