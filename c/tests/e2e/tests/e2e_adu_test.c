// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* In-process Device Update (ADU) end-to-end scenario against a real Azure IoT
 * Hub / DPS instance.
 *
 * ONE process plays both halves, exactly like e2e_scenarios_test.c:
 *   - Device: the shipping SDK's ADU client over the Paho MQTT adapter,
 *     provisioned via DPS with an X.509 individual enrollment (shared fixture in
 *     e2e_device.[ch]).
 *   - Service (cloud): the az_iot_e2e_service facade, which PATCHes the device
 *     twin's desired properties with a deployment and reads the reported state
 *     back over the IoT Hub REST API.
 *
 * A deployment is driven PURELY by a twin desired-property PATCH carrying the
 * "deviceUpdate" component: no Device Update account, `az iot du`, or blob
 * storage is required. The device parses the signed manifest, runs the full
 * download -> backup -> install -> apply workflow through platform hooks, and
 * reports the outcome back to the twin.
 *
 * Crypto and the platform payload operations are MOCKED (as in the ADU unit
 * test): this is a transport/integration test of the twin<->ADU round-trip, not
 * a validation of RSA/SHA-256 or a real package install. Signature and hash
 * correctness are covered by the unit tests (adu_client_test.c and the
 * adu_crypto_openssl unit tests). The mock verify hook accepts a
 * structurally-valid JWS whose payload carries the fixed digest the mock
 * SHA-256 returns, so the manifest-binding check passes without real signing.
 *
 * Configuration comes entirely from the environment (set by the e2e CI job);
 * see e2e_device.h for the device variables and az_iot_e2e_service.h for the
 * service (IoT Hub connection string) variables.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <azure/core/az_base64.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_adu.h"

#include "az_iot_e2e_service.h"
#include "e2e_device.h"

/* ---- timing budgets ------------------------------------------------------- */
#define E2E_ADU_PUMP_MS          20  /* per do_work / poll slice                */
#define E2E_ADU_SETTLE_ITERS     80  /* pumps to subscribe + flush first report */
#define E2E_ADU_REST_TIMEOUT_S   60  /* one twin GET / PATCH round-trip         */
#define E2E_ADU_DEPLOY_TIMEOUT_S 180 /* desired PATCH -> workflow back to Idle  */

/* ------------------------------------------------------------------------- */
/* signed-manifest scaffolding (shared with the ADU unit test's approach)    */
/* ------------------------------------------------------------------------- */

/* A single-step, single-file v5 deployment wrapped in the deviceUpdate
 * component envelope. `%s` slots are filled at runtime with the workflow object,
 * the manifest version, and a structurally-valid JWS (see build_jws): core fully
 * parses the JWS/SJWK chain, so a real compact-token structure is required even
 * though the mock verify hook does not check the signature bytes. */
static const char k_patch_fmt[]
    = "{\"deviceUpdate\":{\"__t\":\"c\",\"service\":{"
      "%s,"
      "\"updateManifest\":\"{\\\"manifestVersion\\\":\\\"5\\\",\\\"updateId\\\":{\\\"provider\\\":"
      "\\\"Contoso\\\",\\\"name\\\":\\\"Foobar\\\",\\\"version\\\":\\\"%s\\\"},"
      "\\\"compatibility\\\":[{\\\"deviceManufacturer\\\":\\\"Contoso\\\",\\\"deviceModel\\\":"
      "\\\"Foobar\\\"}],\\\"instructions\\\":{\\\"steps\\\":[{\\\"handler\\\":\\\"microsoft/"
      "swupdate:1\\\",\\\"files\\\":[\\\"f2f4a804ca17afbae\\\"],\\\"handlerProperties\\\":{"
      "\\\"installedCriteria\\\":\\\"1.0\\\"}}]},\\\"files\\\":{\\\"f2f4a804ca17afbae\\\":{"
      "\\\"fileName\\\":\\\"iot-middleware-sample-adu-v1.1\\\",\\\"sizeInBytes\\\":844976,"
      "\\\"hashes\\\":{\\\"sha256\\\":\\\"xsoCnYAMkZZ7m9RL9Vyg9jKfFehCNxyuPFaJVM/"
      "WBi0=\\\"}}},\\\"createdDateTime\\\":\\\"2022-07-07T03:02:48.8449038Z\\\"}\","
      "\"updateManifestSignature\":\"%s\","
      "\"fileUrls\":{\"f2f4a804ca17afbae\":\"http://example.com/payload.bin\"}}}}";

/* The fixed digest the mock SHA-256 returns; the JWS payload carries its base64,
 * so the manifest-binding check passes regardless of the actual manifest bytes. */
#define ADU_TEST_HASH_BYTE 0xAB

/* The base64 SHA-256 the test manifest carries for its single file. The
 * streaming file-hash mock returns the decoded bytes so verification passes. */
#define ADU_TEST_FILE_HASH_B64 "xsoCnYAMkZZ7m9RL9Vyg9jKfFehCNxyuPFaJVM/WBi0="

/* Root key trusted to sign the SJWK, matched by `kid`. The mock verify hook
 * ignores the key bytes, so dummy modulus/exponent are sufficient here. */
static const uint8_t k_root_mod[] = { 0x01, 0x02, 0x03 };
static const uint8_t k_root_exp[] = { 0x01, 0x00, 0x01 };
static const az_iot_adu_root_key k_root_keys[]
    = { { "testkid", k_root_mod, sizeof(k_root_mod), k_root_exp, sizeof(k_root_exp), false } };

/* base64url-encode `data` into a NUL-terminated C string (translate +/ to -_ and
 * drop padding; az_core 1.5.0 ships only standard base64 encode). */
static void b64url_str(const void* data, int32_t len, char* dst, int32_t cap)
{
    int32_t w = 0;
    assert_true(az_result_succeeded(az_base64_encode(
        az_span_create((uint8_t*)dst, cap),
        az_span_create((uint8_t*)(uintptr_t)data, len), &w)));
    int32_t out = 0;
    for (int32_t i = 0; i < w; ++i)
    {
        char c = dst[i];
        if (c == '=') continue;
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
        dst[out++] = c;
    }
    assert_true(out < cap);
    dst[out] = '\0';
}

/* Build a structurally-valid manifest JWS: header carries alg=RS256 + an SJWK
 * (itself a JWS over a JWK signing key, signed by root key `testkid`); the
 * payload carries SHA-256(manifest) so the binding check passes. Signature bytes
 * are arbitrary because the mock verify hook does not validate them. */
static void build_jws(char* out, int32_t out_cap)
{
    uint8_t fixed_hash[32];
    memset(fixed_hash, ADU_TEST_HASH_BYTE, sizeof(fixed_hash));
    char hash_b64[64];
    int32_t w = 0;
    assert_true(az_result_succeeded(az_base64_encode(
        az_span_create((uint8_t*)hash_b64, (int32_t)sizeof(hash_b64)),
        az_span_create(fixed_hash, (int32_t)sizeof(fixed_hash)), &w)));
    hash_b64[w] = '\0';

    char pl_json[128];
    snprintf(pl_json, sizeof(pl_json), "{\"sha256\":\"%s\"}", hash_b64);
    char pl_b64[256];
    b64url_str(pl_json, (int32_t)strlen(pl_json), pl_b64, (int32_t)sizeof(pl_b64));

    static const char sjwk_hdr[] = "{\"alg\":\"RS256\",\"kid\":\"testkid\"}";
    char shdr_b64[128];
    b64url_str(sjwk_hdr, (int32_t)strlen(sjwk_hdr), shdr_b64, (int32_t)sizeof(shdr_b64));

    char n_b64[16];
    char e_b64[16];
    b64url_str(k_root_mod, (int32_t)sizeof(k_root_mod), n_b64, (int32_t)sizeof(n_b64));
    b64url_str(k_root_exp, (int32_t)sizeof(k_root_exp), e_b64, (int32_t)sizeof(e_b64));
    char sp_json[128];
    snprintf(sp_json, sizeof(sp_json), "{\"kty\":\"RSA\",\"n\":\"%s\",\"e\":\"%s\"}", n_b64, e_b64);
    char sp_b64[256];
    b64url_str(sp_json, (int32_t)strlen(sp_json), sp_b64, (int32_t)sizeof(sp_b64));

    const uint8_t dummy_sig[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    char sig_b64[16];
    b64url_str(dummy_sig, (int32_t)sizeof(dummy_sig), sig_b64, (int32_t)sizeof(sig_b64));

    char sjwk[800];
    snprintf(sjwk, sizeof(sjwk), "%s.%s.%s", shdr_b64, sp_b64, sig_b64);

    char mhdr_json[1024];
    snprintf(mhdr_json, sizeof(mhdr_json), "{\"alg\":\"RS256\",\"sjwk\":\"%s\"}", sjwk);
    char mhdr_b64[1536];
    b64url_str(mhdr_json, (int32_t)strlen(mhdr_json), mhdr_b64, (int32_t)sizeof(mhdr_b64));

    int n = snprintf(out, (size_t)out_cap, "%s.%s.%s", mhdr_b64, pl_b64, sig_b64);
    assert_true(n > 0 && n < out_cap);
}

/* Build a single-step deployment patch carrying a freshly built JWS with the
 * given workflow `id`. Returns a pointer to a static buffer (valid until the
 * next call). */
static const char* build_patch(const char* id)
{
    static char patch[4096];
    char jws[2048];
    build_jws(jws, (int32_t)sizeof(jws));

    char workflow[256];
    snprintf(workflow, sizeof(workflow), "\"workflow\":{\"action\":3,\"id\":\"%s\"}", id);

    int n = snprintf(patch, sizeof(patch), k_patch_fmt, workflow, "1.1", jws);
    assert_true(n > 0 && (size_t)n < sizeof(patch));
    return patch;
}

/* ------------------------------------------------------------------------- */
/* recording platform + crypto hooks (mocked)                                */
/* ------------------------------------------------------------------------- */

typedef enum
{
    OP_VERIFY = 1, OP_IS_INSTALLED, OP_DOWNLOAD, OP_BACKUP, OP_INSTALL,
    OP_APPLY, OP_RESTORE
} op_kind;

#define MAX_OPS 32

typedef struct
{
    op_kind ops[MAX_OPS];
    size_t    op_count;

    int32_t   download_result;
    int32_t   backup_result;
    int32_t   install_result;
    int32_t   apply_result;
    int32_t   restore_result;
    int32_t   is_installed_result;
    int32_t   verify_result;

    uint8_t   file_hash[32]; /* what the incremental SHA-256 mock returns */
    size_t    file_len;      /* bytes the read-back mock serves           */
} hook_log;

static void log_op(hook_log* l, op_kind k)
{
    if (l->op_count < MAX_OPS)
    {
        l->ops[l->op_count++] = k;
    }
}

static bool ops_contain(const hook_log* l, op_kind k)
{
    for (size_t i = 0; i < l->op_count; ++i)
    {
        if (l->ops[i] == k) return true;
    }
    return false;
}

/* Verify the ops appear in the given order (as an ordered subsequence). */
static bool ops_in_order(const hook_log* l, const op_kind* seq, size_t n)
{
    size_t si = 0;
    for (size_t i = 0; i < l->op_count && si < n; ++i)
    {
        if (l->ops[i] == seq[si]) si++;
    }
    return si == n;
}

static int32_t mock_download(
    const az_iot_adu_client_update_manifest_file* file, az_span url,
    uint32_t file_index, uint32_t file_count, void* ctx)
{
    (void)file; (void)url; (void)file_index; (void)file_count;
    hook_log* l = (hook_log*)ctx;
    log_op(l, OP_DOWNLOAD);
    return l->download_result;
}

static int32_t mock_install(
    const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
    (void)m; (void)step;
    hook_log* l = (hook_log*)ctx;
    log_op(l, OP_INSTALL);
    return l->install_result;
}

static int32_t mock_apply(
    const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
    (void)m; (void)step;
    hook_log* l = (hook_log*)ctx;
    log_op(l, OP_APPLY);
    return l->apply_result;
}

static int32_t mock_backup(
    const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
    (void)m; (void)step;
    hook_log* l = (hook_log*)ctx;
    log_op(l, OP_BACKUP);
    return l->backup_result;
}

static int32_t mock_restore(
    const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
    (void)m; (void)step;
    hook_log* l = (hook_log*)ctx;
    log_op(l, OP_RESTORE);
    return l->restore_result;
}

static int32_t mock_is_installed(
    const az_iot_adu_client_update_manifest* m, void* ctx)
{
    (void)m;
    hook_log* l = (hook_log*)ctx;
    log_op(l, OP_IS_INSTALLED);
    return l->is_installed_result;
}

static int32_t mock_verify_rs256(
    const uint8_t* mod, size_t mod_len, const uint8_t* exp, size_t exp_len,
    const uint8_t* signed_data, size_t signed_len,
    const uint8_t* sig, size_t sig_len, void* ctx)
{
    (void)mod; (void)mod_len; (void)exp; (void)exp_len;
    (void)signed_data; (void)signed_len; (void)sig; (void)sig_len;
    hook_log* l = (hook_log*)ctx;
    log_op(l, OP_VERIFY);
    return l->verify_result;
}

static int32_t mock_sha256(
    const uint8_t* data, size_t len, uint8_t out[32], void* ctx)
{
    (void)data; (void)len; (void)ctx;
    memset(out, ADU_TEST_HASH_BYTE, 32);
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

/* Serve l->file_len bytes of dummy content in chunks, then EOF. */
static int32_t mock_read_file(
    const az_iot_adu_client_update_manifest_file* file, uint32_t file_index,
    size_t offset, uint8_t* buffer, size_t buffer_size, size_t* out_read, void* ctx)
{
    (void)file; (void)file_index;
    hook_log* l = (hook_log*)ctx;
    if (offset >= l->file_len)
    {
        *out_read = 0;
        return AZ_IOT_ADU_RESULT_SUCCESS;
    }
    size_t remain = l->file_len - offset;
    size_t n = remain < buffer_size ? remain : buffer_size;
    memset(buffer, 0x55, n);
    *out_read = n;
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t mock_sha_init(void** ctx_out, void* ctx)
{
    (void)ctx;
    *ctx_out = (void*)(uintptr_t)1; /* non-NULL opaque handle */
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t mock_sha_update(void* c, const uint8_t* data, size_t len, void* ctx)
{
    (void)c; (void)data; (void)len; (void)ctx;
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t mock_sha_final(void* c, uint8_t out[32], void* ctx)
{
    (void)c;
    hook_log* l = (hook_log*)ctx;
    memcpy(out, l->file_hash, 32);
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

static void wire_hooks(hook_log* log,
                       az_iot_adu_platform_hooks* hooks,
                       az_iot_adu_crypto_hooks* crypto)
{
    memset(hooks, 0, sizeof(*hooks));
    memset(crypto, 0, sizeof(*crypto));

    log->download_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->backup_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->install_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->apply_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->restore_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->is_installed_result = AZ_IOT_ADU_RESULT_SUCCESS; /* SUCCESS => not already installed */
    log->verify_result = AZ_IOT_ADU_RESULT_SUCCESS;

    hooks->download_fn = mock_download;
    hooks->read_file_fn = mock_read_file;
    hooks->install_fn = mock_install;
    hooks->apply_fn = mock_apply;
    hooks->backup_fn = mock_backup;
    hooks->restore_fn = mock_restore;
    hooks->is_installed_fn = mock_is_installed;
    hooks->user_ctx = log;

    crypto->verify_rs256_fn = mock_verify_rs256;
    crypto->sha256_fn = mock_sha256;
    crypto->sha256_init_fn = mock_sha_init;
    crypto->sha256_update_fn = mock_sha_update;
    crypto->sha256_final_fn = mock_sha_final;
    crypto->user_ctx = log;

    /* The streaming file-hash matches the manifest so verification passes; serve
     * a multi-chunk file to exercise the read loop. */
    log->file_len = 700;
    int32_t hw = 0;
    assert_true(az_result_succeeded(az_base64_decode(
        az_span_create(log->file_hash, (int32_t)sizeof(log->file_hash)),
        AZ_SPAN_FROM_STR(ADU_TEST_FILE_HASH_B64), &hw)));
    assert_int_equal(hw, 32);
}

/* ------------------------------------------------------------------------- */
/* fixture                                                                   */
/* ------------------------------------------------------------------------- */

/* An ADU agent (twin + ADU client) bound to the shared device connection. One
 * agent is shared by every test in this group -- see adu_agent_get(). */
typedef struct
{
    az_iot_twin_client twin;
    az_iot_adu_client_t  adu;
    hook_log           log;
    uint8_t              dp_buf[256];
} adu_agent;

typedef struct
{
    e2e_device        dev;
    az_iot_e2e_service* service;
    adu_agent         agent;       /* shared by all tests; see adu_agent_get() */
    bool                agent_ready; /* lazily initialized on first use      */
} e2e_fixture;

static e2e_fixture g_fixture;

/* Initialize a twin + ADU client on the shared device connection, with mocked
 * hooks and Contoso/Foobar/1.0 device identity. */
static void adu_agent_init(adu_agent* a)
{
    memset(a, 0, sizeof(*a));
    assert_int_equal(az_iot_twin_client_init(&a->twin, &g_fixture.dev.conn), AZ_IOT_OK);

    az_iot_adu_platform_hooks hooks;
    az_iot_adu_crypto_hooks   crypto;
    wire_hooks(&a->log, &hooks, &crypto);

    az_iot_adu_device_properties dp = { 0 };
    dp.manufacturer = "Contoso";
    dp.model = "Foobar";
    dp.installed_update_id.provider = "Contoso";
    dp.installed_update_id.name = "Foobar";
    dp.installed_update_id.version = "1.0";

    az_iot_adu_client_config_options adu_opts = az_iot_adu_client_config_options_default();
    adu_opts.hooks = &hooks;
    adu_opts.crypto = &crypto;
    adu_opts.root_keys = k_root_keys;
    adu_opts.root_key_count = sizeof(k_root_keys) / sizeof(k_root_keys[0]);
    adu_opts.device_props = &dp;
    adu_opts.device_props_buffer = a->dp_buf;
    adu_opts.device_props_buffer_size = sizeof(a->dp_buf);
    assert_int_equal(
        az_iot_adu_client_initialize(&a->adu, &a->twin, &adu_opts),
        AZ_IOT_OK);
}

static void adu_agent_destroy(adu_agent* a)
{
    az_iot_adu_client_destroy(&a->adu);
    az_iot_twin_client_destroy(&a->twin);
}

/* Advance the device MQTT stack and drive the ADU state machine for one slice. */
static void adu_do_work(adu_agent* a, int ms)
{
    e2e_device_do_work(&g_fixture.dev, ms);
    (void)az_iot_adu_client_do_work(&a->adu);
}

/* Drive an in-flight service REST request to completion while keeping the device
 * and ADU state machine serviced. Returns the poll result (1 complete, 0 pending
 * timed out, -1 error). */
static int adu_drive_request(
    adu_agent* a, int* status, char* resp, size_t resp_size, int timeout_s)
{
    int rc = 0;
    time_t start = time(NULL);
    while ((time(NULL) - start) < timeout_s)
    {
        adu_do_work(a, E2E_ADU_PUMP_MS);
        rc = az_iot_e2e_service_request_poll(g_fixture.service, status, resp, resp_size);
        if (rc != 0)
        {
            break;
        }
    }
    return rc;
}

/* Build a short, run-unique workflow id. */
static void make_deploy_id(char* out, size_t cap)
{
    static unsigned counter = 0;
    unsigned long long t = (unsigned long long)time(NULL);
    unsigned r = (unsigned)rand();
    snprintf(out, cap, "e2e-adu-%08llx%04x%02x", t & 0xffffffffull, r & 0xffff, (counter++) & 0xff);
}

/* Cloud: PATCH a freshly-built single-step deployment (with a run-unique id)
 * into the device twin's desired properties and drive the REST call to
 * completion. The op log is reset first so the caller's assertions see ONLY this
 * deployment's platform ops: a fresh agent may have consumed a prior deployment
 * still sitting in desired properties while it subscribed (the startup twin GET
 * replays the last PATCH). Set any per-op result overrides on the agent (e.g. a
 * forced install failure) BEFORE calling this. */
static void adu_deploy(adu_agent* a, e2e_fixture* fx)
{
    a->log.op_count = 0;

    char deploy_id[64];
    make_deploy_id(deploy_id, sizeof(deploy_id));
    const char* patch = build_patch(deploy_id);

    assert_true(
        az_iot_e2e_service_twin_patch_desired_begin(fx->service, fx->dev.device_id, patch));

    int  status = 0;
    char resp[4096];
    int  rc = adu_drive_request(a, &status, resp, sizeof(resp), E2E_ADU_REST_TIMEOUT_S);
    if (rc != 1)
    {
        fprintf(stderr, "[e2e-adu] deployment desired patch rc=%d: %s\n",
            rc, az_iot_e2e_service_last_error(fx->service));
    }
    assert_int_equal(rc, 1);
    assert_int_equal(status, 200);
}

/* Device: pump the deployment forward until the workflow has returned to Idle
 * with `terminal_op` recorded (the last platform op of the expected path:
 * OP_APPLY on success, OP_RESTORE after a rollback, OP_VERIFY for a rejected
 * signature, OP_IS_INSTALLED for an already-installed no-op). Because adu_deploy
 * clears the op log, the presence of `terminal_op` unambiguously marks THIS
 * deployment reaching its terminal state -- robust even if the (mocked, fast)
 * workflow already completed while the desired PATCH was still being polled.
 * Returns true once that terminal state is observed, false on timeout. */
static bool adu_drive_until_idle(adu_agent* a, op_kind terminal_op)
{
    time_t start = time(NULL);
    while ((time(NULL) - start) < E2E_ADU_DEPLOY_TIMEOUT_S)
    {
        adu_do_work(a, E2E_ADU_PUMP_MS);
        if (az_iot_adu_client_get_state(&a->adu) == AZ_IOT_ADU_STATE_IDLE
            && ops_contain(&a->log, terminal_op))
        {
            return true;
        }
    }
    return false;
}

/* Cloud: flush the final reported-property publish, then read the device twin
 * back over REST into `resp`. Asserts the GET round-trip succeeds (HTTP 200). */
static void adu_read_reported_twin(
    adu_agent* a, e2e_fixture* fx, char* resp, size_t resp_size)
{
    for (int i = 0; i < E2E_ADU_SETTLE_ITERS; ++i)
    {
        adu_do_work(a, E2E_ADU_PUMP_MS);
    }

    assert_true(az_iot_e2e_service_twin_get_begin(fx->service, fx->dev.device_id));
    int status = 0;
    memset(resp, 0, resp_size);
    int rc = adu_drive_request(a, &status, resp, resp_size, E2E_ADU_REST_TIMEOUT_S);
    if (rc != 1)
    {
        fprintf(stderr, "[e2e-adu] reported twin get rc=%d: %s\n",
            rc, az_iot_e2e_service_last_error(fx->service));
    }
    assert_int_equal(rc, 1);
    assert_int_equal(status, 200);
}

/* Lazily create the single ADU agent shared by every test in this group. One
 * twin+ADU client is reused across all tests because az_iot_twin_client_init
 * registers connection subscriptions that persist for the connection's lifetime
 * (there is no per-client unsubscribe); churning a fresh twin per test would
 * exhaust AZ_IOT_MAX_PERSISTENT_SUBS. It also mirrors a real device, which runs
 * one ADU agent across many deployments. Torn down in group_teardown. */
static adu_agent* adu_agent_get(e2e_fixture* fx)
{
    if (!fx->agent_ready)
    {
        adu_agent_init(&fx->agent);
        fx->agent_ready = true;
    }
    return &fx->agent;
}

/* Reset the mocked platform/crypto outcomes to their all-success defaults and
 * clear the recorded op log, so each test drives an independent deployment
 * through the shared agent. The wired hook function pointers and the file
 * hash/length set by adu_agent_init are left intact. */
static void adu_log_reset(hook_log* log)
{
    log->op_count            = 0;
    log->download_result     = AZ_IOT_ADU_RESULT_SUCCESS;
    log->backup_result       = AZ_IOT_ADU_RESULT_SUCCESS;
    log->install_result      = AZ_IOT_ADU_RESULT_SUCCESS;
    log->apply_result        = AZ_IOT_ADU_RESULT_SUCCESS;
    log->restore_result      = AZ_IOT_ADU_RESULT_SUCCESS;
    log->is_installed_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->verify_result       = AZ_IOT_ADU_RESULT_SUCCESS;
}

/* ---- group setup / teardown ----------------------------------------------- */

static int group_setup(void** state)
{
    memset(&g_fixture, 0, sizeof(g_fixture));
    srand((unsigned)time(NULL));

    az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_ERROR);
    az_iot_log_set_global_sink(&log);

    const char* svc_err = NULL;
    g_fixture.service = az_iot_e2e_service_create(&svc_err);
    if (g_fixture.service == NULL)
    {
        fprintf(stderr, "[e2e-adu] service client create failed: %s\n",
            (svc_err != NULL) ? svc_err : "unknown");
        return -1;
    }

    if (e2e_device_connect(&g_fixture.dev) != 0)
    {
        az_iot_e2e_service_destroy(g_fixture.service);
        g_fixture.service = NULL;
        return -1;
    }

    *state = &g_fixture;
    return 0;
}

static int group_teardown(void** state)
{
    e2e_fixture* fx = (e2e_fixture*)*state;
    if (fx == NULL)
    {
        return 0;
    }
    if (fx->agent_ready)
    {
        adu_agent_destroy(&fx->agent);
        fx->agent_ready = false;
    }
    e2e_device_disconnect(&fx->dev);
    if (fx->service != NULL)
    {
        az_iot_e2e_service_destroy(fx->service);
        fx->service = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

/* Foundational: on startup the ADU agent reports its identity to the twin. The
 * cloud reads it back, proving the device<->twin ADU reporting path works before
 * the heavier deployment scenario runs. */
static void test_adu_agent_state_report(void** state)
{
    e2e_fixture* fx = (e2e_fixture*)*state;
    adu_agent*   a  = adu_agent_get(fx);

    /* Subscribe to the twin, run the startup twin GET, and publish the initial
     * agent-state reported property. */
    for (int i = 0; i < E2E_ADU_SETTLE_ITERS; ++i)
    {
        adu_do_work(a, E2E_ADU_PUMP_MS);
    }

    assert_true(az_iot_e2e_service_twin_get_begin(fx->service, fx->dev.device_id));
    int  status = 0;
    static char resp[32768];
    memset(resp, 0, sizeof(resp));
    int rc = adu_drive_request(a, &status, resp, sizeof(resp), E2E_ADU_REST_TIMEOUT_S);
    if (rc != 1)
    {
        fprintf(stderr, "[e2e-adu] agent-state twin get rc=%d: %s\n",
            rc, az_iot_e2e_service_last_error(fx->service));
    }
    assert_int_equal(rc, 1);
    assert_int_equal(status, 200);

    /* The reported deviceUpdate agent carries the device identity + installed
     * update id (Contoso / Foobar). */
    assert_non_null(strstr(resp, "deviceUpdate"));
    assert_non_null(strstr(resp, "Contoso"));
    assert_non_null(strstr(resp, "Foobar"));
}

/* High-value: the cloud PATCHes a signed deployment into desired properties and
 * the device runs the full workflow to completion, reporting success. */
static void test_adu_update_deployment(void** state)
{
    e2e_fixture* fx = (e2e_fixture*)*state;
    adu_agent*   a  = adu_agent_get(fx);
    adu_log_reset(&a->log);

    /* Cloud: PATCH the signed deployment; device: drive it to completion. On the
     * happy path the final platform op is OP_APPLY. */
    adu_deploy(a, fx);
    assert_true(adu_drive_until_idle(a, OP_APPLY));

    /* The full happy-path op sequence ran, with no rollback. */
    static const op_kind expect[]
        = { OP_VERIFY, OP_IS_INSTALLED, OP_DOWNLOAD, OP_BACKUP, OP_INSTALL, OP_APPLY };
    assert_true(ops_in_order(&a->log, expect, sizeof(expect) / sizeof(expect[0])));
    assert_false(ops_contain(&a->log, OP_RESTORE));

    /* Cloud: the reported twin shows the deployment succeeded (result code 700). */
    static char resp[32768];
    adu_read_reported_twin(a, fx, resp, sizeof(resp));
    assert_non_null(strstr(resp, "deviceUpdate"));
    assert_non_null(strstr(resp, "\"resultCode\":700")); /* AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS */
}

/* Error path: the platform install step fails, so the agent rolls the
 * backed-up step back and reports a non-success result. The ADU unit tests cover
 * this state machine in isolation (adu_client_test.c); the e2e value here is
 * proving the FAILURE outcome round-trips through the real hub twin -- reported
 * result code 695 (= 700 - AZ_IOT_ADU_FACILITY_INSTALL). */
static void test_adu_install_failure_rollback(void** state)
{
    e2e_fixture* fx = (e2e_fixture*)*state;
    adu_agent*   a  = adu_agent_get(fx);
    adu_log_reset(&a->log);

    /* Force the install step to fail; the workflow must back out via restore. */
    a->log.install_result = AZ_IOT_ADU_RESULT_FAILURE;

    adu_deploy(a, fx);
    assert_true(adu_drive_until_idle(a, OP_RESTORE));

    /* Download + backup happened, install was attempted and failed, rollback ran
     * for the backed-up step, and apply never did. */
    static const op_kind expect[]
        = { OP_VERIFY, OP_IS_INSTALLED, OP_DOWNLOAD, OP_BACKUP, OP_INSTALL, OP_RESTORE };
    assert_true(ops_in_order(&a->log, expect, sizeof(expect) / sizeof(expect[0])));
    assert_false(ops_contain(&a->log, OP_APPLY));

    /* Cloud: the reported twin carries the install-failure result code. */
    static char resp[32768];
    adu_read_reported_twin(a, fx, resp, sizeof(resp));
    assert_non_null(strstr(resp, "deviceUpdate"));
    assert_non_null(strstr(resp, "\"resultCode\":695")); /* 700 - FACILITY_INSTALL */
}

/* Error path: manifest signature verification fails, so the deployment is
 * rejected before anything is downloaded or installed. Proves the reject-early
 * failure is reported through the real hub twin -- reported result code 699
 * (= 700 - AZ_IOT_ADU_FACILITY_MANIFEST). */
static void test_adu_verify_rejects_deployment(void** state)
{
    e2e_fixture* fx = (e2e_fixture*)*state;
    adu_agent*   a  = adu_agent_get(fx);
    adu_log_reset(&a->log);

    /* Force signature verification to reject the manifest. */
    a->log.verify_result = AZ_IOT_ADU_RESULT_FAILURE;

    adu_deploy(a, fx);
    assert_true(adu_drive_until_idle(a, OP_VERIFY));

    /* Verification ran and rejected the update: the install-eligibility check,
     * download, install and apply never followed. */
    assert_true(ops_contain(&a->log, OP_VERIFY));
    assert_false(ops_contain(&a->log, OP_IS_INSTALLED));
    assert_false(ops_contain(&a->log, OP_DOWNLOAD));
    assert_false(ops_contain(&a->log, OP_INSTALL));
    assert_false(ops_contain(&a->log, OP_APPLY));

    /* Cloud: the reported twin carries the manifest-verification failure code. */
    static char resp[32768];
    adu_read_reported_twin(a, fx, resp, sizeof(resp));
    assert_non_null(strstr(resp, "deviceUpdate"));
    assert_non_null(strstr(resp, "\"resultCode\":699")); /* 700 - FACILITY_MANIFEST */
}

/* Idempotency path: the device reports the target update is already installed,
 * so the agent accepts and short-circuits the deployment without downloading or
 * installing anything. Proves the real hub twin delivers a deployment the device
 * recognizes as already applied and that it returns cleanly to Idle. */
static void test_adu_already_installed_noop(void** state)
{
    e2e_fixture* fx = (e2e_fixture*)*state;
    adu_agent*   a  = adu_agent_get(fx);
    adu_log_reset(&a->log);

    /* Report the target update as already installed. */
    a->log.is_installed_result = AZ_IOT_ADU_RESULT_ALREADY_INSTALLED;

    adu_deploy(a, fx);
    assert_true(adu_drive_until_idle(a, OP_IS_INSTALLED));

    /* Verification and the installed check ran, but the payload was never
     * downloaded, installed, applied, or rolled back. */
    assert_true(ops_contain(&a->log, OP_VERIFY));
    assert_true(ops_contain(&a->log, OP_IS_INSTALLED));
    assert_false(ops_contain(&a->log, OP_DOWNLOAD));
    assert_false(ops_contain(&a->log, OP_INSTALL));
    assert_false(ops_contain(&a->log, OP_APPLY));
    assert_false(ops_contain(&a->log, OP_RESTORE));

    /* Cloud: the agent republished its state; the reporting path still works. */
    static char resp[32768];
    adu_read_reported_twin(a, fx, resp, sizeof(resp));
    assert_non_null(strstr(resp, "deviceUpdate"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_adu_agent_state_report),
        cmocka_unit_test(test_adu_update_deployment),
        cmocka_unit_test(test_adu_install_failure_rollback),
        cmocka_unit_test(test_adu_verify_rejects_deployment),
        cmocka_unit_test(test_adu_already_installed_noop),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
