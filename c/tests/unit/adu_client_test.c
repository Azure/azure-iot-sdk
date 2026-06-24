// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* ADU client (Phase 1) unit tests. Drives the state machine through the public
 * API + the in-memory mock_mqtt_iface, with recording platform/crypto hooks.
 *
 * The service-property + manifest payloads are taken from azure-sdk-for-c's own
 * parser tests (the only known parser-valid v5 manifest) and wrapped in the
 * real twin component envelope {"deviceUpdate":{"__t":"c","service":{...}}}. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include <azure/core/az_base64.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_twin_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_adu.h"

#include "support/mock_mqtt_iface.h"

/* ------------------------------------------------------------------------- */
/* parser-valid payloads (from azure-sdk-for-c test_az_iot_adu.c)            */
/* ------------------------------------------------------------------------- */

/* A single-step, single-file v5 deployment wrapped in the deviceUpdate
 * component envelope. The `%s` is filled at runtime with a structurally-valid
 * JWS (see signed_patch()): core fully parses the JWS/SJWK chain, so a real
 * compact-token structure is required even though the mock crypto hook does not
 * check the signature bytes themselves. */
static const char k_patch_fmt[]
    = "{\"deviceUpdate\":{\"__t\":\"c\",\"service\":{"
      "\"workflow\":{\"action\":3,\"id\":\"51552a54-765e-419f-892a-c822549b6f38\"},"
      "\"updateManifest\":\"{\\\"manifestVersion\\\":\\\"5\\\",\\\"updateId\\\":{\\\"provider\\\":"
      "\\\"Contoso\\\",\\\"name\\\":\\\"Foobar\\\",\\\"version\\\":\\\"1.1\\\"},"
      "\\\"compatibility\\\":[{\\\"deviceManufacturer\\\":\\\"Contoso\\\",\\\"deviceModel\\\":"
      "\\\"Foobar\\\"}],\\\"instructions\\\":{\\\"steps\\\":[{\\\"handler\\\":\\\"microsoft/"
      "swupdate:1\\\",\\\"files\\\":[\\\"f2f4a804ca17afbae\\\"],\\\"handlerProperties\\\":{"
      "\\\"installedCriteria\\\":\\\"1.0\\\"}}]},\\\"files\\\":{\\\"f2f4a804ca17afbae\\\":{"
      "\\\"fileName\\\":\\\"iot-middleware-sample-adu-v1.1\\\",\\\"sizeInBytes\\\":844976,"
      "\\\"hashes\\\":{\\\"sha256\\\":\\\"xsoCnYAMkZZ7m9RL9Vyg9jKfFehCNxyuPFaJVM/"
      "WBi0=\\\"}}},\\\"createdDateTime\\\":\\\"2022-07-07T03:02:48.8449038Z\\\"}\","
      "\"updateManifestSignature\":\"%s\","
      "\"fileUrls\":{\"f2f4a804ca17afbae\":\"http://example.com/payload.bin\"}}}}";

/* The fixed digest the mock SHA-256 returns; the JWS payload below carries its
 * base64, so the manifest-binding check (step 6 of verify) passes regardless of
 * the actual manifest bytes. */
#define ADU_TEST_HASH_BYTE 0xAB

/* The base64 SHA-256 the test manifest carries for its single file. The
 * streaming file-hash mock returns the decoded bytes so verification passes;
 * the mismatch test overrides it. */
#define ADU_TEST_FILE_HASH_B64 "xsoCnYAMkZZ7m9RL9Vyg9jKfFehCNxyuPFaJVM/WBi0="

/* Root key trusted to sign the SJWK; matched by `kid`. The mock verify hook
 * ignores the key bytes, so dummy modulus/exponent are sufficient here. */
static const uint8_t k_root_mod[] = { 0x01, 0x02, 0x03 };
static const uint8_t k_root_exp[] = { 0x01, 0x00, 0x01 };
static const az_iot_adu_root_key_t k_root_keys[]
    = { { "testkid", k_root_mod, sizeof(k_root_mod), k_root_exp, sizeof(k_root_exp), false } };

/* base64url-encode `data` into a NUL-terminated C string. az_core 1.5.0 ships
 * only standard base64 encode, so we translate +/ to -_ and drop padding. */
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
 * (itself a JWS over a JWK signing key, signed by the root key `testkid`); the
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

/* Return the single-step patch with a freshly built, structurally-valid JWS. */
static const char* signed_patch(void)
{
    static char patch[4096];
    char jws[2048];
    build_jws(jws, (int32_t)sizeof(jws));
    int n = snprintf(patch, sizeof(patch), k_patch_fmt, jws);
    assert_true(n > 0 && (size_t)n < sizeof(patch));
    return patch;
}

/* A Cancel action (action=255), no manifest. */
static const char k_patch_cancel[]
    = "{\"deviceUpdate\":{\"__t\":\"c\",\"service\":{"
      "\"workflow\":{\"action\":255,\"id\":\"nodeployment\"},"
      "\"updateManifest\":null,\"updateManifestSignature\":null,\"fileUrls\":null}}}";

/* ------------------------------------------------------------------------- */
/* recording hooks                                                           */
/* ------------------------------------------------------------------------- */

typedef enum
{
    OP_VERIFY = 1, OP_IS_INSTALLED, OP_DOWNLOAD, OP_BACKUP, OP_INSTALL,
    OP_APPLY, OP_RESTORE
} op_kind_t;

#define MAX_OPS 32

typedef struct
{
    op_kind_t ops[MAX_OPS];
    uint32_t  op_steps[MAX_OPS];
    size_t    op_count;

    /* Configurable per-op return codes (default SUCCESS). */
    int32_t   download_result;
    int32_t   backup_result;
    int32_t   install_result;
    int32_t   apply_result;
    int32_t   restore_result;
    int32_t   is_installed_result;
    int32_t   verify_result;

    bool      install_in_progress_once; /* first install returns IN_PROGRESS */
    bool      install_in_progress_consumed;

    /* Streaming per-file hash verification. */
    uint8_t   file_hash[32];  /* what the incremental SHA-256 mock returns */
    size_t    file_len;       /* bytes the read-back mock serves */

    /* Persistence / resume. */
    uint8_t   persist_blob[AZ_IOT_ADU_REQUEST_BUFFER_SIZE + 128];
    size_t    persist_len;
    bool      have_persist;
} hook_log_t;

static void log_op(hook_log_t* l, op_kind_t k, uint32_t step)
{
    if (l->op_count < MAX_OPS)
    {
        l->ops[l->op_count] = k;
        l->op_steps[l->op_count] = step;
        l->op_count++;
    }
}

static int32_t mock_download(
    const az_iot_adu_client_update_manifest_file* file, az_span url,
    uint32_t file_index, uint32_t file_count, void* ctx)
{
    (void)file; (void)url; (void)file_index; (void)file_count;
    hook_log_t* l = (hook_log_t*)ctx;
    log_op(l, OP_DOWNLOAD, file_index);
    return l->download_result;
}

static int32_t mock_install(
    const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
    (void)m;
    hook_log_t* l = (hook_log_t*)ctx;
    log_op(l, OP_INSTALL, step);
    if (l->install_in_progress_once && !l->install_in_progress_consumed)
    {
        l->install_in_progress_consumed = true;
        return AZ_IOT_ADU_RESULT_IN_PROGRESS;
    }
    return l->install_result;
}

static int32_t mock_apply(
    const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
    (void)m;
    hook_log_t* l = (hook_log_t*)ctx;
    log_op(l, OP_APPLY, step);
    return l->apply_result;
}

static int32_t mock_backup(
    const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
    (void)m;
    hook_log_t* l = (hook_log_t*)ctx;
    log_op(l, OP_BACKUP, step);
    return l->backup_result;
}

static int32_t mock_restore(
    const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
    (void)m;
    hook_log_t* l = (hook_log_t*)ctx;
    log_op(l, OP_RESTORE, step);
    return l->restore_result;
}

static int32_t mock_is_installed(
    const az_iot_adu_client_update_manifest* m, void* ctx)
{
    (void)m;
    hook_log_t* l = (hook_log_t*)ctx;
    log_op(l, OP_IS_INSTALLED, 0);
    return l->is_installed_result;
}

static int32_t mock_verify_rs256(
    const uint8_t* mod, size_t mod_len, const uint8_t* exp, size_t exp_len,
    const uint8_t* signed_data, size_t signed_len,
    const uint8_t* sig, size_t sig_len, void* ctx)
{
    (void)mod; (void)mod_len; (void)exp; (void)exp_len;
    (void)signed_data; (void)signed_len; (void)sig; (void)sig_len;
    hook_log_t* l = (hook_log_t*)ctx;
    log_op(l, OP_VERIFY, 0);
    return l->verify_result;
}

static int32_t mock_sha256(
    const uint8_t* data, size_t len, uint8_t out[32], void* ctx)
{
    (void)data; (void)len; (void)ctx;
    memset(out, ADU_TEST_HASH_BYTE, 32);
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

/* Serve fx->log.file_len bytes of dummy content in chunks, then EOF. */
static int32_t mock_read_file(
    const az_iot_adu_client_update_manifest_file* file, uint32_t file_index,
    size_t offset, uint8_t* buffer, size_t buffer_size, size_t* out_read, void* ctx)
{
    (void)file; (void)file_index;
    hook_log_t* l = (hook_log_t*)ctx;
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
    hook_log_t* l = (hook_log_t*)ctx;
    memcpy(out, l->file_hash, 32);
    return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t mock_persist(const uint8_t* blob, size_t len, void* ctx)
{
    hook_log_t* l = (hook_log_t*)ctx;
    assert_true(len <= sizeof(l->persist_blob));
    memcpy(l->persist_blob, blob, len);
    l->persist_len = len;
    l->have_persist = true;
    return 0;
}

static int32_t mock_load(uint8_t* blob, size_t cap, size_t* out_len, void* ctx)
{
    hook_log_t* l = (hook_log_t*)ctx;
    if (!l->have_persist || l->persist_len > cap) return 1; /* nothing persisted */
    memcpy(blob, l->persist_blob, l->persist_len);
    *out_len = l->persist_len;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* fixture                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
    az_iot_connection_client_t conn;
    az_iot_twin_client_t       twin;
    az_iot_adu_client_t        adu;
    az_iot_mqtt_factory_t*     factory;
    az_iot_mock_mqtt_client_t* mock;

    hook_log_t                 log;
    uint8_t                    dp_buf[256];
} fixture_t;

static void wire_hooks(hook_log_t* log,
                       az_iot_adu_platform_hooks_t* hooks,
                       az_iot_adu_crypto_hooks_t* crypto)
{
    memset(hooks, 0, sizeof(*hooks));
    memset(crypto, 0, sizeof(*crypto));

    /* default all results to SUCCESS */
    log->download_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->backup_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->install_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->apply_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->restore_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->is_installed_result = AZ_IOT_ADU_RESULT_SUCCESS;
    log->verify_result = AZ_IOT_ADU_RESULT_SUCCESS;

    hooks->download_fn = mock_download;
    hooks->read_file_fn = mock_read_file;
    hooks->install_fn = mock_install;
    hooks->apply_fn = mock_apply;
    hooks->backup_fn = mock_backup;
    hooks->restore_fn = mock_restore;
    hooks->is_installed_fn = mock_is_installed;
    hooks->persist_state_fn = mock_persist;
    hooks->load_state_fn = mock_load;
    hooks->user_ctx = log;

    crypto->verify_rs256_fn = mock_verify_rs256;
    crypto->sha256_fn = mock_sha256;
    crypto->sha256_init_fn = mock_sha_init;
    crypto->sha256_update_fn = mock_sha_update;
    crypto->sha256_final_fn = mock_sha_final;
    crypto->user_ctx = log;

    /* By default the streaming file-hash matches the manifest, so the happy
     * paths pass verification. Serve a multi-chunk file to exercise the loop. */
    log->file_len = 700;
    int32_t hw = 0;
    assert_true(az_result_succeeded(az_base64_decode(
        az_span_create(log->file_hash, (int32_t)sizeof(log->file_hash)),
        AZ_SPAN_FROM_STR(ADU_TEST_FILE_HASH_B64), &hw)));
    assert_int_equal(hw, 32);
}

static void init_hooks(fixture_t* fx,
                       az_iot_adu_platform_hooks_t* hooks,
                       az_iot_adu_crypto_hooks_t* crypto)
{
    wire_hooks(&fx->log, hooks, crypto);
}

static int setup(void** state)
{
    fixture_t* fx = (fixture_t*)calloc(1, sizeof(*fx));
    assert_non_null(fx);

    az_iot_connection_client_options_t opts = {0};
    opts.host = "broker.example";
    opts.port = 8883;
    opts.client_id = "ut-device";
    assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

    fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
    assert_non_null(fx->factory);

    assert_int_equal(az_iot_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);

    az_iot_adu_platform_hooks_t hooks;
    az_iot_adu_crypto_hooks_t crypto;
    init_hooks(fx, &hooks, &crypto);

    az_iot_adu_device_properties_t dp = {0};
    dp.manufacturer = "Contoso";
    dp.model = "Foobar";
    dp.installed_update_id.provider = "Contoso";
    dp.installed_update_id.name = "Foobar";
    dp.installed_update_id.version = "1.0";

    assert_int_equal(
        az_iot_adu_client_initialize(
            &fx->adu, &fx->twin, &hooks, &crypto,
            k_root_keys, sizeof(k_root_keys) / sizeof(k_root_keys[0]),
            &dp, fx->dp_buf, sizeof(fx->dp_buf)),
        AZ_IOT_OK);

    *state = fx;
    return 0;
}

static int teardown(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    if (fx)
    {
        /* A registered factory is adopted by the connection client and freed by
         * its deinit(); an unregistered one (tests that never call
         * open_to_connected) is still owned by the test and must be destroyed
         * here. Check before deinit() clears factory_count. */
        bool factory_adopted = (fx->conn.factory_count > 0);
        az_iot_adu_client_deinit(&fx->adu);
        az_iot_twin_client_deinit(&fx->twin);
        az_iot_connection_client_deinit(&fx->conn);
        if (!factory_adopted)
            az_iot_mock_mqtt_factory_destroy(fx->factory);
        free(fx);
    }
    return 0;
}

static void open_to_connected(fixture_t* fx)
{
    assert_int_equal(
        az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
    fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(fx->mock);
    assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
    az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

static void inject_patch(fixture_t* fx, const char* body)
{
    char topic[] = "$iothub/twin/PATCH/properties/desired/?$version=7";
    assert_true(az_iot_mock_mqtt_client_inject_message(
        fx->mock, topic, (const uint8_t*)body, strlen(body), AZ_IOT_MQTT_QOS_0));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

/* Pump the ADU state machine until Idle or a max iteration cap. */
static void pump(fixture_t* fx, int max_iters)
{
    for (int i = 0; i < max_iters; ++i)
    {
        assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
        if (az_iot_adu_client_get_state(&fx->adu) == AZ_IOT_ADU_STATE_IDLE &&
            i > 0)
        {
            break;
        }
    }
}

static bool ops_contain_sequence(const hook_log_t* l, const op_kind_t* seq, size_t n)
{
    if (l->op_count < n) return false;
    /* Find seq as an ordered (contiguous-relative) subsequence. */
    size_t si = 0;
    for (size_t i = 0; i < l->op_count && si < n; ++i)
    {
        if (l->ops[i] == seq[si]) si++;
    }
    return si == n;
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void init_starts_idle_and_pending_report(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
    /* The startup device-properties report is pending; first do_work consumes
     * it and stays Idle. */
    open_to_connected(fx);
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void deployment_drives_full_workflow_single_step(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    inject_patch(fx, signed_patch());
    /* The patch moves us out of Idle into ManifestReceived. */
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu),
                     AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);

    pump(fx, 40);

    /* Ends back at Idle after a successful single-step deployment. */
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);

    /* Expected ordered op sequence for one step. */
    static const op_kind_t expect[]
        = { OP_VERIFY, OP_IS_INSTALLED, OP_DOWNLOAD, OP_BACKUP, OP_INSTALL, OP_APPLY };
    assert_true(ops_contain_sequence(&fx->log, expect, sizeof(expect) / sizeof(expect[0])));
    /* No rollback on the happy path. */
    for (size_t i = 0; i < fx->log.op_count; ++i)
    {
        assert_int_not_equal(fx->log.ops[i], OP_RESTORE);
    }
}

static void verify_failure_blocks_download_and_fails(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    fx->log.verify_result = AZ_IOT_ADU_RESULT_FAILURE;
    inject_patch(fx, signed_patch());
    pump(fx, 40);

    /* Verify ran; download never did. */
    bool saw_verify = false, saw_download = false;
    for (size_t i = 0; i < fx->log.op_count; ++i)
    {
        if (fx->log.ops[i] == OP_VERIFY) saw_verify = true;
        if (fx->log.ops[i] == OP_DOWNLOAD) saw_download = true;
    }
    assert_true(saw_verify);
    assert_false(saw_download);
    /* Terminal: machine returns to Idle after reporting FAILED. */
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void install_failure_triggers_rollback(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    fx->log.install_result = AZ_IOT_ADU_RESULT_FAILURE;
    inject_patch(fx, signed_patch());
    pump(fx, 40);

    /* Install ran and failed; restore (rollback) ran for step 0; apply never ran. */
    bool saw_install = false, saw_restore = false, saw_apply = false;
    for (size_t i = 0; i < fx->log.op_count; ++i)
    {
        if (fx->log.ops[i] == OP_INSTALL) saw_install = true;
        if (fx->log.ops[i] == OP_RESTORE) saw_restore = true;
        if (fx->log.ops[i] == OP_APPLY) saw_apply = true;
    }
    assert_true(saw_install);
    assert_true(saw_restore);
    assert_false(saw_apply);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void hash_mismatch_blocks_install_and_fails(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    /* Make the streaming SHA-256 disagree with the manifest's expected hash. */
    memset(fx->log.file_hash, 0x00, sizeof(fx->log.file_hash));
    inject_patch(fx, signed_patch());
    pump(fx, 40);

    /* Download happened, but the hash check failed before install/apply. */
    bool saw_download = false, saw_install = false, saw_apply = false;
    for (size_t i = 0; i < fx->log.op_count; ++i)
    {
        if (fx->log.ops[i] == OP_DOWNLOAD) saw_download = true;
        if (fx->log.ops[i] == OP_INSTALL) saw_install = true;
        if (fx->log.ops[i] == OP_APPLY) saw_apply = true;
    }
    assert_true(saw_download);
    assert_false(saw_install);
    assert_false(saw_apply);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void already_installed_is_rejected_without_download(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    fx->log.is_installed_result = AZ_IOT_ADU_RESULT_ALREADY_INSTALLED;
    inject_patch(fx, signed_patch());
    pump(fx, 40);

    /* is_installed consulted; download never ran; back to Idle. */
    bool saw_is_installed = false, saw_download = false;
    for (size_t i = 0; i < fx->log.op_count; ++i)
    {
        if (fx->log.ops[i] == OP_IS_INSTALLED) saw_is_installed = true;
        if (fx->log.ops[i] == OP_DOWNLOAD) saw_download = true;
    }
    assert_true(saw_is_installed);
    assert_false(saw_download);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void install_in_progress_reenters_then_completes(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    fx->log.install_in_progress_once = true;
    inject_patch(fx, signed_patch());
    pump(fx, 40);

    /* Install was called at least twice (once IN_PROGRESS, once SUCCESS), then
     * apply ran and we finished Idle. */
    int install_calls = 0;
    bool saw_apply = false;
    for (size_t i = 0; i < fx->log.op_count; ++i)
    {
        if (fx->log.ops[i] == OP_INSTALL) install_calls++;
        if (fx->log.ops[i] == OP_APPLY) saw_apply = true;
    }
    assert_true(install_calls >= 2);
    assert_true(saw_apply);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void reboot_required_persists_and_resumes(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    /* Install requires a reboot: the workflow snapshots itself via persist. */
    fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
    inject_patch(fx, signed_patch());
    pump(fx, 40);
    assert_true(fx->log.have_persist);
    assert_true(fx->log.persist_len > 40);

    /* Simulate a reboot: forget the in-RAM workflow and the pre-reboot op log,
     * then resume purely from the persisted blob (post-reboot the install is
     * already applied, so it now reports SUCCESS). */
    fx->log.op_count = 0;
    fx->log.install_result = AZ_IOT_ADU_RESULT_SUCCESS;

    assert_int_equal(az_iot_adu_client_resume(&fx->adu), AZ_IOT_OK);
    /* Resumed at the post-install boundary, not Idle. */
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu),
                     AZ_IOT_ADU_STATE_INSTALL_COMPLETE);

    /* Continue: apply runs for the resumed step and the workflow finishes. */
    pump(fx, 40);
    bool saw_apply = false;
    for (size_t i = 0; i < fx->log.op_count; ++i)
    {
        if (fx->log.ops[i] == OP_APPLY) saw_apply = true;
    }
    assert_true(saw_apply);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void resume_with_no_persisted_state_stays_idle(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    /* No blob persisted yet: resume is a clean no-op. */
    assert_false(fx->log.have_persist);
    assert_int_equal(az_iot_adu_client_resume(&fx->adu), AZ_IOT_OK);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void cancel_action_sets_cancelled_flag(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    /* Start a deployment, then cancel mid-flight at a phase boundary. */
    inject_patch(fx, signed_patch());
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK); /* ManifestReceived -> Verifying */

    inject_patch(fx, k_patch_cancel);
    assert_true(az_iot_adu_is_cancelled(&fx->adu));

    /* Next do_work honors cancellation and returns to Idle. */
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
    assert_false(az_iot_adu_is_cancelled(&fx->adu));
}

static void update_device_properties_sets_report_pending(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);
    /* drain the startup report */
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
    az_iot_mock_mqtt_client_clear_calls(fx->mock);

    az_iot_adu_device_properties_t dp = {0};
    dp.manufacturer = "Contoso";
    dp.model = "Foobar2";
    dp.installed_update_id.provider = "Contoso";
    dp.installed_update_id.name = "Foobar";
    dp.installed_update_id.version = "2.0";
    assert_int_equal(
        az_iot_adu_client_update_device_properties(&fx->adu, &dp), AZ_IOT_OK);

    /* Next do_work publishes a reported-property PATCH. */
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);

    size_t n = az_iot_mock_mqtt_client_call_count(fx->mock);
    bool saw_reported_publish = false;
    for (size_t i = 0; i < n; ++i)
    {
        const az_iot_mock_call_t* c = az_iot_mock_mqtt_client_call_at(fx->mock, i);
        if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH && c->topic[0] != '\0' &&
            strstr(c->topic, "twin/PATCH/properties/reported") != NULL)
        {
            saw_reported_publish = true;
        }
    }
    assert_true(saw_reported_publish);
}

/* Search a recorded PUBLISH payload for a literal needle. */
static bool payload_contains(const az_iot_mock_call_t* c, const char* needle)
{
    size_t nlen = strlen(needle);
    if (c->payload_len < nlen) return false;
    for (size_t i = 0; i + nlen <= c->payload_len; ++i)
    {
        if (memcmp(c->payload + i, needle, nlen) == 0) return true;
    }
    return false;
}

static void custom_device_properties_are_reported(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);
    /* drain the startup report */
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
    az_iot_mock_mqtt_client_clear_calls(fx->mock);

    static const az_iot_adu_custom_property_t customs[] = {
        { "location", "building42" },
        { "tier", "gold" },
    };
    az_iot_adu_device_properties_t dp = {0};
    dp.manufacturer = "Contoso";
    dp.model = "Foobar";
    dp.installed_update_id.provider = "Contoso";
    dp.installed_update_id.name = "Foobar";
    dp.installed_update_id.version = "1.0";
    dp.custom_properties = customs;
    dp.custom_properties_count = sizeof(customs) / sizeof(customs[0]);
    assert_int_equal(
        az_iot_adu_client_update_device_properties(&fx->adu, &dp), AZ_IOT_OK);

    /* Next do_work publishes the reported-property PATCH carrying the customs. */
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);

    size_t n = az_iot_mock_mqtt_client_call_count(fx->mock);
    bool saw_customs = false;
    for (size_t i = 0; i < n; ++i)
    {
        const az_iot_mock_call_t* c = az_iot_mock_mqtt_client_call_at(fx->mock, i);
        if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH &&
            strstr(c->topic, "twin/PATCH/properties/reported") != NULL &&
            payload_contains(c, "location") && payload_contains(c, "building42") &&
            payload_contains(c, "tier") && payload_contains(c, "gold"))
        {
            saw_customs = true;
        }
    }
    assert_true(saw_customs);
}

static void device_props_too_small_is_rejected(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    (void)fx;

    az_iot_connection_client_t conn;
    az_iot_twin_client_t twin;
    az_iot_adu_client_t adu;
    az_iot_connection_client_options_t opts = {0};
    opts.host = "broker.example";
    opts.port = 8883;
    opts.client_id = "ut-device2";
    assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);
    assert_int_equal(az_iot_twin_client_init(&twin, &conn), AZ_IOT_OK);

    hook_log_t log = {0};
    az_iot_adu_platform_hooks_t hooks = {0};
    az_iot_adu_crypto_hooks_t crypto = {0};
    hooks.install_fn = mock_install;
    hooks.apply_fn = mock_apply;
    hooks.user_ctx = &log;
    crypto.verify_rs256_fn = mock_verify_rs256;
    crypto.user_ctx = &log;

    az_iot_adu_device_properties_t dp = {0};
    dp.manufacturer = "AReallyLongManufacturerNameThatWillNotFit";
    dp.model = "AndAModelToo";

    uint8_t tiny[8];
    assert_int_equal(
        az_iot_adu_client_initialize(
            &adu, &twin, &hooks, &crypto, NULL, 0, &dp, tiny, sizeof(tiny)),
        AZ_IOT_ERR_NOT_ENOUGH_SPACE);

    az_iot_twin_client_deinit(&twin);
    az_iot_connection_client_deinit(&conn);
}

static void microsoft_root_keys_are_embedded(void** state)
{
    (void)state;
    size_t count = 0;
    const az_iot_adu_root_key_t* keys = az_iot_adu_microsoft_root_keys(&count);
    assert_non_null(keys);
    assert_true(count >= 2);
    for (size_t i = 0; i < count; i++)
    {
        assert_non_null(keys[i].kid);
        assert_non_null(keys[i].modulus);
        assert_true(keys[i].modulus_len > 0);
        assert_non_null(keys[i].exponent);
        assert_true(keys[i].exponent_len > 0);
        assert_false(keys[i].disabled);
    }
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(init_starts_idle_and_pending_report, setup, teardown),
        cmocka_unit_test_setup_teardown(deployment_drives_full_workflow_single_step, setup, teardown),
        cmocka_unit_test_setup_teardown(verify_failure_blocks_download_and_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(install_failure_triggers_rollback, setup, teardown),
        cmocka_unit_test_setup_teardown(hash_mismatch_blocks_install_and_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(already_installed_is_rejected_without_download, setup, teardown),
        cmocka_unit_test_setup_teardown(install_in_progress_reenters_then_completes, setup, teardown),
        cmocka_unit_test_setup_teardown(reboot_required_persists_and_resumes, setup, teardown),
        cmocka_unit_test_setup_teardown(resume_with_no_persisted_state_stays_idle, setup, teardown),
        cmocka_unit_test_setup_teardown(cancel_action_sets_cancelled_flag, setup, teardown),
        cmocka_unit_test_setup_teardown(update_device_properties_sets_report_pending, setup, teardown),
        cmocka_unit_test_setup_teardown(custom_device_properties_are_reported, setup, teardown),
        cmocka_unit_test_setup_teardown(device_props_too_small_is_rejected, setup, teardown),
        cmocka_unit_test(microsoft_root_keys_are_embedded),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
