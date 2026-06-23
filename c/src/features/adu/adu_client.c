// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Azure Device Update (ADU) core state machine.
 *
 * Single-threaded, callback-driven. A desired-property patch carrying the
 * "deviceUpdate" component is parsed into current_request/current_manifest; the
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
#include <stdio.h>
#include <string.h>

#include <azure/core/az_base64.h>
#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_adu.h"
#include "azure/iot/az_iot_twin_client.h"

#include "internal/adu_internal.h"
#include "internal/twin_client_internal.h"

/* ------------------------------------------------------------------------- */
/* device-properties cache                                                   */
/* ------------------------------------------------------------------------- */
/*
 * Layout inside the caller-provided device_props_buffer:
 *   [ az_iot_adu_device_properties_t header ][ packed NUL-terminated strings ]
 * The header's pointers are rebased to point into the packed-string region, so
 * the cache is fully self-contained and the caller's original struct/strings
 * can be freed after the copy.
 *
 * Caches manufacturer, model, installed_update_id, and any custom properties
 * (packed as NUL-terminated strings, with an upstream-shaped az_span view built
 * over them for the agent-state formatter).
 */
static az_iot_result_t cache_device_properties(
    az_iot_adu_client_t* client,
    const az_iot_adu_device_properties_t* src)
{
    if (src == NULL) return AZ_IOT_ERR_INVALID_ARG;

    uint8_t* buf = ADU_I(client).device_props_buffer;
    size_t cap = ADU_I(client).device_props_buffer_size;
    if (buf == NULL || cap < sizeof(az_iot_adu_device_properties_t))
    {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }

    az_iot_adu_device_properties_t* hdr = (az_iot_adu_device_properties_t*)(void*)buf;
    char* strings = (char*)(buf + sizeof(*hdr));
    char* const strings_end = (char*)(buf + cap);

    memset(hdr, 0, sizeof(*hdr));
    hdr->custom_properties = NULL;
    hdr->custom_properties_count = 0;
    memset(&ADU_I(client).custom_props_view, 0, sizeof(ADU_I(client).custom_props_view));

    /* Copy a NUL-terminated string into the packed region, advancing the
     * cursor; returns the stored pointer (or NULL for a NULL source). */
    struct pack_ctx { char* cur; char* end; bool ok; } pc = { strings, strings_end, true };
    /* Inline helper via a small lambda-style macro to avoid a separate fn. */
    #define PACK_STR(dst, s)                                                   \
        do {                                                                   \
            if ((s) == NULL) { (dst) = NULL; break; }                          \
            size_t _len = strlen(s) + 1;                                       \
            if ((size_t)(pc.end - pc.cur) < _len) { pc.ok = false; break; }    \
            memcpy(pc.cur, (s), _len);                                         \
            (dst) = pc.cur;                                                    \
            pc.cur += _len;                                                    \
        } while (0)

    PACK_STR(hdr->manufacturer, src->manufacturer);
    PACK_STR(hdr->model, src->model);
    PACK_STR(hdr->installed_update_id.provider, src->installed_update_id.provider);
    PACK_STR(hdr->installed_update_id.name, src->installed_update_id.name);
    PACK_STR(hdr->installed_update_id.version, src->installed_update_id.version);

    /* Pack custom properties (clamped to the upstream array capacity) and build
     * the az_span view the formatter consumes. */
    if (pc.ok && src->custom_properties != NULL && src->custom_properties_count > 0)
    {
        const size_t max_cp = sizeof(ADU_I(client).custom_props_view.names)
            / sizeof(ADU_I(client).custom_props_view.names[0]);
        if (src->custom_properties_count > max_cp)
        {
            memset(hdr, 0, sizeof(*hdr));
            return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
        }
        for (size_t i = 0; i < src->custom_properties_count && pc.ok; ++i)
        {
            char* nm = NULL;
            char* vl = NULL;
            PACK_STR(nm, src->custom_properties[i].name);
            PACK_STR(vl, src->custom_properties[i].value);
            if (!pc.ok || nm == NULL || vl == NULL) { pc.ok = false; break; }
            ADU_I(client).custom_props_view.names[i] = az_span_create_from_str(nm);
            ADU_I(client).custom_props_view.values[i] = az_span_create_from_str(vl);
        }
        if (pc.ok)
        {
            ADU_I(client).custom_props_view.count = (int32_t)src->custom_properties_count;
        }
    }

    #undef PACK_STR

    if (!pc.ok)
    {
        memset(hdr, 0, sizeof(*hdr));
        memset(&ADU_I(client).custom_props_view, 0, sizeof(ADU_I(client).custom_props_view));
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }

    /* Build the serialized installed-update-id JSON object the ADU service
     * expects ({"provider":..,"name":..,"version":..}). Reported as the
     * device's installedUpdateId via the upstream agent-state payload. */
    {
        const char* prov = src->installed_update_id.provider ? src->installed_update_id.provider : "";
        const char* name = src->installed_update_id.name ? src->installed_update_id.name : "";
        const char* ver  = src->installed_update_id.version ? src->installed_update_id.version : "";
        int idn = snprintf(
            ADU_I(client).update_id_json, sizeof(ADU_I(client).update_id_json),
            "{\"provider\":\"%s\",\"name\":\"%s\",\"version\":\"%s\"}", prov, name, ver);
        if (idn < 0 || (size_t)idn >= sizeof(ADU_I(client).update_id_json))
        {
            memset(hdr, 0, sizeof(*hdr));
            return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
        }
        ADU_I(client).update_id_json_len = (size_t)idn;
    }

    return AZ_IOT_OK;
}

az_iot_result_t az_iot_adu__cache_device_properties(
    az_iot_adu_client_t* client,
    const az_iot_adu_device_properties_t* device_props)
{
    return cache_device_properties(client, device_props);
}

/* ------------------------------------------------------------------------- */
/* result-code accumulation                                                  */
/* ------------------------------------------------------------------------- */

static void result_init_steps(az_iot_adu_client_t* client, int32_t step_count)
{
    az_iot_adu_client_install_result* r = &ADU_I(client).install_result;
    memset(r, 0, sizeof(*r));
    if (step_count < 0) step_count = 0;
    if (step_count > _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS)
    {
        step_count = _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS;
    }
    r->step_results_count = step_count;
}

static void result_step_success(az_iot_adu_client_t* client, uint32_t step)
{
    az_iot_adu_client_install_result* r = &ADU_I(client).install_result;
    if ((int32_t)step < r->step_results_count)
    {
        r->step_results[step].result_code = AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS;
        r->step_results[step].extended_result_code = 0;
    }
}

/* Record a phase failure for the given step + overall result. The overall
 * result mirrors the FIRST failing step (root cause); later calls don't
 * overwrite an already-recorded overall failure. */
static void result_step_failure(
    az_iot_adu_client_t* client, uint32_t step, uint32_t facility, int32_t sub_code)
{
    az_iot_adu_client_install_result* r = &ADU_I(client).install_result;
    int32_t extended = AZ_IOT_ADU_EXTENDED_RESULT(facility, (uint32_t)sub_code);
    int32_t code = (int32_t)(700 - facility); /* a non-success ADU code (< 700) */

    if ((int32_t)step < r->step_results_count)
    {
        r->step_results[step].result_code = code;
        r->step_results[step].extended_result_code = extended;
    }
    if (r->result_code == 0 || r->result_code == AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS)
    {
        r->result_code = code;
        r->extended_result_code = extended;
    }
}

static void result_overall_success(az_iot_adu_client_t* client)
{
    az_iot_adu_client_install_result* r = &ADU_I(client).install_result;
    r->result_code = AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS;
    r->extended_result_code = 0;
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
    az_span jws, az_span* header, az_span* payload, az_span* sig, az_span* signed_bytes)
{
    az_span dot = AZ_SPAN_FROM_STR(".");
    int32_t n = az_span_size(jws);

    /* Locate the first '.' and, within the remainder, the second one. */
    int32_t d1 = az_span_find(jws, dot);
    if (d1 <= 0) return false;
    int32_t d2_rel = az_span_find(az_span_slice_to_end(jws, d1 + 1), dot);
    if (d2_rel < 0) return false;
    int32_t d2 = d1 + 1 + d2_rel;

    /* Need all three parts non-empty, and the signature must not itself contain a
     * further '.' (a 4th segment ⇒ not a valid compact JWS). */
    if (d2 <= d1 + 1 || d2 >= n - 1) return false;
    if (az_span_find(az_span_slice_to_end(jws, d2 + 1), dot) >= 0) return false;
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
 * standard base64. The ADU service emits the signing-key modulus (n) as
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
    if (az_result_succeeded(az_base64_decode(az_span_create(buf, cap), src, &written))
        && written > 0)
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
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME) continue;
        bool match = az_json_token_is_text_equal(&jr.token, name);
        if (az_result_failed(az_json_reader_next_token(&jr))) return AZ_SPAN_EMPTY;
        if (match)
        {
            if (jr.token.kind != AZ_JSON_TOKEN_STRING) return AZ_SPAN_EMPTY;
            int32_t len = 0;
            if (az_result_failed(az_json_token_get_string(&jr.token, out_buf, cap, &len)))
            {
                return AZ_SPAN_EMPTY;
            }
            return az_span_create((uint8_t*)out_buf, len);
        }
        /* Skip a non-matching object/array value so we stay at object scope. */
        if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT
            || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
        {
            if (az_result_failed(az_json_reader_skip_children(&jr))) return AZ_SPAN_EMPTY;
        }
    }
    return AZ_SPAN_EMPTY;
}

static const az_span k_alg_rs256 = AZ_SPAN_LITERAL_FROM_STR("RS256");

/* Resolve a JWS `kid` against the root-key store. Returns the matching key, or
 * NULL if unknown or disabled (revoked). */
static const az_iot_adu_root_key_t* resolve_root_key(az_iot_adu_client_t* client, az_span kid)
{
    for (size_t i = 0; i < ADU_I(client).root_key_count; ++i)
    {
        const az_iot_adu_root_key_t* rk = &ADU_I(client).root_keys[i];
        if (rk->kid == NULL) continue;
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
 * diagnosable from the device console instead of a single opaque "Failed". */
#define ADU_VERIFY_FAIL(why) \
    do { fprintf(stderr, "[adu] manifest verification failed: %s\n", (why)); \
         return AZ_IOT_ADU_RESULT_FAILURE; } while (0)

static int32_t verify_manifest(az_iot_adu_client_t* client)
{
    az_iot_adu_crypto_hooks_t* crypto = &ADU_I(client).crypto;
    if (crypto->verify_rs256_fn == NULL || crypto->sha256_fn == NULL)
    {
        ADU_VERIFY_FAIL("crypto hooks not configured (verify_rs256_fn/sha256_fn)");
    }

    az_span jws = ADU_I(client).current_request.update_manifest_signature;
    /* parse_manifest() already unescaped the manifest IN PLACE and recorded the
     * (shorter) unescaped span as manifest_text. current_request.update_manifest
     * still carries the original escaped LENGTH, so hashing it would digest
     * unescaped-content + trailing leftover bytes. The service signs the hash of
     * the unescaped manifest body, so step 6 must use manifest_text. */
    az_span manifest = ADU_I(client).manifest_text;
    if (az_span_size(jws) <= 0 || az_span_size(manifest) <= 0)
    {
        ADU_VERIFY_FAIL("empty updateManifest or updateManifestSignature");
    }

    /* 1. Split the manifest JWS and decode + parse its protected header. */
    az_span m_hdr_b64, m_pl_b64, m_sig_b64, m_signed;
    if (!jws_split(jws, &m_hdr_b64, &m_pl_b64, &m_sig_b64, &m_signed))
    {
        ADU_VERIFY_FAIL("step 1: manifest JWS is not a valid 3-part token");
    }

    uint8_t sjwk_buf[2048]; /* outlives steps 2-6: SJWK segments point into it */
    az_span sjwk;
    {
        uint8_t hdr_buf[2048];
        az_span hdr = jws_b64url(m_hdr_b64, hdr_buf, (int32_t)sizeof(hdr_buf));
        if (az_span_size(hdr) <= 0) { ADU_VERIFY_FAIL("step 1: manifest JWS header is not valid base64url"); }

        char alg_buf[16];
        az_span alg = jws_json_str(hdr, AZ_SPAN_FROM_STR("alg"), alg_buf, (int32_t)sizeof(alg_buf));
        if (!az_span_is_content_equal(alg, k_alg_rs256)) { ADU_VERIFY_FAIL("step 1: manifest JWS alg is not RS256"); }

        sjwk = jws_json_str(
            hdr, AZ_SPAN_FROM_STR("sjwk"), (char*)sjwk_buf, (int32_t)sizeof(sjwk_buf));
        if (az_span_size(sjwk) <= 0) { ADU_VERIFY_FAIL("step 1: manifest JWS header has no sjwk"); }
    }

    /* 2. Split the SJWK and read its header (alg + kid), then resolve the kid. */
    az_span s_hdr_b64, s_pl_b64, s_sig_b64, s_signed;
    if (!jws_split(sjwk, &s_hdr_b64, &s_pl_b64, &s_sig_b64, &s_signed))
    {
        ADU_VERIFY_FAIL("step 2: sjwk is not a valid 3-part token");
    }

    const az_iot_adu_root_key_t* root = NULL;
    {
        uint8_t shdr_buf[512];
        az_span shdr = jws_b64url(s_hdr_b64, shdr_buf, (int32_t)sizeof(shdr_buf));
        if (az_span_size(shdr) <= 0) { ADU_VERIFY_FAIL("step 2: sjwk header is not valid base64url"); }

        char salg_buf[16];
        az_span salg
            = jws_json_str(shdr, AZ_SPAN_FROM_STR("alg"), salg_buf, (int32_t)sizeof(salg_buf));
        if (!az_span_is_content_equal(salg, k_alg_rs256)) { ADU_VERIFY_FAIL("step 2: sjwk alg is not RS256"); }

        char kid_buf[128];
        az_span kid
            = jws_json_str(shdr, AZ_SPAN_FROM_STR("kid"), kid_buf, (int32_t)sizeof(kid_buf));
        if (az_span_size(kid) <= 0) { ADU_VERIFY_FAIL("step 2: sjwk header has no kid"); }

        root = resolve_root_key(client, kid);
        if (root == NULL) { ADU_VERIFY_FAIL("step 2: sjwk kid does not match any known (enabled) root key"); }
    }

    /* 3. Verify the SJWK signature with the resolved root key. */
    {
        uint8_t s_sig_buf[1024];
        az_span s_sig = jws_b64url(s_sig_b64, s_sig_buf, (int32_t)sizeof(s_sig_buf));
        if (az_span_size(s_sig) <= 0) ADU_VERIFY_FAIL("step 3: sjwk signature is not valid base64url");
        if (crypto->verify_rs256_fn(
                root->modulus, root->modulus_len, root->exponent, root->exponent_len,
                az_span_ptr(s_signed), (size_t)az_span_size(s_signed),
                az_span_ptr(s_sig), (size_t)az_span_size(s_sig), crypto->user_ctx)
            != AZ_IOT_ADU_RESULT_SUCCESS)
        {
            ADU_VERIFY_FAIL("step 3: sjwk signature does not verify against the root key");
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
        if (az_span_size(spl) <= 0) ADU_VERIFY_FAIL("step 4: sjwk payload is not valid base64url");

        char n_b64[1024];
        char e_b64[64];
        az_span n_field = jws_json_str(spl, AZ_SPAN_FROM_STR("n"), n_b64, (int32_t)sizeof(n_b64));
        az_span e_field = jws_json_str(spl, AZ_SPAN_FROM_STR("e"), e_b64, (int32_t)sizeof(e_b64));
        if (az_span_size(n_field) <= 0 || az_span_size(e_field) <= 0)
        {
            ADU_VERIFY_FAIL("step 4: signing JWK is missing modulus (n) or exponent (e)");
        }
        n_raw = jws_b64_any(n_field, n_buf, (int32_t)sizeof(n_buf));
        e_raw = jws_b64_any(e_field, e_buf, (int32_t)sizeof(e_buf));
        if (az_span_size(n_raw) <= 0 || az_span_size(e_raw) <= 0)
        {
            ADU_VERIFY_FAIL("step 4: signing JWK n/e are not valid base64url");
        }
    }

    /* 5. Verify the manifest JWS signature with the trusted signing key. */
    {
        uint8_t m_sig_buf[1024];
        az_span m_sig = jws_b64url(m_sig_b64, m_sig_buf, (int32_t)sizeof(m_sig_buf));
        if (az_span_size(m_sig) <= 0) ADU_VERIFY_FAIL("step 5: manifest signature is not valid base64url");
        if (crypto->verify_rs256_fn(
                az_span_ptr(n_raw), (size_t)az_span_size(n_raw),
                az_span_ptr(e_raw), (size_t)az_span_size(e_raw),
                az_span_ptr(m_signed), (size_t)az_span_size(m_signed),
                az_span_ptr(m_sig), (size_t)az_span_size(m_sig), crypto->user_ctx)
            != AZ_IOT_ADU_RESULT_SUCCESS)
        {
            ADU_VERIFY_FAIL("step 5: manifest signature does not verify against the signing key");
        }
    }

    /* 6. Bind the signed manifest to THIS deployment: the manifest JWS payload
     *    carries SHA-256(manifest body); recompute and compare. */
    {
        uint8_t pl_buf[256];
        az_span pl = jws_b64url(m_pl_b64, pl_buf, (int32_t)sizeof(pl_buf));
        if (az_span_size(pl) <= 0) ADU_VERIFY_FAIL("step 6: manifest JWS payload is not valid base64url");

        char hash_b64[128];
        az_span hash_field
            = jws_json_str(pl, AZ_SPAN_FROM_STR("sha256"), hash_b64, (int32_t)sizeof(hash_b64));
        if (az_span_size(hash_field) <= 0) ADU_VERIFY_FAIL("step 6: manifest JWS payload has no sha256");

        uint8_t expected[32];
        int32_t exp_written = 0;
        if (az_result_failed(az_base64_decode(
                az_span_create(expected, (int32_t)sizeof(expected)), hash_field, &exp_written))
            || exp_written != 32)
        {
            ADU_VERIFY_FAIL("step 6: manifest sha256 field is not a 32-byte base64 hash");
        }

        uint8_t actual[32];
        if (crypto->sha256_fn(
                az_span_ptr(manifest), (size_t)az_span_size(manifest), actual, crypto->user_ctx)
            != AZ_IOT_ADU_RESULT_SUCCESS)
        {
            ADU_VERIFY_FAIL("step 6: sha256_fn hook failed over the manifest body");
        }
        if (!az_span_is_content_equal(
                AZ_SPAN_FROM_BUFFER(expected), AZ_SPAN_FROM_BUFFER(actual)))
        {
            ADU_VERIFY_FAIL("step 6: computed manifest SHA-256 does not match the signed hash");
        }
    }

    return AZ_IOT_ADU_RESULT_SUCCESS;
}

/* Verify a downloaded file's SHA-256 against the signed manifest by streaming
 * the file back through the incremental crypto hooks. Returns SUCCESS when the
 * hash matches; FAILURE on any mismatch or hook/read error. */
static int32_t verify_file_hash(
    az_iot_adu_client_t* client,
    const az_iot_adu_client_update_manifest_file* file,
    uint32_t file_index)
{
    az_iot_adu_crypto_hooks_t* crypto = &ADU_I(client).crypto;
    az_iot_adu_platform_hooks_t* hooks = &ADU_I(client).hooks;

    /* Locate the SHA-256 hash entry for this file. */
    az_span hash_b64 = AZ_SPAN_EMPTY;
    for (uint32_t i = 0; i < file->hashes_count; ++i)
    {
        if (az_span_is_content_equal(
                file->hashes[i].hash_type, AZ_SPAN_FROM_STR("sha256")))
        {
            hash_b64 = file->hashes[i].hash_value;
            break;
        }
    }
    if (az_span_size(hash_b64) <= 0)
    {
        return AZ_IOT_ADU_RESULT_FAILURE; /* manifest must carry a sha256 hash */
    }

    /* Decode the expected hash (standard base64) into 32 bytes. */
    uint8_t expected[32];
    int32_t exp_written = 0;
    if (az_result_failed(az_base64_decode(
            az_span_create(expected, (int32_t)sizeof(expected)), hash_b64, &exp_written))
        || exp_written != (int32_t)sizeof(expected))
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }

    /* Stream the file through the incremental SHA-256 hooks. */
    void* ctx = NULL;
    if (crypto->sha256_init_fn(&ctx, crypto->user_ctx) != AZ_IOT_ADU_RESULT_SUCCESS)
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }

    uint8_t chunk[256];
    size_t offset = 0;
    for (;;)
    {
        size_t read = 0;
        if (hooks->read_file_fn(
                file, file_index, offset, chunk, sizeof(chunk), &read, hooks->user_ctx)
            != AZ_IOT_ADU_RESULT_SUCCESS)
        {
            uint8_t scratch[32];
            (void)crypto->sha256_final_fn(ctx, scratch, crypto->user_ctx); /* free ctx */
            return AZ_IOT_ADU_RESULT_FAILURE;
        }
        if (read == 0)
        {
            break; /* end of file */
        }
        if (crypto->sha256_update_fn(ctx, chunk, read, crypto->user_ctx)
            != AZ_IOT_ADU_RESULT_SUCCESS)
        {
            uint8_t scratch[32];
            (void)crypto->sha256_final_fn(ctx, scratch, crypto->user_ctx); /* free ctx */
            return AZ_IOT_ADU_RESULT_FAILURE;
        }
        offset += read;
    }

    uint8_t actual[32];
    if (crypto->sha256_final_fn(ctx, actual, crypto->user_ctx) != AZ_IOT_ADU_RESULT_SUCCESS)
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }
    if (!az_span_is_content_equal(
            AZ_SPAN_FROM_BUFFER(expected), AZ_SPAN_FROM_BUFFER(actual)))
    {
        return AZ_IOT_ADU_RESULT_FAILURE;
    }
    return AZ_IOT_ADU_RESULT_SUCCESS;
}


/* Reset the workflow back to Idle, clearing the parsed request. */
static void reset_to_idle(az_iot_adu_client_t* client)
{
    ADU_I(client).state = AZ_IOT_ADU_STATE_IDLE;
    ADU_I(client).have_request = false;
    ADU_I(client).current_step = 0;
    ADU_I(client).current_file = 0;
    ADU_I(client).cancel_requested = false;
    memset(&ADU_I(client).current_request, 0, sizeof(ADU_I(client).current_request));
    memset(&ADU_I(client).current_manifest, 0, sizeof(ADU_I(client).current_manifest));
    ADU_I(client).request_len = 0;
}

/* Parse the manifest body (an escaped JSON string in the request) into
 * current_manifest. Returns AZ_IOT_OK on success. The unescape happens into the
 * tail of a caller-independent scratch buffer owned by the request span; since
 * the upstream manifest parser only stores spans pointing into the unescaped
 * text, that text MUST remain valid as long as current_manifest is used — so we
 * unescape in place within a client-owned scratch buffer. */
static az_iot_result_t parse_manifest(az_iot_adu_client_t* client)
{
    az_span manifest = ADU_I(client).current_request.update_manifest;
    if (az_span_size(manifest) <= 0) return AZ_IOT_ERR_INVALID_ARG;

    /* Unescape in place: the unescaped form is never longer than the source.
     * az_json_string_unescape returns the unescaped span (empty on failure). */
    az_span unescaped = az_json_string_unescape(manifest, manifest);
    if (az_span_size(unescaped) <= 0)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }

    az_json_reader jr;
    if (az_result_failed(az_json_reader_init(&jr, unescaped, NULL)))
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }
    if (az_result_failed(az_iot_adu_client_parse_update_manifest(
            &ADU_I(client).az, &jr, &ADU_I(client).current_manifest)))
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }
    ADU_I(client).manifest_text = unescaped;
    return AZ_IOT_OK;
}

/* Twin desired-property subscriber callback (feature-client pool).
 *
 * The patch buffer is only valid for the duration of this call, but the
 * workflow is processed asynchronously over many do_work() iterations and the
 * upstream parser stores spans that point into the payload. So we COPY the patch
 * into the client-owned request_buffer and parse from there; current_request /
 * current_manifest then reference stable storage. */
static void process_desired_patch(
    az_iot_adu_client_t* client, const uint8_t* patch, size_t patch_len)
{
    if (client == NULL || patch == NULL || patch_len == 0) return;
    if (ADU_I(client).detached) return;
    if (patch_len > sizeof(ADU_I(client).request_buffer)) return; /* too large to back */

    /* Stage the patch into client-owned storage. */
    memcpy(ADU_I(client).request_buffer, patch, patch_len);
    ADU_I(client).request_len = patch_len;
    fprintf(stderr, "[adu] deviceUpdate desired-property received (%zu bytes):\n%.*s\n",
            patch_len, (int)patch_len, (const char*)ADU_I(client).request_buffer);
    az_span buf = az_span_create(ADU_I(client).request_buffer, (int32_t)patch_len);

    az_json_reader jr;
    if (az_result_failed(az_json_reader_init(&jr, buf, NULL))) return;

    /* Navigate: { "deviceUpdate": { ... "service": {...} } }. The upstream
     * parser must be positioned ON the "service" property name. If the patch
     * doesn't carry our component (or its service object), ignore it; other
     * subscribers may consume it. */
    if (az_result_failed(az_json_reader_next_token(&jr))) return;
    if (jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT) return;

    /* Find the "deviceUpdate" component value object. */
    bool in_component = false;
    while (az_result_succeeded(az_json_reader_next_token(&jr)))
    {
        if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT) break;
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME) return;

        bool is_component = az_json_token_is_text_equal(
            &jr.token, AZ_SPAN_FROM_STR(AZ_IOT_ADU_CLIENT_PROPERTIES_COMPONENT_NAME));
        if (az_result_failed(az_json_reader_next_token(&jr))) return;
        if (is_component)
        {
            if (jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT) return;
            in_component = true;
            break;
        }
        if (az_result_failed(az_json_reader_skip_children(&jr))) return;
    }
    if (!in_component) return;

    /* Inside "deviceUpdate": locate the "service" property name (skipping
     * "__t" and any agent-side properties). Stop ON the "service" prop name. */
    bool on_service = false;
    while (az_result_succeeded(az_json_reader_next_token(&jr)))
    {
        if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT) break;
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME) return;

        if (az_json_token_is_text_equal(
                &jr.token,
                AZ_SPAN_FROM_STR("service")))
        {
            on_service = true;
            break;
        }
        /* Skip this property's value. */
        if (az_result_failed(az_json_reader_next_token(&jr))) return;
        if (az_result_failed(az_json_reader_skip_children(&jr))) return;
    }
    if (!on_service) return;

    az_iot_adu_client_update_request req;
    memset(&req, 0, sizeof(req));
    if (az_result_failed(az_iot_adu_client_parse_service_properties(
            &ADU_I(client).az, &jr, &req)))
    {
        return;
    }

    /* A Cancel action supersedes any in-progress workflow. */
    if (req.workflow.action == AZ_IOT_ADU_CLIENT_SERVICE_ACTION_CANCEL)
    {
        ADU_I(client).cancel_requested = true;
        ADU_I(client).current_request = req;
        ADU_I(client).have_request = true;
        return;
    }

    ADU_I(client).current_request = req;
    ADU_I(client).have_request = true;
    ADU_I(client).cancel_requested = false;
    ADU_I(client).current_step = 0;
    ADU_I(client).current_file = 0;
    ADU_I(client).state = AZ_IOT_ADU_STATE_MANIFEST_RECEIVED;
}

/* Desired-property push subscriber: forward the patch to the shared handler. */
static void on_desired(
    const uint8_t* patch, size_t patch_len, uint64_t version, void* user_ctx)
{
    (void)version;
    process_desired_patch((az_iot_adu_client_t*)user_ctx, patch, patch_len);
}

/* Initial twin GET response: a deployment may already be sitting in the desired
 * properties (e.g. the device was offline when it was created, so no push will
 * arrive). The GET payload is the full twin document
 *   { "desired": { "deviceUpdate": {...}, "$version": N }, "reported": {...} }.
 * Extract the raw "desired" object and feed it through the same path as a push. */
static void on_initial_twin_get(
    az_iot_result_t status, const uint8_t* twin_payload, size_t twin_payload_len, void* user_ctx)
{
    az_iot_adu_client_t* client = (az_iot_adu_client_t*)user_ctx;
    if (client == NULL || ADU_I(client).detached) return;
    if (status != AZ_IOT_OK || twin_payload == NULL || twin_payload_len == 0) return;

    az_span doc = az_span_create((uint8_t*)(uintptr_t)twin_payload, (int32_t)twin_payload_len);
    az_json_reader jr;
    if (az_result_failed(az_json_reader_init(&jr, doc, NULL))) return;
    if (az_result_failed(az_json_reader_next_token(&jr))) return;
    if (jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT) return;

    while (az_result_succeeded(az_json_reader_next_token(&jr)))
    {
        if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT) break;
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME) return;

        bool is_desired = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("desired"));
        if (az_result_failed(az_json_reader_next_token(&jr))) return;
        if (is_desired)
        {
            if (jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT) return;
            /* Capture the raw object text from its '{' to the matching '}'. The
             * token slice points into the source buffer, so start/end bracket
             * the whole sub-object. */
            uint8_t* start = az_span_ptr(jr.token.slice);
            if (az_result_failed(az_json_reader_skip_children(&jr))) return;
            uint8_t* end = az_span_ptr(jr.token.slice) + az_span_size(jr.token.slice);
            if (end <= start) return;
            process_desired_patch(client, start, (size_t)(end - start));
            return;
        }
        if (az_result_failed(az_json_reader_skip_children(&jr))) return;
    }
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

az_iot_result_t az_iot_adu_client_initialize(
    az_iot_adu_client_t* client,
    az_iot_twin_client_t* twin,
    const az_iot_adu_platform_hooks_t* hooks,
    const az_iot_adu_crypto_hooks_t* crypto,
    const az_iot_adu_root_key_t* root_keys,
    size_t root_key_count,
    const az_iot_adu_device_properties_t* device_props,
    uint8_t* device_props_buffer,
    size_t device_props_buffer_size)
{
    if (client == NULL || twin == NULL || hooks == NULL || crypto == NULL ||
        device_props == NULL || device_props_buffer == NULL)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }
    if (root_key_count > AZ_IOT_ADU_MAX_ROOT_KEYS)
    {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }

    memset(client, 0, sizeof(*client));
    ADU_I(client).twin = twin;
    ADU_I(client).hooks = *hooks;
    ADU_I(client).crypto = *crypto;
    ADU_I(client).device_props_buffer = device_props_buffer;
    ADU_I(client).device_props_buffer_size = device_props_buffer_size;
    ADU_I(client).state = AZ_IOT_ADU_STATE_IDLE;

    if (root_keys != NULL && root_key_count > 0)
    {
        for (size_t i = 0; i < root_key_count; ++i)
        {
            ADU_I(client).root_keys[i] = root_keys[i];
        }
        ADU_I(client).root_key_count = root_key_count;
    }

    if (az_result_failed(az_iot_adu_client_init(&ADU_I(client).az, NULL)))
    {
        memset(client, 0, sizeof(*client));
        return AZ_IOT_ERR_INTERNAL;
    }

    az_iot_result_t r = cache_device_properties(client, device_props);
    if (r != AZ_IOT_OK)
    {
        memset(client, 0, sizeof(*client));
        return r;
    }

    r = az_iot_twin_client__subscribe_desired(twin, on_desired, client);
    if (r != AZ_IOT_OK)
    {
        memset(client, 0, sizeof(*client));
        return r;
    }

    /* Report the initial Idle agent state + installed update id on startup. */
    ADU_I(client).device_props_report_pending = true;
    /* And proactively pull the twin so a deployment that arrived while we were
     * offline (no fresh desired push) is still picked up. */
    ADU_I(client).initial_get_pending = true;
    return AZ_IOT_OK;
}

void az_iot_adu_client_deinit(az_iot_adu_client_t* client)
{
    if (client == NULL) return;
    if (ADU_I(client).twin != NULL)
    {
        (void)az_iot_twin_client_unsubscribe_desired(ADU_I(client).twin, on_desired, client);
    }
    memset(client, 0, sizeof(*client));
}

/* az_iot_adu_microsoft_root_keys() — Microsoft's compiled-in ADU production
 * root public keys — is defined in adu_root_keys_microsoft.c (generated from the
 * official agent's hardcoded key list). Kept in a separate translation unit so
 * the large key blobs live apart from the state machine. */

/* ------------------------------------------------------------------------- */
/* persistence & resume (Phase 5)                                            */
/* ------------------------------------------------------------------------- */

/* Versioned, integrity-checked workflow snapshot. Layout (all little-endian):
 *   [0]  magic[4]   = 'A','D','U','1'
 *   [4]  u16 version (= AZ_IOT_ADU_PERSIST_VERSION)
 *   [6]  u16 flags   (bit0 cancel_requested, bit1 have_request)
 *   [8]  u32 state
 *   [12] u32 current_step
 *   [16] u32 current_file
 *   [20] u32 workflow_id_off   (offset into request_buffer)
 *   [24] u32 workflow_id_len
 *   [28] u32 manifest_off      (offset into request_buffer)
 *   [32] u32 manifest_len
 *   [36] u32 request_len
 *   [40] request_buffer[request_len]
 *   [40+request_len] u32 crc32 (over bytes [0 .. 40+request_len))           */
#define AZ_IOT_ADU_PERSIST_MAGIC0 'A'
#define AZ_IOT_ADU_PERSIST_MAGIC1 'D'
#define AZ_IOT_ADU_PERSIST_MAGIC2 'U'
#define AZ_IOT_ADU_PERSIST_MAGIC3 '1'
#define AZ_IOT_ADU_PERSIST_VERSION 1u
#define AZ_IOT_ADU_PERSIST_HEADER_SIZE 40u

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
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), computed bytewise. */
static uint32_t adu_crc32(const uint8_t* data, size_t len)
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
static uint32_t request_offset(const az_iot_adu_client_t* client, az_span s)
{
    const uint8_t* base = ADU_I(client).request_buffer;
    const uint8_t* p = az_span_ptr(s);
    if (p < base || p > base + ADU_I(client).request_len) return 0;
    return (uint32_t)(p - base);
}

/* Serialize the current workflow position and hand it to persist_state_fn. */
static void adu_persist(az_iot_adu_client_t* client)
{
    az_iot_adu_platform_hooks_t* h = &ADU_I(client).hooks;
    if (h->persist_state_fn == NULL) return;

    uint32_t request_len = (uint32_t)ADU_I(client).request_len;
    if (request_len > AZ_IOT_ADU_REQUEST_BUFFER_SIZE) return;

    static uint8_t blob[AZ_IOT_ADU_PERSIST_HEADER_SIZE + AZ_IOT_ADU_REQUEST_BUFFER_SIZE + 4u];
    uint16_t flags = 0;
    if (ADU_I(client).cancel_requested) flags |= 0x1u;
    if (ADU_I(client).have_request) flags |= 0x2u;

    blob[0] = AZ_IOT_ADU_PERSIST_MAGIC0;
    blob[1] = AZ_IOT_ADU_PERSIST_MAGIC1;
    blob[2] = AZ_IOT_ADU_PERSIST_MAGIC2;
    blob[3] = AZ_IOT_ADU_PERSIST_MAGIC3;
    wr_u16le(&blob[4], (uint16_t)AZ_IOT_ADU_PERSIST_VERSION);
    wr_u16le(&blob[6], flags);
    wr_u32le(&blob[8], (uint32_t)ADU_I(client).state);
    wr_u32le(&blob[12], ADU_I(client).current_step);
    wr_u32le(&blob[16], ADU_I(client).current_file);
    wr_u32le(&blob[20], request_offset(client, ADU_I(client).current_request.workflow.id));
    wr_u32le(&blob[24], (uint32_t)az_span_size(ADU_I(client).current_request.workflow.id));
    wr_u32le(&blob[28], request_offset(client, ADU_I(client).manifest_text));
    wr_u32le(&blob[32], (uint32_t)az_span_size(ADU_I(client).manifest_text));
    wr_u32le(&blob[36], request_len);
    memcpy(&blob[AZ_IOT_ADU_PERSIST_HEADER_SIZE], ADU_I(client).request_buffer, request_len);

    uint32_t crc_region = AZ_IOT_ADU_PERSIST_HEADER_SIZE + request_len;
    wr_u32le(&blob[crc_region], adu_crc32(blob, crc_region));

    (void)h->persist_state_fn(blob, (size_t)crc_region + 4u, h->user_ctx);
}

az_iot_result_t az_iot_adu_client_resume(az_iot_adu_client_t* client)
{
    if (client == NULL) return AZ_IOT_ERR_INVALID_ARG;
    if (ADU_I(client).detached) return AZ_IOT_ERR_DETACHED;
    if (ADU_I(client).hooks.load_state_fn == NULL) return AZ_IOT_OK;

    static uint8_t blob[AZ_IOT_ADU_PERSIST_HEADER_SIZE + AZ_IOT_ADU_REQUEST_BUFFER_SIZE + 4u];
    size_t blen = 0;
    if (ADU_I(client).hooks.load_state_fn(
            blob, sizeof(blob), &blen, ADU_I(client).hooks.user_ctx)
        != 0)
    {
        return AZ_IOT_OK; /* nothing persisted */
    }
    if (blen < AZ_IOT_ADU_PERSIST_HEADER_SIZE + 4u) return AZ_IOT_OK; /* too small */
    if (blob[0] != AZ_IOT_ADU_PERSIST_MAGIC0 || blob[1] != AZ_IOT_ADU_PERSIST_MAGIC1
        || blob[2] != AZ_IOT_ADU_PERSIST_MAGIC2 || blob[3] != AZ_IOT_ADU_PERSIST_MAGIC3)
    {
        return AZ_IOT_OK; /* not our blob */
    }
    if (rd_u16le(&blob[4]) != AZ_IOT_ADU_PERSIST_VERSION) return AZ_IOT_OK;

    uint32_t request_len = rd_u32le(&blob[36]);
    if (request_len > AZ_IOT_ADU_REQUEST_BUFFER_SIZE) return AZ_IOT_OK;
    uint32_t crc_region = AZ_IOT_ADU_PERSIST_HEADER_SIZE + request_len;
    if ((size_t)crc_region + 4u > blen) return AZ_IOT_OK;
    if (adu_crc32(blob, crc_region) != rd_u32le(&blob[crc_region])) return AZ_IOT_OK; /* corrupt */

    uint16_t flags = rd_u16le(&blob[6]);
    uint32_t state = rd_u32le(&blob[8]);
    uint32_t step = rd_u32le(&blob[12]);
    uint32_t file = rd_u32le(&blob[16]);
    uint32_t wf_off = rd_u32le(&blob[20]);
    uint32_t wf_len = rd_u32le(&blob[24]);
    uint32_t mf_off = rd_u32le(&blob[28]);
    uint32_t mf_len = rd_u32le(&blob[32]);
    if (mf_off + mf_len > request_len || wf_off + wf_len > request_len) return AZ_IOT_OK;

    /* Restore the request payload and re-derive the manifest from it. */
    memset(&ADU_I(client).current_request, 0, sizeof(ADU_I(client).current_request));
    memset(&ADU_I(client).current_manifest, 0, sizeof(ADU_I(client).current_manifest));
    memcpy(ADU_I(client).request_buffer, &blob[AZ_IOT_ADU_PERSIST_HEADER_SIZE], request_len);
    ADU_I(client).request_len = request_len;

    az_span manifest_text
        = az_span_create(ADU_I(client).request_buffer + mf_off, (int32_t)mf_len);
    ADU_I(client).manifest_text = manifest_text;

    az_json_reader jr;
    if (az_result_failed(az_json_reader_init(&jr, manifest_text, NULL))
        || az_result_failed(az_iot_adu_client_parse_update_manifest(
               &ADU_I(client).az, &jr, &ADU_I(client).current_manifest)))
    {
        /* Corrupt manifest text: discard the snapshot and stay Idle. */
        ADU_I(client).request_len = 0;
        return AZ_IOT_OK;
    }

    ADU_I(client).current_request.workflow.id
        = az_span_create(ADU_I(client).request_buffer + wf_off, (int32_t)wf_len);
    ADU_I(client).state = (az_iot_adu_state_t)state;
    ADU_I(client).current_step = step;
    ADU_I(client).current_file = file;
    ADU_I(client).cancel_requested = (flags & 0x1u) != 0;
    ADU_I(client).have_request = (flags & 0x2u) != 0;
    return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* runtime: state machine                                                    */
/* ------------------------------------------------------------------------- */

static uint32_t step_file_count(const az_iot_adu_client_t* client, uint32_t step)
{
    const az_iot_adu_client_update_manifest* m = &ADU_I(client).current_manifest;
    if (step >= m->instructions.steps_count) return 0;
    return m->instructions.steps[step].files_count;
}

/* Begin a best-effort reverse-order rollback and finish in Failed/Idle.
 * restore_count is the number of leading steps that have a backup to undo;
 * steps [restore_count-1 .. 0] are restored in reverse order. A failure during
 * download or backup of step N leaves step N without a backup (restore_count =
 * N); a failure during install or apply of step N means step N was backed up
 * (restore_count = N + 1). */
static void begin_rollback(az_iot_adu_client_t* client, uint32_t restore_count)
{
    /* Since backup/restore are non-blocking in practice (simulated or fast OTA
     * slot swaps), this increment performs the rollback synchronously. */
    if (ADU_I(client).hooks.restore_fn != NULL)
    {
        for (int32_t s = (int32_t)restore_count - 1; s >= 0; --s)
        {
            int32_t rr = ADU_I(client).hooks.restore_fn(
                &ADU_I(client).current_manifest, (uint32_t)s, ADU_I(client).hooks.user_ctx);
            if (rr != AZ_IOT_ADU_RESULT_SUCCESS)
            {
                /* Record restore failure but continue restoring earlier steps. */
                az_iot_adu_client_install_result* r = &ADU_I(client).install_result;
                r->extended_result_code =
                    AZ_IOT_ADU_EXTENDED_RESULT(AZ_IOT_ADU_FACILITY_RESTORE, (uint32_t)rr);
            }
        }
    }
    ADU_I(client).state = AZ_IOT_ADU_STATE_FAILED;
}

az_iot_result_t az_iot_adu_client_do_work(az_iot_adu_client_t* client)
{
    if (client == NULL) return AZ_IOT_ERR_INVALID_ARG;
    if (ADU_I(client).detached) return AZ_IOT_ERR_DETACHED;

    /* A pending device-properties / startup report takes priority. */
    if (ADU_I(client).device_props_report_pending)
    {
        ADU_I(client).device_props_report_pending = false;
        (void)az_iot_adu__report_state(client);
        /* Piggyback the initial twin pull on the same startup tick so a
         * deployment already waiting in desired properties is consumed without
         * needing a fresh push. Clear the flag only once the GET is actually
         * issued (the connection may not be ready yet). */
        if (ADU_I(client).initial_get_pending &&
            az_iot_twin_client_get(ADU_I(client).twin, on_initial_twin_get, client) == AZ_IOT_OK)
        {
            ADU_I(client).initial_get_pending = false;
        }
        return AZ_IOT_OK;
    }

    /* Retry the initial twin pull if it could not be issued at startup (e.g.
     * the connection was not ready). Self-heals across do_work iterations; does
     * not preempt state-machine progress. */
    if (ADU_I(client).initial_get_pending &&
        az_iot_twin_client_get(ADU_I(client).twin, on_initial_twin_get, client) == AZ_IOT_OK)
    {
        ADU_I(client).initial_get_pending = false;
    }

    /* Cancellation at a phase boundary returns immediately to Idle. */
    if (ADU_I(client).cancel_requested &&
        ADU_I(client).state != AZ_IOT_ADU_STATE_IDLE)
    {
        reset_to_idle(client);
        (void)az_iot_adu__report_state(client);
        return AZ_IOT_OK;
    }

    az_iot_adu_platform_hooks_t* h = &ADU_I(client).hooks;

    switch (ADU_I(client).state)
    {
        case AZ_IOT_ADU_STATE_IDLE:
            /* Nothing to do until a deployment arrives. */
            break;

        case AZ_IOT_ADU_STATE_MANIFEST_RECEIVED:
        {
            if (parse_manifest(client) != AZ_IOT_OK)
            {
                result_init_steps(client, 1);
                result_step_failure(client, 0, AZ_IOT_ADU_FACILITY_INTERNAL, 0);
                ADU_I(client).state = AZ_IOT_ADU_STATE_FAILED;
                (void)az_iot_adu__report_state(client);
                break;
            }
            result_init_steps(
                client, (int32_t)ADU_I(client).current_manifest.instructions.steps_count);
            ADU_I(client).state = AZ_IOT_ADU_STATE_VERIFYING_MANIFEST;
            (void)az_iot_adu__report_state(client);
            break;
        }

        case AZ_IOT_ADU_STATE_VERIFYING_MANIFEST:
        {
            if (verify_manifest(client) != AZ_IOT_ADU_RESULT_SUCCESS)
            {
                result_step_failure(client, 0, AZ_IOT_ADU_FACILITY_MANIFEST, 0);
                ADU_I(client).state = AZ_IOT_ADU_STATE_FAILED;
                (void)az_iot_adu__report_state(client);
                break;
            }
            /* Accept/Reject: reject (406) if already installed, else accept. */
            int32_t inst = (h->is_installed_fn != NULL)
                ? h->is_installed_fn(&ADU_I(client).current_manifest, h->user_ctx)
                : AZ_IOT_ADU_RESULT_SUCCESS;
            if (inst == AZ_IOT_ADU_RESULT_ALREADY_INSTALLED)
            {
                reset_to_idle(client);
                (void)az_iot_adu__report_state(client);
                break;
            }
            ADU_I(client).current_step = 0;
            ADU_I(client).current_file = 0;
            ADU_I(client).state = AZ_IOT_ADU_STATE_DOWNLOAD_STARTED;
            (void)az_iot_adu__report_state(client);
            break;
        }

        case AZ_IOT_ADU_STATE_DOWNLOAD_STARTED:
        {
            uint32_t step = ADU_I(client).current_step;
            uint32_t fcount = step_file_count(client, step);
            if (ADU_I(client).current_file >= fcount)
            {
                ADU_I(client).state = AZ_IOT_ADU_STATE_DOWNLOAD_COMPLETE;
                break;
            }
            /* Resolve the file + its download url, then drive download_fn. */
            const az_iot_adu_client_update_manifest* m = &ADU_I(client).current_manifest;
            uint32_t fidx = ADU_I(client).current_file;
            const az_iot_adu_client_update_manifest_file* file = &m->files[fidx];
            az_span url = AZ_SPAN_EMPTY;
            for (uint32_t i = 0; i < ADU_I(client).current_request.file_urls_count; ++i)
            {
                if (az_span_is_content_equal(
                        ADU_I(client).current_request.file_urls[i].id, file->id))
                {
                    url = ADU_I(client).current_request.file_urls[i].url;
                    break;
                }
            }
            int32_t dr = (h->download_fn != NULL)
                ? h->download_fn(file, url, fidx, fcount, h->user_ctx)
                : AZ_IOT_ADU_RESULT_FAILURE;
            if (dr == AZ_IOT_ADU_RESULT_IN_PROGRESS)
            {
                break; /* re-enter on next do_work */
            }
            if (dr != AZ_IOT_ADU_RESULT_SUCCESS)
            {
                result_step_failure(client, step, AZ_IOT_ADU_FACILITY_DOWNLOAD, dr);
                begin_rollback(client, step);
                (void)az_iot_adu__report_state(client);
                break;
            }
            /* Streaming per-file SHA-256 verification (opt-in: requires a
             * read-back hook plus the incremental crypto hooks). */
            if (h->read_file_fn != NULL
                && ADU_I(client).crypto.sha256_init_fn != NULL
                && ADU_I(client).crypto.sha256_update_fn != NULL
                && ADU_I(client).crypto.sha256_final_fn != NULL)
            {
                int32_t hr = verify_file_hash(client, file, fidx);
                if (hr != AZ_IOT_ADU_RESULT_SUCCESS)
                {
                    result_step_failure(client, step, AZ_IOT_ADU_FACILITY_HASH, hr);
                    begin_rollback(client, step);
                    (void)az_iot_adu__report_state(client);
                    break;
                }
            }
            ADU_I(client).current_file++;
            break;
        }

        case AZ_IOT_ADU_STATE_DOWNLOAD_COMPLETE:
            ADU_I(client).state = AZ_IOT_ADU_STATE_BACKUP_STARTED;
            break;

        case AZ_IOT_ADU_STATE_BACKUP_STARTED:
        {
            uint32_t step = ADU_I(client).current_step;
            int32_t br = (h->backup_fn != NULL)
                ? h->backup_fn(&ADU_I(client).current_manifest, step, h->user_ctx)
                : AZ_IOT_ADU_RESULT_SUCCESS;
            if (br == AZ_IOT_ADU_RESULT_IN_PROGRESS) break;
            if (br != AZ_IOT_ADU_RESULT_SUCCESS)
            {
                result_step_failure(client, step, AZ_IOT_ADU_FACILITY_BACKUP, br);
                begin_rollback(client, step);
                (void)az_iot_adu__report_state(client);
                break;
            }
            ADU_I(client).state = AZ_IOT_ADU_STATE_BACKUP_COMPLETE;
            break;
        }

        case AZ_IOT_ADU_STATE_BACKUP_COMPLETE:
            ADU_I(client).state = AZ_IOT_ADU_STATE_INSTALL_STARTED;
            break;

        case AZ_IOT_ADU_STATE_INSTALL_STARTED:
        {
            uint32_t step = ADU_I(client).current_step;
            int32_t ir = (h->install_fn != NULL)
                ? h->install_fn(&ADU_I(client).current_manifest, step, h->user_ctx)
                : AZ_IOT_ADU_RESULT_FAILURE;
            if (ir == AZ_IOT_ADU_RESULT_IN_PROGRESS) break;
            if (ir == AZ_IOT_ADU_RESULT_REBOOT_REQUIRED)
            {
                /* The install needs a reboot to take effect: snapshot the
                 * workflow so az_iot_adu_client_resume() can continue at Apply
                 * on the next boot, report, and advance (a real device reboots
                 * here; the loop simply continues if it does not). */
                ADU_I(client).state = AZ_IOT_ADU_STATE_INSTALL_COMPLETE;
                adu_persist(client);
                (void)az_iot_adu__report_state(client);
                break;
            }
            if (ir != AZ_IOT_ADU_RESULT_SUCCESS)
            {
                result_step_failure(client, step, AZ_IOT_ADU_FACILITY_INSTALL, ir);
                begin_rollback(client, step + 1); /* step was backed up */
                (void)az_iot_adu__report_state(client);
                break;
            }
            ADU_I(client).state = AZ_IOT_ADU_STATE_INSTALL_COMPLETE;
            break;
        }

        case AZ_IOT_ADU_STATE_INSTALL_COMPLETE:
            ADU_I(client).state = AZ_IOT_ADU_STATE_APPLY_STARTED;
            break;

        case AZ_IOT_ADU_STATE_APPLY_STARTED:
        {
            uint32_t step = ADU_I(client).current_step;
            int32_t ar = (h->apply_fn != NULL)
                ? h->apply_fn(&ADU_I(client).current_manifest, step, h->user_ctx)
                : AZ_IOT_ADU_RESULT_SUCCESS;
            if (ar == AZ_IOT_ADU_RESULT_IN_PROGRESS) break;
            if (ar != AZ_IOT_ADU_RESULT_SUCCESS && ar != AZ_IOT_ADU_RESULT_REBOOT_REQUIRED)
            {
                result_step_failure(client, step, AZ_IOT_ADU_FACILITY_APPLY, ar);
                begin_rollback(client, step + 1); /* step was backed up */
                (void)az_iot_adu__report_state(client);
                break;
            }
            result_step_success(client, step);

            /* Advance to the next step, or finish. */
            uint32_t steps = ADU_I(client).current_manifest.instructions.steps_count;
            if (step + 1 < steps)
            {
                ADU_I(client).current_step = step + 1;
                ADU_I(client).current_file = 0;
                ADU_I(client).state = AZ_IOT_ADU_STATE_DOWNLOAD_STARTED;
                (void)az_iot_adu__report_state(client);
                break;
            }
            result_overall_success(client);
            reset_to_idle(client);
            (void)az_iot_adu__report_state(client);
            break;
        }

        case AZ_IOT_ADU_STATE_RESTORE_STARTED:
            /* begin_rollback() performs restore synchronously then sets Failed;
             * this state is reserved for a future chunked rollback. */
            ADU_I(client).state = AZ_IOT_ADU_STATE_FAILED;
            break;

        case AZ_IOT_ADU_STATE_FAILED:
            /* Terminal failure already reported; return to Idle for the next
             * deployment. */
            reset_to_idle(client);
            break;
    }

    return AZ_IOT_OK;
}

bool az_iot_adu_is_cancelled(const az_iot_adu_client_t* client)
{
    if (client == NULL) return false;
    return ADU_I(client).cancel_requested;
}

az_iot_adu_state_t az_iot_adu_client_get_state(const az_iot_adu_client_t* client)
{
    if (client == NULL) return AZ_IOT_ADU_STATE_IDLE;
    return ADU_I(client).state;
}

az_iot_result_t az_iot_adu_client_update_device_properties(
    az_iot_adu_client_t* client,
    const az_iot_adu_device_properties_t* device_props)
{
    if (client == NULL || device_props == NULL) return AZ_IOT_ERR_INVALID_ARG;
    if (ADU_I(client).detached) return AZ_IOT_ERR_DETACHED;

    az_iot_result_t r = cache_device_properties(client, device_props);
    if (r != AZ_IOT_OK) return r;

    ADU_I(client).device_props_report_pending = true;
    return AZ_IOT_OK;
}
