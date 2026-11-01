// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file e2e_su_offer_test.c
 * @brief Software updates e2e: updates OFFERED by the real service, driven
 *        through the whole client.
 *
 * Everything under the test is shipping code: az_iot_su_client (engine), the
 * DPS device-update channel, the MQTT adapter (Paho or az_mqtt), X.509 auth and the OpenSSL
 * crypto backend. Files are downloaded for real (libcurl) and hashed by the
 * engine. Install and apply only record that they ran, with failures injected
 * per scenario.
 *
 * A spy wraps the shipping channel to record every report the engine sends
 * and the service's verdict on it; the client exposes neither.
 *
 * Offers are staged by the service side before the run (see
 * tests/e2e/scripts/SuE2E.psm1). Each is told apart by its compatibility
 * model, because the certificate fixes the device identity:
 *
 *   AZ_IOT_E2E_SU_OFFER_MANUFACTURER             all offers
 *   AZ_IOT_E2E_SU_OFFER_MODEL                    installs and succeeds
 *   AZ_IOT_E2E_SU_OFFER_MODEL_INSTALL_FAILURE    install fails
 *   AZ_IOT_E2E_SU_OFFER_MODEL_ALREADY_INSTALLED  device already has it
 *   AZ_IOT_E2E_SU_OFFER_MODEL_UNTRUSTED          verified against a wrong key
 *   AZ_IOT_E2E_SU_OFFER_MODEL_LIBRARY_MODE       handed to the test (library mode)
 *   AZ_IOT_E2E_SU_OFFER_MODEL_DELTA              built from a related file
 *   AZ_IOT_E2E_SU_OFFER_MODEL_DELTA_NO_SOURCE    alike, device has no source
 *   AZ_IOT_E2E_SU_OFFER_MODEL_MICROSOFT_DELTA    built by the reference microsoft/delta:1 handler
 *
 * The DELTA offers name the test download handler E2E_SU_TEST_DELTA_HANDLER_ID: their file's
 * related file is the file XOR a source both sides generate (delta_source_byte()). The
 * MICROSOFT_DELTA offer is the unit tests' vectors (tests/unit/data/su_delta): the file is
 * target.swu, its related file delta.bin, made from source.swu. The reference handler
 * (az_iot_su_delta.h) builds it with the real delta processor, AZ_IOT_SU_DELTA_TEST_LIBRARY; POSIX
 * only (AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA), so not run elsewhere.
 *
 * A missing variable fails the scenario that needs it.
 *
 * E2E-PLACEHOLDER: scenarios not written yet:
 *   - operational route (az_iot_su_client_request_update() after registration);
 *     the client supports it, the e2e scripts stage only onboarding jobs
 *   - multi-step update (reports with more than one step are unmeasured)
 *   - reboot and resume() mid-workflow (reboot-safe reporting)
 */

#define _POSIX_C_SOURCE 200809L
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#endif

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#endif

#include <cmocka.h>

#include <curl/curl.h>

#include <azure/core/az_base64.h>
#include <azure/core/az_json.h>

/* The MQTT adapter this build of the suite uses (see tests/e2e/CMakeLists.txt). */
#include "e2e_mqtt_adapter.h"
#include "azure/iot/az_iot_certificate_provider_pem.h"
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_su.h"

#include "internal/connection_client_internal.h"

#include "su_internal.h"
#include "su_protocol_internal.h"

#include "az_iot_crypto_openssl.h"
#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
#include "az_iot_su_delta.h"
#endif

#include "e2e_su_env.h"
#include "support/test_env.h"
#include "e2e_su_test_roots.h"

/* --- budgets ------------------------------------------------------------- */

#define E2E_SU_SESSION_BUDGET_S 60 /* provisioning session up */
#define E2E_SU_FETCH_BUDGET_S 90 /* update check answered */
#define E2E_SU_WORKFLOW_BUDGET_S 300 /* offer -> terminal report acknowledged */
#define E2E_SU_VERDICT_BUDGET_S 60 /* one report acknowledged */
#define E2E_SU_REQUEST_TIMEOUT_MS 60000u
#define E2E_SU_DOWNLOAD_TIMEOUT_S 120L

#define E2E_SU_MAX_FILES AZ_IOT_SU_MAX_FILE_COUNT
#define E2E_SU_MAX_STEPS AZ_IOT_SU_MAX_STEP_COUNT

/* --- delta updates ------------------------------------------------------- */

/* The test download handler's id; SuE2E.psm1 names it in the delta offers. */
#define E2E_SU_TEST_DELTA_HANDLER_ID "sdke2e/xor-delta:1"
/* Bytes of the source the delta offers' related file was made from. */
#define E2E_SU_DELTA_SOURCE_SIZE 4096u

/* The delta engine's code the no-source scenario asserts on. */
#define E2E_SU_DELTA_NO_SOURCE_CODE \
  AZ_IOT_SU_EXTENDED_RESULT(AZ_IOT_SU_FACILITY_DELTA, AZ_IOT_SU_DELTA_ERROR_NO_SOURCE)

#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
/* Update provider of the offers (SuE2E.psm1); the reference handler caches sources under it. */
#define E2E_SU_OFFER_PROVIDER "sdke2e"

/* Set by tests/e2e/CMakeLists.txt with the reference handler: its test vectors, and the delta
 * processor (AZ_IOT_SU_DELTA_TEST_LIBRARY; empty when not configured). */
#if !defined(AZ_IOT_E2E_SU_DELTA_VECTORS_DIRECTORY) || !defined(AZ_IOT_E2E_SU_DELTA_LIBRARY)
#error "Define AZ_IOT_E2E_SU_DELTA_VECTORS_DIRECTORY and AZ_IOT_E2E_SU_DELTA_LIBRARY"
#endif
#endif

/* --- run-wide state ------------------------------------------------------ */

static e2e_su_env g_env;
static char* g_manufacturer;
static char* g_model;
/* The offer a scenario asked require_model() for; freed after the scenario. */
static char* g_scenario_model;

/* Microsoft production roots plus the test roots. */
#define E2E_SU_MAX_ROOTS 8
static az_iot_su_root_key g_trusted[E2E_SU_MAX_ROOTS];
static size_t g_trusted_count;
/* Same kids, each bound to another key's modulus: every signature fails. */
static az_iot_su_root_key g_wrong[E2E_SU_MAX_ROOTS];

/* Base64 SHA-256 of the delta source, as the offers' `microsoft.sourceFileHash`. */
static char g_delta_source_hash[48];
#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
/* Base64 SHA-256 of the reference handler's source.swu and target.swu vectors. */
static char g_reference_source_hash[48];
static char g_reference_target_hash[48];
#endif

/* --- platform hooks ------------------------------------------------------ */

typedef enum
{
  OP_DOWNLOAD,
  OP_READ,
  OP_IS_INSTALLED,
  OP_BACKUP,
  OP_INSTALL,
  OP_APPLY,
  OP_RESTORE,
} op_kind;

#define E2E_SU_MAX_OPS 64
/* The scratch directory: Windows' temporary directory can be long. */
#define E2E_SU_DIR_MAX 260
#define E2E_SU_PATH_MAX (E2E_SU_DIR_MAX + 16)
/* Longest file id kept, terminator included. */
#define E2E_SU_FILE_ID_SIZE 64

/** @brief Platform-hook state: a scratch directory, an op log and injected faults. */
typedef struct
{
  char dir[E2E_SU_DIR_MAX];
  op_kind ops[E2E_SU_MAX_OPS];
  size_t op_count;

  int32_t fail_install_step; /* -1: never */
  bool already_installed;

  int64_t expected_size[E2E_SU_MAX_FILES];
  int64_t downloaded_size[E2E_SU_MAX_FILES];

  /* Delta updates: the download handler under test (the test one or the reference one), whether
   * the device holds the test handler's source, the reference handler's source cache, the id of
   * the related file chosen (download_fn stores that one apart), and what ran. */
  az_iot_su_download_handler delta_handler;
  bool has_delta_source;
  char delta_cache_dir[E2E_SU_PATH_MAX];
  char chosen_related_file_id[E2E_SU_FILE_ID_SIZE];
  size_t related_file_choices;
  size_t related_file_downloads;
  size_t full_downloads;
  size_t builds_from_related_file;
  size_t workflow_ends;
  size_t workflow_successes;
} platform;

static void op_log(platform* p, op_kind k)
{
  if (p->op_count < E2E_SU_MAX_OPS)
  {
    p->ops[p->op_count++] = k;
  }
}

static bool op_seen(const platform* p, op_kind k)
{
  for (size_t i = 0; i < p->op_count; ++i)
  {
    if (p->ops[i] == k)
    {
      return true;
    }
  }
  return false;
}

/** @brief True when @p seq occurs in the log in that order (not necessarily adjacent). */
static bool ops_in_order(const platform* p, const op_kind* seq, size_t n)
{
  size_t j = 0;
  for (size_t i = 0; i < p->op_count && j < n; ++i)
  {
    if (p->ops[i] == seq[j])
    {
      ++j;
    }
  }
  return j == n;
}

/** @brief True when @p file is the related file the test download handler chose. */
static bool is_chosen_related_file(platform* p, const az_iot_su_client_update_manifest_file* file)
{
  return p->chosen_related_file_id[0] != '\0'
      && az_span_is_content_equal(file->id, az_span_create_from_str(p->chosen_related_file_id));
}

/** @brief Where file @p index is stored; @p related: the related file chosen for it. */
static void file_path(const platform* p, uint32_t index, bool related, char* out, size_t cap)
{
  (void)snprintf(out, cap, "%s/%c%u", p->dir, related ? 'r' : 'f', (unsigned)index);
}

/** @brief CURLOPT_WRITEFUNCTION: writes with this program's C runtime, not libcurl's (Windows). */
static size_t write_downloaded_chunk(char* data, size_t size, size_t count, void* file)
{
  return fwrite(data, size, count, (FILE*)file);
}

static int32_t hook_download(
    const az_iot_su_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* ctx)
{
  (void)file_count;
  platform* p = (platform*)ctx;
  op_log(p, OP_DOWNLOAD);

  char url_z[2048];
  if (file_index >= E2E_SU_MAX_FILES || az_span_size(url) <= 0
      || (size_t)az_span_size(url) >= sizeof(url_z))
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  memcpy(url_z, az_span_ptr(url), (size_t)az_span_size(url));
  url_z[az_span_size(url)] = '\0';

  bool related = is_chosen_related_file(p, file);
  if (related)
  {
    p->related_file_downloads++;
  }
  else
  {
    p->full_downloads++;
  }
  char path[E2E_SU_PATH_MAX];
  file_path(p, file_index, related, path, sizeof(path));
  FILE* f = fopen(path, "wb");
  if (f == NULL)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  CURL* c = curl_easy_init();
  CURLcode rc = CURLE_FAILED_INIT;
  long http = 0;
  if (c != NULL)
  {
    (void)curl_easy_setopt(c, CURLOPT_URL, url_z);
    (void)curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_downloaded_chunk);
    (void)curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    (void)curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    (void)curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    (void)curl_easy_setopt(c, CURLOPT_TIMEOUT, E2E_SU_DOWNLOAD_TIMEOUT_S);
    rc = curl_easy_perform(c);
    (void)curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(c);
  }
  long size = ftell(f);
  fclose(f);

  /* The URL may carry a token, so it is not printed. */
  if (rc != CURLE_OK)
  {
    printf(
        "  download of file %u failed: %s (HTTP %ld)\n", file_index, curl_easy_strerror(rc), http);
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  if (!related)
  {
    p->expected_size[file_index] = file->size_in_bytes;
    p->downloaded_size[file_index] = size;
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t hook_read_file(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* ctx)
{
  platform* p = (platform*)ctx;
  if (offset == 0)
  {
    op_log(p, OP_READ);
  }
  char path[E2E_SU_PATH_MAX];
  file_path(p, file_index, is_chosen_related_file(p, file), path, sizeof(path));
  FILE* f = fopen(path, "rb");
  if (f == NULL)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  int32_t r = AZ_IOT_SU_RESULT_FAILURE;
  if (offset <= (size_t)LONG_MAX && fseek(f, (long)offset, SEEK_SET) == 0)
  {
    *out_read = fread(buffer, 1, buffer_size, f);
    r = ferror(f) ? AZ_IOT_SU_RESULT_FAILURE : AZ_IOT_SU_RESULT_SUCCESS;
  }
  fclose(f);
  return r;
}

static int32_t hook_is_installed(const az_iot_su_client_update_manifest* m, void* ctx)
{
  (void)m;
  platform* p = (platform*)ctx;
  op_log(p, OP_IS_INSTALLED);
  return p->already_installed ? AZ_IOT_SU_RESULT_ALREADY_INSTALLED : AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t hook_backup(const az_iot_su_client_update_manifest* m, uint32_t step, void* ctx)
{
  (void)m;
  (void)step;
  op_log((platform*)ctx, OP_BACKUP);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/* Installing is checking the payload arrived whole; nothing is written. */
static int32_t hook_install(const az_iot_su_client_update_manifest* m, uint32_t step, void* ctx)
{
  platform* p = (platform*)ctx;
  op_log(p, OP_INSTALL);
  if (p->fail_install_step >= 0 && (uint32_t)p->fail_install_step == step)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  for (uint32_t i = 0; i < m->files_count && i < E2E_SU_MAX_FILES; ++i)
  {
    if (p->downloaded_size[i] != m->files[i].size_in_bytes)
    {
      return AZ_IOT_SU_RESULT_FAILURE;
    }
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t hook_apply(const az_iot_su_client_update_manifest* m, uint32_t step, void* ctx)
{
  (void)m;
  (void)step;
  op_log((platform*)ctx, OP_APPLY);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t hook_restore(const az_iot_su_client_update_manifest* m, uint32_t step, void* ctx)
{
  (void)m;
  (void)step;
  op_log((platform*)ctx, OP_RESTORE);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/* No reboot happens here, so there is nothing to keep. */
static int32_t hook_persist(const uint8_t* blob, size_t len, void* ctx)
{
  (void)blob;
  (void)len;
  (void)ctx;
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t hook_load(uint8_t* blob, size_t cap, size_t* out_len, void* ctx)
{
  (void)blob;
  (void)cap;
  (void)out_len;
  (void)ctx;
  return 1; /* nothing persisted */
}

/* --- delta updates: hashes and files -------------------------------------- */

/** @brief Finishes @p ctx into the base64 SHA-256 at @p out; @p ok: whether every update did. */
static bool finish_sha256_base64(
    const az_iot_crypto* crypto,
    az_iot_sha256_ctx* ctx,
    bool ok,
    char* out,
    size_t out_size)
{
  uint8_t digest[AZ_IOT_SHA256_SIZE];
  ok = crypto->sha256_final(crypto, ctx, digest) == AZ_IOT_OK && ok;
  int32_t written = 0;
  ok = ok
      && az_result_succeeded(az_base64_encode(
          az_span_create((uint8_t*)out, (int32_t)out_size - 1),
          az_span_create(digest, (int32_t)sizeof(digest)),
          &written));
  out[ok ? written : 0] = '\0';
  return ok;
}

/** @brief Size of the file at @p path, or -1. */
static int64_t size_of_file(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (f == NULL)
  {
    return -1;
  }
  long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1L;
  (void)fclose(f);
  return (int64_t)size;
}

#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA

/** @brief Writes the base64 SHA-256 of the file at @p path to @p out. */
static bool sha256_base64_of_file(const char* path, char* out, size_t out_size)
{
  const az_iot_crypto* crypto = az_iot_crypto_openssl();
  az_iot_sha256_ctx ctx;
  FILE* in = fopen(path, "rb");
  if (in == NULL)
  {
    return false;
  }
  if (crypto->sha256_init(crypto, &ctx) != AZ_IOT_OK)
  {
    (void)fclose(in);
    return false;
  }
  bool ok = true;
  uint8_t chunk[512];
  size_t n = 0;
  while (ok && (n = fread(chunk, 1, sizeof(chunk), in)) > 0)
  {
    ok = crypto->sha256_update(crypto, &ctx, chunk, n) == AZ_IOT_OK;
  }
  ok = ok && !ferror(in);
  (void)fclose(in);
  return finish_sha256_base64(crypto, &ctx, ok, out, out_size);
}

/** @brief Copies the file at @p from to @p to. */
static bool copy_file(const char* from, const char* to)
{
  FILE* in = fopen(from, "rb");
  FILE* out = (in != NULL) ? fopen(to, "wb") : NULL;
  bool ok = in != NULL && out != NULL;
  uint8_t chunk[512];
  size_t n = 0;
  while (ok && (n = fread(chunk, 1, sizeof(chunk), in)) > 0)
  {
    ok = fwrite(chunk, 1, n, out) == n;
  }
  ok = ok && !ferror(in);
  if (out != NULL)
  {
    ok = fclose(out) == 0 && ok;
  }
  if (in != NULL)
  {
    (void)fclose(in);
  }
  return ok;
}

/** @brief Creates the directory @p path; true when it exists. */
static bool make_directory(const char* path)
{
#ifdef _WIN32
  return _mkdir(path) == 0 || errno == EEXIST;
#else
  return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

/** @brief Path of a vector of the reference handler's tests (tests/unit/data/su_delta). */
static void reference_delta_vector_path(const char* name, char* out, size_t cap)
{
  (void)snprintf(out, cap, "%s/%s", AZ_IOT_E2E_SU_DELTA_VECTORS_DIRECTORY, name);
}
#endif

/* --- delta updates: test download handler --------------------------------- */

/** @brief Byte @p i of the delta source. SuE2E.psm1 (Get-SuE2EDeltaSource) generates the same. */
static uint8_t delta_source_byte(size_t i) { return (uint8_t)((i * 31u + 7u) & 0xFFu); }

/** @brief Sets g_delta_source_hash to the base64 SHA-256 of the delta source. */
static bool compute_delta_source_hash(void)
{
  const az_iot_crypto* crypto = az_iot_crypto_openssl();
  az_iot_sha256_ctx ctx;
  if (crypto->sha256_init(crypto, &ctx) != AZ_IOT_OK)
  {
    return false;
  }
  bool ok = true;
  for (size_t i = 0; i < E2E_SU_DELTA_SOURCE_SIZE; ++i)
  {
    uint8_t b = delta_source_byte(i);
    ok = ok && crypto->sha256_update(crypto, &ctx, &b, 1) == AZ_IOT_OK;
  }
  return finish_sha256_base64(crypto, &ctx, ok, g_delta_source_hash, sizeof(g_delta_source_hash));
}

/** @brief True when @p related's `microsoft.sourceFileHash` property is the delta source's hash. */
static bool is_made_from_delta_source(const az_iot_su_client_update_manifest_file* related)
{
  az_json_reader reader;
  if (az_result_failed(az_json_reader_init(&reader, related->properties, NULL))
      || az_result_failed(az_json_reader_next_token(&reader))
      || reader.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return false;
  }
  while (az_result_succeeded(az_json_reader_next_token(&reader))
         && reader.token.kind == AZ_JSON_TOKEN_PROPERTY_NAME)
  {
    bool is_source_hash
        = az_json_token_is_text_equal(&reader.token, AZ_SPAN_FROM_STR("microsoft.sourceFileHash"));
    if (az_result_failed(az_json_reader_next_token(&reader)))
    {
      return false;
    }
    if (is_source_hash)
    {
      return reader.token.kind == AZ_JSON_TOKEN_STRING
          && az_json_token_is_text_equal(
                 &reader.token, az_span_create_from_str(g_delta_source_hash));
    }
    if (az_result_failed(az_json_reader_skip_children(&reader)))
    {
      return false;
    }
  }
  return false;
}

/** @brief Chooses the related file made from the delta source, when the device holds it. */
static int32_t test_delta_choose_related_file(
    const az_iot_su_client_update_manifest* manifest,
    const az_iot_su_client_update_manifest_file* file,
    uint32_t* out_related_file_index,
    void* ctx)
{
  const platform* p = (const platform*)ctx;
  for (uint32_t i = 0; p->has_delta_source && i < file->related_files_count
       && file->related_files_index + i < manifest->related_files_count;
       ++i)
  {
    if (is_made_from_delta_source(&manifest->related_files[file->related_files_index + i]))
    {
      *out_related_file_index = i;
      return AZ_IOT_SU_RESULT_SUCCESS;
    }
  }
  return AZ_IOT_SU_RESULT_FAILURE;
}

/** @brief Builds file @p file_index as its downloaded related file XOR the delta source. */
static int32_t test_delta_build_file_from_related(
    const az_iot_su_client_update_manifest_file* file,
    const az_iot_su_client_update_manifest_file* related_file,
    uint32_t file_index,
    void* ctx)
{
  (void)file;
  (void)related_file;
  const platform* p = (const platform*)ctx;
  char related_path[E2E_SU_PATH_MAX];
  char built_path[E2E_SU_PATH_MAX];
  file_path(p, file_index, true, related_path, sizeof(related_path));
  file_path(p, file_index, false, built_path, sizeof(built_path));
  FILE* in = fopen(related_path, "rb");
  FILE* out = (in != NULL) ? fopen(built_path, "wb") : NULL;
  size_t size = 0;
  bool ok = in != NULL && out != NULL;
  while (ok)
  {
    uint8_t chunk[512];
    size_t n = fread(chunk, 1, sizeof(chunk), in);
    for (size_t i = 0; i < n; ++i)
    {
      chunk[i] ^= delta_source_byte(size + i);
    }
    ok = !ferror(in) && fwrite(chunk, 1, n, out) == n;
    size += n;
    if (n < sizeof(chunk))
    {
      break;
    }
  }
  if (out != NULL)
  {
    ok = fclose(out) == 0 && ok;
  }
  if (in != NULL)
  {
    (void)fclose(in);
  }
  return ok ? AZ_IOT_SU_RESULT_SUCCESS : AZ_IOT_SU_RESULT_FAILURE;
}

/* --- delta updates: reference handler ------------------------------------- */

#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA

/** @brief az_iot_su_delta_get_file_path_fn: where hook_download() stores @p file. */
static int32_t reference_delta_get_file_path(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    char* path,
    size_t path_size,
    void* ctx)
{
  platform* p = (platform*)ctx;
  file_path(p, file_index, is_chosen_related_file(p, file), path, path_size);
  return AZ_IOT_SU_RESULT_SUCCESS;
}
#endif

/* --- delta updates: recording handler ------------------------------------- */

/*
 * Registered in place of platform::delta_handler, with its id: records what the client asked of
 * it and the related file it chose, so hook_download() and hook_read_file() store that one apart.
 */

/** @brief Calls the handler under test, recording the related file it chose. */
static int32_t recording_choose_related_file(
    const az_iot_su_client_update_manifest* manifest,
    const az_iot_su_client_update_manifest_file* file,
    uint32_t* out_related_file_index,
    void* ctx)
{
  platform* p = (platform*)ctx;
  int32_t result = p->delta_handler.choose_related_file_fn(
      manifest, file, out_related_file_index, p->delta_handler.user_ctx);
  if (result == AZ_IOT_SU_RESULT_IN_PROGRESS)
  {
    return result;
  }
  p->related_file_choices++;
  if (result == AZ_IOT_SU_RESULT_SUCCESS)
  {
    uint32_t index = file->related_files_index + *out_related_file_index;
    if (index >= manifest->related_files_count
        || az_span_size(manifest->related_files[index].id)
            >= (int32_t)sizeof(p->chosen_related_file_id))
    {
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    az_span_to_str(
        p->chosen_related_file_id,
        (int32_t)sizeof(p->chosen_related_file_id),
        manifest->related_files[index].id);
  }
  return result;
}

/** @brief Calls the handler under test, recording the size of the file it built. */
static int32_t recording_build_file_from_related(
    const az_iot_su_client_update_manifest_file* file,
    const az_iot_su_client_update_manifest_file* related_file,
    uint32_t file_index,
    void* ctx)
{
  platform* p = (platform*)ctx;
  if (file_index >= E2E_SU_MAX_FILES)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  int32_t result = p->delta_handler.build_file_from_related_fn(
      file, related_file, file_index, p->delta_handler.user_ctx);
  if (result == AZ_IOT_SU_RESULT_IN_PROGRESS)
  {
    return result;
  }
  p->builds_from_related_file++;
  if (result == AZ_IOT_SU_RESULT_SUCCESS)
  {
    char built_path[E2E_SU_PATH_MAX];
    file_path(p, file_index, false, built_path, sizeof(built_path));
    p->expected_size[file_index] = file->size_in_bytes;
    p->downloaded_size[file_index] = size_of_file(built_path);
  }
  return result;
}

/** @brief Counts the ended and the successful workflows, then calls the handler under test. */
static void recording_workflow_ended(
    const az_iot_su_client_update_manifest* manifest,
    az_iot_su_outcome outcome,
    void* ctx)
{
  platform* p = (platform*)ctx;
  p->workflow_ends++;
  if (outcome == AZ_IOT_SU_OUTCOME_SUCCEEDED)
  {
    p->workflow_successes++;
  }
  if (p->delta_handler.workflow_ended_fn != NULL)
  {
    p->delta_handler.workflow_ended_fn(manifest, outcome, p->delta_handler.user_ctx);
  }
}

/* --- report capture ------------------------------------------------------ */

/** @brief One report the engine handed the channel, and the service's verdict. */
typedef struct
{
  az_iot_su_outcome outcome;
  az_iot_su_failure_origin failure_origin;
  int32_t result_code;
  int32_t step_count;
  az_iot_su_outcome step0_outcome;

  bool has_verdict;
  az_iot_result result;
  az_iot_su_error_action action;
  int32_t service_code;
  char service_message[128];
} spy_report;

/** @brief Owned copy of a report, so it can be sent again from a later scenario. */
typedef struct
{
  bool valid;
  char workflow_id[AZ_IOT_SU_WORKFLOW_ID_SIZE + 1];
  bool has_installed;
  char provider[64];
  char name[64];
  char version[64];
  az_iot_su_report_update_id installed;
  az_iot_su_outcome outcome;
  az_iot_su_failure_origin failure_origin;
  int32_t result_code;
  char extended_result_codes[64];
  bool has_details;
  char result_details[128];
  az_iot_su_step_result steps[E2E_SU_MAX_STEPS];
  char step_details[E2E_SU_MAX_STEPS][64];
  int32_t step_count;
  az_iot_su_report view;
} saved_report;

static void copy_str(char* dst, size_t cap, const char* src)
{
  snprintf(dst, cap, "%s", src != NULL ? src : "");
}

static void save_report(saved_report* s, const az_iot_su_report* r)
{
  memset(s, 0, sizeof(*s));
  copy_str(s->workflow_id, sizeof(s->workflow_id), r->workflow_id);
  s->has_installed = r->installed_update_id != NULL;
  if (s->has_installed)
  {
    copy_str(s->provider, sizeof(s->provider), r->installed_update_id->provider);
    copy_str(s->name, sizeof(s->name), r->installed_update_id->name);
    copy_str(s->version, sizeof(s->version), r->installed_update_id->version);
  }
  s->outcome = r->outcome;
  s->failure_origin = r->failure_origin;
  s->result_code = r->result_code;
  copy_str(s->extended_result_codes, sizeof(s->extended_result_codes), r->extended_result_codes);
  s->has_details = r->result_details != NULL;
  copy_str(s->result_details, sizeof(s->result_details), r->result_details);
  s->step_count = r->step_results_count;
  if (s->step_count > E2E_SU_MAX_STEPS)
  {
    s->step_count = E2E_SU_MAX_STEPS;
  }
  for (int32_t i = 0; i < s->step_count; ++i)
  {
    s->steps[i] = r->step_results[i];
    size_t n = (size_t)az_span_size(r->step_results[i].result_details);
    if (n >= sizeof(s->step_details[i]))
    {
      n = sizeof(s->step_details[i]) - 1;
    }
    if (n > 0)
    {
      memcpy(s->step_details[i], az_span_ptr(r->step_results[i].result_details), n);
    }
    s->step_details[i][n] = '\0';
  }
  s->valid = true;
}

/** @brief A report pointing into @p s. Rebuilt on every call, so @p s may be copied. */
static const az_iot_su_report* saved_report_view(saved_report* s)
{
  s->installed = (az_iot_su_report_update_id){ s->provider, s->name, s->version };
  for (int32_t i = 0; i < s->step_count; ++i)
  {
    s->steps[i].result_details = az_span_create_from_str(s->step_details[i]);
  }
  s->view = (az_iot_su_report){
    .workflow_id = s->workflow_id,
    .installed_update_id = s->has_installed ? &s->installed : NULL,
    .outcome = s->outcome,
    .failure_origin = s->failure_origin,
    .result_code = s->result_code,
    .extended_result_codes = s->extended_result_codes,
    .result_details = s->has_details ? s->result_details : NULL,
    .step_results = s->step_count > 0 ? s->steps : NULL,
    .step_results_count = s->step_count,
  };
  return &s->view;
}

/* --- spy channel --------------------------------------------------------- */

#define E2E_SU_MAX_REPORTS 24

/** @brief Wraps the shipping channel, recording what passes through it. */
typedef struct
{
  az_iot_su_channel inner;
  az_iot_su_channel_update_cb engine_update;
  az_iot_su_channel_result_cb engine_result;
  void* engine_ctx;

  size_t offers;
  bool fetch_answered;
  az_iot_result fetch_result;
  az_iot_su_error_action fetch_action;

  spy_report reports[E2E_SU_MAX_REPORTS];
  size_t report_count;
  saved_report last_terminal;
} spy_channel;

static void spy_on_update(const uint8_t* payload, size_t len, void* ctx)
{
  spy_channel* s = (spy_channel*)ctx;
  s->offers++;
  s->engine_update(payload, len, s->engine_ctx);
}

/** @brief True when the engine will not retry after this verdict (mirrors on_channel_result). */
static bool verdict_is_final(az_iot_result result, az_iot_su_error_action action)
{
  return result == AZ_IOT_OK || action == AZ_IOT_SU_ERROR_ACTION_NONE
      || action == AZ_IOT_SU_ERROR_ACTION_FATAL || action == AZ_IOT_SU_ERROR_ACTION_PROCEED
      || action == AZ_IOT_SU_ERROR_ACTION_RECHECK;
}

static void spy_on_result(
    az_iot_su_operation op,
    az_iot_result result,
    az_iot_su_error_action action,
    const az_iot_su_service_error* se,
    void* ctx)
{
  spy_channel* s = (spy_channel*)ctx;
  if (!verdict_is_final(result, action))
  {
    /* The engine retries; the resend gets its own entry and verdict. */
    printf("  retryable verdict on op %d: %s\n", (int)op, az_iot_result_to_string(result));
  }
  else if (op == AZ_IOT_SU_OP_REPORT_STATUS)
  {
    /* One operation at a time, so the verdict is the newest report's. */
    if (s->report_count > 0 && !s->reports[s->report_count - 1].has_verdict)
    {
      spy_report* r = &s->reports[s->report_count - 1];
      r->has_verdict = true;
      r->result = result;
      r->action = action;
      r->service_code = (se != NULL) ? se->code : 0;
      copy_str(r->service_message, sizeof(r->service_message), (se != NULL) ? se->message : "");
    }
  }
  else
  {
    s->fetch_answered = true;
    s->fetch_result = result;
    s->fetch_action = action;
  }
  s->engine_result(op, result, action, se, s->engine_ctx);
}

static az_iot_result spy_open(
    void* ctx,
    az_iot_su_channel_update_cb cb,
    az_iot_su_channel_result_cb result_cb,
    void* engine_ctx)
{
  spy_channel* s = (spy_channel*)ctx;
  s->engine_update = cb;
  s->engine_result = result_cb;
  s->engine_ctx = engine_ctx;
  return s->inner.vtable->open(s->inner.ctx, spy_on_update, spy_on_result, s);
}

static void spy_close(void* ctx)
{
  spy_channel* s = (spy_channel*)ctx;
  s->inner.vtable->close(s->inner.ctx);
}

static az_iot_result spy_request_update(void* ctx, az_iot_su_operation op)
{
  spy_channel* s = (spy_channel*)ctx;
  return s->inner.vtable->request_update(s->inner.ctx, op);
}

static az_iot_result spy_report_fn(void* ctx, const az_iot_su_report* report)
{
  spy_channel* s = (spy_channel*)ctx;
  az_iot_result r = s->inner.vtable->report(s->inner.ctx, report);
  /* Only an accepted report gets a verdict, so only those are recorded. */
  if (r == AZ_IOT_OK && s->report_count < E2E_SU_MAX_REPORTS)
  {
    spy_report* e = &s->reports[s->report_count++];
    memset(e, 0, sizeof(*e));
    e->outcome = report->outcome;
    e->failure_origin = report->failure_origin;
    e->result_code = report->result_code;
    e->step_count = report->step_results_count;
    e->step0_outcome = report->step_results_count > 0 ? report->step_results[0].outcome
                                                      : AZ_IOT_SU_OUTCOME_IN_PROGRESS;
    if (report->outcome != AZ_IOT_SU_OUTCOME_IN_PROGRESS)
    {
      save_report(&s->last_terminal, report);
    }
  }
  return r;
}

static az_iot_result spy_set_device_properties(
    void* ctx,
    const az_iot_su_device_properties* properties)
{
  spy_channel* s = (spy_channel*)ctx;
  return s->inner.vtable->set_device_properties != NULL
      ? s->inner.vtable->set_device_properties(s->inner.ctx, properties)
      : AZ_IOT_OK;
}

static az_iot_result spy_do_work(void* ctx)
{
  spy_channel* s = (spy_channel*)ctx;
  return s->inner.vtable->do_work != NULL ? s->inner.vtable->do_work(s->inner.ctx) : AZ_IOT_OK;
}

static void spy_cancel_update(void* ctx, az_iot_su_operation op)
{
  spy_channel* s = (spy_channel*)ctx;
  s->inner.vtable->cancel_update(s->inner.ctx, op);
}

static const az_iot_su_channel_vtable k_spy_vtable = {
  .open = spy_open,
  .close = spy_close,
  .request_update = spy_request_update,
  .report = spy_report_fn,
  .set_device_properties = spy_set_device_properties,
  .do_work = spy_do_work,
  .cancel_update = spy_cancel_update,
};

/* --- fixture ------------------------------------------------------------- */

typedef struct
{
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client conn;
  uint8_t* mqtt_workspace; /**< free() after az_iot_connection_client_deinit(). */
  az_iot_su_channel_dps channel_state;
  uint8_t channel_body[AZ_IOT_SU_DEFAULT_MAX_MESSAGE_SIZE];
  char channel_device_properties
      [AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE + AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE];
  spy_channel spy;
  az_iot_su_client su;
  platform plat;
  /* The client references its download handlers for its lifetime: the recording handler, and
   * the reference handler it may call. */
  az_iot_su_download_handler delta_handler;
#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
  az_iot_su_delta_handler reference_delta_handler;
#endif
  uint8_t su_workspace[AZ_IOT_SU_WORKSPACE_DEFAULT_SIZE];
  const char* model;

  /* What fixture_open() got through, so teardown releases exactly that. */
  bool have_dir;
  bool have_certs;
  bool have_conn;
  bool have_su;
#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
  bool have_reference_delta_handler;
#endif

  bool faulted;
  size_t abandoned;
  az_iot_su_operation abandoned_op;
  az_iot_result abandoned_reason;

  /* Library mode: the update handed over by AZ_IOT_SU_EVENT_UPDATE_AVAILABLE. */
  size_t updates_available;
  const az_iot_su_client_update_request* delegated_request;
  const az_iot_su_client_update_manifest* delegated_manifest;
} fixture;

/* Large, and scenarios run one at a time. */
static fixture g_fx;

static void on_conn_state(const az_iot_connection_state_event* event, void* ctx)
{
  if (event != NULL && event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    ((fixture*)ctx)->faulted = true;
  }
}

static void on_su_event(const az_iot_su_event* event, void* ctx)
{
  fixture* fx = (fixture*)ctx;
  if (event->kind == AZ_IOT_SU_EVENT_OPERATION_ABANDONED)
  {
    fx->abandoned++;
    fx->abandoned_op = event->operation;
    fx->abandoned_reason = event->reason;
    printf(
        "  abandoned op %d: %s (service code %d \"%s\")\n",
        (int)event->operation,
        az_iot_result_to_string(event->reason),
        (int)event->service_error.code,
        event->service_error.message);
  }
  else if (event->kind == AZ_IOT_SU_EVENT_UPDATE_AVAILABLE)
  {
    fx->updates_available++;
    fx->delegated_request = event->request;
    fx->delegated_manifest = event->manifest;
  }
}

typedef enum
{
  DELTA_HANDLER_NONE,
  /* The test download handler, E2E_SU_TEST_DELTA_HANDLER_ID. */
  DELTA_HANDLER_TEST,
#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
  /* The reference microsoft/delta:1 handler, with the real delta processor. */
  DELTA_HANDLER_REFERENCE,
#endif
} delta_handler_kind;

typedef struct
{
  const char* manufacturer;
  const char* model;
  const az_iot_su_root_key* roots;
  size_t root_count;
  bool request_onboarding_update;
  int32_t fail_install_step;
  bool already_installed;
  /* No platform hooks: the verified update is handed to the test. */
  bool library_mode;
  /* The download handler registered, through the recording handler. */
  delta_handler_kind delta_handler;
  /* The device holds the source the delta handler builds from. */
  bool has_delta_source;
} fixture_options;

static fixture_options fixture_options_default(const char* model)
{
  return (fixture_options){
    .manufacturer = g_manufacturer,
    .model = model,
    .roots = g_trusted,
    .root_count = g_trusted_count,
    .request_onboarding_update = true,
    .fail_install_step = -1,
    .already_installed = false,
    .library_mode = false,
    .delta_handler = DELTA_HANDLER_NONE,
    .has_delta_source = false,
  };
}

/** @brief Creates an empty scratch directory under the system temporary directory. */
static bool make_scratch_dir(char* dir, size_t size)
{
  if (!az_iot_test_temp_path(dir, size, "az_iot_e2e_su_XXXXXX"))
  {
    return false;
  }
#ifdef _WIN32
  return _mktemp_s(dir, strlen(dir) + 1) == 0 && _mkdir(dir) == 0;
#else
  return mkdtemp(dir) != NULL;
#endif
}

#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA

/** @brief Writes the reference handler's source cache path of the file with base64 SHA-256
 * @p hash, from an update of the offers' provider. */
static az_iot_result reference_delta_cache_path(
    const fixture* fx,
    char* hash,
    char* out,
    size_t out_size)
{
  return az_iot_su_delta_handler_get_source_cache_path(
      &fx->reference_delta_handler,
      AZ_SPAN_FROM_STR(E2E_SU_OFFER_PROVIDER),
      AZ_SPAN_FROM_STR("sha256"),
      az_span_create_from_str(hash),
      out,
      out_size);
}

/** @brief Path of the reference handler's cache directory for the offers' provider. */
static void reference_delta_provider_dir(const fixture* fx, char* out, size_t cap)
{
  (void)snprintf(out, cap, "%s/%s", fx->plat.delta_cache_dir, E2E_SU_OFFER_PROVIDER);
}

/** @brief Puts source.swu in the reference handler's cache, as an earlier update would have. */
static void seed_reference_delta_source(fixture* fx)
{
  char provider_dir[E2E_SU_PATH_MAX];
  char cache_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  char source_path[E2E_SU_PATH_MAX];
  reference_delta_provider_dir(fx, provider_dir, sizeof(provider_dir));
  reference_delta_vector_path("source.swu", source_path, sizeof(source_path));
  assert_true(make_directory(fx->plat.delta_cache_dir));
  assert_true(make_directory(provider_dir));
  assert_int_equal(
      reference_delta_cache_path(fx, g_reference_source_hash, cache_path, sizeof(cache_path)),
      AZ_IOT_OK);
  assert_true(copy_file(source_path, cache_path));
}

/** @brief Removes the reference handler's cache: the vectors' entries and its directories. */
static void remove_reference_delta_cache(const fixture* fx)
{
  char* hashes[] = { g_reference_source_hash, g_reference_target_hash };
  for (size_t i = 0; i < sizeof(hashes) / sizeof(hashes[0]); ++i)
  {
    char cache_path[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
    if (reference_delta_cache_path(fx, hashes[i], cache_path, sizeof(cache_path)) == AZ_IOT_OK)
    {
      (void)remove(cache_path);
    }
  }
  char provider_dir[E2E_SU_PATH_MAX];
  reference_delta_provider_dir(fx, provider_dir, sizeof(provider_dir));
  (void)remove(provider_dir);
  (void)remove(fx->plat.delta_cache_dir);
}
#endif

/**
 * @brief Sets up the download handler @p o asks for, and the recording handler registered in its
 * place. The reference handler caches sources under the scratch directory.
 */
static void fixture_open_delta_handler(fixture* fx, const fixture_options* o)
{
  platform* p = &fx->plat;
  if (o->delta_handler == DELTA_HANDLER_TEST)
  {
    p->delta_handler = (az_iot_su_download_handler){
      .id = E2E_SU_TEST_DELTA_HANDLER_ID,
      .choose_related_file_fn = test_delta_choose_related_file,
      .build_file_from_related_fn = test_delta_build_file_from_related,
      .user_ctx = p,
    };
  }
#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
  else
  {
    (void)snprintf(p->delta_cache_dir, sizeof(p->delta_cache_dir), "%s/sdc", p->dir);
    az_iot_su_delta_handler_options options = az_iot_su_delta_handler_options_default();
    options.source_cache_directory = p->delta_cache_dir;
    options.get_file_path_fn = reference_delta_get_file_path;
    options.library_path = AZ_IOT_E2E_SU_DELTA_LIBRARY;
    options.user_ctx = p;
    assert_int_equal(
        az_iot_su_delta_handler_init(&fx->reference_delta_handler, &options), AZ_IOT_OK);
    fx->have_reference_delta_handler = true;
    p->delta_handler = az_iot_su_delta_handler_get_download_handler(&fx->reference_delta_handler);
    if (o->has_delta_source)
    {
      seed_reference_delta_source(fx);
    }
  }
#endif
  fx->delta_handler = (az_iot_su_download_handler){
    .id = p->delta_handler.id,
    .choose_related_file_fn = recording_choose_related_file,
    .build_file_from_related_fn = recording_build_file_from_related,
    .workflow_ended_fn = recording_workflow_ended,
    .user_ctx = p,
  };
}

static void fixture_open(fixture* fx, const fixture_options* o)
{
  memset(fx, 0, sizeof(*fx));
  fx->model = o->model;
  assert_true(make_scratch_dir(fx->plat.dir, sizeof(fx->plat.dir)));
  fx->have_dir = true;
  fx->plat.fail_install_step = o->fail_install_step;
  fx->plat.already_installed = o->already_installed;
  fx->plat.has_delta_source = o->has_delta_source;

  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  e2e_su_env_apply(&g_env, &opts, &pem);
  /* Every operation here runs on the provisioning session; no hub is needed. */
  opts.dps.provision_only = true;
  assert_int_equal(az_iot_certificate_provider_pem_init(&fx->certs, &pem), AZ_IOT_OK);
  fx->have_certs = true;
  opts.certificate_provider = &fx->certs.base;
  fx->mqtt_workspace = e2e_use_mqtt_adapter(&opts);
  assert_non_null(fx->mqtt_workspace);
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);
  fx->have_conn = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->conn, on_conn_state, fx), AZ_IOT_OK);

  az_iot_su_device_properties device_properties = { 0 };
  device_properties.manufacturer = o->manufacturer;
  device_properties.model = o->model;
  device_properties.installed_update_id.provider = "e2e";
  device_properties.installed_update_id.name = "e2e";
  device_properties.installed_update_id.version = "0.0.0";
  assert_int_equal(
      az_iot_su_channel_dps_init(
          &fx->channel_state,
          &fx->conn,
          &device_properties,
          fx->channel_body,
          sizeof(fx->channel_body),
          fx->channel_device_properties,
          sizeof(fx->channel_device_properties),
          &fx->spy.inner),
      AZ_IOT_OK);

  az_iot_su_platform_hooks hooks = {
    .download_fn = hook_download,
    .read_file_fn = hook_read_file,
    .install_fn = hook_install,
    .apply_fn = hook_apply,
    .backup_fn = hook_backup,
    .restore_fn = hook_restore,
    .is_installed_fn = hook_is_installed,
    .persist_workflow_state_fn = hook_persist,
    .load_workflow_state_fn = hook_load,
    .user_ctx = &fx->plat,
  };
  az_iot_su_client_config_options su = az_iot_su_client_config_options_default();
  su.hooks = o->library_mode ? NULL : &hooks;
  su.library_mode = o->library_mode;
  su.root_keys = o->roots;
  su.root_key_count = o->root_count;
  su.device_properties = &device_properties;
  su.workspace = fx->su_workspace;
  su.workspace_size = sizeof(fx->su_workspace);
  if (o->delta_handler != DELTA_HANDLER_NONE)
  {
    fixture_open_delta_handler(fx, o);
    su.download_handlers = &fx->delta_handler;
    su.download_handler_count = 1;
  }
  az_iot_su_channel spy = { .vtable = &k_spy_vtable, .ctx = &fx->spy };
  assert_int_equal(
      az_iot_su_client__initialize_with_channel(&fx->su, &spy, az_iot_crypto_openssl(), &su),
      AZ_IOT_OK);
  fx->have_su = true;
  assert_int_equal(az_iot_su_client_add_observer(&fx->su, on_su_event, fx), AZ_IOT_OK);

  if (o->request_onboarding_update)
  {
    assert_int_equal(
        az_iot_su_client_request_onboarding_update(&fx->su, E2E_SU_REQUEST_TIMEOUT_MS), AZ_IOT_OK);
  }
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
}

/* Idempotent, and safe after a fixture_open() that failed partway. */
static void fixture_close(fixture* fx)
{
  if (fx->have_su)
  {
    az_iot_su_client_deinit(&fx->su);
    fx->have_su = false;
  }
#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
  if (fx->have_reference_delta_handler)
  {
    remove_reference_delta_cache(fx);
    az_iot_su_delta_handler_deinit(&fx->reference_delta_handler);
    fx->have_reference_delta_handler = false;
  }
#endif
  if (fx->have_conn)
  {
    az_iot_connection_client_deinit(&fx->conn);
    fx->have_conn = false;
  }
  free(fx->mqtt_workspace);
  fx->mqtt_workspace = NULL;
  if (fx->have_certs)
  {
    az_iot_certificate_provider_pem_deinit(&fx->certs);
    fx->have_certs = false;
  }
  if (!fx->have_dir)
  {
    return;
  }
  fx->have_dir = false;
  for (uint32_t i = 0; i < E2E_SU_MAX_FILES; ++i)
  {
    char path[E2E_SU_PATH_MAX];
    file_path(&fx->plat, i, false, path, sizeof(path));
    (void)remove(path);
    file_path(&fx->plat, i, true, path, sizeof(path));
    (void)remove(path);
  }
#ifdef _WIN32
  (void)_rmdir(fx->plat.dir);
#else
  (void)remove(fx->plat.dir);
#endif
}

/* Pump both clients until @p cond holds, a fault, or the budget runs out. */
#define PUMP_UNTIL(fx, cond, seconds)                           \
  do                                                            \
  {                                                             \
    time_t deadline_ = time(NULL) + (seconds);                  \
    while (!(cond) && !(fx)->faulted && time(NULL) < deadline_) \
    {                                                           \
      (void)az_iot_connection_client_do_work(&(fx)->conn, 50);  \
      (void)az_iot_su_client_do_work(&(fx)->su);                \
    }                                                           \
  } while (0)

/** @brief The newest terminal report sent, answered or not, or NULL. */
static const spy_report* newest_sent_terminal_report(const fixture* fx)
{
  for (size_t i = fx->spy.report_count; i > 0; --i)
  {
    const spy_report* r = &fx->spy.reports[i - 1];
    if (r->outcome != AZ_IOT_SU_OUTCOME_IN_PROGRESS)
    {
      return r;
    }
  }
  return NULL;
}

/** @brief The acknowledged terminal report, or NULL. */
static const spy_report* terminal_report(const fixture* fx)
{
  const spy_report* r = newest_sent_terminal_report(fx);
  return (r != NULL && r->has_verdict) ? r : NULL;
}

/* Runs the offered workflow to its acknowledged terminal report. */
static const spy_report* run_offered_workflow(fixture* fx)
{
  PUMP_UNTIL(fx, terminal_report(fx) != NULL || fx->abandoned > 0, E2E_SU_WORKFLOW_BUDGET_S);
  assert_false(fx->faulted);
  assert_int_equal(fx->abandoned, 0);
  if (fx->spy.offers == 0)
  {
    fail_msg("no update was offered for model \"%s\"", fx->model);
  }
  const spy_report* r = terminal_report(fx);
  assert_non_null(r);
  printf(
      "  terminal report: outcome %d, resultCode %d, verdict %s (service code %d \"%s\")\n",
      (int)r->outcome,
      (int)r->result_code,
      az_iot_result_to_string(r->result),
      (int)r->service_code,
      r->service_message);
  return r;
}

/* Sends @p report on the channel directly and waits for its verdict. */
static const spy_report* send_report(fixture* fx, const az_iot_su_report* report)
{
  size_t before = fx->spy.report_count;
  assert_int_equal(k_spy_vtable.report(&fx->spy, report), AZ_IOT_OK);
  assert_int_equal(fx->spy.report_count, before + 1);
  const spy_report* r = &fx->spy.reports[before];
  PUMP_UNTIL(fx, r->has_verdict, E2E_SU_VERDICT_BUDGET_S);
  assert_false(fx->faulted);
  assert_true(r->has_verdict);
  printf(
      "  report outcome %d: verdict %s, action %d (service code %d \"%s\")\n",
      (int)report->outcome,
      az_iot_result_to_string(r->result),
      (int)r->action,
      (int)r->service_code,
      r->service_message);
  return r;
}

static const char* require_model(const char* name)
{
  free(g_scenario_model);
  g_scenario_model = e2e_su_env_require(name);
  if (g_scenario_model == NULL)
  {
    fail_msg("%s is not set: stage an offer for this scenario", name);
  }
  return g_scenario_model;
}

/* --- scenarios ----------------------------------------------------------- */

/* The terminal report of the first scenario, re-sent by the second. */
static saved_report g_real_report;

/* The user scenario: a real offer is fetched, verified against real roots,
 * downloaded, hashed, installed, applied and reported SUCCEEDED. */
static void real_update_is_downloaded_verified_installed_and_reported(void** state)
{
  (void)state;
  fixture* fx = &g_fx;
  fixture_options o = fixture_options_default(g_model);
  fixture_open(fx, &o);

  const spy_report* r = run_offered_workflow(fx);

  static const op_kind k_order[]
      = { OP_IS_INSTALLED, OP_DOWNLOAD, OP_READ, OP_BACKUP, OP_INSTALL, OP_APPLY };
  assert_true(ops_in_order(&fx->plat, k_order, sizeof(k_order) / sizeof(k_order[0])));
  assert_false(op_seen(&fx->plat, OP_RESTORE));
  assert_true(fx->plat.downloaded_size[0] > 0);
  assert_true(fx->plat.downloaded_size[0] == fx->plat.expected_size[0]);

  assert_int_equal(r->outcome, AZ_IOT_SU_OUTCOME_SUCCEEDED);
  assert_int_equal(r->result_code, 700);
  assert_int_equal(r->failure_origin, AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE);
  assert_true(r->step_count >= 1);
  assert_int_equal(r->step0_outcome, AZ_IOT_SU_OUTCOME_SUCCEEDED);
  assert_int_equal(r->result, AZ_IOT_OK);
  assert_int_equal(az_iot_su_client_get_state(&fx->su), AZ_IOT_SU_STATE_IDLE);

  g_real_report = fx->spy.last_terminal;
}

/* An identical report is accepted. An explicit conflict rejects this payload
 * permanently and asks for a new check; it is not acceptance of a duplicate. */
static void terminal_report_is_idempotent_and_a_conflict_is_detected(void** state)
{
  (void)state;
  if (!g_real_report.valid)
  {
    fail_msg("needs the terminal report of "
             "real_update_is_downloaded_verified_installed_and_reported");
  }
  fixture* fx = &g_fx;
  fixture_options o = fixture_options_default(g_model);
  o.request_onboarding_update = false;
  fixture_open(fx, &o);
  PUMP_UNTIL(fx, az_iot_connection_client__dps_session_ready(&fx->conn), E2E_SU_SESSION_BUDGET_S);
  assert_true(az_iot_connection_client__dps_session_ready(&fx->conn));

  saved_report same = g_real_report;
  const spy_report* r = send_report(fx, saved_report_view(&same));
  assert_int_equal(r->result, AZ_IOT_OK);

  saved_report conflicting = g_real_report;
  conflicting.outcome = AZ_IOT_SU_OUTCOME_FAILED;
  conflicting.failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_AGENT_CORE;
  conflicting.result_code = -1;
  copy_str(
      conflicting.extended_result_codes, sizeof(conflicting.extended_result_codes), "0x80000001");
  for (int32_t i = 0; i < conflicting.step_count; ++i)
  {
    conflicting.steps[i].outcome = AZ_IOT_SU_OUTCOME_FAILED;
    conflicting.steps[i].failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_AGENT_CORE;
    conflicting.steps[i].result_code = -1;
  }
  r = send_report(fx, saved_report_view(&conflicting));
  assert_int_not_equal(r->result, AZ_IOT_OK);
  assert_int_equal(r->service_code, 409000);
  assert_int_equal(r->action, AZ_IOT_SU_ERROR_ACTION_RECHECK);
}

/* A device whose compatibility matches no update is answered "no update". */
static void incompatible_device_is_offered_nothing(void** state)
{
  (void)state;
  char model[48];
  snprintf(model, sizeof(model), "e2e-no-offer-%lld", (long long)time(NULL));
  fixture* fx = &g_fx;
  fixture_options o = fixture_options_default(model);
  fixture_open(fx, &o);

  PUMP_UNTIL(fx, fx->spy.fetch_answered, E2E_SU_FETCH_BUDGET_S);
  assert_false(fx->faulted);
  assert_true(fx->spy.fetch_answered);
  assert_int_equal(fx->spy.fetch_result, AZ_IOT_OK);
  assert_int_equal(fx->spy.offers, 0);
  assert_int_equal(fx->spy.report_count, 0);
  assert_false(op_seen(&fx->plat, OP_DOWNLOAD));
  assert_true(fx->channel_state.agent_info_etag[0] != '\0');
}

/* An install failure rolls the step back and is reported FAILED. */
static void install_failure_is_rolled_back_and_reported_failed(void** state)
{
  (void)state;
  fixture* fx = &g_fx;
  fixture_options o
      = fixture_options_default(require_model("AZ_IOT_E2E_SU_OFFER_MODEL_INSTALL_FAILURE"));
  o.fail_install_step = 0;
  fixture_open(fx, &o);

  const spy_report* r = run_offered_workflow(fx);

  static const op_kind k_order[]
      = { OP_IS_INSTALLED, OP_DOWNLOAD, OP_READ, OP_BACKUP, OP_INSTALL, OP_RESTORE };
  assert_true(ops_in_order(&fx->plat, k_order, sizeof(k_order) / sizeof(k_order[0])));
  assert_false(op_seen(&fx->plat, OP_APPLY));
  assert_int_equal(r->outcome, AZ_IOT_SU_OUTCOME_FAILED);
  assert_int_equal(r->result_code, -(int32_t)AZ_IOT_SU_FACILITY_INSTALL);
  assert_int_not_equal(r->failure_origin, AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE);
  assert_true(r->step_count >= 1);
  assert_int_equal(r->step0_outcome, AZ_IOT_SU_OUTCOME_FAILED);
  assert_int_equal(r->result, AZ_IOT_OK);
}

/* An update the device already has is reported SKIPPED, with nothing fetched. */
static void already_installed_update_is_reported_skipped(void** state)
{
  (void)state;
  fixture* fx = &g_fx;
  fixture_options o
      = fixture_options_default(require_model("AZ_IOT_E2E_SU_OFFER_MODEL_ALREADY_INSTALLED"));
  o.already_installed = true;
  fixture_open(fx, &o);

  const spy_report* r = run_offered_workflow(fx);

  assert_true(op_seen(&fx->plat, OP_IS_INSTALLED));
  assert_false(op_seen(&fx->plat, OP_DOWNLOAD));
  assert_int_equal(r->outcome, AZ_IOT_SU_OUTCOME_SKIPPED);
  assert_int_equal(r->result, AZ_IOT_OK);
}

/* A manifest that does not verify is reported FAILED, with nothing fetched. */
static void untrusted_manifest_is_reported_failed(void** state)
{
  (void)state;
  fixture* fx = &g_fx;
  fixture_options o = fixture_options_default(require_model("AZ_IOT_E2E_SU_OFFER_MODEL_UNTRUSTED"));
  o.roots = g_wrong;
  fixture_open(fx, &o);

  const spy_report* r = run_offered_workflow(fx);

  assert_false(op_seen(&fx->plat, OP_DOWNLOAD));
  assert_int_equal(r->outcome, AZ_IOT_SU_OUTCOME_FAILED);
  /* The failure is the signature check, not anything else before download. */
  assert_int_equal(r->result_code, -(int32_t)AZ_IOT_SU_FACILITY_MANIFEST);
  assert_int_equal(r->result, AZ_IOT_OK);
}

/** @brief One downloaded file, as az_iot_su_verify_file_hash() reads it. */
typedef struct
{
  platform* plat;
  const az_iot_su_client_update_manifest_file* file;
  uint32_t index;
} downloaded_file;

static int32_t read_downloaded_chunk(
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* ctx)
{
  downloaded_file* d = (downloaded_file*)ctx;
  return hook_read_file(d->file, d->index, offset, buffer, buffer_size, out_read, d->plat);
}

static az_span find_file_url(
    const az_iot_su_client_update_request* request,
    const az_iot_su_client_update_manifest_file* file)
{
  for (uint32_t i = 0; i < request->file_urls_count; ++i)
  {
    if (az_span_is_content_equal(request->file_urls[i].id, file->id))
    {
      return request->file_urls[i].url;
    }
  }
  return AZ_SPAN_EMPTY;
}

/* Library mode: the verified offer is handed to the application, which
 * downloads and hashes the files itself and reports progress, then success. */
static void library_mode_hands_over_the_offer_and_reports_the_applications_result(void** state)
{
  (void)state;
  fixture* fx = &g_fx;
  fixture_options o
      = fixture_options_default(require_model("AZ_IOT_E2E_SU_OFFER_MODEL_LIBRARY_MODE"));
  o.library_mode = true;
  fixture_open(fx, &o);

  PUMP_UNTIL(
      fx,
      fx->updates_available > 0 || fx->abandoned > 0 || newest_sent_terminal_report(fx) != NULL,
      E2E_SU_WORKFLOW_BUDGET_S);
  assert_false(fx->faulted);
  assert_int_equal(fx->abandoned, 0);
  if (fx->spy.offers == 0)
  {
    fail_msg("no update was offered for model \"%s\"", fx->model);
  }
  assert_int_equal(fx->updates_available, 1);
  assert_int_equal(az_iot_su_client_get_state(&fx->su), AZ_IOT_SU_STATE_DELEGATED);
  assert_null(newest_sent_terminal_report(fx));

  const az_iot_su_client_update_request* request = fx->delegated_request;
  const az_iot_su_client_update_manifest* manifest = fx->delegated_manifest;
  assert_non_null(request);
  assert_non_null(manifest);
  assert_true(manifest->files_count >= 1 && manifest->files_count <= E2E_SU_MAX_FILES);
  for (uint32_t i = 0; i < manifest->files_count; ++i)
  {
    const az_iot_su_client_update_manifest_file* file = &manifest->files[i];
    assert_int_equal(
        hook_download(file, find_file_url(request, file), i, manifest->files_count, &fx->plat),
        AZ_IOT_SU_RESULT_SUCCESS);
    assert_true(fx->plat.downloaded_size[i] == file->size_in_bytes);
    downloaded_file d = { .plat = &fx->plat, .file = file, .index = i };
    assert_int_equal(
        az_iot_su_verify_file_hash(file, az_iot_crypto_openssl(), read_downloaded_chunk, &d),
        AZ_IOT_OK);
  }

  az_iot_su_step_result progress = {
    .outcome = AZ_IOT_SU_OUTCOME_IN_PROGRESS,
    .failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE,
    .result_details = AZ_SPAN_FROM_STR("downloaded"),
  };
  /* Waits out the SDK's own IN_PROGRESS verdict first: one report is in flight at a time. */
  PUMP_UNTIL(
      fx,
      fx->spy.report_count == 0 || fx->spy.reports[fx->spy.report_count - 1].has_verdict,
      E2E_SU_VERDICT_BUDGET_S);
  size_t before = fx->spy.report_count;
  assert_int_equal(az_iot_su_client_report_status(&fx->su, &progress, NULL, 0), AZ_IOT_OK);
  /* A retryable verdict leaves its attempt without one: the resend, newest, gets it. */
  PUMP_UNTIL(
      fx,
      fx->spy.report_count > before && fx->spy.reports[fx->spy.report_count - 1].has_verdict,
      E2E_SU_VERDICT_BUDGET_S);
  assert_false(fx->faulted);
  assert_true(fx->spy.report_count > before);
  const spy_report* reported = &fx->spy.reports[fx->spy.report_count - 1];
  assert_true(reported->has_verdict);
  assert_int_equal(reported->outcome, AZ_IOT_SU_OUTCOME_IN_PROGRESS);
  assert_int_equal(reported->result, AZ_IOT_OK);
  assert_int_equal(az_iot_su_client_get_state(&fx->su), AZ_IOT_SU_STATE_DELEGATED);

  /* The hand-off's request and manifest are invalid once the terminal report is in. */
  uint32_t step_count = manifest->instructions.steps_count;
  assert_true(step_count >= 1 && step_count <= E2E_SU_MAX_STEPS);
  az_iot_su_step_result steps[E2E_SU_MAX_STEPS];
  for (uint32_t i = 0; i < step_count; ++i)
  {
    steps[i] = (az_iot_su_step_result){
      .outcome = AZ_IOT_SU_OUTCOME_SUCCEEDED,
      .result_code = AZ_IOT_SU_AGENT_RESULT_CODE_SUCCESS,
      .failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE,
    };
  }
  az_iot_su_step_result success = {
    .outcome = AZ_IOT_SU_OUTCOME_SUCCEEDED,
    .result_code = AZ_IOT_SU_AGENT_RESULT_CODE_SUCCESS,
    .failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE,
  };
  assert_int_equal(
      az_iot_su_client_report_status(&fx->su, &success, steps, (int32_t)step_count), AZ_IOT_OK);
  fx->delegated_request = NULL;
  fx->delegated_manifest = NULL;
  assert_int_equal(az_iot_su_client_get_state(&fx->su), AZ_IOT_SU_STATE_IDLE);

  const spy_report* r = run_offered_workflow(fx);
  assert_int_equal(r->outcome, AZ_IOT_SU_OUTCOME_SUCCEEDED);
  assert_int_equal(r->result_code, 700);
  assert_int_equal(r->failure_origin, AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE);
  assert_int_equal(r->step_count, (int32_t)step_count);
  assert_int_equal(r->step0_outcome, AZ_IOT_SU_OUTCOME_SUCCEEDED);
  assert_int_equal(r->result, AZ_IOT_OK);
  assert_int_equal(fx->updates_available, 1);
}

/**
 * @brief Runs a delta offer with download handler @p handler registered: asserts what ran and the
 * terminal report, a succeeded one-step update with @p step0_extended_result_code.
 */
static void run_delta_offer(
    const char* model_variable,
    delta_handler_kind handler,
    bool has_delta_source,
    int32_t step0_extended_result_code)
{
  fixture* fx = &g_fx;
  fixture_options o = fixture_options_default(require_model(model_variable));
  o.delta_handler = handler;
  o.has_delta_source = has_delta_source;
  fixture_open(fx, &o);

  const spy_report* r = run_offered_workflow(fx);

  assert_int_equal(fx->plat.related_file_choices, 1);
  assert_int_equal(fx->plat.related_file_downloads, has_delta_source ? 1 : 0);
  assert_int_equal(fx->plat.builds_from_related_file, has_delta_source ? 1 : 0);
  assert_int_equal(fx->plat.full_downloads, has_delta_source ? 0 : 1);
  assert_int_equal(fx->plat.workflow_ends, 1);
  assert_int_equal(fx->plat.workflow_successes, 1);
  assert_true(fx->plat.downloaded_size[0] > 0);
  assert_true(fx->plat.downloaded_size[0] == fx->plat.expected_size[0]);
  assert_true(op_seen(&fx->plat, OP_INSTALL));
  assert_false(op_seen(&fx->plat, OP_RESTORE));

  assert_int_equal(r->outcome, AZ_IOT_SU_OUTCOME_SUCCEEDED);
  assert_int_equal(r->result_code, 700);
  assert_int_equal(r->failure_origin, AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE);
  assert_int_equal(r->step_count, 1);
  assert_int_equal(r->step0_outcome, AZ_IOT_SU_OUTCOME_SUCCEEDED);
  assert_int_equal(r->result, AZ_IOT_OK);
  const saved_report* sent = &fx->spy.last_terminal;
  assert_true(sent->valid);
  assert_int_equal(sent->steps[0].extended_result_code, step0_extended_result_code);
  /* One step: the update's code is the step's. */
  char expected[16];
  az_iot_su__format_extended_result_code(expected, sizeof(expected), step0_extended_result_code);
  assert_string_equal(sent->extended_result_codes, expected);
  assert_int_equal(az_iot_su_client_get_state(&fx->su), AZ_IOT_SU_STATE_IDLE);
}

/** @brief A device holding the source builds the file from its related file instead of downloading
 * it in full, and reports SUCCEEDED with no delta code. */
static void delta_update_is_built_from_the_related_file_and_reported(void** state)
{
  (void)state;
  run_delta_offer("AZ_IOT_E2E_SU_OFFER_MODEL_DELTA", DELTA_HANDLER_TEST, true, 0);
}

/** @brief A device without the source: the handler declines, the file is downloaded in full, and
 * the update is reported SUCCEEDED with the delta engine's no-source code. */
static void delta_update_without_source_falls_back_to_full_download(void** state)
{
  (void)state;
  run_delta_offer(
      "AZ_IOT_E2E_SU_OFFER_MODEL_DELTA_NO_SOURCE",
      DELTA_HANDLER_TEST,
      false,
      E2E_SU_DELTA_NO_SOURCE_CODE);
}

#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA

/**
 * @brief The reference microsoft/delta:1 handler, with the real delta processor, builds the file
 * (target.swu) from its related file (delta.bin) and the cached source (source.swu); the update is
 * reported SUCCEEDED with no delta code, and the file replaces the source in the cache.
 */
static void reference_delta_update_is_built_by_the_delta_processor_and_cached(void** state)
{
  (void)state;
  if (strlen(AZ_IOT_E2E_SU_DELTA_LIBRARY) == 0 || size_of_file(AZ_IOT_E2E_SU_DELTA_LIBRARY) < 0)
  {
    fail_msg(
        "no delta processor at \"%s\": configure with -DAZ_IOT_SU_DELTA_TEST_LIBRARY=<path of "
        "libadudiffapi.so> (eng/install-su-delta-processor.sh)",
        AZ_IOT_E2E_SU_DELTA_LIBRARY);
  }
  run_delta_offer("AZ_IOT_E2E_SU_OFFER_MODEL_MICROSOFT_DELTA", DELTA_HANDLER_REFERENCE, true, 0);

  /* The built file moved into the cache as the provider's one entry. */
  fixture* fx = &g_fx;
  char target_entry[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  char source_entry[AZ_IOT_SU_DELTA_MAX_PATH_SIZE];
  char built_path[E2E_SU_PATH_MAX];
  char hash[sizeof(g_reference_target_hash)];
  assert_int_equal(
      reference_delta_cache_path(fx, g_reference_target_hash, target_entry, sizeof(target_entry)),
      AZ_IOT_OK);
  assert_int_equal(
      reference_delta_cache_path(fx, g_reference_source_hash, source_entry, sizeof(source_entry)),
      AZ_IOT_OK);
  file_path(&fx->plat, 0, false, built_path, sizeof(built_path));
  assert_true(sha256_base64_of_file(target_entry, hash, sizeof(hash)));
  assert_string_equal(hash, g_reference_target_hash);
  assert_true(size_of_file(source_entry) < 0);
  assert_true(size_of_file(built_path) < 0);
}
#endif

/* --- main ---------------------------------------------------------------- */

static int group_setup(void** state)
{
  (void)state;
  int rc = e2e_su_env_load(&g_env);
  g_manufacturer = e2e_su_env_require("AZ_IOT_E2E_SU_OFFER_MANUFACTURER");
  g_model = e2e_su_env_require("AZ_IOT_E2E_SU_OFFER_MODEL");
  if (g_manufacturer == NULL || g_model == NULL)
  {
    rc = 1;
  }

  size_t ms_count = 0;
  size_t test_count = 0;
  const az_iot_su_root_key* ms = az_iot_su_microsoft_root_keys(&ms_count);
  const az_iot_su_root_key* test = e2e_su_test_roots(&test_count);
  if (ms_count + test_count > E2E_SU_MAX_ROOTS)
  {
    return 1;
  }
  memcpy(g_trusted, ms, ms_count * sizeof(*ms));
  memcpy(g_trusted + ms_count, test, test_count * sizeof(*test));
  g_trusted_count = ms_count + test_count;
  for (size_t i = 0; i < g_trusted_count; ++i)
  {
    g_wrong[i] = g_trusted[i];
    g_wrong[i].modulus = g_trusted[(i + 1) % g_trusted_count].modulus;
    g_wrong[i].modulus_len = g_trusted[(i + 1) % g_trusted_count].modulus_len;
  }

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK || !compute_delta_source_hash())
  {
    return 1;
  }
#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
  char source_path[E2E_SU_PATH_MAX];
  char target_path[E2E_SU_PATH_MAX];
  reference_delta_vector_path("source.swu", source_path, sizeof(source_path));
  reference_delta_vector_path("target.swu", target_path, sizeof(target_path));
  if (!sha256_base64_of_file(source_path, g_reference_source_hash, sizeof(g_reference_source_hash))
      || !sha256_base64_of_file(
          target_path, g_reference_target_hash, sizeof(g_reference_target_hash)))
  {
    return 1;
  }
#endif
  return rc;
}

/* Runs after every scenario, including one that failed an assertion, so a
 * live connection never outlives it. */
static int scenario_teardown(void** state)
{
  (void)state;
  fixture_close(&g_fx);
  free(g_scenario_model);
  g_scenario_model = NULL;
  return 0;
}

static int group_teardown(void** state)
{
  (void)state;
  curl_global_cleanup();
  free(g_manufacturer);
  free(g_model);
  e2e_su_env_free(&g_env);
  return 0;
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_WARN);
  az_iot_log_set_global_sink(&log);

  /* Order matters: the second and third scenarios build on the first one's
   * terminal report. */
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(
        real_update_is_downloaded_verified_installed_and_reported, scenario_teardown),
    cmocka_unit_test_teardown(
        terminal_report_is_idempotent_and_a_conflict_is_detected, scenario_teardown),
    cmocka_unit_test_teardown(incompatible_device_is_offered_nothing, scenario_teardown),
    cmocka_unit_test_teardown(
        install_failure_is_rolled_back_and_reported_failed, scenario_teardown),
    cmocka_unit_test_teardown(already_installed_update_is_reported_skipped, scenario_teardown),
    cmocka_unit_test_teardown(untrusted_manifest_is_reported_failed, scenario_teardown),
    cmocka_unit_test_teardown(
        library_mode_hands_over_the_offer_and_reports_the_applications_result, scenario_teardown),
    cmocka_unit_test_teardown(
        delta_update_is_built_from_the_related_file_and_reported, scenario_teardown),
    cmocka_unit_test_teardown(
        delta_update_without_source_falls_back_to_full_download, scenario_teardown),
#ifdef AZ_IOT_E2E_SU_WITH_REFERENCE_DELTA
    cmocka_unit_test_teardown(
        reference_delta_update_is_built_by_the_delta_processor_and_cached, scenario_teardown),
#endif
  };
  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
