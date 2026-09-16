// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

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
#include "../../src/features/adu/internal/adu_channel_internal.h"
#include "../../src/features/adu/internal/adu_internal.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_adu.h"

#include "support/mock_mqtt_iface.h"
#include "support/subscription_ack.h"

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
static const az_iot_adu_root_key k_root_keys[]
    = { { "testkid", k_root_mod, sizeof(k_root_mod), k_root_exp, sizeof(k_root_exp), false } };

/* base64url-encode `data` into a NUL-terminated C string. az_core 1.5.0 ships
 * only standard base64 encode, so we translate +/ to -_ and drop padding. */
static void b64url_str(const void* data, int32_t len, char* dst, int32_t cap)
{
  int32_t w = 0;
  assert_true(az_result_succeeded(az_base64_encode(
      az_span_create((uint8_t*)dst, cap), az_span_create((uint8_t*)(uintptr_t)data, len), &w)));
  int32_t out = 0;
  for (int32_t i = 0; i < w; ++i)
  {
    char c = dst[i];
    if (c == '=')
    {
      continue;
    }
    if (c == '+')
    {
      c = '-';
    }
    else if (c == '/')
    {
      c = '_';
    }
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
      az_span_create(fixed_hash, (int32_t)sizeof(fixed_hash)),
      &w)));
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

/* Build a single-step patch carrying a freshly built, structurally-valid JWS,
 * with a caller-chosen workflow `id`, optional `retryTimestamp` (pass NULL or
 * "" to omit it), and a manifest `version` (lets a test vary the manifest while
 * keeping the same id). Returns a pointer to a static buffer (valid until the
 * next call), which is fine because each is injected before the next is built. */
static const char* build_patch_ex(const char* id, const char* retry_ts, const char* version)
{
  static char patch[4096];
  char jws[2048];
  build_jws(jws, (int32_t)sizeof(jws));

  char workflow[256];
  if (retry_ts != NULL && retry_ts[0] != '\0')
  {
    snprintf(
        workflow,
        sizeof(workflow),
        "\"workflow\":{\"action\":3,\"id\":\"%s\",\"retryTimestamp\":\"%s\"}",
        id,
        retry_ts);
  }
  else
  {
    snprintf(workflow, sizeof(workflow), "\"workflow\":{\"action\":3,\"id\":\"%s\"}", id);
  }

  int n = snprintf(patch, sizeof(patch), k_patch_fmt, workflow, version, jws);
  assert_true(n > 0 && (size_t)n < sizeof(patch));
  return patch;
}

/* Build a single-step patch with the default manifest version ("1.1"). */
static const char* build_patch(const char* id, const char* retry_ts)
{
  return build_patch_ex(id, retry_ts, "1.1");
}

/* The default single-step deployment (fixed id, no retryTimestamp). */
static const char* signed_patch(void)
{
  return build_patch("51552a54-765e-419f-892a-c822549b6f38", NULL);
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
  OP_VERIFY = 1,
  OP_IS_INSTALLED,
  OP_DOWNLOAD,
  OP_BACKUP,
  OP_INSTALL,
  OP_APPLY,
  OP_RESTORE
} op_kind;

#define MAX_OPS 32

typedef struct
{
  op_kind ops[MAX_OPS];
  uint32_t op_steps[MAX_OPS];
  size_t op_count;

  /* Configurable per-op return codes (default SUCCESS). */
  int32_t download_result;
  int32_t backup_result;
  int32_t install_result;
  int32_t apply_result;
  int32_t restore_result;
  int32_t is_installed_result;
  int32_t verify_result;

  bool install_in_progress_once; /* first install returns IN_PROGRESS */
  bool install_in_progress_consumed;

  /* Streaming per-file hash verification. */
  uint8_t file_hash[32]; /* what the incremental SHA-256 mock returns */
  size_t file_len; /* bytes the read-back mock serves */

  /* Persistence / resume. */
  uint8_t persist_blob[AZ_IOT_ADU_REQUEST_BUFFER_SIZE + 128];
  size_t persist_len;
  bool have_persist;
} hook_log;

static void log_op(hook_log* l, op_kind k, uint32_t step)
{
  if (l->op_count < MAX_OPS)
  {
    l->ops[l->op_count] = k;
    l->op_steps[l->op_count] = step;
    l->op_count++;
  }
}

static int32_t mock_download(
    const az_iot_adu_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* ctx)
{
  (void)file;
  (void)url;
  (void)file_index;
  (void)file_count;
  hook_log* l = (hook_log*)ctx;
  log_op(l, OP_DOWNLOAD, file_index);
  return l->download_result;
}

static int32_t mock_install(const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
  (void)m;
  hook_log* l = (hook_log*)ctx;
  log_op(l, OP_INSTALL, step);
  if (l->install_in_progress_once && !l->install_in_progress_consumed)
  {
    l->install_in_progress_consumed = true;
    return AZ_IOT_ADU_RESULT_IN_PROGRESS;
  }
  return l->install_result;
}

static int32_t mock_apply(const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
  (void)m;
  hook_log* l = (hook_log*)ctx;
  log_op(l, OP_APPLY, step);
  return l->apply_result;
}

static int32_t mock_backup(const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
  (void)m;
  hook_log* l = (hook_log*)ctx;
  log_op(l, OP_BACKUP, step);
  return l->backup_result;
}

static int32_t mock_restore(const az_iot_adu_client_update_manifest* m, uint32_t step, void* ctx)
{
  (void)m;
  hook_log* l = (hook_log*)ctx;
  log_op(l, OP_RESTORE, step);
  return l->restore_result;
}

static int32_t mock_is_installed(const az_iot_adu_client_update_manifest* m, void* ctx)
{
  (void)m;
  hook_log* l = (hook_log*)ctx;
  log_op(l, OP_IS_INSTALLED, 0);
  return l->is_installed_result;
}

static int32_t mock_verify_rs256(
    const uint8_t* mod,
    size_t mod_len,
    const uint8_t* exp,
    size_t exp_len,
    const uint8_t* signed_data,
    size_t signed_len,
    const uint8_t* sig,
    size_t sig_len,
    void* ctx)
{
  (void)mod;
  (void)mod_len;
  (void)exp;
  (void)exp_len;
  (void)signed_data;
  (void)signed_len;
  (void)sig;
  (void)sig_len;
  hook_log* l = (hook_log*)ctx;
  log_op(l, OP_VERIFY, 0);
  return l->verify_result;
}

static int32_t mock_sha256(const uint8_t* data, size_t len, uint8_t out[32], void* ctx)
{
  (void)data;
  (void)len;
  (void)ctx;
  memset(out, ADU_TEST_HASH_BYTE, 32);
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

/* Serve fx->log.file_len bytes of dummy content in chunks, then EOF. */
static int32_t mock_read_file(
    const az_iot_adu_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* ctx)
{
  (void)file;
  (void)file_index;
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
  (void)c;
  (void)data;
  (void)len;
  (void)ctx;
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t mock_sha_final(void* c, uint8_t out[32], void* ctx)
{
  (void)c;
  hook_log* l = (hook_log*)ctx;
  memcpy(out, l->file_hash, 32);
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t mock_persist(const uint8_t* blob, size_t len, void* ctx)
{
  hook_log* l = (hook_log*)ctx;
  assert_true(len <= sizeof(l->persist_blob));
  memcpy(l->persist_blob, blob, len);
  l->persist_len = len;
  l->have_persist = true;
  return 0;
}

static int32_t mock_load(uint8_t* blob, size_t cap, size_t* out_len, void* ctx)
{
  hook_log* l = (hook_log*)ctx;
  if (!l->have_persist || l->persist_len > cap)
  {
    return 1; /* nothing persisted */
  }
  memcpy(blob, l->persist_blob, l->persist_len);
  *out_len = l->persist_len;
  return 0;
}

/* ------------------------------------------------------------------------- */
/* fake channel                                                              */
/* ------------------------------------------------------------------------- */
/* The engine names no transport, so the suite drives it through a channel that
 * carries nothing: deliveries are injected by the test and reports are captured
 * for inspection. No MQTT, no HTTP, no service. */

typedef struct
{
  az_iot_adu_channel_update_cb cb;
  az_iot_adu_channel_result_cb result_cb;
  void* engine_ctx;
  bool opened;
  int request_update_count;
  az_iot_result request_update_result;

  int report_count;
  az_iot_adu_report last_report;
  char last_workflow_id[128];
  char last_extended[32];
  char last_details[256];
  bool last_had_installed_update_id;
  char last_installed_provider[64];
  char last_installed_name[64];
  char last_installed_version[64];
  az_iot_adu_client_step_result last_step_results[_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS];
  uint8_t last_step_details[_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS][256];
  size_t do_work_count;
} fake_channel;

static az_iot_result fake_channel_open(
    void* ctx,
    az_iot_adu_channel_update_cb cb,
    az_iot_adu_channel_result_cb result_cb,
    void* engine_ctx)
{
  fake_channel* fc = (fake_channel*)ctx;
  fc->cb = cb;
  fc->result_cb = result_cb;
  fc->engine_ctx = engine_ctx;
  fc->opened = true;
  return AZ_IOT_OK;
}

static void fake_channel_close(void* ctx) { ((fake_channel*)ctx)->opened = false; }

static az_iot_result fake_channel_do_work(void* ctx)
{
  ((fake_channel*)ctx)->do_work_count++;
  return AZ_IOT_OK;
}

static az_iot_result fake_channel_request_update(void* ctx)
{
  fake_channel* fc = (fake_channel*)ctx;
  fc->request_update_count++;
  return fc->request_update_result;
}

static void copy_str(char* dst, size_t cap, const char* src)
{
  if (src == NULL)
  {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n > cap - 1)
  {
    n = cap - 1;
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static az_iot_result fake_channel_report(void* ctx, const az_iot_adu_report* report)
{
  fake_channel* fc = (fake_channel*)ctx;
  fc->report_count++;
  fc->last_report = *report;
  assert_true(report->step_results_count >= 0);
  assert_true(report->step_results_count <= _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS);
  if (report->step_results_count == 0)
  {
    assert_null(report->step_results);
  }
  else
  {
    assert_non_null(report->step_results);
  }
  for (int32_t i = 0; i < report->step_results_count; ++i)
  {
    fc->last_step_results[i] = report->step_results[i];
    az_span details = report->step_results[i].result_details;
    int32_t len = az_span_size(details);
    assert_true(len >= 0 && (size_t)len <= sizeof(fc->last_step_details[i]));
    if (len > 0)
    {
      memcpy(fc->last_step_details[i], az_span_ptr(details), (size_t)len);
      fc->last_step_results[i].result_details = az_span_create(fc->last_step_details[i], len);
    }
  }
  fc->last_report.step_results = (report->step_results_count > 0) ? fc->last_step_results : NULL;
  copy_str(fc->last_workflow_id, sizeof(fc->last_workflow_id), report->workflow_id);
  copy_str(fc->last_extended, sizeof(fc->last_extended), report->extended_result_codes);
  copy_str(fc->last_details, sizeof(fc->last_details), report->result_details);
  fc->last_had_installed_update_id = (report->installed_update_id != NULL);
  if (report->installed_update_id != NULL)
  {
    copy_str(
        fc->last_installed_provider,
        sizeof(fc->last_installed_provider),
        report->installed_update_id->provider);
    copy_str(
        fc->last_installed_name,
        sizeof(fc->last_installed_name),
        report->installed_update_id->name);
    copy_str(
        fc->last_installed_version,
        sizeof(fc->last_installed_version),
        report->installed_update_id->version);
  }
  return AZ_IOT_OK;
}

static const az_iot_adu_channel_vtable k_fake_channel_vtable = {
  .open = fake_channel_open,
  .close = fake_channel_close,
  .request_update = fake_channel_request_update,
  .report = fake_channel_report,
  .set_device_properties = NULL,
  .do_work = fake_channel_do_work,
};

/* ------------------------------------------------------------------------- */
/* fixture                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
  az_iot_connection_client conn;
  fake_channel chan;
  az_iot_adu_channel channel;
  az_iot_adu_client_t adu;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock;

  hook_log log;
  uint8_t dp_buf[256];
} fixture;

static void wire_hooks(
    hook_log* log,
    az_iot_adu_platform_hooks* hooks,
    az_iot_adu_crypto_hooks* crypto)
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
      AZ_SPAN_FROM_STR(ADU_TEST_FILE_HASH_B64),
      &hw)));
  assert_int_equal(hw, 32);
}

static void init_hooks(
    fixture* fx,
    az_iot_adu_platform_hooks* hooks,
    az_iot_adu_crypto_hooks* crypto)
{
  wire_hooks(&fx->log, hooks, crypto);
}

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  memset(&fx->chan, 0, sizeof(fx->chan));
  fx->channel.vtable = &k_fake_channel_vtable;
  fx->channel.ctx = &fx->chan;

  az_iot_adu_platform_hooks hooks;
  az_iot_adu_crypto_hooks crypto;
  init_hooks(fx, &hooks, &crypto);

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
  adu_opts.device_props_buffer = fx->dp_buf;
  adu_opts.device_props_buffer_size = sizeof(fx->dp_buf);
  assert_int_equal(
      az_iot_adu_client__initialize_with_channel(&fx->adu, &fx->channel, &adu_opts), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    /* A registered factory is adopted by the connection client and freed by
     * its deinit(); an unregistered one (tests that never call
     * open_to_connected) is still owned by the test and must be destroyed
     * here. Check before deinit() clears factory_count. */
    bool factory_adopted = (fx->conn.factory_count > 0);
    az_iot_adu_client_destroy(&fx->adu);

    az_iot_connection_client_destroy(&fx->conn);
    if (!factory_adopted)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
    free(fx);
  }
  return 0;
}

static void open_to_connected(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
  az_iot_test_ack_subscriptions(&fx->conn, fx->mock);
  az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

/* Deliver an update payload through the channel. Under ADUv1 this arrived as an
 * MQTT twin desired-property PATCH; the engine no longer knows or cares what
 * carried it, so the test hands the payload straight to the channel callback. */
static void inject_patch(fixture* fx, const char* body)
{
  assert_true(fx->chan.opened);
  assert_non_null(fx->chan.cb);
  fx->chan.cb((const uint8_t*)body, strlen(body), fx->chan.engine_ctx);
}

/* Pump the ADU state machine until Idle or a max iteration cap. */
static void pump(fixture* fx, int max_iters)
{
  for (int i = 0; i < max_iters; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
    if (az_iot_adu_client_get_state(&fx->adu) == AZ_IOT_ADU_STATE_IDLE && i > 0)
    {
      break;
    }
  }
}

static bool ops_contain_sequence(const hook_log* l, const op_kind* seq, size_t n)
{
  if (l->op_count < n)
  {
    return false;
  }
  /* Find seq as an ordered (contiguous-relative) subsequence. */
  size_t si = 0;
  for (size_t i = 0; i < l->op_count && si < n; ++i)
  {
    if (l->ops[i] == seq[si])
    {
      si++;
    }
  }
  return si == n;
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void init_starts_idle_and_pending_report(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  /* The startup device-properties report is pending; first do_work consumes
   * it and stays Idle. */
  open_to_connected(fx);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void deployment_drives_full_workflow_single_step(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  inject_patch(fx, signed_patch());
  /* The patch moves us out of Idle into ManifestReceived. */
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);

  pump(fx, 40);

  /* Ends back at Idle after a successful single-step deployment. */
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);

  /* Expected ordered op sequence for one step. */
  static const op_kind expect[]
      = { OP_VERIFY, OP_IS_INSTALLED, OP_DOWNLOAD, OP_BACKUP, OP_INSTALL, OP_APPLY };
  assert_true(ops_contain_sequence(&fx->log, expect, sizeof(expect) / sizeof(expect[0])));
  /* No rollback on the happy path. */
  for (size_t i = 0; i < fx->log.op_count; ++i)
  {
    assert_int_not_equal(fx->log.ops[i], OP_RESTORE);
  }
  assert_int_equal(fx->chan.last_report.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(fx->chan.last_report.step_results_count, 1);
  assert_int_equal(
      fx->chan.last_report.step_results[0].result_code, AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS);
  assert_int_equal(fx->chan.last_report.step_results[0].extended_result_code, 0);
}

static void verify_failure_blocks_download_and_fails(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->log.verify_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, signed_patch());
  pump(fx, 40);

  /* Verify ran; download never did. */
  bool saw_verify = false, saw_download = false;
  for (size_t i = 0; i < fx->log.op_count; ++i)
  {
    if (fx->log.ops[i] == OP_VERIFY)
    {
      saw_verify = true;
    }
    if (fx->log.ops[i] == OP_DOWNLOAD)
    {
      saw_download = true;
    }
  }
  assert_true(saw_verify);
  assert_false(saw_download);
  /* Terminal: machine returns to Idle after reporting FAILED. */
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void install_failure_triggers_rollback(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->log.install_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, signed_patch());
  pump(fx, 40);

  /* Install ran and failed; restore (rollback) ran for step 0; apply never ran. */
  bool saw_install = false, saw_restore = false, saw_apply = false;
  for (size_t i = 0; i < fx->log.op_count; ++i)
  {
    if (fx->log.ops[i] == OP_INSTALL)
    {
      saw_install = true;
    }
    if (fx->log.ops[i] == OP_RESTORE)
    {
      saw_restore = true;
    }
    if (fx->log.ops[i] == OP_APPLY)
    {
      saw_apply = true;
    }
  }
  assert_true(saw_install);
  assert_true(saw_restore);
  assert_false(saw_apply);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void hash_mismatch_blocks_install_and_fails(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Make the streaming SHA-256 disagree with the manifest's expected hash. */
  memset(fx->log.file_hash, 0x00, sizeof(fx->log.file_hash));
  inject_patch(fx, signed_patch());
  pump(fx, 40);

  /* Download happened, but the hash check failed before install/apply. */
  bool saw_download = false, saw_install = false, saw_apply = false;
  for (size_t i = 0; i < fx->log.op_count; ++i)
  {
    if (fx->log.ops[i] == OP_DOWNLOAD)
    {
      saw_download = true;
    }
    if (fx->log.ops[i] == OP_INSTALL)
    {
      saw_install = true;
    }
    if (fx->log.ops[i] == OP_APPLY)
    {
      saw_apply = true;
    }
  }
  assert_true(saw_download);
  assert_false(saw_install);
  assert_false(saw_apply);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void already_installed_is_rejected_without_download(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->log.is_installed_result = AZ_IOT_ADU_RESULT_ALREADY_INSTALLED;
  inject_patch(fx, signed_patch());
  pump(fx, 40);

  /* is_installed consulted; download never ran; back to Idle. */
  bool saw_is_installed = false, saw_download = false;
  for (size_t i = 0; i < fx->log.op_count; ++i)
  {
    if (fx->log.ops[i] == OP_IS_INSTALLED)
    {
      saw_is_installed = true;
    }
    if (fx->log.ops[i] == OP_DOWNLOAD)
    {
      saw_download = true;
    }
  }
  assert_true(saw_is_installed);
  assert_false(saw_download);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void install_in_progress_reenters_then_completes(void** state)
{
  fixture* fx = (fixture*)*state;
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
    if (fx->log.ops[i] == OP_INSTALL)
    {
      install_calls++;
    }
    if (fx->log.ops[i] == OP_APPLY)
    {
      saw_apply = true;
    }
  }
  assert_true(install_calls >= 2);
  assert_true(saw_apply);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void reboot_required_persists_and_resumes(void** state)
{
  fixture* fx = (fixture*)*state;
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
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_INSTALL_COMPLETE);

  /* Continue: apply runs for the resumed step and the workflow finishes. */
  pump(fx, 40);
  bool saw_apply = false;
  for (size_t i = 0; i < fx->log.op_count; ++i)
  {
    if (fx->log.ops[i] == OP_APPLY)
    {
      saw_apply = true;
    }
  }
  assert_true(saw_apply);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void resume_with_no_persisted_state_stays_idle(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* No blob persisted yet: resume is a clean no-op. */
  assert_false(fx->log.have_persist);
  assert_int_equal(az_iot_adu_client_resume(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void cancel_action_sets_cancelled_flag(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Start a deployment, then cancel mid-flight at a phase boundary. */
  inject_patch(fx, signed_patch());
  assert_int_equal(
      az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK); /* ManifestReceived -> Verifying */

  inject_patch(fx, k_patch_cancel);
  assert_true(az_iot_adu_is_cancelled(&fx->adu));

  /* Next do_work honors cancellation and returns to Idle. */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_false(az_iot_adu_is_cancelled(&fx->adu));
}

/* Reporting is keyed on workflowId and is therefore per-workflow: a device with
 * no workflow in flight has nothing the service could attribute a report to.
 * Refreshing device properties must be accepted and must NOT manufacture a
 * report. Under ADUv1 this same call produced an unsolicited reported-property
 * PATCH; that channel, and the concept, are gone. */
static void update_device_properties_is_accepted_without_reporting(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  /* drain the startup tick */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  fx->chan.report_count = 0;

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Foobar2";
  dp.installed_update_id.provider = "Contoso";
  dp.installed_update_id.name = "Foobar";
  dp.installed_update_id.version = "2.0";
  assert_int_equal(az_iot_adu_client_update_device_properties(&fx->adu, &dp), AZ_IOT_OK);

  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.report_count, 0);
}

/* With a workflow in flight the same tick DOES report, and the report carries
 * the workflow id the deployment was delivered with. */
static void report_carries_the_active_workflow_id(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  fx->chan.report_count = 0;

  inject_patch(fx, signed_patch());
  pump(fx, 40);

  assert_true(fx->chan.report_count > 0);
  assert_string_equal(fx->chan.last_workflow_id, "51552a54-765e-419f-892a-c822549b6f38");
}

static void report_before_manifest_parse_has_no_step_results(void** state)
{
  fixture* fx = (fixture*)*state;
  inject_patch(fx, signed_patch());

  assert_int_equal(az_iot_adu__report_state(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.report_count, 1);
  assert_int_equal(fx->chan.last_report.outcome, AZ_IOT_ADU_OUTCOME_IN_PROGRESS);
  assert_int_equal(fx->chan.last_report.step_results_count, 0);
  assert_null(fx->chan.last_report.step_results);
}

static void report_preserves_step_results_at_capacity(void** state)
{
  fixture* fx = (fixture*)*state;
  inject_patch(fx, signed_patch());

  az_iot_adu_client_install_result* result = &fx->adu._internal.install_result;
  result->step_results_count = _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS;
  uint8_t details[] = { 'a', '\0', 'b' };
  for (int32_t i = 0; i < result->step_results_count; ++i)
  {
    result->step_results[i].result_code = 100 + i;
    result->step_results[i].extended_result_code
        = AZ_IOT_ADU_EXTENDED_RESULT(AZ_IOT_ADU_FACILITY_INTERNAL, (uint32_t)i);
    result->step_results[i].result_details = AZ_SPAN_FROM_BUFFER(details);
  }

  assert_int_equal(az_iot_adu__report_state(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.report_count, 1);
  assert_int_equal(
      fx->chan.last_report.step_results_count, _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS);

  /* A retaining channel copies the array and span bytes before the engine
   * reuses its storage for another workflow. */
  memset(result, 0, sizeof(*result));
  memset(details, 0, sizeof(details));
  static const uint8_t expected_details[] = { 'a', '\0', 'b' };
  for (int32_t i = 0; i < fx->chan.last_report.step_results_count; ++i)
  {
    const az_iot_adu_client_step_result* step = &fx->chan.last_report.step_results[i];
    assert_int_equal(step->result_code, 100 + i);
    assert_int_equal(
        step->extended_result_code,
        AZ_IOT_ADU_EXTENDED_RESULT(AZ_IOT_ADU_FACILITY_INTERNAL, (uint32_t)i));
    assert_int_equal(az_span_size(step->result_details), sizeof(expected_details));
    assert_memory_equal(
        az_span_ptr(step->result_details), expected_details, sizeof(expected_details));
  }
}

/* Custom (compatibility) properties are cached by the engine and, under ADUv2,
 * are carried in agentInfo on the fetch — which is the channel's business, not
 * the engine's. What remains engine-side is that they are accepted and that the
 * standalone builder serializes them; the old assertion on a twin
 * reported-property PATCH tested the deleted channel and is gone. */
static void custom_device_properties_are_accepted_and_serialized(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);

  static const az_iot_adu_custom_property customs[] = {
    { "location", "building42" },
    { "tier", "gold" },
  };
  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Foobar";
  dp.installed_update_id.provider = "Contoso";
  dp.installed_update_id.name = "Foobar";
  dp.installed_update_id.version = "1.0";
  dp.custom_properties = customs;
  dp.custom_properties_count = sizeof(customs) / sizeof(customs[0]);
  assert_int_equal(az_iot_adu_client_update_device_properties(&fx->adu, &dp), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);

  /* The builder reports bytes used, not a C string, so reserve a byte for the
   * terminator rather than writing at json[json_len] on a full buffer. */
  uint8_t json[1024];
  size_t json_len = 0;
  assert_int_equal(
      az_iot_adu_build_report(
          &dp, NULL, NULL, AZ_IOT_ADU_STATE_IDLE, json, sizeof(json) - 1, &json_len),
      AZ_IOT_OK);
  assert_true(json_len > 0);
  assert_true(json_len < sizeof(json));

  char* text = (char*)json;
  text[json_len] = '\0';
  assert_non_null(strstr(text, "location"));
  assert_non_null(strstr(text, "building42"));
  assert_non_null(strstr(text, "tier"));
  assert_non_null(strstr(text, "gold"));
}

/* The public entry point takes a CONNECTION and nothing else: the SDK owns the
 * device-update protocol end to end. */

static void public_initialize_takes_a_connection_and_builds_its_own_channel(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)fx;

  az_iot_connection_client conn;
  az_iot_connection_client_options copts = { 0 };
  copts.host = "broker.example";
  copts.port = 8883;
  copts.client_id = "ut-adu-public";
  assert_int_equal(az_iot_connection_client_init(&conn, &copts), AZ_IOT_OK);

  hook_log log = { 0 };
  az_iot_adu_platform_hooks hooks = { 0 };
  az_iot_adu_crypto_hooks crypto = { 0 };
  hooks.install_fn = mock_install;
  hooks.apply_fn = mock_apply;
  hooks.user_ctx = &log;
  crypto.verify_rs256_fn = mock_verify_rs256;
  crypto.user_ctx = &log;

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Foobar";
  dp.installed_update_id.provider = "Contoso";
  dp.installed_update_id.name = "Foobar";
  dp.installed_update_id.version = "1.0";

  uint8_t buf[256];
  az_iot_adu_client_config_options o = az_iot_adu_client_config_options_default();
  o.hooks = &hooks;
  o.crypto = &crypto;
  o.device_props = &dp;
  o.device_props_buffer = buf;
  o.device_props_buffer_size = sizeof(buf);

  /* The connection is NOT open: the bootstrap update check runs before the
   * device registers, so initialize must not require a live session. */
  az_iot_adu_client_t adu;
  assert_int_equal(az_iot_adu_client_initialize(&adu, &conn, &o), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&adu), AZ_IOT_ADU_STATE_IDLE);
  az_iot_adu_client_destroy(&adu);

  /* The channel state lives INSIDE the client. Initialization must not zero the
   * client after building it there, or the channel would be left bound to a
   * wiped state struct -- with a NULL connection -- and would fail only later,
   * on the first operation. Reaching the connection through the client proves
   * it survived initialization. */
  az_iot_adu_client_t adu_state;
  assert_int_equal(az_iot_adu_client_initialize(&adu_state, &conn, &o), AZ_IOT_OK);
  const az_iot_adu_channel_dps* bound
      = (const az_iot_adu_channel_dps*)(const void*)&adu_state._internal.channel_storage;
  assert_ptr_equal(bound->connection, &conn);
  assert_ptr_equal(adu_state._internal.channel.ctx, bound);
  az_iot_adu_client_destroy(&adu_state);

  az_iot_adu_client_t adu_no_conn;
  assert_int_equal(az_iot_adu_client_initialize(&adu_no_conn, NULL, &o), AZ_IOT_ERR_INVALID_ARG);

  az_iot_connection_client_destroy(&conn);
}

/* extendedResultCodes is contract-shaped: comma-separated UNSIGNED hex int32,
 * NO "0x" prefix, no fixed width, case-insensitive. Pinned here because nothing
 * else asserts the wire form, and a prefixed or zero-padded value is accepted by
 * the compiler while being wrong on the wire. */
static void extended_result_codes_are_bare_hex(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  fx->chan.report_count = 0;

  fx->log.download_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, signed_patch());
  pump(fx, 40);

  assert_true(fx->chan.report_count > 0);
  const char* ext = fx->chan.last_extended;
  assert_non_null(ext);
  assert_true(ext[0] != '\0');
  /* no 0x/0X prefix */
  assert_false(ext[0] == '0' && (ext[1] == 'x' || ext[1] == 'X'));
  /* Walk the comma-separated list explicitly: every segment must be non-empty
   * and hex-only. Checking the character set alone would accept a leading or
   * trailing comma and empty segments such as "1,,2", none of which are valid
   * values of this field. */
  const char* c = ext;
  size_t segments = 0;
  while (*c != '\0')
  {
    size_t digits = 0;
    while (*c != '\0' && *c != ',')
    {
      assert_true((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F'));
      ++digits;
      ++c;
    }
    /* Rejects "", a leading comma, a trailing comma, and ",,". */
    assert_true(digits > 0);
    /* Unsigned int32, so at most 8 hex digits. */
    assert_true(digits <= 8);
    ++segments;

    if (*c == ',')
    {
      ++c; /* a separator must be followed by another segment */
      assert_true(*c != '\0');
    }
  }
  assert_true(segments > 0);
}

static void device_props_too_small_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)fx;

  az_iot_connection_client conn;
  fake_channel fc;
  az_iot_adu_channel channel;
  az_iot_adu_client_t adu;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device2";
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);
  memset(&fc, 0, sizeof(fc));
  channel.vtable = &k_fake_channel_vtable;
  channel.ctx = &fc;

  hook_log log = { 0 };
  az_iot_adu_platform_hooks hooks = { 0 };
  az_iot_adu_crypto_hooks crypto = { 0 };
  hooks.install_fn = mock_install;
  hooks.apply_fn = mock_apply;
  hooks.user_ctx = &log;
  crypto.verify_rs256_fn = mock_verify_rs256;
  crypto.user_ctx = &log;

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "AReallyLongManufacturerNameThatWillNotFit";
  dp.model = "AndAModelToo";

  uint8_t tiny[8];
  az_iot_adu_client_config_options adu_opts = az_iot_adu_client_config_options_default();
  adu_opts.hooks = &hooks;
  adu_opts.crypto = &crypto;
  adu_opts.device_props = &dp;
  adu_opts.device_props_buffer = tiny;
  adu_opts.device_props_buffer_size = sizeof(tiny);
  assert_int_equal(
      az_iot_adu_client__initialize_with_channel(&adu, &channel, &adu_opts),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  az_iot_connection_client_destroy(&conn);
}

static void device_props_buffer_size_matches_need(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)fx;

  assert_int_equal(az_iot_adu_device_props_buffer_size(NULL), 0);

  az_iot_connection_client conn;
  fake_channel fc;
  az_iot_adu_channel channel;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device3";
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);
  memset(&fc, 0, sizeof(fc));
  channel.vtable = &k_fake_channel_vtable;
  channel.ctx = &fc;

  hook_log log = { 0 };
  az_iot_adu_platform_hooks hooks = { 0 };
  az_iot_adu_crypto_hooks crypto = { 0 };
  hooks.install_fn = mock_install;
  hooks.apply_fn = mock_apply;
  hooks.user_ctx = &log;
  crypto.verify_rs256_fn = mock_verify_rs256;
  crypto.user_ctx = &log;

  az_iot_adu_custom_property customs[] = { { "location", "building42" } };
  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Foobar";
  dp.installed_update_id.provider = "Contoso";
  dp.installed_update_id.name = "Foobar";
  dp.installed_update_id.version = "1.0";
  dp.custom_properties = customs;
  dp.custom_properties_count = 1;

  size_t need = az_iot_adu_device_props_buffer_size(&dp);
  assert_true(need > sizeof(az_iot_adu_device_properties));

  uint8_t buf[256];
  assert_true(need <= sizeof(buf));

  az_iot_adu_client_config_options o = az_iot_adu_client_config_options_default();
  o.hooks = &hooks;
  o.crypto = &crypto;
  o.device_props = &dp;
  o.device_props_buffer = buf;

  /* Exactly `need` bytes must succeed; one byte short must be rejected. */
  az_iot_adu_client_t adu_ok;
  o.device_props_buffer_size = need;
  assert_int_equal(az_iot_adu_client__initialize_with_channel(&adu_ok, &channel, &o), AZ_IOT_OK);
  az_iot_adu_client_destroy(&adu_ok);

  az_iot_adu_client_t adu_short;
  o.device_props_buffer_size = need - 1;
  assert_int_equal(
      az_iot_adu_client__initialize_with_channel(&adu_short, &channel, &o),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  az_iot_connection_client_destroy(&conn);
}

static void duplicate_redelivery_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Run the deployment to completion. */
  inject_patch(fx, signed_patch());
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);

  /* Redeliver the identical deployment (same id, no retryTimestamp), as a
   * reconnect twin GET would. It MUST be ignored: state stays Idle and no
   * platform hooks are invoked a second time. */
  fx->log.op_count = 0;
  inject_patch(fx, signed_patch());
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal((int)fx->log.op_count, 0);

  for (int i = 0; i < 5; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal((int)fx->log.op_count, 0);
}

static void retry_with_newer_timestamp_restarts(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Initial deployment (no retryTimestamp) runs to completion. */
  inject_patch(fx, signed_patch());
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);

  /* Same id, now WITH a retryTimestamp: the service is forcing a retry, so the
   * workflow must restart from scratch (not be ignored as a duplicate). */
  fx->log.op_count = 0;
  inject_patch(fx, build_patch("51552a54-765e-419f-892a-c822549b6f38", "2022-08-01T00:00:00Z"));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);

  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  /* The full op sequence ran a second time. */
  static const op_kind expect[]
      = { OP_VERIFY, OP_IS_INSTALLED, OP_DOWNLOAD, OP_BACKUP, OP_INSTALL, OP_APPLY };
  assert_true(ops_contain_sequence(&fx->log, expect, sizeof(expect) / sizeof(expect[0])));
}

static void replacement_with_new_id_restarts(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  /* Drain the startup device-properties report so the next do_work advances
   * the state machine rather than the report. */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);

  /* Start deployment A and let it advance past ManifestReceived. */
  inject_patch(fx, build_patch("aaaaaaaa-0000-0000-0000-000000000001", NULL));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_VERIFYING_MANIFEST);

  /* A different deployment id arrives mid-flight: a replacement restarts from
   * ManifestReceived (state moves backwards, proving it was not ignored). */
  inject_patch(fx, build_patch("bbbbbbbb-0000-0000-0000-000000000002", NULL));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);

  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void retry_timestamp_survives_resume(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* A deployment carrying a retryTimestamp installs and requires a reboot, so
   * the workflow snapshots itself (including the retryTimestamp) via persist. */
  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  inject_patch(fx, build_patch("51552a54-765e-419f-892a-c822549b6f38", "2022-08-01T00:00:00Z"));
  pump(fx, 40);
  assert_true(fx->log.have_persist);

  /* Simulate the reboot: forget the in-RAM workflow, resume from the blob. */
  fx->log.op_count = 0;
  fx->log.install_result = AZ_IOT_ADU_RESULT_SUCCESS;
  assert_int_equal(az_iot_adu_client_resume(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_INSTALL_COMPLETE);

  /* The service redelivers the identical deployment (same id AND same
   * retryTimestamp) while the post-reboot workflow is still finishing. Because
   * the retryTimestamp round-tripped through the snapshot, this is recognized
   * as a duplicate and ignored: the workflow does NOT restart from
   * ManifestReceived. (If retryTimestamp had not survived the reboot, the
   * active timestamp would be empty and this would be mistaken for a retry.) */
  inject_patch(fx, build_patch("51552a54-765e-419f-892a-c822549b6f38", "2022-08-01T00:00:00Z"));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_INSTALL_COMPLETE);

  /* And the resumed workflow still completes normally. */
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void same_id_changed_manifest_restarts(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* A deployment (id X, manifest version 1.1) runs to completion. */
  inject_patch(fx, build_patch_ex("51552a54-765e-419f-892a-c822549b6f38", NULL, "1.1"));
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);

  /* The SAME id with NO retryTimestamp but a DIFFERENT manifest (version 1.2)
   * is an anomalous re-publish: it must be treated as a replacement and
   * restart, not silently ignored as a duplicate. */
  fx->log.op_count = 0;
  inject_patch(fx, build_patch_ex("51552a54-765e-419f-892a-c822549b6f38", NULL, "1.2"));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);

  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  static const op_kind expect[]
      = { OP_VERIFY, OP_IS_INSTALLED, OP_DOWNLOAD, OP_BACKUP, OP_INSTALL, OP_APPLY };
  assert_true(ops_contain_sequence(&fx->log, expect, sizeof(expect) / sizeof(expect[0])));
}

static void microsoft_root_keys_are_embedded(void** state)
{
  (void)state;
  size_t count = 0;
  const az_iot_adu_root_key* keys = az_iot_adu_microsoft_root_keys(&count);
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

/* ------------------------------------------------------------------------- */
/* workflow: multi-step, download failure, cancel mid-flight                 */
/* ------------------------------------------------------------------------- */

/* Two-step variant of k_patch_fmt. Both steps use the same file so the fixture's
 * single hash still verifies; what is under test is the ordering, not the file
 * set. */
static const char k_patch_two_steps_fmt[]
    = "{\"deviceUpdate\":{\"__t\":\"c\",\"service\":{"
      "%s,"
      "\"updateManifest\":\"{\\\"manifestVersion\\\":\\\"5\\\",\\\"updateId\\\":{\\\"provider\\\":"
      "\\\"Contoso\\\",\\\"name\\\":\\\"Foobar\\\",\\\"version\\\":\\\"%s\\\"},"
      "\\\"compatibility\\\":[{\\\"deviceManufacturer\\\":\\\"Contoso\\\",\\\"deviceModel\\\":"
      "\\\"Foobar\\\"}],\\\"instructions\\\":{\\\"steps\\\":[{\\\"handler\\\":\\\"microsoft/"
      "swupdate:1\\\",\\\"files\\\":[\\\"f2f4a804ca17afbae\\\"],\\\"handlerProperties\\\":{"
      "\\\"installedCriteria\\\":\\\"1.0\\\"}},{\\\"handler\\\":\\\"microsoft/"
      "swupdate:1\\\",\\\"files\\\":[\\\"f2f4a804ca17afbae\\\"],\\\"handlerProperties\\\":{"
      "\\\"installedCriteria\\\":\\\"1.1\\\"}}]},\\\"files\\\":{\\\"f2f4a804ca17afbae\\\":{"
      "\\\"fileName\\\":\\\"iot-middleware-sample-adu-v1.1\\\",\\\"sizeInBytes\\\":844976,"
      "\\\"hashes\\\":{\\\"sha256\\\":\\\"xsoCnYAMkZZ7m9RL9Vyg9jKfFehCNxyuPFaJVM/"
      "WBi0=\\\"}}},\\\"createdDateTime\\\":\\\"2022-07-07T03:02:48.8449038Z\\\"}\","
      "\"updateManifestSignature\":\"%s\","
      "\"fileUrls\":{\"f2f4a804ca17afbae\":\"http://example.com/payload.bin\"}}}}";

static const char* two_step_patch(void)
{
  static char patch[4096];
  char jws[2048];
  build_jws(jws, (int32_t)sizeof(jws));
  int n = snprintf(
      patch,
      sizeof(patch),
      k_patch_two_steps_fmt,
      "\"workflow\":{\"action\":3,\"id\":\"multi-step-deployment\"}",
      "1.1",
      jws);
  assert_true(n > 0 && (size_t)n < sizeof(patch));
  return patch;
}

static void multi_step_update_runs_every_step_in_order(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  inject_patch(fx, two_step_patch());
  pump(fx, 60);

  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);

  /* Step 1 must be fully installed and applied before step 2 begins: a build
   * that interleaved them would install over a half-applied step. */
  int install0 = -1, apply0 = -1, install1 = -1, apply1 = -1;
  for (size_t i = 0; i < fx->log.op_count; ++i)
  {
    if (fx->log.ops[i] == OP_INSTALL && fx->log.op_steps[i] == 0 && install0 < 0)
    {
      install0 = (int)i;
    }
    if (fx->log.ops[i] == OP_APPLY && fx->log.op_steps[i] == 0 && apply0 < 0)
    {
      apply0 = (int)i;
    }
    if (fx->log.ops[i] == OP_INSTALL && fx->log.op_steps[i] == 1 && install1 < 0)
    {
      install1 = (int)i;
    }
    if (fx->log.ops[i] == OP_APPLY && fx->log.op_steps[i] == 1 && apply1 < 0)
    {
      apply1 = (int)i;
    }
  }
  assert_true(install0 >= 0);
  assert_true(apply0 >= 0);
  assert_true(install1 >= 0);
  assert_true(apply1 >= 0);
  assert_true(install0 < apply0);
  assert_true(apply0 < install1);
  assert_true(install1 < apply1);
  assert_int_equal(fx->chan.last_report.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(fx->chan.last_report.step_results_count, 2);
  for (int32_t i = 0; i < fx->chan.last_report.step_results_count; ++i)
  {
    assert_int_equal(
        fx->chan.last_report.step_results[i].result_code, AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS);
    assert_int_equal(fx->chan.last_report.step_results[i].extended_result_code, 0);
  }
}

static void multi_step_report_preserves_progress_and_failure(void** state)
{
  fixture* fx = (fixture*)*state;
  inject_patch(fx, two_step_patch());
  for (int i = 0; i < 40 && fx->adu._internal.current_step == 0; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_int_equal(fx->adu._internal.current_step, 1);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_DOWNLOAD_STARTED);

  const az_iot_adu_report* report = &fx->chan.last_report;
  assert_int_equal(report->outcome, AZ_IOT_ADU_OUTCOME_IN_PROGRESS);
  assert_int_equal(report->step_results_count, 2);
  assert_int_equal(report->step_results[0].result_code, AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS);
  assert_int_equal(report->step_results[0].extended_result_code, 0);
  assert_int_equal(report->step_results[1].result_code, 0);
  assert_int_equal(report->step_results[1].extended_result_code, 0);

  fx->log.install_result = AZ_IOT_ADU_RESULT_FAILURE;
  fx->log.restore_result = AZ_IOT_ADU_RESULT_FAILURE;
  pump(fx, 40);

  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(report->outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_int_equal(report->step_results_count, 2);
  assert_int_equal(report->step_results[0].result_code, AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS);
  assert_int_equal(report->step_results[0].extended_result_code, 0);
  assert_int_equal(report->step_results[1].result_code, 700 - AZ_IOT_ADU_FACILITY_INSTALL);
  assert_int_equal(
      report->step_results[1].extended_result_code,
      AZ_IOT_ADU_EXTENDED_RESULT(AZ_IOT_ADU_FACILITY_INSTALL, (uint32_t)AZ_IOT_ADU_RESULT_FAILURE));
  assert_int_equal(
      fx->adu._internal.install_result.extended_result_code,
      AZ_IOT_ADU_EXTENDED_RESULT(AZ_IOT_ADU_FACILITY_RESTORE, (uint32_t)AZ_IOT_ADU_RESULT_FAILURE));
}

static void multi_step_failure_preserves_unexecuted_step_results(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->log.download_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, two_step_patch());
  pump(fx, 40);

  const az_iot_adu_report* report = &fx->chan.last_report;
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(report->outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_int_equal(report->step_results_count, 2);
  assert_int_equal(report->step_results[0].result_code, 700 - AZ_IOT_ADU_FACILITY_DOWNLOAD);
  assert_int_equal(
      report->step_results[0].extended_result_code,
      AZ_IOT_ADU_EXTENDED_RESULT(
          AZ_IOT_ADU_FACILITY_DOWNLOAD, (uint32_t)AZ_IOT_ADU_RESULT_FAILURE));
  assert_int_equal(report->step_results[1].result_code, 0);
  assert_int_equal(report->step_results[1].extended_result_code, 0);
}

static void download_failure_is_reported_and_does_not_install(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->log.download_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, signed_patch());
  pump(fx, 40);

  /* Installing bytes that never arrived is the failure mode this guards. */
  bool saw_download = false, saw_install = false, saw_apply = false;
  for (size_t i = 0; i < fx->log.op_count; ++i)
  {
    if (fx->log.ops[i] == OP_DOWNLOAD)
    {
      saw_download = true;
    }
    if (fx->log.ops[i] == OP_INSTALL)
    {
      saw_install = true;
    }
    if (fx->log.ops[i] == OP_APPLY)
    {
      saw_apply = true;
    }
  }
  assert_true(saw_download);
  assert_false(saw_install);
  assert_false(saw_apply);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);

  /* The outcome reaches the service rather than the agent going quiet. Under
   * the structured contract this is an explicit FAILED outcome attributed to
   * the agent core, not an agent-state integer. */
  assert_true(fx->chan.report_count > 0);
  assert_int_equal(fx->chan.last_report.outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_int_equal(fx->chan.last_report.failure_origin, AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE);
  assert_true(fx->chan.last_report.result_code != 700);
  assert_true(fx->chan.last_workflow_id[0] != '\0');
}

static void cancel_during_download_aborts_the_transfer(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Hold the deployment inside download so the cancel lands mid-transfer. */
  fx->log.download_result = AZ_IOT_ADU_RESULT_IN_PROGRESS;
  inject_patch(fx, signed_patch());
  pump(fx, 4);

  bool saw_download = false;
  for (size_t i = 0; i < fx->log.op_count; ++i)
  {
    if (fx->log.ops[i] == OP_DOWNLOAD)
    {
      saw_download = true;
    }
  }
  assert_true(saw_download);

  size_t ops_at_cancel = fx->log.op_count;
  inject_patch(fx, k_patch_cancel);
  /* Let the download hook succeed from here on: if the cancel were ignored the
   * workflow would now run to completion and install. */
  fx->log.download_result = AZ_IOT_ADU_RESULT_SUCCESS;
  pump(fx, 40);

  for (size_t i = ops_at_cancel; i < fx->log.op_count; ++i)
  {
    assert_int_not_equal(fx->log.ops[i], OP_INSTALL);
    assert_int_not_equal(fx->log.ops[i], OP_APPLY);
  }
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

/* ------------------------------------------------------------------------- */
/* standalone api: report building, manifest verification                    */
/* ------------------------------------------------------------------------- */

static void build_report_with_too_small_a_buffer_is_rejected(void** state)
{
  (void)state;
  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Foobar";
  dp.installed_update_id.provider = "Contoso";
  dp.installed_update_id.name = "Foobar";
  dp.installed_update_id.version = "1.0";

  /* Truncating the report would publish JSON the service cannot parse, so the
   * bound is reported instead. */
  uint8_t tiny[8];
  size_t written = 0;
  assert_int_equal(
      az_iot_adu_build_report(&dp, NULL, NULL, AZ_IOT_ADU_STATE_IDLE, tiny, sizeof(tiny), &written),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  /* The same call succeeds once the buffer is big enough, which proves the
   * rejection was about size and not about the arguments. */
  uint8_t big[AZ_IOT_ADU_REQUEST_BUFFER_SIZE];
  assert_int_equal(
      az_iot_adu_build_report(&dp, NULL, NULL, AZ_IOT_ADU_STATE_IDLE, big, sizeof(big), &written),
      AZ_IOT_OK);
  assert_true(written > 0);
}

/* Parse a patch with the fixture's crypto hooks and the given root keys. */
static az_iot_result parse_with_roots(
    hook_log* log,
    const char* patch,
    const az_iot_adu_root_key* roots,
    size_t root_count,
    az_iot_adu_client_update_request* out_req,
    az_iot_adu_client_update_manifest* out_manifest)
{
  az_iot_adu_platform_hooks hooks;
  az_iot_adu_crypto_hooks crypto;
  wire_hooks(log, &hooks, &crypto);

  /* The parser unescapes the manifest in place, so it needs a writable copy. */
  static char scratch[AZ_IOT_ADU_REQUEST_BUFFER_SIZE];
  size_t len = strlen(patch);
  assert_true(len < sizeof(scratch));
  memcpy(scratch, patch, len);

  return az_iot_adu_parse_update_request(
      az_span_create((uint8_t*)scratch, (int32_t)len),
      &crypto,
      roots,
      root_count,
      out_req,
      out_manifest);
}

static void manifest_signed_by_an_unknown_root_key_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;

  /* Same manifest, same signature, a root key set that does not contain the
   * signing kid. Accepting it would let anyone who can reach the twin deploy
   * firmware. */
  static const uint8_t other_mod[] = { 0x09, 0x08, 0x07 };
  static const uint8_t other_exp[] = { 0x01, 0x00, 0x01 };
  const az_iot_adu_root_key strangers[] = {
    { "not-the-testkid", other_mod, sizeof(other_mod), other_exp, sizeof(other_exp), false }
  };

  az_iot_adu_client_update_request req;
  az_iot_adu_client_update_manifest manifest;
  memset(&req, 0, sizeof(req));
  memset(&manifest, 0, sizeof(manifest));
  assert_int_equal(
      parse_with_roots(&fx->log, signed_patch(), strangers, 1, &req, &manifest), AZ_IOT_ERR_AUTH);
}

static void malformed_jws_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;

  /* Two segments instead of three, then a third with bytes that are not
   * base64url. Both must be refused before any manifest field is trusted. */
  const char* broken[] = {
    "aGVhZGVy.cGF5bG9hZA",
    "aGVhZGVy.cGF5bG9hZA.!!!not-base64url!!!",
    "",
  };
  for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i)
  {
    static char patch[4096];
    int n = snprintf(
        patch,
        sizeof(patch),
        k_patch_fmt,
        "\"workflow\":{\"action\":3,\"id\":\"bad-jws\"}",
        "1.1",
        broken[i]);
    assert_true(n > 0 && (size_t)n < sizeof(patch));

    az_iot_adu_client_update_request req;
    az_iot_adu_client_update_manifest manifest;
    memset(&req, 0, sizeof(req));
    memset(&manifest, 0, sizeof(manifest));
    az_iot_result r = parse_with_roots(&fx->log, patch, k_root_keys, 1, &req, &manifest);
    assert_int_not_equal(r, AZ_IOT_OK);
  }
}

static void malformed_manifest_json_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;

  char jws[2048];
  build_jws(jws, (int32_t)sizeof(jws));

  /* A well-formed envelope whose updateManifest is not parseable JSON. */
  static char patch[4096];
  int n = snprintf(
      patch,
      sizeof(patch),
      "{\"deviceUpdate\":{\"__t\":\"c\",\"service\":{"
      "\"workflow\":{\"action\":3,\"id\":\"bad-manifest\"},"
      "\"updateManifest\":\"{\\\"manifestVersion\\\":\\\"5\\\",\\\"updateId\\\":{\","
      "\"updateManifestSignature\":\"%s\","
      "\"fileUrls\":{\"f\":\"http://example.com/p.bin\"}}}}",
      jws);
  assert_true(n > 0 && (size_t)n < sizeof(patch));

  az_iot_adu_client_update_request req;
  az_iot_adu_client_update_manifest manifest;
  memset(&req, 0, sizeof(req));
  memset(&manifest, 0, sizeof(manifest));
  assert_int_not_equal(
      parse_with_roots(&fx->log, patch, k_root_keys, 1, &req, &manifest), AZ_IOT_OK);
}

/* Serves bytes for the standalone hash check. */
static int32_t standalone_read_chunk(
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* read_ctx)
{
  (void)read_ctx;
  size_t total = 64;
  if (offset >= total)
  {
    *out_read = 0;
    return AZ_IOT_ADU_RESULT_SUCCESS;
  }
  size_t n = total - offset;
  if (n > buffer_size)
  {
    n = buffer_size;
  }
  memset(buffer, 0x5A, n);
  *out_read = n;
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static void verify_file_hash_rejects_an_unsupported_algorithm(void** state)
{
  fixture* fx = (fixture*)*state;

  /* A manifest whose only listed digest is sha512. The agent cannot compute it,
   * and treating "no algorithm I know" as a pass would skip integrity entirely. */
  static const char k_patch_sha512_fmt[]
      = "{\"deviceUpdate\":{\"__t\":\"c\",\"service\":{"
        "\"workflow\":{\"action\":3,\"id\":\"sha512-only\"},"
        "\"updateManifest\":\"{\\\"manifestVersion\\\":\\\"5\\\",\\\"updateId\\\":{"
        "\\\"provider\\\":\\\"Contoso\\\",\\\"name\\\":\\\"Foobar\\\",\\\"version\\\":"
        "\\\"1.1\\\"},\\\"compatibility\\\":[{\\\"deviceManufacturer\\\":\\\"Contoso\\\","
        "\\\"deviceModel\\\":\\\"Foobar\\\"}],\\\"instructions\\\":{\\\"steps\\\":[{"
        "\\\"handler\\\":\\\"microsoft/swupdate:1\\\",\\\"files\\\":[\\\"f2f4a804ca17afbae\\\"],"
        "\\\"handlerProperties\\\":{\\\"installedCriteria\\\":\\\"1.0\\\"}}]},\\\"files\\\":{"
        "\\\"f2f4a804ca17afbae\\\":{\\\"fileName\\\":\\\"payload.bin\\\",\\\"sizeInBytes\\\":64,"
        "\\\"hashes\\\":{\\\"sha512\\\":\\\"AAAA\\\"}}},\\\"createdDateTime\\\":"
        "\\\"2022-07-07T03:02:48.8449038Z\\\"}\","
        "\"updateManifestSignature\":\"%s\","
        "\"fileUrls\":{\"f2f4a804ca17afbae\":\"http://example.com/payload.bin\"}}}}";

  char jws[2048];
  build_jws(jws, (int32_t)sizeof(jws));
  static char patch[4096];
  int n = snprintf(patch, sizeof(patch), k_patch_sha512_fmt, jws);
  assert_true(n > 0 && (size_t)n < sizeof(patch));

  az_iot_adu_client_update_request req;
  az_iot_adu_client_update_manifest manifest;
  memset(&req, 0, sizeof(req));
  memset(&manifest, 0, sizeof(manifest));
  assert_int_equal(parse_with_roots(&fx->log, patch, k_root_keys, 1, &req, &manifest), AZ_IOT_OK);
  assert_true(manifest.files_count > 0);

  az_iot_adu_platform_hooks hooks;
  az_iot_adu_crypto_hooks crypto;
  wire_hooks(&fx->log, &hooks, &crypto);
  assert_int_equal(
      az_iot_adu_verify_file_hash(&manifest.files[0], &crypto, standalone_read_chunk, &fx->log),
      AZ_IOT_ERR_AUTH);
}

/* The vtable advertises an optional do_work hook for a channel with
 * asynchronous work of its own. A channel that reports lost operations there
 * depends on actually being ticked, so pin that the engine drives it. */
static void do_work_drives_the_channel(void** state)
{
  fixture* fx = (fixture*)*state;
  size_t before = fx->chan.do_work_count;

  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.do_work_count, before + 1);

  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.do_work_count, before + 2);
}

/* A verdict the engine must NOT retry. ALREADY_REPORTED means a terminal result
 * is already recorded for this workflow, so the report HAS been delivered --
 * reporting is idempotent on workflowId. Re-arming it would retry forever, and
 * during a held bootstrap session that starves the update check until the hold
 * expires.
 *
 * Asserted on the pending flag rather than a report count: an advancing
 * workflow emits progress reports of its own, which would mask the difference.
 */
static void a_terminal_verdict_does_not_re_arm_the_report(void** state)
{
  fixture* fx = (fixture*)*state;

  fx->adu._internal.device_props_report_pending = false;

  assert_non_null(fx->chan.result_cb);
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_REPORT_STATUS,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED,
      fx->chan.engine_ctx);
  assert_false(fx->adu._internal.device_props_report_pending);

  /* Same for the other terminal verdicts. */
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_REPORT_STATUS,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_FATAL,
      fx->chan.engine_ctx);
  assert_false(fx->adu._internal.device_props_report_pending);

  /* A retryable verdict IS re-armed -- otherwise the assertions above would
   * pass for a callback that simply did nothing. */
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_REPORT_STATUS,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_RETRY,
      fx->chan.engine_ctx);
  assert_true(fx->adu._internal.device_props_report_pending);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_starts_idle_and_pending_report, setup, teardown),
    cmocka_unit_test_setup_teardown(deployment_drives_full_workflow_single_step, setup, teardown),
    cmocka_unit_test_setup_teardown(verify_failure_blocks_download_and_fails, setup, teardown),
    cmocka_unit_test_setup_teardown(install_failure_triggers_rollback, setup, teardown),
    cmocka_unit_test_setup_teardown(hash_mismatch_blocks_install_and_fails, setup, teardown),
    cmocka_unit_test_setup_teardown(
        already_installed_is_rejected_without_download, setup, teardown),
    cmocka_unit_test_setup_teardown(install_in_progress_reenters_then_completes, setup, teardown),
    cmocka_unit_test_setup_teardown(reboot_required_persists_and_resumes, setup, teardown),
    cmocka_unit_test_setup_teardown(resume_with_no_persisted_state_stays_idle, setup, teardown),
    cmocka_unit_test_setup_teardown(cancel_action_sets_cancelled_flag, setup, teardown),
    cmocka_unit_test_setup_teardown(
        update_device_properties_is_accepted_without_reporting, setup, teardown),
    cmocka_unit_test_setup_teardown(report_carries_the_active_workflow_id, setup, teardown),
    cmocka_unit_test_setup_teardown(
        report_before_manifest_parse_has_no_step_results, setup, teardown),
    cmocka_unit_test_setup_teardown(report_preserves_step_results_at_capacity, setup, teardown),
    cmocka_unit_test_setup_teardown(
        custom_device_properties_are_accepted_and_serialized, setup, teardown),
    cmocka_unit_test_setup_teardown(
        public_initialize_takes_a_connection_and_builds_its_own_channel, setup, teardown),
    cmocka_unit_test_setup_teardown(extended_result_codes_are_bare_hex, setup, teardown),
    cmocka_unit_test_setup_teardown(device_props_too_small_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(device_props_buffer_size_matches_need, setup, teardown),
    cmocka_unit_test_setup_teardown(duplicate_redelivery_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(retry_with_newer_timestamp_restarts, setup, teardown),
    cmocka_unit_test_setup_teardown(replacement_with_new_id_restarts, setup, teardown),
    cmocka_unit_test_setup_teardown(retry_timestamp_survives_resume, setup, teardown),
    cmocka_unit_test_setup_teardown(same_id_changed_manifest_restarts, setup, teardown),
    cmocka_unit_test(microsoft_root_keys_are_embedded),
    cmocka_unit_test_setup_teardown(multi_step_update_runs_every_step_in_order, setup, teardown),
    cmocka_unit_test_setup_teardown(
        multi_step_report_preserves_progress_and_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(
        multi_step_failure_preserves_unexecuted_step_results, setup, teardown),
    cmocka_unit_test_setup_teardown(
        download_failure_is_reported_and_does_not_install, setup, teardown),
    cmocka_unit_test_setup_teardown(cancel_during_download_aborts_the_transfer, setup, teardown),
    cmocka_unit_test(build_report_with_too_small_a_buffer_is_rejected),
    cmocka_unit_test_setup_teardown(
        manifest_signed_by_an_unknown_root_key_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(malformed_jws_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(malformed_manifest_json_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        verify_file_hash_rejects_an_unsupported_algorithm, setup, teardown),
    cmocka_unit_test_setup_teardown(do_work_drives_the_channel, setup, teardown),
    cmocka_unit_test_setup_teardown(a_terminal_verdict_does_not_re_arm_the_report, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
