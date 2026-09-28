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
 * DPS device-update channel, the Paho adapter, X.509 auth and the OpenSSL
 * crypto adapter. Files are downloaded for real (libcurl) and hashed by the
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
 *
 * A missing variable fails the scenario that needs it.
 *
 * E2E-PLACEHOLDER: scenarios not written yet, each blocked on the client:
 *   - operational route (az_iot_su_client_request_update() after registration)
 *   - multi-step update (reports with more than one step are unmeasured)
 *   - reboot and resume() mid-workflow (reboot-safe reporting)
 */

#define _POSIX_C_SOURCE 200809L

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

#include <cmocka.h>

#include <curl/curl.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_certificate_provider_pem.h"
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_su.h"

#include "internal/connection_client_internal.h"

#include "su_internal.h"

#include "az_iot_su_crypto_openssl.h"

#include "e2e_su_env.h"
#include "e2e_su_test_roots.h"

/* --- budgets ------------------------------------------------------------- */

#define E2E_SU_SESSION_BUDGET_S 60 /* provisioning session up */
#define E2E_SU_FETCH_BUDGET_S 90 /* update check answered */
#define E2E_SU_WORKFLOW_BUDGET_S 300 /* offer -> terminal report acknowledged */
#define E2E_SU_VERDICT_BUDGET_S 60 /* one report acknowledged */
#define E2E_SU_REQUEST_TIMEOUT_MS 60000u
#define E2E_SU_DOWNLOAD_TIMEOUT_S 120L

#define E2E_SU_MAX_FILES _az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT
#define E2E_SU_MAX_STEPS _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS

/* --- run-wide state ------------------------------------------------------ */

static e2e_su_env g_env;
static const char* g_manufacturer;
static const char* g_model;

/* Microsoft production roots plus the test roots. */
#define E2E_SU_MAX_ROOTS 8
static az_iot_su_root_key g_trusted[E2E_SU_MAX_ROOTS];
static size_t g_trusted_count;
/* Same kids, each bound to another key's modulus: every signature fails. */
static az_iot_su_root_key g_wrong[E2E_SU_MAX_ROOTS];

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

/** @brief Platform-hook state: a scratch directory, an op log and injected faults. */
typedef struct
{
  char dir[64];
  op_kind ops[E2E_SU_MAX_OPS];
  size_t op_count;

  int32_t fail_install_step; /* -1: never */
  bool already_installed;

  int64_t expected_size[E2E_SU_MAX_FILES];
  int64_t downloaded_size[E2E_SU_MAX_FILES];
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

static void file_path(const platform* p, uint32_t index, char* out, size_t cap)
{
  snprintf(out, cap, "%s/f%u", p->dir, (unsigned)index);
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

  char path[96];
  file_path(p, file_index, path, sizeof(path));
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
  p->expected_size[file_index] = file->size_in_bytes;
  p->downloaded_size[file_index] = size;
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
  (void)file;
  platform* p = (platform*)ctx;
  if (offset == 0)
  {
    op_log(p, OP_READ);
  }
  char path[96];
  file_path(p, file_index, path, sizeof(path));
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
  char workflow_id[AZ_IOT_SU_MAX_WORKFLOW_ID_LEN];
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
      || action == AZ_IOT_SU_ERROR_ACTION_ALREADY_REPORTED;
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
  az_iot_su_channel_dps channel_state;
  spy_channel spy;
  az_iot_su_client su;
  platform plat;
  AZ_IOT_SU_DEVICE_PROPERTIES_STORAGE(dp_buf);
  const char* model;

  /* What fixture_open() got through, so teardown releases exactly that. */
  bool have_dir;
  bool have_certs;
  bool have_conn;
  bool have_su;

  bool faulted;
  size_t abandoned;
  az_iot_su_operation abandoned_op;
  az_iot_result abandoned_reason;
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
}

typedef struct
{
  const char* manufacturer;
  const char* model;
  const az_iot_su_root_key* roots;
  size_t root_count;
  bool request_onboarding_update;
  int32_t fail_install_step;
  bool already_installed;
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
  };
}

static void fixture_open(fixture* fx, const fixture_options* o)
{
  memset(fx, 0, sizeof(*fx));
  fx->model = o->model;
  snprintf(fx->plat.dir, sizeof(fx->plat.dir), "/tmp/az_iot_e2e_su_XXXXXX");
  assert_non_null(mkdtemp(fx->plat.dir));
  fx->have_dir = true;
  fx->plat.fail_install_step = o->fail_install_step;
  fx->plat.already_installed = o->already_installed;

  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  e2e_su_env_apply(&g_env, &opts, &pem);
  /* Every operation here runs on the provisioning session; no hub is needed. */
  opts.dps.provision_only = true;
  assert_int_equal(az_iot_certificate_provider_pem_init(&fx->certs, &pem), AZ_IOT_OK);
  fx->have_certs = true;
  opts.certificate_provider = &fx->certs.base;
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);
  fx->have_conn = true;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->conn, on_conn_state, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(
          &fx->conn, az_iot_paho_factory_create_v3_1_1()),
      AZ_IOT_OK);

  az_iot_su_device_properties dp = { 0 };
  dp.manufacturer = o->manufacturer;
  dp.model = o->model;
  dp.installed_update_id.provider = "e2e";
  dp.installed_update_id.name = "e2e";
  dp.installed_update_id.version = "0.0.0";
  assert_int_equal(
      az_iot_su_channel_dps_init(&fx->channel_state, &fx->conn, &dp, &fx->spy.inner), AZ_IOT_OK);

  az_iot_su_platform_hooks hooks = {
    .download_fn = hook_download,
    .read_file_fn = hook_read_file,
    .install_fn = hook_install,
    .apply_fn = hook_apply,
    .backup_fn = hook_backup,
    .restore_fn = hook_restore,
    .is_installed_fn = hook_is_installed,
    .persist_state_fn = hook_persist,
    .load_state_fn = hook_load,
    .user_ctx = &fx->plat,
  };
  az_iot_su_crypto_hooks crypto = az_iot_su_crypto_openssl_hooks();

  az_iot_su_client_config_options su = az_iot_su_client_config_options_default();
  su.hooks = &hooks;
  su.crypto = &crypto;
  su.root_keys = o->roots;
  su.root_key_count = o->root_count;
  su.device_properties = &dp;
  su.device_properties_buffer = fx->dp_buf;
  su.device_properties_buffer_size = sizeof(fx->dp_buf);
  az_iot_su_channel spy = { .vtable = &k_spy_vtable, .ctx = &fx->spy };
  assert_int_equal(az_iot_su_client__initialize_with_channel(&fx->su, &spy, &su), AZ_IOT_OK);
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
  if (fx->have_conn)
  {
    az_iot_connection_client_deinit(&fx->conn);
    fx->have_conn = false;
  }
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
    char path[96];
    file_path(&fx->plat, i, path, sizeof(path));
    (void)remove(path);
  }
  (void)remove(fx->plat.dir);
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

/** @brief The acknowledged terminal report, or NULL. */
static const spy_report* terminal_report(const fixture* fx)
{
  for (size_t i = fx->spy.report_count; i > 0; --i)
  {
    const spy_report* r = &fx->spy.reports[i - 1];
    if (r->outcome != AZ_IOT_SU_OUTCOME_IN_PROGRESS)
    {
      return r->has_verdict ? r : NULL;
    }
  }
  return NULL;
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
  const char* v = e2e_su_env_require(name);
  if (v == NULL)
  {
    fail_msg("%s is not set: stage an offer for this scenario", name);
  }
  return v;
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

/* Reporting is idempotent on workflowId, and a different terminal result for
 * the same workflow is a conflict the client reads as "already reported". */
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
  assert_int_equal(r->action, AZ_IOT_SU_ERROR_ACTION_ALREADY_REPORTED);
}

/* The service keeps offering a workflow after its terminal report. A client
 * with no record of it runs it again, and the identical report is accepted. */
static void workflow_is_offered_again_after_its_terminal_report(void** state)
{
  (void)state;
  if (!g_real_report.valid)
  {
    fail_msg("needs the terminal report of "
             "real_update_is_downloaded_verified_installed_and_reported");
  }
  fixture* fx = &g_fx;
  fixture_options o = fixture_options_default(g_model);
  fixture_open(fx, &o);

  const spy_report* r = run_offered_workflow(fx);

  assert_string_equal(fx->spy.last_terminal.workflow_id, g_real_report.workflow_id);
  assert_int_equal(r->outcome, AZ_IOT_SU_OUTCOME_SUCCEEDED);
  assert_int_equal(r->result, AZ_IOT_OK);
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
  assert_int_equal(r->result_code, 700 - (int32_t)AZ_IOT_SU_FACILITY_INSTALL);
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
  assert_int_equal(r->result_code, 700 - (int32_t)AZ_IOT_SU_FACILITY_MANIFEST);
  assert_int_equal(r->result, AZ_IOT_OK);
}

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

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
  {
    return 1;
  }
  return rc;
}

/* Runs after every scenario, including one that failed an assertion, so a
 * live connection never outlives it. */
static int scenario_teardown(void** state)
{
  (void)state;
  fixture_close(&g_fx);
  return 0;
}

static int group_teardown(void** state)
{
  (void)state;
  curl_global_cleanup();
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
    cmocka_unit_test_teardown(
        workflow_is_offered_again_after_its_terminal_report, scenario_teardown),
    cmocka_unit_test_teardown(incompatible_device_is_offered_nothing, scenario_teardown),
    cmocka_unit_test_teardown(
        install_failure_is_rolled_back_and_reported_failed, scenario_teardown),
    cmocka_unit_test_teardown(already_installed_update_is_reported_skipped, scenario_teardown),
    cmocka_unit_test_teardown(untrusted_manifest_is_reported_failed, scenario_teardown),
  };
  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
