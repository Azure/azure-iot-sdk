// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* ADU client (Phase 1) unit tests. Drives the state machine through the public
 * API + the in-memory mock_mqtt_iface, with recording platform/crypto hooks.
 *
 * The manifest is taken from azure-sdk-for-c's own parser tests (the only known
 * parser-valid v5 manifest) and wrapped in the ADUv2 updateMetadata object. */
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

#include "internal/reconnect.h" /* az_iot_time_mono_ms */
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

/* The escaped updateManifest property shared by both payload shapes; `%s` is
 * the manifest version. */
#define ADU_TEST_MANIFEST_PROPERTY                                                              \
  "\"updateManifest\":\"{\\\"manifestVersion\\\":\\\"5\\\",\\\"updateId\\\":{\\\"provider\\\":" \
  "\\\"Contoso\\\",\\\"name\\\":\\\"Foobar\\\",\\\"version\\\":\\\"%s\\\"},"                    \
  "\\\"compatibility\\\":[{\\\"deviceManufacturer\\\":\\\"Contoso\\\",\\\"deviceModel\\\":"     \
  "\\\"Foobar\\\"}],\\\"instructions\\\":{\\\"steps\\\":[{\\\"handler\\\":\\\"microsoft/"       \
  "swupdate:1\\\",\\\"files\\\":[\\\"f2f4a804ca17afbae\\\"],\\\"handlerProperties\\\":{"        \
  "\\\"installedCriteria\\\":\\\"1.0\\\"}}]},\\\"files\\\":{\\\"f2f4a804ca17afbae\\\":{"        \
  "\\\"fileName\\\":\\\"iot-middleware-sample-adu-v1.1\\\",\\\"sizeInBytes\\\":844976,"         \
  "\\\"hashes\\\":{\\\"sha256\\\":\\\"xsoCnYAMkZZ7m9RL9Vyg9jKfFehCNxyuPFaJVM/"                  \
  "WBi0=\\\"}}},\\\"createdDateTime\\\":\\\"2022-07-07T03:02:48.8449038Z\\\"}\""

/* A single-step, single-file v5 deployment as the updateMetadata object the
 * channel delivers. Filled with workflow id, manifest version and a
 * structurally-valid JWS (see signed_patch()): core fully parses the JWS/SJWK
 * chain even though the mock crypto hook ignores the signature bytes. The
 * unknown property must be skipped. */
static const char k_patch_fmt[]
    = "{\"workflowId\":\"%s\",\"futureField\":{\"a\":[1,2]}," ADU_TEST_MANIFEST_PROPERTY ","
      "\"updateManifestSignature\":\"%s\","
      "\"fileUrls\":{\"f2f4a804ca17afbae\":\"http://example.com/payload.bin\"}}";

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

/* Build a single-step payload with a caller-chosen workflow `id` and manifest
 * `version`. Returns a static buffer, valid until the next call. */
static const char* build_patch_ex(const char* id, const char* version)
{
  static char patch[4096];
  char jws[2048];
  build_jws(jws, (int32_t)sizeof(jws));
  int n = snprintf(patch, sizeof(patch), k_patch_fmt, id, version, jws);
  assert_true(n > 0 && (size_t)n < sizeof(patch));
  return patch;
}

/* Build a single-step payload with the default manifest version ("1.1"). */
static const char* build_patch(const char* id) { return build_patch_ex(id, "1.1"); }

/* The default single-step deployment. */
static const char* signed_patch(void)
{
  return build_patch("51552a54-765e-419f-892a-c822549b6f38");
}

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
  const char* expected_download_url;
  int download_calls;

  /* Per-download record of which manifest file entry the engine resolved, so a
   * test can prove step-local file ids map to the right manifest entry. */
  char download_file_ids[MAX_OPS][32];
  char download_urls[MAX_OPS][64];

  bool install_in_progress_once; /* first install returns IN_PROGRESS */
  bool install_in_progress_consumed;

  /* Streaming per-file hash verification. */
  uint8_t file_hash[32]; /* what the incremental SHA-256 mock returns */
  size_t file_len; /* bytes the read-back mock serves */

  /* Persistence / resume. */
  uint8_t persist_blob[AZ_IOT_ADU_PERSIST_BLOB_SIZE];
  size_t persist_len;
  bool have_persist;
  int32_t persist_result;
  int persist_calls;
  int clear_calls;
  /* Last non-empty checkpoint, retained after the live record is retired. */
  uint8_t last_checkpoint[AZ_IOT_ADU_PERSIST_BLOB_SIZE];
  size_t last_checkpoint_len;

  /* URL the last download was handed. */
  char last_download_url[128];
} hook_log;

static void span_to_cstr(az_span s, char* out, size_t cap)
{
  size_t n = (size_t)az_span_size(s);
  if (n >= cap)
  {
    n = cap - 1;
  }
  if (n > 0)
  {
    memcpy(out, az_span_ptr(s), n);
  }
  out[n] = '\0';
}

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
  (void)file_count;
  hook_log* l = (hook_log*)ctx;
  int slot = l->download_calls;
  ++l->download_calls;
  if (slot < MAX_OPS)
  {
    span_to_cstr(file->id, l->download_file_ids[slot], sizeof(l->download_file_ids[slot]));
    span_to_cstr(url, l->download_urls[slot], sizeof(l->download_urls[slot]));
  }
  if (l->expected_download_url != NULL)
  {
    assert_true(az_span_is_content_equal(
        url, az_span_create_from_str((char*)(uintptr_t)l->expected_download_url)));
  }
  int32_t n = az_span_size(url);
  if (n < 0 || (size_t)n >= sizeof(l->last_download_url))
  {
    n = 0;
  }
  if (n > 0)
  {
    memcpy(l->last_download_url, az_span_ptr(url), (size_t)n);
  }
  l->last_download_url[n] = '\0';
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
  if (len == 0)
  {
    /* Invalidation. Mirrors the shipped adapters: the record is emptied, and a
     * later load reports a successful zero-length read rather than "absent". */
    ++l->clear_calls;
    if (l->persist_result != 0)
    {
      return l->persist_result;
    }
    l->persist_len = 0;
    l->have_persist = true;
    return 0;
  }
  ++l->persist_calls;
  if (l->persist_result != 0)
  {
    return l->persist_result;
  }
  assert_true(len <= sizeof(l->persist_blob));
  memcpy(l->persist_blob, blob, len);
  l->persist_len = len;
  l->have_persist = true;
  /* Sticky copy of the last checkpoint, so a test can still simulate a reboot
   * after the workflow finished and retired the live record. */
  memcpy(l->last_checkpoint, blob, len);
  l->last_checkpoint_len = len;
  return l->persist_result;
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
  az_iot_adu_operation last_request_operation;
  /* When true, the verdict is delivered from INSIDE request_update(), which the
   * channel contract explicitly permits for a synchronous channel. */
  bool result_is_synchronous;
  az_iot_adu_error_action synchronous_action;

  int report_count;
  /* AZ_IOT_OK unless a test wants to see a refusal handled. */
  az_iot_result report_result;
  az_iot_adu_report last_report;
  az_iot_adu_install_result last_install_result;
  const az_iot_adu_install_result* last_result_source;
  char last_workflow_id[128];
  char last_extended[AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH + 1];
  char last_details[256];
  bool last_had_installed_update_id;
  char last_installed_provider[64];
  char last_installed_name[64];
  char last_installed_version[64];
  size_t do_work_count;
  /* Simulates the channel deferring because the SERVICE asked, as opposed to
   * because an operation is already outstanding. Monotonic instant; 0 = none. */
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
  fake_channel* fc = (fake_channel*)ctx;
  /* Faithful to the DPS channel: its retry_after gate SELF-EXPIRES as a side
   * effect of being read, and several channel operations read it -- the
   * status-report path among them, which do_work() runs ahead of the fetch
   * path. Modelled on the channel tick because that is the earliest of them.
   * Without this the fake cannot reproduce the ordering hazard at all. */
  fc->do_work_count++;
  return AZ_IOT_OK;
}

static az_iot_result fake_channel_request_update(void* ctx, az_iot_adu_operation operation)
{
  fake_channel* fc = (fake_channel*)ctx;
  fc->request_update_count++;
  fc->last_request_operation = operation;
  if (fc->result_is_synchronous && fc->result_cb != NULL)
  {
    fc->result_cb(operation, AZ_IOT_ERR_DPS, fc->synchronous_action, NULL, fc->engine_ctx);
  }
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
  assert_non_null(report->install_result);
  /* The engine's result is reset on every workflow and on resume; a reset that
   * dropped the stamp would have the real channel's serializer refuse it. */
  assert_int_equal(report->_internal_size, sizeof(az_iot_adu_report));
  assert_int_equal(report->install_result->_internal_size, sizeof(az_iot_adu_install_result));
  fc->last_result_source = report->install_result;
  fc->last_install_result = *report->install_result;
  fc->last_report.install_result = &fc->last_install_result;
  assert_true(fc->last_install_result.step_results_count >= 0);
  assert_true(
      fc->last_install_result.step_results_count <= _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS);
  copy_str(fc->last_workflow_id, sizeof(fc->last_workflow_id), report->workflow_id);
  int32_t ext_len = report->install_result->extended_result_codes_length;
  assert_true(ext_len >= 0 && (size_t)ext_len < sizeof(fc->last_extended));
  if (ext_len > 0)
  {
    memcpy(fc->last_extended, report->install_result->extended_result_codes, (size_t)ext_len);
  }
  fc->last_extended[ext_len] = '\0';
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
  return fc->report_result;
}

/* The public macros mean what the header says they mean. Pinned because a
 * sample or an application compiled against a changed value would fail
 * silently, and because NO_TIMEOUT must stay the value the engine reads as
 * "no bound". */
static void the_public_timeout_macros_hold_their_contract(void** state)
{
  (void)state;
  assert_int_equal(AZ_IOT_ADU_REQUEST_NO_TIMEOUT, 0u);
  assert_int_equal(AZ_IOT_ADU_REQUEST_DEFAULT_TIMEOUT_MS, 60000u);
}

/* The request timeout these tests pass to the request functions. */
#define UT_TIMEOUT_MS 300000u
/* A service-requested delay far longer than UT_TIMEOUT_MS -- the case that
 * matters, since the protocol accepts retry-after values in hours. */
#define SERVICE_DELAY_MS 3600000u

static const az_iot_adu_channel_vtable k_fake_channel_vtable = {
  .open = fake_channel_open,
  .close = fake_channel_close,
  .request_update = fake_channel_request_update,
  .report = fake_channel_report,
  .set_device_properties = NULL,
  .do_work = fake_channel_do_work,
};

static void assert_extended_codes(const az_iot_adu_step_result* result, const char* expected)
{
  assert_int_equal(result->extended_result_codes_length, strlen(expected));
  assert_memory_equal(result->extended_result_codes, expected, strlen(expected));
}

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

  /* What the application would have been told. */
  int abandoned_count;
  az_iot_result last_abandoned_reason;
  az_iot_adu_operation last_abandoned_operation;
  int32_t last_error_code;
  char last_error_text[64];
  char last_tracking_id[64];
  uint32_t last_retry_after_ms;

  int state_event_count;
  az_iot_adu_state last_state;
  az_iot_adu_state last_previous_state;
} fixture;

/* Records whatever the client raises. */
static void on_event(const az_iot_adu_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  /* Every event the SDK raises carries its own size, so a callback compiled
   * against an older header can tell which fields are present. Checked on
   * every event rather than in one dedicated test: an absent or wrong stamp
   * is a silent ABI break that every other assertion here would survive. */
  assert_int_equal(event->_internal_size, (uint32_t)sizeof(*event));
  if (event->kind == AZ_IOT_ADU_EVENT_OPERATION_ABANDONED)
  {
    fx->abandoned_count++;
    fx->last_abandoned_reason = event->reason;
    fx->last_abandoned_operation = event->operation;
    fx->last_error_code = event->service_error.code;
    snprintf(fx->last_error_text, sizeof(fx->last_error_text), "%s", event->service_error.message);
    snprintf(
        fx->last_tracking_id, sizeof(fx->last_tracking_id), "%s", event->service_error.tracking_id);
    fx->last_retry_after_ms = event->service_error.retry_after_ms;
  }
  else if (event->kind == AZ_IOT_ADU_EVENT_WORKFLOW_STATE_CHANGED)
  {
    fx->state_event_count++;
    fx->last_state = event->state;
    fx->last_previous_state = event->previous_state;
  }
}

/* Counts events through a bare int, for the standalone-init case. */
static void count_events(const az_iot_adu_event* event, void* user_ctx)
{
  assert_int_equal(event->_internal_size, (uint32_t)sizeof(*event));
  (*(int*)user_ctx)++;
}

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
  fx->chan.report_result = AZ_IOT_OK;
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

/* ADUv2's updateMetadata carries no Cancel action (#243). The one way a
 * cancellation still reaches the engine is a checkpoint saved with it pending,
 * which az_iot_adu_client_resume() restores into this flag. Set it directly so
 * the engine's cancel handling stays covered. */
static void request_cancel(fixture* fx) { fx->adu._internal.cancel_requested = true; }

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

/* Pump only until a checkpoint has been written, modelling a device that
 * reboots the moment its state is durable. Pumping to Idle instead would let
 * the workflow finish and correctly retire the checkpoint, leaving nothing to
 * resume from. */
static void pump_to_checkpoint(fixture* fx, int max_iters)
{
  for (int i = 0; i < max_iters && fx->log.persist_calls == 0; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_true(fx->log.persist_calls > 0);
}

/* After Idle, every further report repeats the terminal result unchanged. */
static void assert_idle_report_retains_outcome(fixture* fx, az_iot_adu_outcome outcome)
{
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_true(fx->chan.report_count > 0);
  static az_iot_adu_install_result expected;
  expected = fx->chan.last_install_result;
  assert_int_equal(expected.outcome, outcome);
  char workflow_id[sizeof(fx->chan.last_workflow_id)];
  copy_str(workflow_id, sizeof(workflow_id), fx->chan.last_workflow_id);

  for (int i = 0; i < 2; ++i)
  {
    int report_count = fx->chan.report_count;
    assert_int_equal(az_iot_adu__report_state(&fx->adu), AZ_IOT_OK);
    assert_int_equal(fx->chan.report_count, report_count + 1);
    const az_iot_adu_install_result* actual = &fx->chan.last_install_result;
    assert_int_equal(actual->outcome, expected.outcome);
    assert_true(actual->result_code == expected.result_code);
    assert_int_equal(actual->failure_origin, expected.failure_origin);
    assert_int_equal(actual->extended_result_codes_length, expected.extended_result_codes_length);
    assert_memory_equal(
        actual->extended_result_codes,
        expected.extended_result_codes,
        expected.extended_result_codes_length);
    assert_int_equal(actual->step_results_count, expected.step_results_count);
    for (int32_t s = 0; s < expected.step_results_count; ++s)
    {
      assert_int_equal(actual->step_results[s].outcome, expected.step_results[s].outcome);
      assert_true(actual->step_results[s].result_code == expected.step_results[s].result_code);
    }
    assert_string_equal(fx->chan.last_workflow_id, workflow_id);
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
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_ptr_equal(fx->chan.last_result_source, &fx->adu._internal.install_result);
  assert_int_equal(fx->chan.last_install_result.step_results_count, 1);
  assert_int_equal(
      fx->chan.last_install_result.step_results[0].result_code,
      AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS);
  assert_extended_codes(&fx->chan.last_install_result.step_results[0], "0");
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
}

/* The provisioning channel delivers the ADUv2 updateMetadata object, not a twin
 * patch. It must drive the same workflow, keyed on its workflowId. */
static void update_metadata_drives_full_workflow(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  inject_patch(fx, build_patch("56db153e-6ae7-410f-9949-c201b6fd0d59"));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);

  pump(fx, 40);

  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  static const op_kind expect[]
      = { OP_VERIFY, OP_IS_INSTALLED, OP_DOWNLOAD, OP_BACKUP, OP_INSTALL, OP_APPLY };
  assert_true(ops_contain_sequence(&fx->log, expect, sizeof(expect) / sizeof(expect[0])));
  assert_string_equal(fx->log.last_download_url, "http://example.com/payload.bin");
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_string_equal(fx->chan.last_workflow_id, "56db153e-6ae7-410f-9949-c201b6fd0d59");

  /* The service re-offers the same workflow until it is superseded. */
  fx->log.op_count = 0;
  inject_patch(fx, build_patch("56db153e-6ae7-410f-9949-c201b6fd0d59"));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal((int)fx->log.op_count, 0);
}

/* A fileUrls value using JSON escapes reaches download_fn decoded. */
static void escaped_file_url_is_decoded(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  static const char plain[] = "\"http://example.com/payload.bin\"";
  static const char escaped[] = "\"https:\\/\\/host\\/p.bin?x=1\\u0026y=2\"";
  static char doc[4096];
  const char* base = build_patch("escaped-url");
  const char* at = strstr(base, plain);
  assert_non_null(at);
  int n = snprintf(
      doc, sizeof(doc), "%.*s%s%s", (int)(at - base), base, escaped, at + sizeof(plain) - 1);
  assert_true(n > 0 && (size_t)n < sizeof(doc));

  inject_patch(fx, doc);
  pump(fx, 40);
  assert_string_equal(fx->log.last_download_url, "https://host/p.bin?x=1&y=2");
}

static az_iot_result parse_with_roots(
    hook_log* log,
    const char* patch,
    const az_iot_adu_root_key* roots,
    size_t root_count,
    az_iot_adu_client_update_request* out_req,
    az_iot_adu_client_update_manifest* out_manifest);

/* updateManifest is decoded with \u support on both the managed and the
 * public path; a \u escape must not truncate it. */
static const char* manifest_with_unicode_escape(void)
{
  static char doc[4096];
  static const char plain[] = "iot-middleware-sample-adu-v1.1";
  const char* base = build_patch("unicode-manifest");
  const char* at = strstr(base, plain);
  assert_non_null(at);
  int n = snprintf(
      doc,
      sizeof(doc),
      "%.*siot-middleware-sample-adu-v1\\u002e1%s",
      (int)(at - base),
      base,
      at + sizeof(plain) - 1);
  assert_true(n > 0 && (size_t)n < sizeof(doc));
  return doc;
}

static void manifest_unicode_escape_is_decoded(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  inject_patch(fx, manifest_with_unicode_escape());
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);

  az_iot_adu_client_update_request req;
  az_iot_adu_client_update_manifest manifest;
  assert_int_equal(
      parse_with_roots(&fx->log, manifest_with_unicode_escape(), k_root_keys, 1, &req, &manifest),
      AZ_IOT_OK);
  assert_true(az_span_is_content_equal(
      manifest.files[0].file_name, AZ_SPAN_FROM_STR("iot-middleware-sample-adu-v1.1")));
  /* out_request exposes the decoded manifest, with no stale escaped tail. */
  int32_t len = az_span_size(req.update_manifest);
  assert_true(len > 0);
  assert_int_equal(az_span_ptr(req.update_manifest)[len - 1], (uint8_t)'}');
  assert_null(memchr(az_span_ptr(req.update_manifest), '\\', (size_t)len));
  assert_true(az_span_find(req.update_manifest, AZ_SPAN_FROM_STR("sample-adu-v1.1")) >= 0);
}

/* workflowId is decoded before it is stored, reported and compared: an escaped
 * spelling of the active id is a duplicate, and the report carries the decoded
 * id. An undecodable id is ignored. */
static void escaped_workflow_id_is_decoded(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  inject_patch(fx, build_patch("wf\\u002d1"));
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_string_equal(fx->chan.last_workflow_id, "wf-1");

  fx->log.op_count = 0;
  int reports = fx->chan.report_count;
  inject_patch(fx, build_patch("wf\\u002D1")); /* another spelling of the same id */
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  pump(fx, 5);
  assert_int_equal((int)fx->log.op_count, 0);
  assert_int_equal(fx->chan.report_count, reports);

  inject_patch(fx, build_patch("wf\\q"));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal((int)fx->log.op_count, 0);
}

/* An updateMetadata the engine cannot act on is ignored, not half-applied. */
static void unusable_update_metadata_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  const char* bad[] = {
    "{\"updateManifest\":\"{}\",\"updateManifestSignature\":\"a.b.c\"}",
    "{\"workflowId\":\"w\",\"updateManifestSignature\":\"a.b.c\"}",
    "{\"workflowId\":7,\"updateManifest\":\"{}\"}",
    "{\"workflowId\":\"w\",\"updateManifest\":\"{}\",\"fileUrls\":{\"f\":1}}",
    "{\"workflowId\":\"w\",\"updateManifest\":\"{}\",\"fileUrls\":\"u\"}",
    "{\"workflowId\":\"w\",\"updateManifest\":\"{}\",\"fileUrls\":[\"u\"]}",
    "{\"workflowId\":\"w\",\"updateManifest\":\"{}\",\"fileUrls\":null}",
    "{\"workflowId\":\"w\",\"updateManifest\":\"{}\"",
    "{\"workflowId\":\"w\",\"updateManifest\":\"{}\"} x",
    "{\"workflowId\":\"w\",\"updateManifest\":\"{}\",\"fileUrls\":{\"f\":\"a\\uD800\"}}",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
  {
    inject_patch(fx, bad[i]);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  }
  assert_int_equal((int)fx->log.op_count, 0);
}

/* fileUrls is bounded by _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT: at the bound
 * the payload is accepted, one past it is ignored rather than overflowing. */
static void file_urls_are_bounded(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  for (int count = _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT + 1;
       count >= _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT;
       --count)
  {
    char urls[256] = "";
    size_t used = 0;
    for (int i = 0; i < count; ++i)
    {
      int w = snprintf(urls + used, sizeof(urls) - used, "%s\"f%d\":\"u\"", i ? "," : "", i);
      assert_true(w > 0 && (size_t)w < sizeof(urls) - used);
      used += (size_t)w;
    }
    char doc[512];
    int n = snprintf(
        doc,
        sizeof(doc),
        "{\"workflowId\":\"w%d\",\"updateManifest\":\"{}\",\"fileUrls\":{%s}}",
        count,
        urls);
    assert_true(n > 0 && (size_t)n < sizeof(doc));

    inject_patch(fx, doc);
    assert_int_equal(
        az_iot_adu_client_get_state(&fx->adu),
        count > _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT ? AZ_IOT_ADU_STATE_IDLE
                                                        : AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);
  }
}

/* A payload larger than the request buffer is ignored, not truncated. */
static void oversized_update_metadata_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  static char big[AZ_IOT_ADU_REQUEST_BUFFER_SIZE + 64];
  const char* base = signed_patch();
  size_t len = strlen(base);
  memcpy(big, base, len - 1); /* drop the closing brace */
  size_t pos = len - 1;
  pos += (size_t)snprintf(big + pos, sizeof(big) - pos, ",\"pad\":\"");
  while (pos < sizeof(big) - 3)
  {
    big[pos++] = 'x';
  }
  big[pos++] = '"';
  big[pos++] = '}';
  big[pos] = '\0';
  assert_true(strlen(big) > AZ_IOT_ADU_REQUEST_BUFFER_SIZE);

  inject_patch(fx, big);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal((int)fx->log.op_count, 0);
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
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_FAILED);
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
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_FAILED);
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
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_FAILED);
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
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_SKIPPED);
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
  pump_to_checkpoint(fx, 40);
  assert_true(fx->log.have_persist);
  assert_true(fx->log.persist_len > 40);
  assert_true(fx->log.persist_len <= AZ_IOT_ADU_STATE_BLOB_MAX_SIZE);

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

/* The reported defect: the loaders are repeatable, so a checkpoint left behind
 * after the workflow finished would be reloaded on every later boot and the
 * obsolete workflow applied and reported again. */
static void finished_workflow_is_not_replayed_after_reboot(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  inject_patch(fx, signed_patch());
  pump_to_checkpoint(fx, 40);
  assert_true(fx->log.persist_len > 40);

  /* Reboot, then let the resumed workflow run to completion. */
  fx->log.install_result = AZ_IOT_ADU_RESULT_SUCCESS;
  assert_int_equal(az_iot_adu_client_resume(&fx->adu), AZ_IOT_OK);
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);

  /* Completion retired the record, so the store is now empty. */
  assert_true(fx->log.clear_calls > 0);
  assert_int_equal(fx->log.persist_len, 0);

  /* A later, unrelated reboot must not replay anything: resume is a clean
   * no-op and no further apply or report is produced. */
  fx->log.op_count = 0;
  fx->chan.last_install_result.outcome = AZ_IOT_ADU_OUTCOME_IN_PROGRESS;
  size_t reports_before = (size_t)fx->chan.report_count;
  assert_int_equal(az_iot_adu_client_resume(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(fx->log.op_count, 0);
  assert_int_equal((size_t)fx->chan.report_count, reports_before);
}

/* A superseding deployment must retire the stored checkpoint of the workflow
 * it replaces, otherwise a reboot resurrects the abandoned one. */
static void replacement_deployment_retires_the_stored_checkpoint(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  inject_patch(fx, build_patch("11111111-1111-1111-1111-111111111111"));
  pump_to_checkpoint(fx, 40);
  assert_true(fx->log.persist_len > 40);

  /* A different deployment id supersedes the checkpointed one. */
  inject_patch(fx, build_patch("22222222-2222-2222-2222-222222222222"));
  assert_true(fx->log.clear_calls > 0);
  assert_int_equal(fx->log.persist_len, 0);
}

/* Resume raises its state change, synchronously, from inside resume().
 *
 * Deliberately a SECOND client over the same persisted store: that is the real
 * post-reboot shape, and it is the only way the event is observable. The other
 * resume tests reuse the instance that is already at INSTALL_COMPLETE, so
 * set_adu_state() suppresses the unchanged value and nothing is raised -- they
 * would pass whether or not resume reported at all. */
static void resuming_a_fresh_client_reports_the_restored_state(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Get a non-Idle state into the store, stopping at the checkpoint: a workflow
   * that runs to completion now retires its own record, which would leave the
   * fresh client nothing to restore. */
  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  inject_patch(fx, signed_patch());
  pump_to_checkpoint(fx, 40);
  assert_true(fx->log.have_persist);

  az_iot_adu_platform_hooks hooks;
  az_iot_adu_crypto_hooks crypto;
  init_hooks(fx, &hooks, &crypto);

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Foobar";
  dp.installed_update_id.provider = "Contoso";
  dp.installed_update_id.name = "Foobar";
  dp.installed_update_id.version = "1.0";

  uint8_t dp_buf[256];
  az_iot_adu_client_config_options adu_opts = az_iot_adu_client_config_options_default();
  adu_opts.hooks = &hooks;
  adu_opts.crypto = &crypto;
  adu_opts.root_keys = k_root_keys;
  adu_opts.root_key_count = sizeof(k_root_keys) / sizeof(k_root_keys[0]);
  adu_opts.device_props = &dp;
  adu_opts.device_props_buffer = dp_buf;
  adu_opts.device_props_buffer_size = sizeof(dp_buf);

  fake_channel chan2;
  memset(&chan2, 0, sizeof(chan2));
  chan2.report_result = AZ_IOT_OK;
  az_iot_adu_channel channel2;
  channel2.vtable = &k_fake_channel_vtable;
  channel2.ctx = &chan2;

  /* Heap-allocated: the client struct is far too large for a test frame. */
  az_iot_adu_client_t* fresh = (az_iot_adu_client_t*)calloc(1, sizeof(*fresh));
  assert_non_null(fresh);
  assert_int_equal(
      az_iot_adu_client__initialize_with_channel(fresh, &channel2, &adu_opts), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(fresh), AZ_IOT_ADU_STATE_IDLE);

  /* Observing from before the resume, which is the only way to see it. */
  fx->state_event_count = 0;
  assert_int_equal(az_iot_adu_client_add_observer(fresh, on_event, fx), AZ_IOT_OK);

  assert_int_equal(az_iot_adu_client_resume(fresh), AZ_IOT_OK);

  /* Raised during resume(), not on a later pump. */
  assert_int_equal(fx->state_event_count, 1);
  assert_int_equal(fx->last_previous_state, AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(fx->last_state, AZ_IOT_ADU_STATE_INSTALL_COMPLETE);
  assert_int_equal(az_iot_adu_client_get_state(fresh), AZ_IOT_ADU_STATE_INSTALL_COMPLETE);

  az_iot_adu_client_destroy(fresh);
  free(fresh);
}

/* The engine's own result is stamped from initialize on, not only once the
 * first workflow resets it: nothing may ever serialize an unstamped result. */
static void a_fresh_client_result_is_already_stamped(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      fx->adu._internal.install_result._internal_size, sizeof(az_iot_adu_install_result));
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

/* A cancel that lands on the single tick the workflow spends in FAILED must not
 * displace the terminal failure. Reports are idempotent by workflow id, so
 * emitting CANCELED afterwards would publish two conflicting terminal outcomes
 * for the same deployment -- and contradict the step results, which stay
 * FAILED. */
static void late_cancel_does_not_overwrite_a_reported_failure(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->log.install_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, signed_patch());
  for (int i = 0; i < 40 && az_iot_adu_client_get_state(&fx->adu) != AZ_IOT_ADU_STATE_FAILED; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_FAILED);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  int reports_after_failure = fx->chan.report_count;

  /* The service cancels just as the failure is being reported. */
  request_cancel(fx);
  assert_true(az_iot_adu_is_cancelled(&fx->adu));
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);

  /* The failure stands, no second terminal report is emitted, and the workflow
   * still returns to Idle ready for the next deployment. */
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_int_equal(fx->chan.report_count, reports_after_failure);
  assert_false(az_iot_adu_is_cancelled(&fx->adu));
}

static void cancel_action_sets_cancelled_flag(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Start a deployment, then cancel mid-flight at a phase boundary. */
  inject_patch(fx, signed_patch());
  assert_int_equal(
      az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK); /* ManifestReceived -> Verifying */

  request_cancel(fx);
  assert_true(az_iot_adu_is_cancelled(&fx->adu));

  /* Next do_work honors cancellation and returns to Idle. */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_false(az_iot_adu_is_cancelled(&fx->adu));
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_CANCELED);
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

static void failure_after_success_does_not_replay_success(void** state)
{
  fixture* fx = (fixture*)*state;
  inject_patch(fx, signed_patch());
  pump(fx, 40);
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_SUCCEEDED);

  fx->log.verify_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, build_patch("failed-after-success"));
  pump(fx, 40);
  assert_string_equal(fx->chan.last_workflow_id, "failed-after-success");
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_FAILED);

  fx->log.verify_result = AZ_IOT_ADU_RESULT_SUCCESS;
  inject_patch(fx, build_patch("new-after-failure"));
  pump(fx, 2);
  assert_string_equal(fx->chan.last_workflow_id, "new-after-failure");
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_IN_PROGRESS);
  pump(fx, 40);
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
}

static void rejected_failure_report_retains_outcome_after_idle(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  fx->log.download_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, signed_patch());
  for (int i = 0; i < 40 && az_iot_adu_client_get_state(&fx->adu) != AZ_IOT_ADU_STATE_FAILED; ++i)
  {
    if (az_iot_adu_client_get_state(&fx->adu) == AZ_IOT_ADU_STATE_DOWNLOAD_STARTED)
    {
      fx->chan.report_result = AZ_IOT_ERR_TIMEOUT;
    }
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_FAILED);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_true(fx->adu._internal.device_props_report_pending);

  /* The refused failure report is retried, then the machine returns to Idle. */
  fx->chan.report_result = AZ_IOT_OK;
  pump(fx, 40);
  assert_false(fx->adu._internal.device_props_report_pending);
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_FAILED);
}

static void report_before_manifest_parse_has_no_step_results(void** state)
{
  fixture* fx = (fixture*)*state;
  inject_patch(fx, signed_patch());

  assert_int_equal(az_iot_adu__report_state(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.report_count, 1);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_IN_PROGRESS);
  assert_int_equal(fx->chan.last_install_result.step_results_count, 0);
}

static void report_preserves_step_results_at_capacity(void** state)
{
  fixture* fx = (fixture*)*state;
  inject_patch(fx, signed_patch());

  az_iot_adu_install_result* result = &fx->adu._internal.install_result;
  result->step_results_count = _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS;
  uint8_t details[] = { 'a', '\0', 'b' };
  for (int32_t i = 0; i < result->step_results_count; ++i)
  {
    result->step_results[i].result_code = 100 + i;
    result->step_results[i].outcome = AZ_IOT_ADU_OUTCOME_FAILED;
    result->step_results[i].failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE;
    az_iot_adu__set_extended_result(
        AZ_SPAN_FROM_BUFFER(result->step_results[i].extended_result_codes),
        &result->step_results[i].extended_result_codes_length,
        (uint32_t)AZ_IOT_ADU_EXTENDED_RESULT(AZ_IOT_ADU_FACILITY_INTERNAL, (uint32_t)i));
    memcpy(result->step_results[i].result_details, details, sizeof(details));
    result->step_results[i].result_details_length = sizeof(details);
  }

  assert_int_equal(az_iot_adu__report_state(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.report_count, 1);
  assert_int_equal(
      fx->chan.last_install_result.step_results_count, _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS);

  /* A retaining channel copies the array and span bytes before the engine
   * reuses its storage for another workflow. */
  memset(result, 0, sizeof(*result));
  memset(details, 0, sizeof(details));
  static const uint8_t expected_details[] = { 'a', '\0', 'b' };
  for (int32_t i = 0; i < fx->chan.last_install_result.step_results_count; ++i)
  {
    const az_iot_adu_step_result* step = &fx->chan.last_install_result.step_results[i];
    assert_int_equal(step->result_code, 100 + i);
    char extended[9];
    snprintf(
        extended,
        sizeof(extended),
        "%x",
        (unsigned)AZ_IOT_ADU_EXTENDED_RESULT(AZ_IOT_ADU_FACILITY_INTERNAL, (uint32_t)i));
    assert_extended_codes(step, extended);
    assert_int_equal(step->result_details_length, sizeof(expected_details));
    assert_memory_equal(step->result_details, expected_details, sizeof(expected_details));
  }
}

/* Compatibility properties belong to fetches, not ADUv2 status reports. */
static void custom_device_properties_remain_cached(void** state)
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

  const az_iot_adu_device_custom_properties* cached = &fx->adu._internal.custom_props_view;
  assert_int_equal(cached->count, 2);
  assert_true(az_span_is_content_equal(cached->names[0], AZ_SPAN_FROM_STR("location")));
  assert_true(az_span_is_content_equal(cached->values[0], AZ_SPAN_FROM_STR("building42")));
  assert_true(az_span_is_content_equal(cached->names[1], AZ_SPAN_FROM_STR("tier")));
  assert_true(az_span_is_content_equal(cached->values[1], AZ_SPAN_FROM_STR("gold")));
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

static void replacement_with_new_id_restarts(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);
  /* Drain the startup device-properties report so the next do_work advances
   * the state machine rather than the report. */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);

  /* Start deployment A and let it advance past ManifestReceived. */
  inject_patch(fx, build_patch("aaaaaaaa-0000-0000-0000-000000000001"));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_VERIFYING_MANIFEST);

  /* A different deployment id arrives mid-flight: a replacement restarts from
   * ManifestReceived (state moves backwards, proving it was not ignored). */
  inject_patch(fx, build_patch("bbbbbbbb-0000-0000-0000-000000000002"));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);

  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

static void workflow_id_survives_resume(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* The install requires a reboot, so the workflow snapshots itself. */
  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  inject_patch(fx, signed_patch());
  /* Stop at the checkpoint: run to completion, the workflow retires its own
   * record and the resume below would find nothing to restore. */
  pump_to_checkpoint(fx, 40);
  assert_true(fx->log.have_persist);

  /* Simulate the reboot: forget the in-RAM workflow, resume from the blob. */
  fx->log.op_count = 0;
  fx->log.install_result = AZ_IOT_ADU_RESULT_SUCCESS;
  assert_int_equal(az_iot_adu_client_resume(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_INSTALL_COMPLETE);

  /* The service re-offers the same workflow while the resumed one finishes. The
   * id round-tripped through the snapshot, so it is ignored as a duplicate. */
  inject_patch(fx, signed_patch());
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_INSTALL_COMPLETE);

  /* And the resumed workflow still completes normally. */
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
}

/* workflowId is the sole identity: the same id with different manifest bytes
 * is a redelivery, not a new deployment, so no second install or report. */
static void same_id_changed_manifest_is_a_duplicate(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  inject_patch(fx, build_patch_ex("51552a54-765e-419f-892a-c822549b6f38", "1.1"));
  pump(fx, 40);
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  int reports = fx->chan.report_count;

  fx->log.op_count = 0;
  inject_patch(fx, build_patch_ex("51552a54-765e-419f-892a-c822549b6f38", "1.2"));
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  pump(fx, 5);
  assert_int_equal((int)fx->log.op_count, 0);
  assert_int_equal(fx->chan.report_count, reports);
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
/* workflow: multi-step, download failure                                    */
/* ------------------------------------------------------------------------- */

/* Two-step variant of k_patch_fmt. Both steps use the same file so the fixture's
 * single hash still verifies; what is under test is the ordering, not the file
 * set. */
static const char k_patch_two_steps_fmt[]
    = "{\"workflowId\":\"%s\","
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
      "\"fileUrls\":{\"f2f4a804ca17afbae\":\"http://example.com/payload.bin\"}}";

static const char* two_step_patch(void)
{
  static char patch[4096];
  char jws[2048];
  build_jws(jws, (int32_t)sizeof(jws));
  int n
      = snprintf(patch, sizeof(patch), k_patch_two_steps_fmt, "multi-step-deployment", "1.1", jws);
  assert_true(n > 0 && (size_t)n < sizeof(patch));
  return patch;
}

/* Two steps, each with its own file. The `files` map deliberately lists
 * "fa00000000000001" before "fb00000000000002" while step 0 references the
 * *second* entry, so resolving a step-local file slot positionally against
 * manifest.files[] yields the wrong file. Both entries share one hash because
 * the streaming SHA-256 mock returns a single fixed digest. */
static const char k_patch_distinct_files_fmt[]
    = "{\"workflowId\":\"%s\","
      "\"updateManifest\":\"{\\\"manifestVersion\\\":\\\"5\\\",\\\"updateId\\\":{\\\"provider\\\":"
      "\\\"Contoso\\\",\\\"name\\\":\\\"Foobar\\\",\\\"version\\\":\\\"%s\\\"},"
      "\\\"compatibility\\\":[{\\\"deviceManufacturer\\\":\\\"Contoso\\\",\\\"deviceModel\\\":"
      "\\\"Foobar\\\"}],\\\"instructions\\\":{\\\"steps\\\":[{\\\"handler\\\":\\\"microsoft/"
      "swupdate:1\\\",\\\"files\\\":[\\\"fb00000000000002\\\"],\\\"handlerProperties\\\":{"
      "\\\"installedCriteria\\\":\\\"1.0\\\"}},{\\\"handler\\\":\\\"microsoft/"
      "swupdate:1\\\",\\\"files\\\":[\\\"fa00000000000001\\\"],\\\"handlerProperties\\\":{"
      "\\\"installedCriteria\\\":\\\"1.1\\\"}}]},\\\"files\\\":{\\\"fa00000000000001\\\":{"
      "\\\"fileName\\\":\\\"payload-a.bin\\\",\\\"sizeInBytes\\\":844976,"
      "\\\"hashes\\\":{\\\"sha256\\\":\\\"" ADU_TEST_FILE_HASH_B64 "\\\"}},"
      "\\\"fb00000000000002\\\":{"
      "\\\"fileName\\\":\\\"payload-b.bin\\\",\\\"sizeInBytes\\\":844976,"
      "\\\"hashes\\\":{\\\"sha256\\\":\\\"" ADU_TEST_FILE_HASH_B64 "\\\"}}},"
      "\\\"createdDateTime\\\":\\\"2022-07-07T03:02:48.8449038Z\\\"}\","
      "\"updateManifestSignature\":\"%s\","
      "\"fileUrls\":{\"fa00000000000001\":\"http://example.com/payload-a.bin\","
      "\"fb00000000000002\":\"http://example.com/payload-b.bin\"}}";

static const char* distinct_files_patch(void)
{
  static char patch[4096];
  char jws[2048];
  build_jws(jws, (int32_t)sizeof(jws));
  int n = snprintf(
      patch, sizeof(patch), k_patch_distinct_files_fmt, "distinct-files-deployment", "1.1", jws);
  assert_true(n > 0 && (size_t)n < sizeof(patch));
  return patch;
}

/* Each step must download the file its own `files` entry names. Indexing
 * manifest.files[] by the per-step file counter would hand both steps the
 * manifest's first entry. */
static void each_step_downloads_its_own_file(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  inject_patch(fx, distinct_files_patch());
  pump(fx, 60);

  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(fx->log.download_calls, 2);

  assert_string_equal(fx->log.download_file_ids[0], "fb00000000000002");
  assert_string_equal(fx->log.download_urls[0], "http://example.com/payload-b.bin");

  assert_string_equal(fx->log.download_file_ids[1], "fa00000000000001");
  assert_string_equal(fx->log.download_urls[1], "http://example.com/payload-a.bin");
}

/* Resume after step 0 must download step 1's own file, not the one step 0
 * already fetched. */
static void resume_with_distinct_files_downloads_next_step_file(void** state)
{
  fixture* source = (fixture*)*state;
  source->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  inject_patch(source, distinct_files_patch());
  for (int i = 0; i < 40 && !source->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&source->adu), AZ_IOT_OK);
  }
  assert_true(source->log.have_persist);
  assert_int_equal(source->adu._internal.current_step, 0);
  assert_int_equal(source->log.download_calls, 1);
  assert_string_equal(source->log.download_file_ids[0], "fb00000000000002");

  void* fresh_state = NULL;
  assert_int_equal(setup(&fresh_state), 0);
  fixture* fresh = (fixture*)fresh_state;
  memcpy(fresh->log.persist_blob, source->log.persist_blob, source->log.persist_len);
  fresh->log.persist_len = source->log.persist_len;
  fresh->log.have_persist = true;
  assert_int_equal(az_iot_adu_client_resume(&fresh->adu), AZ_IOT_OK);
  pump(fresh, 60);

  assert_int_equal(az_iot_adu_client_get_state(&fresh->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(fresh->log.download_calls, 1);
  assert_string_equal(fresh->log.download_file_ids[0], "fa00000000000001");
  assert_string_equal(fresh->log.download_urls[0], "http://example.com/payload-a.bin");
  assert_int_equal(fresh->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(teardown(&fresh_state), 0);
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
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(fx->chan.last_install_result.step_results_count, 2);
  for (int32_t i = 0; i < fx->chan.last_install_result.step_results_count; ++i)
  {
    assert_int_equal(
        fx->chan.last_install_result.step_results[i].result_code,
        AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS);
    assert_extended_codes(&fx->chan.last_install_result.step_results[i], "0");
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

  const az_iot_adu_install_result* report = &fx->chan.last_install_result;
  assert_int_equal(report->outcome, AZ_IOT_ADU_OUTCOME_IN_PROGRESS);
  assert_int_equal(report->step_results_count, 2);
  assert_int_equal(report->step_results[0].result_code, AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS);
  assert_int_equal(report->step_results[0].outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_extended_codes(&report->step_results[0], "0");
  assert_int_equal(report->step_results[1].result_code, 0);
  assert_int_equal(report->step_results[1].outcome, AZ_IOT_ADU_OUTCOME_IN_PROGRESS);
  assert_extended_codes(&report->step_results[1], "0");

  fx->log.install_result = AZ_IOT_ADU_RESULT_FAILURE;
  fx->log.restore_result = AZ_IOT_ADU_RESULT_FAILURE;
  pump(fx, 40);

  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(report->outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_int_equal(report->step_results_count, 2);
  assert_int_equal(report->step_results[0].result_code, AZ_IOT_ADU_AGENT_RESULT_CODE_SUCCESS);
  assert_int_equal(report->step_results[0].outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(report->step_results[1].result_code, 700 - AZ_IOT_ADU_FACILITY_INSTALL);
  assert_int_equal(report->step_results[1].outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_extended_codes(&report->step_results[1], "5fffffff");
  assert_int_equal(fx->adu._internal.install_result.extended_result_codes_length, 8);
  assert_memory_equal(fx->adu._internal.install_result.extended_result_codes, "7fffffff", 8);
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_FAILED);
}

static void multi_step_failure_preserves_unexecuted_step_results(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->log.download_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, two_step_patch());
  pump(fx, 40);

  const az_iot_adu_install_result* report = &fx->chan.last_install_result;
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(report->outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_int_equal(report->step_results_count, 2);
  assert_int_equal(report->step_results[0].result_code, 700 - AZ_IOT_ADU_FACILITY_DOWNLOAD);
  assert_int_equal(report->step_results[0].outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_extended_codes(&report->step_results[0], "2fffffff");
  assert_int_equal(report->step_results[1].result_code, 0);
  assert_int_equal(report->step_results[1].outcome, AZ_IOT_ADU_OUTCOME_SKIPPED);
  assert_extended_codes(&report->step_results[1], "0");
}

static void canonical_results_survive_idle_and_next_workflow(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->log.download_result = AZ_IOT_ADU_RESULT_FAILURE;
  inject_patch(fx, two_step_patch());
  pump(fx, 40);
  assert_int_equal(az_iot_adu__report_state(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_int_equal(fx->chan.last_install_result.step_results[0].outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_int_equal(
      fx->chan.last_install_result.step_results[1].outcome, AZ_IOT_ADU_OUTCOME_SKIPPED);

  inject_patch(fx, signed_patch());
  assert_int_equal(az_iot_adu__report_state(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.last_install_result.step_results_count, 0);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_IN_PROGRESS);
  fx->log.download_result = AZ_IOT_ADU_RESULT_SUCCESS;
  pump(fx, 40);
  assert_int_equal(az_iot_adu__report_state(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
}

static void canonical_cancel_marks_only_unfinished_steps(void** state)
{
  fixture* fx = (fixture*)*state;
  inject_patch(fx, two_step_patch());
  for (int i = 0; i < 40 && fx->adu._internal.current_step == 0; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_int_equal(fx->adu._internal.current_step, 1);
  request_cancel(fx);
  pump(fx, 2);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_CANCELED);
  assert_int_equal(
      fx->chan.last_install_result.step_results[0].outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(
      fx->chan.last_install_result.step_results[1].outcome, AZ_IOT_ADU_OUTCOME_CANCELED);
}

static void canonical_cancel_skips_future_steps(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->log.download_result = AZ_IOT_ADU_RESULT_IN_PROGRESS;
  inject_patch(fx, two_step_patch());
  pump(fx, 4);
  request_cancel(fx);
  pump(fx, 2);
  assert_int_equal(
      fx->chan.last_install_result.step_results[0].outcome, AZ_IOT_ADU_OUTCOME_CANCELED);
  assert_int_equal(
      fx->chan.last_install_result.step_results[1].outcome, AZ_IOT_ADU_OUTCOME_SKIPPED);
}

static void checkpoint_second_step(fixture* fx)
{
  inject_patch(fx, two_step_patch());
  for (int i = 0; i < 40 && fx->adu._internal.current_step == 0; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_int_equal(fx->adu._internal.current_step, 1);
  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
}

static void canonical_snapshot_preserves_owned_text_and_int64(void** state)
{
  fixture* fx = (fixture*)*state;
  checkpoint_second_step(fx);
  az_iot_adu_install_result* result = &fx->adu._internal.install_result;
  result->result_code = INT64_MIN;
  memcpy(result->result_details, "overall", 7);
  result->result_details_length = 7;
  result->step_results[0].result_code = INT64_MAX;
  memcpy(result->step_results[0].extended_result_codes, "FFFFFFFF,0", 10);
  result->step_results[0].extended_result_codes_length = 10;
  uint8_t details[] = { 'a', 0, 'b' };
  memcpy(result->step_results[0].result_details, details, sizeof(details));
  result->step_results[0].result_details_length = sizeof(details);
  for (int i = 0; i < 40 && !fx->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_true(fx->log.have_persist);
  assert_int_equal(fx->log.persist_blob[4], 4);

  void* fresh_state = NULL;
  assert_int_equal(setup(&fresh_state), 0);
  fixture* fresh = (fixture*)fresh_state;
  memcpy(fresh->log.persist_blob, fx->log.persist_blob, fx->log.persist_len);
  fresh->log.persist_len = fx->log.persist_len;
  fresh->log.have_persist = true;
  assert_int_equal(az_iot_adu_client_resume(&fresh->adu), AZ_IOT_OK);
  memset(fresh->adu._internal.persist_scratch, 0, sizeof(fresh->adu._internal.persist_scratch));
  memset(details, 0xff, sizeof(details));
  assert_int_equal(az_iot_adu__report_state(&fresh->adu), AZ_IOT_OK);
  const az_iot_adu_install_result* restored = &fresh->chan.last_install_result;
  assert_int_equal(restored->result_code, INT64_MIN);
  assert_int_equal(restored->step_results[0].result_code, INT64_MAX);
  assert_int_equal(restored->result_details_length, 7);
  assert_memory_equal(restored->result_details, "overall", 7);
  assert_extended_codes(&restored->step_results[0], "FFFFFFFF,0");
  static const uint8_t expected[] = { 'a', 0, 'b' };
  assert_int_equal(restored->step_results[0].result_details_length, sizeof(expected));
  assert_memory_equal(restored->step_results[0].result_details, expected, sizeof(expected));
  assert_int_equal(restored->step_results[0].outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(restored->step_results[1].outcome, AZ_IOT_ADU_OUTCOME_IN_PROGRESS);
  pump(fresh, 40);
  assert_int_equal(fresh->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(teardown(&fresh_state), 0);
}

static uint32_t snapshot_u32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void snapshot_write_u32(uint8_t* p, uint32_t value)
{
  for (int i = 0; i < 4; ++i)
  {
    p[i] = (uint8_t)(value >> (8 * i));
  }
}

static uint32_t snapshot_crc(const uint8_t* bytes, size_t count)
{
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < count; ++i)
  {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit)
    {
      crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
  }
  return ~crc;
}

static void canonical_resume_accepts_v2_checkpoint(void** state)
{
  fixture* fx = (fixture*)*state;
  checkpoint_second_step(fx);
  for (int i = 0; i < 40 && !fx->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_true(fx->log.have_persist);
  uint8_t* blob = fx->log.persist_blob;
  size_t trailer = 40 + snapshot_u32(blob + 36);
  blob[4] = 2;
  blob[5] = 0;
  snapshot_write_u32(blob + trailer + 12, 0);
  snapshot_write_u32(blob + trailer + 16, 0);
  snapshot_write_u32(blob + trailer + 20, 2);
  snapshot_write_u32(blob + trailer + 24, 700);
  snapshot_write_u32(blob + trailer + 28, 0);
  snapshot_write_u32(blob + trailer + 32, 0);
  snapshot_write_u32(blob + trailer + 36, 0);
  snapshot_write_u32(blob + trailer + 40, snapshot_crc(blob, trailer + 40));
  fx->log.persist_len = trailer + 44;

  void* fresh_state = NULL;
  assert_int_equal(setup(&fresh_state), 0);
  fixture* fresh = (fixture*)fresh_state;
  memcpy(fresh->log.persist_blob, blob, fx->log.persist_len);
  fresh->log.persist_len = fx->log.persist_len;
  fresh->log.have_persist = true;
  assert_int_equal(az_iot_adu_client_resume(&fresh->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu__report_state(&fresh->adu), AZ_IOT_OK);
  assert_int_equal(fresh->chan.last_install_result.result_code, 1);
  assert_int_equal(
      fresh->chan.last_install_result.step_results[0].outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(
      fresh->chan.last_install_result.step_results[1].outcome, AZ_IOT_ADU_OUTCOME_IN_PROGRESS);
  pump(fresh, 40);
  assert_int_equal(fresh->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(teardown(&fresh_state), 0);
}

static void canonical_snapshot_reports_storage_errors(void** state)
{
  fixture* fx = (fixture*)*state;
  checkpoint_second_step(fx);
  fx->log.persist_result = -1;
  az_iot_result status = AZ_IOT_OK;
  for (int i = 0; i < 40 && status == AZ_IOT_OK; ++i)
  {
    status = az_iot_adu_client_do_work(&fx->adu);
  }
  assert_int_equal(status, AZ_IOT_ERR_INTERNAL);
}

static void failed_checkpoint_retries_without_reinstalling_or_applying(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  fx->log.persist_result = -1;
  inject_patch(fx, signed_patch());
  az_iot_result status = AZ_IOT_OK;
  for (int i = 0; i < 40 && status == AZ_IOT_OK; ++i)
  {
    status = az_iot_adu_client_do_work(&fx->adu);
  }
  assert_int_equal(status, AZ_IOT_ERR_INTERNAL);
  assert_true(fx->adu._internal.checkpoint_pending);
  assert_false(fx->log.have_persist);
  assert_int_equal(fx->log.persist_calls, 1);
  size_t operations = fx->log.op_count;
  for (int i = 0; i < 3; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_ERR_INTERNAL);
    assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_INSTALL_COMPLETE);
    assert_int_equal(fx->log.op_count, operations);
  }
  assert_int_equal(fx->log.persist_calls, 4);
  fx->log.persist_result = 0;
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_false(fx->adu._internal.checkpoint_pending);
  assert_true(fx->log.have_persist);
  assert_int_equal(fx->log.persist_calls, 5);
  assert_int_equal(fx->log.op_count, operations);
  pump(fx, 40);
  assert_int_equal(fx->log.op_count, operations + 1);
  assert_int_equal(fx->log.ops[operations], OP_APPLY);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
}

static void missing_checkpoint_hook_blocks_apply(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  fx->adu._internal.hooks.persist_state_fn = NULL;
  inject_patch(fx, signed_patch());
  az_iot_result status = AZ_IOT_OK;
  for (int i = 0; i < 40 && status == AZ_IOT_OK; ++i)
  {
    status = az_iot_adu_client_do_work(&fx->adu);
  }
  assert_int_equal(status, AZ_IOT_ERR_NOT_SUPPORTED);
  size_t operations = fx->log.op_count;
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_int_equal(fx->log.op_count, operations);
  assert_true(fx->adu._internal.checkpoint_pending);
  fx->adu._internal.hooks.persist_state_fn = mock_persist;
  pump(fx, 40);
  assert_int_equal(fx->log.persist_calls, 1);
  /* The completed workflow retired its own checkpoint. */
  assert_int_equal(fx->log.clear_calls, 1);
  assert_int_equal(fx->log.persist_len, 0);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
}

static void cancellation_clears_a_pending_checkpoint(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  fx->log.persist_result = -1;
  inject_patch(fx, signed_patch());
  az_iot_result status = AZ_IOT_OK;
  for (int i = 0; i < 40 && status == AZ_IOT_OK; ++i)
  {
    status = az_iot_adu_client_do_work(&fx->adu);
  }
  assert_int_equal(status, AZ_IOT_ERR_INTERNAL);
  int writes = fx->log.persist_calls;
  request_cancel(fx);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_false(fx->adu._internal.checkpoint_pending);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_CANCELED);
  assert_int_equal(fx->log.persist_calls, writes);
  fx->log.install_result = AZ_IOT_ADU_RESULT_SUCCESS;
  inject_patch(fx, build_patch("after-cancel"));
  pump(fx, 40);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(fx->log.persist_calls, writes);
}

static void replacement_clears_a_pending_checkpoint(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  fx->log.persist_result = -1;
  inject_patch(fx, signed_patch());
  az_iot_result status = AZ_IOT_OK;
  for (int i = 0; i < 40 && status == AZ_IOT_OK; ++i)
  {
    status = az_iot_adu_client_do_work(&fx->adu);
  }
  assert_int_equal(status, AZ_IOT_ERR_INTERNAL);
  int writes = fx->log.persist_calls;
  fx->log.install_result = AZ_IOT_ADU_RESULT_SUCCESS;
  inject_patch(fx, build_patch("replacement"));
  assert_false(fx->adu._internal.checkpoint_pending);
  pump(fx, 40);
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_string_equal(fx->chan.last_workflow_id, "replacement");
  assert_int_equal(fx->log.persist_calls, writes);
}

static void fill_maximum_text(
    uint8_t* codes,
    int32_t* codes_length,
    uint8_t* details,
    int32_t* details_length)
{
  memset(codes, '0', AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH);
  *codes_length = AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH;
  for (size_t j = 0; j < AZ_IOT_ADU_RESULT_DETAILS_MAX_SIZE; j += 4)
  {
    static const uint8_t character[] = { 0xf0, 0x9f, 0x98, 0x80 };
    memcpy(details + j, character, sizeof(character));
  }
  *details_length = AZ_IOT_ADU_RESULT_DETAILS_MAX_SIZE;
}

static void assert_maximum_text(
    const uint8_t* codes,
    int32_t codes_length,
    const uint8_t* details,
    int32_t details_length)
{
  assert_int_equal(codes_length, AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH);
  assert_int_equal(details_length, AZ_IOT_ADU_RESULT_DETAILS_MAX_SIZE);
  for (int32_t i = 0; i < codes_length; ++i)
  {
    assert_int_equal(codes[i], '0');
  }
  for (int32_t i = 0; i < details_length; i += 4)
  {
    static const uint8_t character[] = { 0xf0, 0x9f, 0x98, 0x80 };
    assert_memory_equal(details + i, character, sizeof(character));
  }
}

static void canonical_snapshot_preserves_maximum_text(void** state)
{
  fixture* fx = (fixture*)*state;
  checkpoint_second_step(fx);
  az_iot_adu_install_result* result = &fx->adu._internal.install_result;
  fill_maximum_text(
      result->extended_result_codes,
      &result->extended_result_codes_length,
      result->result_details,
      &result->result_details_length);
  for (int32_t i = 0; i < result->step_results_count; ++i)
  {
    az_iot_adu_step_result* item = &result->step_results[i];
    fill_maximum_text(
        item->extended_result_codes,
        &item->extended_result_codes_length,
        item->result_details,
        &item->result_details_length);
  }
  for (int i = 0; i < 40 && !fx->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_true(fx->log.have_persist);
  void* fresh_state = NULL;
  assert_int_equal(setup(&fresh_state), 0);
  fixture* fresh = (fixture*)fresh_state;
  memcpy(fresh->log.persist_blob, fx->log.persist_blob, fx->log.persist_len);
  fresh->log.persist_len = fx->log.persist_len;
  fresh->log.have_persist = true;
  assert_int_equal(az_iot_adu_client_resume(&fresh->adu), AZ_IOT_OK);
  const az_iot_adu_install_result* restored = &fresh->adu._internal.install_result;
  assert_maximum_text(
      restored->extended_result_codes,
      restored->extended_result_codes_length,
      restored->result_details,
      restored->result_details_length);
  for (int32_t i = 0; i < restored->step_results_count; ++i)
  {
    const az_iot_adu_step_result* actual = &restored->step_results[i];
    assert_maximum_text(
        actual->extended_result_codes,
        actual->extended_result_codes_length,
        actual->result_details,
        actual->result_details_length);
  }
  assert_int_equal(teardown(&fresh_state), 0);
}

static void cancel_after_resume_does_not_overwrite_result_text(void** state)
{
  fixture* fx = (fixture*)*state;
  checkpoint_second_step(fx);
  memcpy(fx->adu._internal.install_result.step_results[0].result_details, "completed", 9);
  fx->adu._internal.install_result.step_results[0].result_details_length = 9;
  for (int i = 0; i < 40 && !fx->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_true(fx->log.have_persist);

  void* fresh_state = NULL;
  assert_int_equal(setup(&fresh_state), 0);
  fixture* fresh = (fixture*)fresh_state;
  memcpy(fresh->log.persist_blob, fx->log.persist_blob, fx->log.persist_len);
  fresh->log.persist_len = fx->log.persist_len;
  fresh->log.have_persist = true;
  assert_int_equal(az_iot_adu_client_resume(&fresh->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fresh->adu), AZ_IOT_OK);
  request_cancel(fresh);
  pump(fresh, 2);

  const az_iot_adu_install_result* result = &fresh->chan.last_install_result;
  assert_int_equal(result->outcome, AZ_IOT_ADU_OUTCOME_CANCELED);
  assert_int_equal(result->step_results[0].outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_extended_codes(&result->step_results[0], "0");
  assert_int_equal(result->step_results[0].result_details_length, 9);
  assert_memory_equal(result->step_results[0].result_details, "completed", 9);
  assert_int_equal(result->step_results[1].outcome, AZ_IOT_ADU_OUTCOME_CANCELED);
  assert_extended_codes(&result->step_results[1], "0");
  az_iot_adu_report report = AZ_IOT_ADU_REPORT_INIT;
  report.workflow_id = fresh->chan.last_workflow_id;
  report.install_result = result;
  uint8_t json[2048];
  size_t written;
  assert_int_equal(az_iot_adu_build_report(&report, json, sizeof(json), &written), AZ_IOT_OK);
  assert_true(written > 0);
  assert_int_equal(teardown(&fresh_state), 0);
}

static void canonical_resume_rejects_corruption(void** state)
{
  fixture* fx = (fixture*)*state;
  checkpoint_second_step(fx);
  for (int i = 0; i < 40 && !fx->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  }
  assert_true(fx->log.have_persist);
  fx->log.persist_blob[fx->log.persist_len - 1] ^= 1;
  assert_int_equal(az_iot_adu_client_resume(&fx->adu), AZ_IOT_ERR_INVALID_ARG);
  fx->log.persist_blob[fx->log.persist_len - 1] ^= 1;
  size_t trailer = 40 + snapshot_u32(fx->log.persist_blob + 36);
  snapshot_write_u32(fx->log.persist_blob + trailer + 16 + 16, UINT32_MAX);
  snapshot_write_u32(
      fx->log.persist_blob + fx->log.persist_len - 4,
      snapshot_crc(fx->log.persist_blob, fx->log.persist_len - 4));
  assert_int_equal(az_iot_adu_client_resume(&fx->adu), AZ_IOT_ERR_INVALID_ARG);
}

/* A record that could never be invalidated is refused, or every later boot
 * would resume and re-apply it. */
static void resume_without_a_persist_hook_is_refused(void** state)
{
  fixture* source = (fixture*)*state;
  checkpoint_second_step(source);
  for (int i = 0; i < 40 && !source->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&source->adu), AZ_IOT_OK);
  }
  assert_true(source->log.have_persist);

  void* fresh_state = NULL;
  assert_int_equal(setup(&fresh_state), 0);
  fixture* fresh = (fixture*)fresh_state;
  memcpy(fresh->log.persist_blob, source->log.persist_blob, source->log.persist_len);
  fresh->log.persist_len = source->log.persist_len;
  fresh->log.have_persist = true;
  fresh->adu._internal.hooks.persist_state_fn = NULL;
  az_iot_adu_client_t* expected = malloc(sizeof(*expected));
  assert_non_null(expected);
  *expected = fresh->adu;

  assert_int_equal(az_iot_adu_client_resume(&fresh->adu), AZ_IOT_ERR_NOT_SUPPORTED);
  memcpy(
      expected->_internal.persist_scratch,
      fresh->adu._internal.persist_scratch,
      sizeof(expected->_internal.persist_scratch));
  assert_memory_equal(&fresh->adu, expected, sizeof(*expected));

  /* An invalidated (zero-length) record is still a clean no-op. */
  fresh->log.persist_len = 0;
  assert_int_equal(az_iot_adu_client_resume(&fresh->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_get_state(&fresh->adu), AZ_IOT_ADU_STATE_IDLE);
  free(expected);
  assert_int_equal(teardown(&fresh_state), 0);
}

static void rejected_snapshot_preserves_live_client(void** state)
{
  fixture* source = (fixture*)*state;
  checkpoint_second_step(source);
  source->adu._internal.install_result.step_results[1].result_details[0] = 'x';
  source->adu._internal.install_result.step_results[1].result_details_length = 1;
  for (int i = 0; i < 40 && !source->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&source->adu), AZ_IOT_OK);
  }
  assert_true(source->log.have_persist);

  void* live_state = NULL;
  assert_int_equal(setup(&live_state), 0);
  fixture* live = (fixture*)live_state;
  inject_patch(live, signed_patch());
  pump(live, 2);
  az_iot_adu_client_t* expected = malloc(sizeof(*expected));
  assert_non_null(expected);
  *expected = live->adu;
  for (int scenario = 0; scenario < 11; ++scenario)
  {
    memcpy(live->log.persist_blob, source->log.persist_blob, source->log.persist_len);
    live->log.persist_len = source->log.persist_len;
    live->log.have_persist = true;
    uint8_t* blob = live->log.persist_blob;
    size_t trailer = 40 + snapshot_u32(blob + 36);
    size_t last = trailer + 16;
    for (int i = 0; i < 2; ++i)
    {
      last += 24 + snapshot_u32(blob + last + 16) + snapshot_u32(blob + last + 20);
    }
    size_t manifest = 40 + snapshot_u32(blob + 28);
    size_t manifest_len = snapshot_u32(blob + 32);
    switch (scenario)
    {
      case 0: /* CRC */
        blob[live->log.persist_len - 1] ^= 1;
        break;
      case 1: /* Last record length */
        snapshot_write_u32(blob + last + 16, UINT32_MAX);
        break;
      case 2: /* Last record origin */
        snapshot_write_u32(blob + last + 4, AZ_IOT_ADU_FAILURE_ORIGIN_DEVICE);
        break;
      case 3: /* Last record hex */
        blob[last + 24] = 'g';
        break;
      case 4: /* Last record UTF-8 */
        blob[last + 24 + snapshot_u32(blob + last + 16)] = 0xff;
        break;
      case 5: /* Wrong manifest root */
        blob[manifest] = '[';
        break;
      case 6: /* Too many steps for the upstream fixed array */
      case 7: /* Valid step count, invalid manifest field */
      {
        const char* replacement = scenario == 6
            ? "{\"instructions\":{\"steps\":[{},{},{}]}}"
            : "{\"instructions\":{\"steps\":[{},{}]},\"files\":null}";
        assert_true(strlen(replacement) < manifest_len);
        memset(blob + manifest, ' ', manifest_len);
        memcpy(blob + manifest, replacement, strlen(replacement));
        break;
      }
      case 8: /* URL table count */
      case 9: /* URL span outside the saved request */
      case 10: /* A remaining step has no matching URL */
      {
        size_t urls = last + 24 + snapshot_u32(blob + last + 16) + snapshot_u32(blob + last + 20);
        assert_true(snapshot_u32(blob + urls) > 0);
        if (scenario == 8)
        {
          snapshot_write_u32(blob + urls, _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT + 1);
        }
        else if (scenario == 9)
        {
          snapshot_write_u32(blob + urls + 4 + 8, UINT32_MAX);
        }
        else
        {
          snapshot_write_u32(blob + 12, 0);
          blob[40 + snapshot_u32(blob + urls + 4)] = 'z';
        }
        break;
      }
    }
    if (scenario != 0)
    {
      snapshot_write_u32(
          blob + live->log.persist_len - 4, snapshot_crc(blob, live->log.persist_len - 4));
    }
    assert_int_equal(az_iot_adu_client_resume(&live->adu), AZ_IOT_ERR_INVALID_ARG);
    /* Only the load scratch may change on a rejected snapshot. */
    memcpy(
        expected->_internal.persist_scratch,
        live->adu._internal.persist_scratch,
        sizeof(expected->_internal.persist_scratch));
    assert_memory_equal(&live->adu, expected, sizeof(*expected));
  }
  free(expected);
  assert_int_equal(teardown(&live_state), 0);
}

static void assert_restored_span(
    az_span actual,
    az_span expected,
    const az_iot_adu_client_t* client)
{
  assert_true(az_span_is_content_equal(actual, expected));
  if (az_span_size(actual) > 0)
  {
    uintptr_t start = (uintptr_t)client->_internal.request_buffer;
    uintptr_t pointer = (uintptr_t)az_span_ptr(actual);
    assert_true(pointer >= start);
    assert_true(pointer - start + (size_t)az_span_size(actual) <= client->_internal.request_len);
  }
}

static void assert_restored_manifest(
    const az_iot_adu_client_t* client,
    const az_iot_adu_client_update_manifest* expected)
{
  const az_iot_adu_client_update_manifest* actual = &client->_internal.current_manifest;
  assert_restored_span(actual->manifest_version, expected->manifest_version, client);
  assert_restored_span(actual->update_id.provider, expected->update_id.provider, client);
  assert_restored_span(actual->update_id.name, expected->update_id.name, client);
  assert_restored_span(actual->update_id.version, expected->update_id.version, client);
  assert_restored_span(actual->create_date_time, expected->create_date_time, client);
  assert_int_equal(actual->instructions.steps_count, expected->instructions.steps_count);
  for (uint32_t i = 0; i < actual->instructions.steps_count; ++i)
  {
    const az_iot_adu_client_update_manifest_instructions_step* step
        = &actual->instructions.steps[i];
    const az_iot_adu_client_update_manifest_instructions_step* original
        = &expected->instructions.steps[i];
    assert_restored_span(step->handler, original->handler, client);
    assert_restored_span(
        step->handler_properties.installed_criteria,
        original->handler_properties.installed_criteria,
        client);
    assert_int_equal(step->files_count, original->files_count);
    for (uint32_t j = 0; j < step->files_count; ++j)
    {
      assert_restored_span(step->files[j], original->files[j], client);
    }
  }
  assert_int_equal(actual->files_count, expected->files_count);
  for (uint32_t i = 0; i < actual->files_count; ++i)
  {
    const az_iot_adu_client_update_manifest_file* file = &actual->files[i];
    const az_iot_adu_client_update_manifest_file* original = &expected->files[i];
    assert_restored_span(file->id, original->id, client);
    assert_restored_span(file->file_name, original->file_name, client);
    assert_int_equal(file->hashes_count, original->hashes_count);
    for (uint32_t j = 0; j < file->hashes_count; ++j)
    {
      assert_restored_span(file->hashes[j].hash_type, original->hashes[j].hash_type, client);
      assert_restored_span(file->hashes[j].hash_value, original->hashes[j].hash_value, client);
    }
  }
}

static void resumed_clients_have_independent_storage(void** state)
{
  fixture* source = (fixture*)*state;
  checkpoint_second_step(source);
  for (int i = 0; i < 40 && !source->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&source->adu), AZ_IOT_OK);
  }
  assert_true(source->log.have_persist);
  void* first_state = NULL;
  void* second_state = NULL;
  assert_int_equal(setup(&first_state), 0);
  assert_int_equal(setup(&second_state), 0);
  fixture* clients[] = { (fixture*)first_state, (fixture*)second_state };
  for (size_t i = 0; i < 2; ++i)
  {
    fixture* client = clients[i];
    memcpy(client->log.persist_blob, source->log.persist_blob, source->log.persist_len);
    client->log.persist_len = source->log.persist_len;
    client->log.have_persist = true;
    assert_int_equal(az_iot_adu_client_resume(&client->adu), AZ_IOT_OK);
    memset(
        client->adu._internal.persist_scratch, 0xa5, sizeof(client->adu._internal.persist_scratch));
    assert_restored_manifest(&client->adu, &source->adu._internal.current_manifest);
  }
  az_iot_adu_install_result* expected = malloc(sizeof(*expected));
  assert_non_null(expected);
  *expected = clients[0]->adu._internal.install_result;
  clients[1]->adu._internal.install_result.step_results[0].result_code = INT64_MAX;
  pump(clients[1], 40);
  assert_memory_equal(&clients[0]->adu._internal.install_result, expected, sizeof(*expected));
  assert_int_equal(
      az_iot_adu_client_get_state(&clients[0]->adu), AZ_IOT_ADU_STATE_INSTALL_COMPLETE);
  assert_restored_manifest(&clients[0]->adu, &source->adu._internal.current_manifest);
  pump(clients[0], 40);
  assert_int_equal(clients[0]->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  free(expected);
  assert_int_equal(teardown(&first_state), 0);
  assert_int_equal(teardown(&second_state), 0);
}

static void resume_before_last_step_restores_download_urls(void** state)
{
  fixture* source = (fixture*)*state;
  const char* original = two_step_patch();
  const char* urls = strstr(original, "\"fileUrls\":{");
  assert_non_null(urls);
  size_t prefix = (size_t)(urls - original) + strlen("\"fileUrls\":{");
  char patch[AZ_IOT_ADU_REQUEST_BUFFER_SIZE];
  /* The matching URL is not the first map entry. */
  int length = snprintf(
      patch,
      sizeof(patch),
      "%.*s\"unused\":\"https://example.com/unused\",%s",
      (int)prefix,
      original,
      original + prefix);
  assert_true(length > 0 && (size_t)length < sizeof(patch));
  source->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  inject_patch(source, patch);
  for (int i = 0; i < 40 && !source->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&source->adu), AZ_IOT_OK);
  }
  assert_true(source->log.have_persist);
  assert_int_equal(source->adu._internal.current_step, 0);
  assert_int_equal(source->adu._internal.current_request.file_urls_count, 2);

  void* fresh_state = NULL;
  assert_int_equal(setup(&fresh_state), 0);
  fixture* fresh = (fixture*)fresh_state;
  memcpy(fresh->log.persist_blob, source->log.persist_blob, source->log.persist_len);
  fresh->log.persist_len = source->log.persist_len;
  fresh->log.have_persist = true;
  fresh->log.expected_download_url = "http://example.com/payload.bin";
  assert_int_equal(az_iot_adu_client_resume(&fresh->adu), AZ_IOT_OK);
  memset(fresh->adu._internal.persist_scratch, 0xa5, sizeof(fresh->adu._internal.persist_scratch));
  assert_int_equal(fresh->adu._internal.current_request.file_urls_count, 2);
  for (uint32_t i = 0; i < 2; ++i)
  {
    assert_restored_span(
        fresh->adu._internal.current_request.file_urls[i].id,
        source->adu._internal.current_request.file_urls[i].id,
        &fresh->adu);
    assert_restored_span(
        fresh->adu._internal.current_request.file_urls[i].url,
        source->adu._internal.current_request.file_urls[i].url,
        &fresh->adu);
  }
  pump(fresh, 60);
  assert_int_equal(fresh->log.download_calls, 1);
  assert_int_equal(fresh->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(teardown(&fresh_state), 0);
}

static void make_legacy_checkpoint(hook_log* log, uint16_t version)
{
  uint8_t* blob = log->persist_blob;
  size_t trailer = 40 + snapshot_u32(blob + 36);
  uint32_t count = snapshot_u32(blob + trailer + 12);
  uint32_t current_step = snapshot_u32(blob + 12);
  blob[4] = (uint8_t)version;
  blob[5] = 0;
  size_t end;
  if (version == 2)
  {
    snapshot_write_u32(blob + trailer + 12, 0);
    snapshot_write_u32(blob + trailer + 16, 0);
    snapshot_write_u32(blob + trailer + 20, count);
    end = trailer + 24;
    for (uint32_t i = 0; i < count; ++i)
    {
      snapshot_write_u32(blob + end, i < current_step ? 700 : 0);
      snapshot_write_u32(blob + end + 4, 0);
      end += 8;
    }
  }
  else
  {
    end = trailer + 16;
    for (uint32_t i = 0; i <= count; ++i)
    {
      end += 24 + snapshot_u32(blob + end + 16) + snapshot_u32(blob + end + 20);
    }
  }
  snapshot_write_u32(blob + end, snapshot_crc(blob, end));
  log->persist_len = end + 4;
}

static void legacy_checkpoints_reject_remaining_downloads(void** state)
{
  fixture* source = (fixture*)*state;
  source->log.install_result = AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  inject_patch(source, two_step_patch());
  for (int i = 0; i < 40 && !source->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&source->adu), AZ_IOT_OK);
  }
  assert_true(source->log.have_persist);
  assert_int_equal(source->adu._internal.current_step, 0);
  void* live_state = NULL;
  assert_int_equal(setup(&live_state), 0);
  fixture* live = (fixture*)live_state;
  inject_patch(live, signed_patch());
  pump(live, 2);
  az_iot_adu_client_t* expected = malloc(sizeof(*expected));
  assert_non_null(expected);
  *expected = live->adu;
  for (uint16_t version = 2; version <= 3; ++version)
  {
    memcpy(live->log.persist_blob, source->log.persist_blob, source->log.persist_len);
    live->log.persist_len = source->log.persist_len;
    live->log.have_persist = true;
    make_legacy_checkpoint(&live->log, version);
    assert_int_equal(az_iot_adu_client_resume(&live->adu), AZ_IOT_ERR_NOT_SUPPORTED);
    memcpy(
        expected->_internal.persist_scratch,
        live->adu._internal.persist_scratch,
        sizeof(expected->_internal.persist_scratch));
    assert_memory_equal(&live->adu, expected, sizeof(*expected));
  }
  free(expected);
  assert_int_equal(teardown(&live_state), 0);
}

static void v3_final_step_checkpoint_remains_supported(void** state)
{
  fixture* source = (fixture*)*state;
  checkpoint_second_step(source);
  for (int i = 0; i < 40 && !source->log.have_persist; ++i)
  {
    assert_int_equal(az_iot_adu_client_do_work(&source->adu), AZ_IOT_OK);
  }
  assert_true(source->log.have_persist);
  make_legacy_checkpoint(&source->log, 3);
  void* fresh_state = NULL;
  assert_int_equal(setup(&fresh_state), 0);
  fixture* fresh = (fixture*)fresh_state;
  memcpy(fresh->log.persist_blob, source->log.persist_blob, source->log.persist_len);
  fresh->log.persist_len = source->log.persist_len;
  fresh->log.have_persist = true;
  assert_int_equal(az_iot_adu_client_resume(&fresh->adu), AZ_IOT_OK);
  assert_int_equal(fresh->adu._internal.current_step, 1);
  pump(fresh, 40);
  assert_int_equal(fresh->log.download_calls, 0);
  assert_int_equal(fresh->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_SUCCEEDED);
  assert_int_equal(teardown(&fresh_state), 0);
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
  assert_int_equal(fx->chan.last_install_result.outcome, AZ_IOT_ADU_OUTCOME_FAILED);
  assert_int_equal(
      fx->chan.last_install_result.failure_origin, AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE);
  assert_true(fx->chan.last_install_result.result_code != 700);
  assert_true(fx->chan.last_workflow_id[0] != '\0');
  assert_idle_report_retains_outcome(fx, AZ_IOT_ADU_OUTCOME_FAILED);
}

/* ------------------------------------------------------------------------- */
/* standalone api: report building, manifest verification                    */
/* ------------------------------------------------------------------------- */

static void build_report_with_too_small_a_buffer_is_rejected(void** state)
{
  (void)state;
  az_iot_adu_install_result result = AZ_IOT_ADU_INSTALL_RESULT_INIT;
  az_iot_adu__set_extended_result(
      AZ_SPAN_FROM_BUFFER(result.extended_result_codes), &result.extended_result_codes_length, 0);
  az_iot_adu_report report = AZ_IOT_ADU_REPORT_INIT;
  report.workflow_id = "workflow";
  report.install_result = &result;

  /* Truncating the report would publish JSON the service cannot parse, so the
   * bound is reported instead. */
  uint8_t tiny[8];
  size_t written = 0;
  assert_int_equal(
      az_iot_adu_build_report(&report, tiny, sizeof(tiny), &written), AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  /* The same call succeeds once the buffer is big enough, which proves the
   * rejection was about size and not about the arguments. */
  uint8_t big[AZ_IOT_ADU_REQUEST_BUFFER_SIZE];
  assert_int_equal(az_iot_adu_build_report(&report, big, sizeof(big), &written), AZ_IOT_OK);
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

/* The public parser accepts the ADUv2 updateMetadata shape too. */
static void public_parser_accepts_update_metadata(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_adu_client_update_request req;
  az_iot_adu_client_update_manifest manifest;
  assert_int_equal(
      parse_with_roots(&fx->log, build_patch("wf-1"), k_root_keys, 1, &req, &manifest), AZ_IOT_OK);
  assert_int_equal(req.workflow.action, AZ_IOT_ADU_CLIENT_SERVICE_ACTION_APPLY_DEPLOYMENT);
  assert_true(az_span_is_content_equal(req.workflow.id, AZ_SPAN_FROM_STR("wf-1")));
  assert_int_equal(req.file_urls_count, 1);
  assert_int_equal(manifest.instructions.steps_count, 1);

  assert_int_equal(
      parse_with_roots(&fx->log, "{\"other\":1}", k_root_keys, 1, &req, &manifest),
      AZ_IOT_ERR_NOT_FOUND);
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
    int n = snprintf(patch, sizeof(patch), k_patch_fmt, "bad-jws", "1.1", broken[i]);
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
      "{\"workflowId\":\"bad-manifest\","
      "\"updateManifest\":\"{\\\"manifestVersion\\\":\\\"5\\\",\\\"updateId\\\":{\","
      "\"updateManifestSignature\":\"%s\","
      "\"fileUrls\":{\"f\":\"http://example.com/p.bin\"}}",
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
      = "{\"workflowId\":\"sha512-only\","
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
        "\"fileUrls\":{\"f2f4a804ca17afbae\":\"http://example.com/payload.bin\"}}";

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
      NULL,
      fx->chan.engine_ctx);
  assert_false(fx->adu._internal.device_props_report_pending);

  /* Same for the other terminal verdicts. */
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_REPORT_STATUS,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_FATAL,
      NULL,
      fx->chan.engine_ctx);
  assert_false(fx->adu._internal.device_props_report_pending);

  /* A retryable verdict IS re-armed -- otherwise the assertions above would
   * pass for a callback that simply did nothing. */
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_REPORT_STATUS,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_RETRY,
      NULL,
      fx->chan.engine_ctx);
  assert_true(fx->adu._internal.device_props_report_pending);
}

/* An operation the client gives up on reaches the application, with the route
 * and what the service said. Without this, "refused permanently" and "checked,
 * nothing to install" are indistinguishable from outside. */
static void an_abandoned_operation_is_reported_to_observers(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);
  fx->abandoned_count = 0;

  assert_non_null(fx->chan.result_cb);

  /* A retryable verdict is not an abandonment. */
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_RETRY,
      NULL,
      fx->chan.engine_ctx);
  assert_int_equal(fx->abandoned_count, 0);

  /* Nor is a success, nor ALREADY_REPORTED -- the service has the report, so
   * nothing was lost. */
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE, AZ_IOT_OK, AZ_IOT_ADU_ERROR_ACTION_NONE, NULL, fx->chan.engine_ctx);
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_REPORT_STATUS,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED,
      NULL,
      fx->chan.engine_ctx);
  assert_int_equal(fx->abandoned_count, 0);

  /* FATAL is, and carries the diagnosis. */
  az_iot_adu_service_error se
      = { .code = 400002, .message = "INVALID_REQUEST", .tracking_id = "abc-123" };
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_FATAL,
      &se,
      fx->chan.engine_ctx);
  assert_int_equal(fx->abandoned_count, 1);
  assert_int_equal(fx->last_abandoned_operation, AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE);
  assert_int_equal(fx->last_abandoned_reason, AZ_IOT_ERR_DPS);
  assert_int_equal(fx->last_error_code, 400002);
  assert_string_equal(fx->last_error_text, "INVALID_REQUEST");
  assert_string_equal(fx->last_tracking_id, "abc-123");
}

/* PROCEED drops the request just as FATAL does, and must be reported for the
 * same reason. It is UPDATE_ACCOUNT_NOT_LINKED on a fetch: the device asked,
 * was refused permanently, and without this it reads exactly like "no update
 * available" -- for ever, with no other signal. */
static void a_proceed_verdict_is_also_reported(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);
  fx->abandoned_count = 0;

  az_iot_adu_service_error se
      = { .code = 409000, .message = "UPDATE_ACCOUNT_NOT_LINKED", .tracking_id = "t-9" };
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_PROCEED,
      &se,
      fx->chan.engine_ctx);

  assert_int_equal(fx->abandoned_count, 1);
  assert_int_equal(fx->last_abandoned_operation, AZ_IOT_ADU_OP_GET_UPDATE);
  assert_string_equal(fx->last_error_text, "UPDATE_ACCOUNT_NOT_LINKED");
}

/* A verdict with no service response behind it still delivers a usable event:
 * the strings are empty, never NULL, so an observer never has to null-check. */
static void an_abandonment_without_a_service_error_carries_empty_strings(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);
  fx->abandoned_count = 0;

  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_PROTOCOL,
      AZ_IOT_ADU_ERROR_ACTION_FATAL,
      NULL,
      fx->chan.engine_ctx);

  assert_int_equal(fx->abandoned_count, 1);
  assert_int_equal(fx->last_error_code, 0);
  assert_string_equal(fx->last_error_text, "");
  assert_string_equal(fx->last_tracking_id, "");
}

/* Registry mechanics: the pool is bounded, the pair is the identity, and the
 * reentrancy rules match the connection client so an application learns one
 * pattern for the whole SDK. */
static az_iot_adu_client_t* g_reentrant_client;
static az_iot_result g_reentrant_add;
static az_iot_result g_reentrant_remove;

static void reentrant_observer(const az_iot_adu_event* event, void* user_ctx)
{
  (void)event;
  (void)user_ctx;
  g_reentrant_add
      = az_iot_adu_client_add_observer(g_reentrant_client, reentrant_observer, (void*)(uintptr_t)1);
  /* Removing itself, which is what an owner torn down in reaction to an event
   * must be able to do. */
  g_reentrant_remove
      = az_iot_adu_client_remove_observer(g_reentrant_client, reentrant_observer, NULL);
}

static void the_observer_registry_enforces_its_contract(void** state)
{
  fixture* fx = (fixture*)*state;

  assert_int_equal(az_iot_adu_client_add_observer(NULL, on_event, fx), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, NULL, fx), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_adu_client_remove_observer(&fx->adu, on_event, fx), AZ_IOT_ERR_NOT_FOUND);

  /* Idempotent on the PAIR, so this consumes one slot, not two. */
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);

  /* ... but the same callback with a different context is a second
   * subscription, and is delivered twice. */
  int seen = 0;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, count_events, &seen), AZ_IOT_OK);
  int other = 0;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, count_events, &other), AZ_IOT_OK);

  /* Pool is bounded and says so rather than overwriting. */
  int spare = 0;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, count_events, &spare), AZ_IOT_OK);
  int overflow = 0;
  assert_int_equal(
      az_iot_adu_client_add_observer(&fx->adu, count_events, &overflow),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_FATAL,
      NULL,
      fx->chan.engine_ctx);
  assert_int_equal(seen, 1);
  assert_int_equal(other, 1);

  /* Withdrawing one context leaves the other subscribed. */
  assert_int_equal(az_iot_adu_client_remove_observer(&fx->adu, count_events, &seen), AZ_IOT_OK);
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_FATAL,
      NULL,
      fx->chan.engine_ctx);
  assert_int_equal(seen, 1);
  assert_int_equal(other, 2);
}

/* Adding from inside a dispatch is refused; removing is permitted, and must be
 * -- an owner destroyed in reaction to an event has to give its seat back
 * before its storage goes away. Same asymmetry as the connection client. */
static void adding_is_refused_from_inside_an_observer_but_removing_is_not(void** state)
{
  fixture* fx = (fixture*)*state;
  g_reentrant_client = &fx->adu;
  g_reentrant_add = AZ_IOT_OK;
  g_reentrant_remove = AZ_IOT_ERR_BUSY;

  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, reentrant_observer, NULL), AZ_IOT_OK);

  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_FATAL,
      NULL,
      fx->chan.engine_ctx);

  assert_int_equal(g_reentrant_add, AZ_IOT_ERR_BUSY);
  assert_int_equal(g_reentrant_remove, AZ_IOT_OK);

  /* The withdrawal took effect: a later event does not reach it. */
  g_reentrant_add = AZ_IOT_OK;
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_FATAL,
      NULL,
      fx->chan.engine_ctx);
  assert_int_equal(g_reentrant_add, AZ_IOT_OK);
}

/* Workflow transitions are reported, so an application no longer has to poll
 * az_iot_adu_client_get_state() to see a deployment move. */
static void a_workflow_transition_is_reported(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);
  fx->state_event_count = 0;

  open_to_connected(fx);

  /* The offer arriving moves Idle -> ManifestReceived, which an application
   * previously could only see by polling. */
  inject_patch(fx, signed_patch());
  assert_int_equal(az_iot_adu_client_get_state(&fx->adu), AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);
  assert_int_equal(fx->state_event_count, 1);
  assert_int_equal(fx->last_previous_state, AZ_IOT_ADU_STATE_IDLE);
  assert_int_equal(fx->last_state, AZ_IOT_ADU_STATE_MANIFEST_RECEIVED);

  /* Driving the deployment reports every further transition, and the last one
   * always agrees with the getter. */
  pump(fx, 40);
  assert_true(fx->state_event_count > 1);
  assert_int_equal(fx->last_state, az_iot_adu_client_get_state(&fx->adu));
}

/* A request the channel never accepts is bounded, and the application is told.
 *
 * The pending slot auto-retries: the channel refusing puts the request straight
 * back, so a request that can NEVER be served -- the device never provisions,
 * the enrollment is missing -- was reissued for the life of the client with the
 * application never told. It could not tell that from "no update available". */
static void a_request_the_channel_never_accepts_is_abandoned(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->chan.request_update_result = AZ_IOT_ERR_NOT_CONNECTED;
  fx->abandoned_count = 0;

  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_not_equal(fx->adu._internal.pending_fetch, 0);
  assert_int_not_equal(fx->adu._internal.pending_fetch_deadline_ms, 0);

  /* Retried while the deadline stands, and not abandoned. */
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_adu_client_do_work(&fx->adu);
  }
  assert_int_not_equal(fx->adu._internal.pending_fetch, 0);
  assert_int_equal(fx->abandoned_count, 0);
  assert_true(fx->chan.request_update_count >= 5);

  /* Force the deadline into the past rather than waiting out the real timeout. */
  fx->adu._internal.pending_fetch_deadline_ms = 1;
  (void)az_iot_adu_client_do_work(&fx->adu);

  /* Given up on, the slot cleared, and the application told WHICH route and
   * why -- not silently dropped. */
  assert_int_equal(fx->adu._internal.pending_fetch, 0);
  assert_int_equal(fx->adu._internal.pending_fetch_deadline_ms, 0);
  assert_int_equal(fx->abandoned_count, 1);
  assert_int_equal(fx->last_abandoned_reason, AZ_IOT_ERR_TIMEOUT);
  /* WHICH route, so a route-mapping regression is caught: the application
   * responds by asking again on the same one. */
  assert_int_equal(fx->last_abandoned_operation, AZ_IOT_ADU_OP_GET_UPDATE);

  /* And it stops: no further attempts, no repeated reports. */
  int attempts_after = fx->chan.request_update_count;
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_adu_client_do_work(&fx->adu);
  }
  assert_int_equal(fx->chan.request_update_count, attempts_after);
  assert_int_equal(fx->abandoned_count, 1);

  /* The onboarding route reports itself, not a constant. */
  assert_int_equal(az_iot_adu_client_request_onboarding_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  fx->adu._internal.pending_fetch_deadline_ms = 1;
  (void)az_iot_adu_client_do_work(&fx->adu);
  assert_int_equal(fx->abandoned_count, 2);
  assert_int_equal(fx->last_abandoned_operation, AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE);
}

/* A newer request queued while an earlier fetch is still in flight keeps its
 * own deadline. The earlier verdict must not strip it -- that would leave the
 * newer request retrying for ever, which is the defect this change removes. */
static void a_newer_request_keeps_its_deadline_when_an_older_verdict_arrives(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->chan.request_update_result = AZ_IOT_OK; /* accepted, verdict comes later */

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  (void)az_iot_adu_client_do_work(&fx->adu);
  assert_int_equal(fx->adu._internal.pending_fetch, 0); /* accepted */

  /* The application asks again before the first verdict arrives. */
  assert_int_equal(az_iot_adu_client_request_onboarding_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_not_equal(fx->adu._internal.pending_fetch, 0);
  uint64_t newer_deadline = fx->adu._internal.pending_fetch_deadline_ms;
  assert_int_not_equal(newer_deadline, 0);

  /* Now the EARLIER request's verdict lands, terminally. */
  assert_non_null(fx->chan.result_cb);
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE, AZ_IOT_OK, AZ_IOT_ADU_ERROR_ACTION_NONE, NULL, fx->chan.engine_ctx);

  /* The newer request still holds the slot AND its deadline. */
  assert_int_not_equal(fx->adu._internal.pending_fetch, 0);
  assert_int_equal(fx->adu._internal.pending_fetch_deadline_ms, newer_deadline);
}

/* Asking again is how an application responds to an abandonment, so the new
 * request must get a fresh clock rather than inherit the expired one. */
static void a_new_request_restarts_the_deadline(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->chan.request_update_result = AZ_IOT_ERR_NOT_CONNECTED;

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  fx->adu._internal.pending_fetch_deadline_ms = 1;
  (void)az_iot_adu_client_do_work(&fx->adu);
  assert_int_equal(fx->adu._internal.pending_fetch, 0);

  /* A fresh request is not abandoned on the next tick. */
  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_not_equal(fx->adu._internal.pending_fetch_deadline_ms, 1);
  (void)az_iot_adu_client_do_work(&fx->adu);
  assert_int_not_equal(fx->adu._internal.pending_fetch, 0);
}

/* A LATE verdict must not resurrect an operation after the slot was abandoned.
 *
 * Request A is accepted, B replaces it in the slot, B is abandoned on its
 * deadline -- which clears it -- and then A's late retryable verdict arrives
 * and re-arms A. Without a deadline armed at that point A would retry for
 * ever, which is exactly what this change removes. */
static void a_late_verdict_cannot_resurrect_an_unbounded_request(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->chan.request_update_result = AZ_IOT_OK; /* A is accepted */

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  (void)az_iot_adu_client_do_work(&fx->adu);
  assert_int_equal(fx->adu._internal.pending_fetch, 0);

  /* B queued behind A, then abandoned on its own deadline. */
  fx->chan.request_update_result = AZ_IOT_ERR_NOT_CONNECTED;
  assert_int_equal(az_iot_adu_client_request_onboarding_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  fx->adu._internal.pending_fetch_deadline_ms = 1;
  (void)az_iot_adu_client_do_work(&fx->adu);
  assert_int_equal(fx->adu._internal.pending_fetch, 0);
  assert_int_equal(fx->adu._internal.pending_fetch_deadline_ms, 0);

  /* A's late retryable verdict re-arms it -- WITH a deadline. */
  assert_non_null(fx->chan.result_cb);
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_RETRY,
      NULL,
      fx->chan.engine_ctx);
  assert_int_not_equal(fx->adu._internal.pending_fetch, 0);
  assert_int_not_equal(fx->adu._internal.pending_fetch_deadline_ms, 0);
}

/* The documented opt-out: AZ_IOT_ADU_REQUEST_NO_TIMEOUT retries indefinitely.
 * Passed as the real argument, so the path is covered through the public API
 * rather than by poking the deadline. */
static void a_disabled_timeout_never_abandons(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->chan.request_update_result = AZ_IOT_ERR_NOT_CONNECTED;
  fx->abandoned_count = 0;

  assert_int_equal(
      az_iot_adu_client_request_update(&fx->adu, AZ_IOT_ADU_REQUEST_NO_TIMEOUT), AZ_IOT_OK);
  assert_int_equal(fx->adu._internal.pending_fetch_deadline_ms, 0);

  for (int i = 0; i < 20; ++i)
  {
    (void)az_iot_adu_client_do_work(&fx->adu);
  }

  /* Still queued, still retrying, never abandoned. */
  assert_int_not_equal(fx->adu._internal.pending_fetch, 0);
  assert_int_equal(fx->adu._internal.pending_fetch_deadline_ms, 0);
  assert_int_equal(fx->abandoned_count, 0);
  assert_true(fx->chan.request_update_count >= 20);
}

/* The deadline is the CALLER's, in wall-clock terms, and each call carries its
 * own. A compile-time constant could not express both a short boot-time probe
 * and a long background poll. */
static void each_request_carries_its_own_timeout(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->chan.request_update_result = AZ_IOT_ERR_NOT_CONNECTED;

  uint64_t before = az_iot_time_mono_ms();
  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, 1000u), AZ_IOT_OK);
  uint64_t d1 = fx->adu._internal.pending_fetch_deadline_ms;
  assert_true(d1 >= before + 1000u && d1 <= az_iot_time_mono_ms() + 1000u);

  before = az_iot_time_mono_ms();
  assert_int_equal(az_iot_adu_client_request_onboarding_update(&fx->adu, 90000u), AZ_IOT_OK);
  uint64_t d2 = fx->adu._internal.pending_fetch_deadline_ms;
  assert_true(d2 >= before + 90000u && d2 <= az_iot_time_mono_ms() + 90000u);

  /* Different calls, different deadlines -- not one shared constant. */
  assert_true(d2 > d1);
}

/* A service-requested delay is NOT excluded from the caller's budget.
 *
 * Excluding it would silently move the deadline the caller set: a device that
 * asked for an answer within 5 minutes would be answered in an hour, and its
 * own scheduling would be built on a promise the SDK had quietly rewritten. */
static void a_service_delay_does_not_extend_the_callers_deadline(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->chan.request_update_result = AZ_IOT_ERR_BUSY;

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  uint64_t armed = fx->adu._internal.pending_fetch_deadline_ms;

  az_iot_adu_service_error se
      = { .code = 429001, .message = "", .tracking_id = "", .retry_after_ms = 1000u };
  for (int i = 0; i < 5; ++i)
  {
    fx->chan.result_cb(
        AZ_IOT_ADU_OP_GET_UPDATE,
        AZ_IOT_ERR_DPS,
        AZ_IOT_ADU_ERROR_ACTION_RETRY,
        &se,
        fx->chan.engine_ctx);
  }

  /* Unmoved, however many delays the service asks for. */
  assert_int_equal(fx->adu._internal.pending_fetch_deadline_ms, armed);
}

/* A delay that cannot fit ends the request AT ONCE, and says how long the
 * service asked for.
 *
 * Waiting for the deadline would buy nothing -- the channel refuses for the
 * whole delay -- and would withhold the one fact the application needs to
 * schedule its next attempt. */
static void a_delay_that_cannot_fit_abandons_immediately_and_reports_it(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);
  fx->abandoned_count = 0;
  fx->last_retry_after_ms = 0;
  fx->chan.request_update_result = AZ_IOT_ERR_BUSY;

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);

  az_iot_adu_service_error se = { .code = 429001,
                                  .message = "TooManyRequests",
                                  .tracking_id = "tid-1",
                                  .retry_after_ms = SERVICE_DELAY_MS };
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_RETRY,
      &se,
      fx->chan.engine_ctx);

  /* Told now, not at the deadline, and told WHEN to come back. */
  assert_int_equal(fx->abandoned_count, 1);
  assert_int_equal(fx->last_abandoned_reason, AZ_IOT_ERR_TIMEOUT);
  assert_int_equal(fx->last_abandoned_operation, AZ_IOT_ADU_OP_GET_UPDATE);
  assert_int_equal(fx->last_retry_after_ms, SERVICE_DELAY_MS);
  assert_int_equal(fx->last_error_code, 429001);

  /* And the slot is genuinely empty: no further attempts. */
  assert_int_equal(fx->adu._internal.pending_fetch, 0);
  assert_int_equal(fx->adu._internal.pending_fetch_deadline_ms, 0);
  size_t before = fx->chan.request_update_count;
  (void)az_iot_adu_client_do_work(&fx->adu);
  assert_int_equal(fx->chan.request_update_count, before);
}

/* A delay that DOES fit is waited out normally: the request stays queued and
 * keeps its deadline. Otherwise any retry-after at all would end a request the
 * service was willing to serve within the caller's budget. */
static void a_delay_that_fits_leaves_the_request_queued(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);
  fx->abandoned_count = 0;
  fx->chan.request_update_result = AZ_IOT_ERR_BUSY;

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  uint64_t armed = fx->adu._internal.pending_fetch_deadline_ms;

  az_iot_adu_service_error se
      = { .code = 429001, .message = "", .tracking_id = "", .retry_after_ms = 1000u };
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_RETRY,
      &se,
      fx->chan.engine_ctx);

  assert_int_equal(fx->abandoned_count, 0);
  assert_int_not_equal(fx->adu._internal.pending_fetch, 0);
  assert_int_equal(fx->adu._internal.pending_fetch_deadline_ms, armed);
}

/* With no bound, even a delay longer than any budget cannot abandon: there is
 * no deadline for it to fail to fit inside. */
static void a_delay_cannot_abandon_an_unbounded_request(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_adu_client_add_observer(&fx->adu, on_event, fx), AZ_IOT_OK);
  fx->abandoned_count = 0;
  fx->chan.request_update_result = AZ_IOT_ERR_BUSY;

  assert_int_equal(
      az_iot_adu_client_request_update(&fx->adu, AZ_IOT_ADU_REQUEST_NO_TIMEOUT), AZ_IOT_OK);

  az_iot_adu_service_error se
      = { .code = 429001, .message = "", .tracking_id = "", .retry_after_ms = SERVICE_DELAY_MS };
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_RETRY,
      &se,
      fx->chan.engine_ctx);

  assert_int_equal(fx->abandoned_count, 0);
  assert_int_not_equal(fx->adu._internal.pending_fetch, 0);
}

/* --- explicit update requests -------------------------------------------- */

/* The application chooses the route, so the SDK must not pick one for it. An
 * unrequested fetch at init would query a route the SDK guessed, and on a
 * device with no device record the regular route is an outright error. */
static void no_update_is_fetched_until_one_is_requested(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 0);
}

static void each_request_function_asks_for_its_own_route(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  assert_int_equal(az_iot_adu_client_request_onboarding_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 1);
  assert_int_equal(fx->chan.last_request_operation, AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE);

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 2);
  assert_int_equal(fx->chan.last_request_operation, AZ_IOT_ADU_OP_GET_UPDATE);

  /* One request, one fetch: a further tick must not re-issue it. */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 2);
}

/* A channel that is not ready yet must not lose the request, and must not
 * downgrade it to the other route on the retry. */
static void a_refused_request_is_retried_on_the_same_route(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->chan.request_update_result = AZ_IOT_ERR_NOT_CONNECTED;
  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 1);
  assert_int_equal(fx->chan.last_request_operation, AZ_IOT_ADU_OP_GET_UPDATE);

  fx->chan.request_update_result = AZ_IOT_OK;
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 2);
  assert_int_equal(fx->chan.last_request_operation, AZ_IOT_ADU_OP_GET_UPDATE);
}

/* The same rule for a service-side retryable rejection: the verdict arrives
 * after the channel already accepted the publish, so the engine re-arms it --
 * and must re-arm the route that was actually asked for. */
static void a_retryable_verdict_re_arms_the_same_route(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 1);

  assert_non_null(fx->chan.result_cb);
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_RETRY,
      NULL,
      fx->chan.engine_ctx);

  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 2);
  assert_int_equal(fx->chan.last_request_operation, AZ_IOT_ADU_OP_GET_UPDATE);
}

/* A verdict belongs to a request the channel accepted earlier, so the
 * application may have queued a different route in the meantime. Re-arming the
 * old route over it would silently discard the newer request. */
static void a_retryable_verdict_does_not_overwrite_a_newer_request(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Regular is asked for and accepted; it is now in flight. */
  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 1);
  assert_int_equal(fx->chan.last_request_operation, AZ_IOT_ADU_OP_GET_UPDATE);

  /* The application changes its mind before the answer arrives. */
  assert_int_equal(az_iot_adu_client_request_onboarding_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);

  /* The in-flight regular request then fails retryably. */
  assert_non_null(fx->chan.result_cb);
  fx->chan.result_cb(
      AZ_IOT_ADU_OP_GET_UPDATE,
      AZ_IOT_ERR_DPS,
      AZ_IOT_ADU_ERROR_ACTION_RETRY,
      NULL,
      fx->chan.engine_ctx);

  /* The newer onboarding request must survive, not be replaced by a regular
   * retry. */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 2);
  assert_int_equal(fx->chan.last_request_operation, AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE);

  /* And it is not issued twice. */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 2);
}

/* A synchronous channel delivers its verdict from inside request_update(), so
 * the re-arm happens before that call returns. Clearing the slot after the call
 * would wipe the retry the verdict just armed. */
static void a_synchronous_retryable_verdict_is_not_lost(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->chan.result_is_synchronous = true;
  fx->chan.synchronous_action = AZ_IOT_ADU_ERROR_ACTION_RETRY;

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 1);

  /* The retry survived the accepted publish and goes out again, on the route
   * that was asked for. */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 2);
  assert_int_equal(fx->chan.last_request_operation, AZ_IOT_ADU_OP_GET_UPDATE);
}

/* The mirror case: a synchronous TERMINAL verdict must not be retried, or the
 * test above would pass for an engine that simply never clears the slot. */
static void a_synchronous_terminal_verdict_is_not_retried(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  fx->chan.result_is_synchronous = true;
  fx->chan.synchronous_action = AZ_IOT_ADU_ERROR_ACTION_FATAL;

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 1);

  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 1);
}

/* One pending slot, newest wins: two requests before a do_work() issue one
 * fetch, not two. This is the documented public contract. */
static void two_requests_before_do_work_issue_only_the_newest(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  assert_int_equal(az_iot_adu_client_request_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_client_request_onboarding_update(&fx->adu, UT_TIMEOUT_MS), AZ_IOT_OK);

  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 1);
  assert_int_equal(fx->chan.last_request_operation, AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE);

  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_int_equal(fx->chan.request_update_count, 1);
}

/* A report the channel cannot take right now must not be lost. Most callers are
 * state transitions that discard the result, so the engine has to re-arm it
 * itself -- and a status report is the only record the service gets of what
 * this device did. */
static void a_refused_report_is_re_armed_and_resent(void** state)
{
  fixture* fx = (fixture*)*state;
  open_to_connected(fx);

  /* Drain the startup report so the next one is the interesting one. */
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  int before = fx->chan.report_count;

  fx->chan.report_result = AZ_IOT_ERR_BUSY;
  inject_patch(fx, signed_patch());
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_true(fx->chan.report_count > before);
  /* Refused, so the engine must be holding it for another go. */
  assert_true(fx->adu._internal.device_props_report_pending);

  fx->chan.report_result = AZ_IOT_OK;
  int refused = fx->chan.report_count;
  assert_int_equal(az_iot_adu_client_do_work(&fx->adu), AZ_IOT_OK);
  assert_true(fx->chan.report_count > refused);
  assert_false(fx->adu._internal.device_props_report_pending);
}

static void a_request_on_a_null_client_is_rejected(void** state)
{
  (void)state;
  assert_int_equal(az_iot_adu_client_request_update(NULL, UT_TIMEOUT_MS), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_adu_client_request_onboarding_update(NULL, UT_TIMEOUT_MS), AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(init_starts_idle_and_pending_report, setup, teardown),
    cmocka_unit_test_setup_teardown(no_update_is_fetched_until_one_is_requested, setup, teardown),
    cmocka_unit_test_setup_teardown(each_request_function_asks_for_its_own_route, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_refused_request_is_retried_on_the_same_route, setup, teardown),
    cmocka_unit_test_setup_teardown(a_retryable_verdict_re_arms_the_same_route, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_retryable_verdict_does_not_overwrite_a_newer_request, setup, teardown),
    cmocka_unit_test_setup_teardown(a_synchronous_retryable_verdict_is_not_lost, setup, teardown),
    cmocka_unit_test_setup_teardown(a_synchronous_terminal_verdict_is_not_retried, setup, teardown),
    cmocka_unit_test_setup_teardown(
        two_requests_before_do_work_issue_only_the_newest, setup, teardown),
    cmocka_unit_test_setup_teardown(a_refused_report_is_re_armed_and_resent, setup, teardown),
    cmocka_unit_test(a_request_on_a_null_client_is_rejected),
    cmocka_unit_test_setup_teardown(deployment_drives_full_workflow_single_step, setup, teardown),
    cmocka_unit_test_setup_teardown(update_metadata_drives_full_workflow, setup, teardown),
    cmocka_unit_test_setup_teardown(escaped_file_url_is_decoded, setup, teardown),
    cmocka_unit_test_setup_teardown(escaped_workflow_id_is_decoded, setup, teardown),
    cmocka_unit_test_setup_teardown(manifest_unicode_escape_is_decoded, setup, teardown),
    cmocka_unit_test_setup_teardown(unusable_update_metadata_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(file_urls_are_bounded, setup, teardown),
    cmocka_unit_test_setup_teardown(oversized_update_metadata_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(public_parser_accepts_update_metadata, setup, teardown),
    cmocka_unit_test_setup_teardown(verify_failure_blocks_download_and_fails, setup, teardown),
    cmocka_unit_test_setup_teardown(install_failure_triggers_rollback, setup, teardown),
    cmocka_unit_test_setup_teardown(hash_mismatch_blocks_install_and_fails, setup, teardown),
    cmocka_unit_test_setup_teardown(
        already_installed_is_rejected_without_download, setup, teardown),
    cmocka_unit_test_setup_teardown(install_in_progress_reenters_then_completes, setup, teardown),
    cmocka_unit_test_setup_teardown(reboot_required_persists_and_resumes, setup, teardown),
    cmocka_unit_test_setup_teardown(
        finished_workflow_is_not_replayed_after_reboot, setup, teardown),
    cmocka_unit_test_setup_teardown(
        replacement_deployment_retires_the_stored_checkpoint, setup, teardown),
    cmocka_unit_test_setup_teardown(
        resuming_a_fresh_client_reports_the_restored_state, setup, teardown),
    cmocka_unit_test_setup_teardown(a_fresh_client_result_is_already_stamped, setup, teardown),
    cmocka_unit_test_setup_teardown(resume_with_no_persisted_state_stays_idle, setup, teardown),
    cmocka_unit_test_setup_teardown(cancel_action_sets_cancelled_flag, setup, teardown),
    cmocka_unit_test_setup_teardown(
        late_cancel_does_not_overwrite_a_reported_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(
        update_device_properties_is_accepted_without_reporting, setup, teardown),
    cmocka_unit_test_setup_teardown(report_carries_the_active_workflow_id, setup, teardown),
    cmocka_unit_test_setup_teardown(failure_after_success_does_not_replay_success, setup, teardown),
    cmocka_unit_test_setup_teardown(
        rejected_failure_report_retains_outcome_after_idle, setup, teardown),
    cmocka_unit_test_setup_teardown(
        report_before_manifest_parse_has_no_step_results, setup, teardown),
    cmocka_unit_test_setup_teardown(report_preserves_step_results_at_capacity, setup, teardown),
    cmocka_unit_test_setup_teardown(custom_device_properties_remain_cached, setup, teardown),
    cmocka_unit_test_setup_teardown(
        public_initialize_takes_a_connection_and_builds_its_own_channel, setup, teardown),
    cmocka_unit_test_setup_teardown(extended_result_codes_are_bare_hex, setup, teardown),
    cmocka_unit_test_setup_teardown(device_props_too_small_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(device_props_buffer_size_matches_need, setup, teardown),
    cmocka_unit_test_setup_teardown(duplicate_redelivery_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(replacement_with_new_id_restarts, setup, teardown),
    cmocka_unit_test_setup_teardown(workflow_id_survives_resume, setup, teardown),
    cmocka_unit_test_setup_teardown(same_id_changed_manifest_is_a_duplicate, setup, teardown),
    cmocka_unit_test(microsoft_root_keys_are_embedded),
    cmocka_unit_test_setup_teardown(multi_step_update_runs_every_step_in_order, setup, teardown),
    cmocka_unit_test_setup_teardown(each_step_downloads_its_own_file, setup, teardown),
    cmocka_unit_test_setup_teardown(
        multi_step_report_preserves_progress_and_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(
        multi_step_failure_preserves_unexecuted_step_results, setup, teardown),
    cmocka_unit_test_setup_teardown(
        canonical_results_survive_idle_and_next_workflow, setup, teardown),
    cmocka_unit_test_setup_teardown(canonical_cancel_marks_only_unfinished_steps, setup, teardown),
    cmocka_unit_test_setup_teardown(canonical_cancel_skips_future_steps, setup, teardown),
    cmocka_unit_test_setup_teardown(
        canonical_snapshot_preserves_owned_text_and_int64, setup, teardown),
    cmocka_unit_test_setup_teardown(canonical_resume_accepts_v2_checkpoint, setup, teardown),
    cmocka_unit_test_setup_teardown(canonical_snapshot_reports_storage_errors, setup, teardown),
    cmocka_unit_test_setup_teardown(
        failed_checkpoint_retries_without_reinstalling_or_applying, setup, teardown),
    cmocka_unit_test_setup_teardown(missing_checkpoint_hook_blocks_apply, setup, teardown),
    cmocka_unit_test_setup_teardown(cancellation_clears_a_pending_checkpoint, setup, teardown),
    cmocka_unit_test_setup_teardown(replacement_clears_a_pending_checkpoint, setup, teardown),
    cmocka_unit_test_setup_teardown(canonical_snapshot_preserves_maximum_text, setup, teardown),
    cmocka_unit_test_setup_teardown(
        cancel_after_resume_does_not_overwrite_result_text, setup, teardown),
    cmocka_unit_test_setup_teardown(canonical_resume_rejects_corruption, setup, teardown),
    cmocka_unit_test_setup_teardown(rejected_snapshot_preserves_live_client, setup, teardown),
    cmocka_unit_test_setup_teardown(resume_without_a_persist_hook_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(resumed_clients_have_independent_storage, setup, teardown),
    cmocka_unit_test_setup_teardown(
        resume_before_last_step_restores_download_urls, setup, teardown),
    cmocka_unit_test_setup_teardown(
        resume_with_distinct_files_downloads_next_step_file, setup, teardown),
    cmocka_unit_test_setup_teardown(legacy_checkpoints_reject_remaining_downloads, setup, teardown),
    cmocka_unit_test_setup_teardown(v3_final_step_checkpoint_remains_supported, setup, teardown),
    cmocka_unit_test_setup_teardown(
        download_failure_is_reported_and_does_not_install, setup, teardown),
    cmocka_unit_test(build_report_with_too_small_a_buffer_is_rejected),
    cmocka_unit_test_setup_teardown(
        manifest_signed_by_an_unknown_root_key_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(malformed_jws_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(malformed_manifest_json_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        verify_file_hash_rejects_an_unsupported_algorithm, setup, teardown),
    cmocka_unit_test_setup_teardown(do_work_drives_the_channel, setup, teardown),
    cmocka_unit_test_setup_teardown(a_terminal_verdict_does_not_re_arm_the_report, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_abandoned_operation_is_reported_to_observers, setup, teardown),
    cmocka_unit_test_setup_teardown(a_proceed_verdict_is_also_reported, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_abandonment_without_a_service_error_carries_empty_strings, setup, teardown),
    cmocka_unit_test_setup_teardown(the_observer_registry_enforces_its_contract, setup, teardown),
    cmocka_unit_test_setup_teardown(
        adding_is_refused_from_inside_an_observer_but_removing_is_not, setup, teardown),
    cmocka_unit_test_setup_teardown(a_workflow_transition_is_reported, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_request_the_channel_never_accepts_is_abandoned, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_newer_request_keeps_its_deadline_when_an_older_verdict_arrives, setup, teardown),
    cmocka_unit_test_setup_teardown(a_new_request_restarts_the_deadline, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_late_verdict_cannot_resurrect_an_unbounded_request, setup, teardown),
    cmocka_unit_test(the_public_timeout_macros_hold_their_contract),
    cmocka_unit_test_setup_teardown(a_disabled_timeout_never_abandons, setup, teardown),
    cmocka_unit_test_setup_teardown(each_request_carries_its_own_timeout, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_service_delay_does_not_extend_the_callers_deadline, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_delay_that_cannot_fit_abandons_immediately_and_reports_it, setup, teardown),
    cmocka_unit_test_setup_teardown(a_delay_that_fits_leaves_the_request_queued, setup, teardown),
    cmocka_unit_test_setup_teardown(a_delay_cannot_abandon_an_unbounded_request, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
