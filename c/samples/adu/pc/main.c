// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* adu/pc - Azure Device Update (ADU) on-device workflow sample.
 *
 * Portable PC sample (Linux + Windows). Runs the ENTIRE ADU workflow end to end
 * against a real Device Update instance, but with SIMULATED download/install
 * hooks so it is safe to run on a dev box (it never touches real firmware).
 * See the companion README.md and docs/azure-device-update.md.
 *
 * NO IOT HUB IS REQUIRED. Every device-update operation runs on the
 * provisioning session, before the device registers, so the sample declares
 * dps.provision_only: keep that session up, never register, never connect to a
 * hub. Set AZ_IOT_ADU_REGISTER_WITH_HUB=1 for a device that should also
 * register and use its assigned hub; the update workflow is identical either
 * way.
 *
 * Real:      connection, update request/response, manifest receipt, JWS
 *            verification (OpenSSL), per-file SHA-256 hash check, status
 *            reporting.
 * Simulated: download_fn (synthesizes deterministic payload bytes),
 *            install/apply/backup/restore (log only, optional forced failure
 *            or reboot), persist/load (a temp file so resume() works).
 *
 * Device identity (environment variables, all optional).
 *
 * MATCHED: manufacturer and model are the COMPATIBILITY PROPERTIES the
 * service matches an update against. If they do not match the imported
 * update, the device is answered "nothing to do" and is never offered
 * anything, so the sample prints what it reported.
 *   AZ_IOT_ADU_MANUFACTURER=<s>        default "Contoso"
 *   AZ_IOT_ADU_MODEL=<s>               default "ADU-Sim"
 *
 * REPORTED, NOT MATCHED: the installed update id says what is on the device
 * now. It takes no part in matching, and the onboarding route this sample uses
 * omits it entirely -- changing it cannot make an update eligible.
 *   AZ_IOT_ADU_INSTALLED_PROVIDER=<s>  default "Contoso"
 *   AZ_IOT_ADU_INSTALLED_NAME=<s>      default "ADU-Sim"
 *   AZ_IOT_ADU_INSTALLED_VERSION=<s>   default "1.0.0"
 *
 * Other knobs (environment variables, all optional):
 *   AZ_IOT_ADU_REGISTER_WITH_HUB=1  register and connect to the assigned hub
 *   AZ_IOT_ADU_LOG_LEVEL=<lvl>      trace|debug|info|warn|error|off (default
 *                                   info). The SDK's "adu:" and "dps:"
 *                                   protocol lines are emitted at debug.
 *   ADU_SIM_FAIL_STEP=<n>     force install_fn to fail at 1-based step n
 *   ADU_SIM_HASH_MISMATCH=1   corrupt the synthesized payload (hash failure)
 *   ADU_SIM_REBOOT=1          install returns REBOOT_REQUIRED; state is
 *                             persisted and the sample exits. Re-run (without
 *                             this knob) to resume() and finish the workflow.
 *   ADU_SIM_DELAY_MS=<ms>     per-download delay so progress is observable
 *   ADU_SIM_STATE_FILE=<path> resume blob path (default ./adu_sim_state.blob)
 */
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_adu.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "az_iot_adu_crypto_openssl.h"

#include "sample_utils.h"

/* ------------------------------------------------------------------------- */
/* root keys                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * ADU anchors manifest trust in one or more RSA root public keys. This sample
 * uses az_iot_adu_microsoft_root_keys() — Microsoft's published ADU production
 * roots, compiled into the SDK — so manifests signed under those roots
 * verify out of the box. Other issuers require explicitly trusted keys.
 *
 * To accept updates signed by your OWN root instead, build your own
 * az_iot_adu_root_key array (kid + big-endian modulus/exponent) and pass it
 * to az_iot_adu_client_initialize() in place of the Microsoft keys.
 */

/* ------------------------------------------------------------------------- */
/* simulation context + hooks                                                */
/* ------------------------------------------------------------------------- */

typedef struct
{
  int fail_step; /* 1-based; 0 = never fail */
  int hash_mismatch; /* corrupt synthesized payload */
  int reboot; /* install returns REBOOT_REQUIRED once */
  long delay_ms; /* per-download delay */
  char* state_file; /* owned */

  int reboot_signalled; /* set by install_fn when it returns REBOOT_REQUIRED */
} sim_ctx;

/* Environment readers built on sample_env_dup(), the helper every sample
 * shares -- which also settles the portability question, since MSVC rejects
 * plain getenv() under /WX. These add only the parsing the knobs below need. */

/* True when @p name is set to anything other than "0". */
static int sample_env_flag(const char* name)
{
  char* v = sample_env_dup(name, NULL);
  int on = (v != NULL && strcmp(v, "0") != 0);
  free(v);
  return on;
}

/* @p name as a decimal number, or @p fallback when unset or not one. */
static long sample_env_long(const char* name, long fallback)
{
  char* v = sample_env_dup(name, NULL);
  long out = fallback;
  if (v != NULL)
  {
    char* end = NULL;
    errno = 0;
    long parsed = strtol(v, &end, 10);
    if (errno == 0 && end != v && *end == '\0')
    {
      out = parsed;
    }
    free(v);
  }
  return out;
}

static int32_t sim_download(
    const az_iot_adu_client_update_manifest_file* file,
    az_span url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx)
{
  (void)url;
  sim_ctx* s = (sim_ctx*)user_ctx;
  printf(
      "  [download] file %u/%u (%lld bytes) [simulated]\n",
      file_index + 1,
      file_count,
      (long long)file->size_in_bytes);
  sample_sleep_ms(s->delay_ms);
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

/* Serve deterministic payload bytes so the SHA-256 the core computes is
 * reproducible. To make the REAL hash check pass against a deployment, the
 * imported payload must be byte-identical (see README: zero-filled file). */
static int32_t sim_read_file(
    const az_iot_adu_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* user_ctx)
{
  (void)file_index;
  sim_ctx* s = (sim_ctx*)user_ctx;
  size_t size = (file->size_in_bytes > 0) ? (size_t)file->size_in_bytes : 0;
  if (offset >= size)
  {
    *out_read = 0; /* EOF */
    return AZ_IOT_ADU_RESULT_SUCCESS;
  }
  size_t remain = size - offset;
  size_t n = remain < buffer_size ? remain : buffer_size;
  memset(buffer, 0x00, n);
  if (s->hash_mismatch && offset == 0 && n > 0)
  {
    buffer[0] = 0xFF; /* corrupt the first byte -> hash verification fails */
  }
  *out_read = n;
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t sim_is_installed(const az_iot_adu_client_update_manifest* manifest, void* user_ctx)
{
  (void)manifest;
  (void)user_ctx;
  /* Always report "not installed" so the deployment proceeds. */
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t sim_backup(
    const az_iot_adu_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  (void)user_ctx;
  printf("  [backup]  step %u [simulated]\n", step);
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t sim_install(
    const az_iot_adu_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  sim_ctx* s = (sim_ctx*)user_ctx;
  if (s->fail_step > 0 && (uint32_t)(s->fail_step - 1) == step)
  {
    printf("  [install] step %u -> FORCED FAILURE (ADU_SIM_FAIL_STEP)\n", step);
    return AZ_IOT_ADU_RESULT_FAILURE;
  }
  if (s->reboot && !s->reboot_signalled)
  {
    s->reboot_signalled = 1;
    printf("  [install] step %u -> REBOOT_REQUIRED (ADU_SIM_REBOOT)\n", step);
    return AZ_IOT_ADU_RESULT_REBOOT_REQUIRED;
  }
  printf("  [install] step %u [simulated]\n", step);
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t sim_apply(
    const az_iot_adu_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  (void)user_ctx;
  printf("  [apply]   step %u [simulated]\n", step);
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t sim_restore(
    const az_iot_adu_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  (void)user_ctx;
  printf("  [restore] step %u (rollback) [simulated]\n", step);
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t sim_persist(const uint8_t* blob, size_t len, void* user_ctx)
{
  sim_ctx* s = (sim_ctx*)user_ctx;
  if (len == 0)
  {
    /* Invalidation: remove the file so a later run finds no checkpoint. */
    if (remove(s->state_file) != 0 && errno != ENOENT)
    {
      return AZ_IOT_ADU_RESULT_FAILURE;
    }
    printf("  [persist] cleared %s\n", s->state_file);
    return AZ_IOT_ADU_RESULT_SUCCESS;
  }
  FILE* f = fopen(s->state_file, "wb");
  if (f == NULL)
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }
  size_t w = fwrite(blob, 1, len, f);
  fclose(f);
  if (w != len)
  {
    return AZ_IOT_ADU_RESULT_FAILURE;
  }
  printf("  [persist] %zu bytes -> %s\n", len, s->state_file);
  return AZ_IOT_ADU_RESULT_SUCCESS;
}

static int32_t sim_load(uint8_t* blob, size_t cap, size_t* out_len, void* user_ctx)
{
  sim_ctx* s = (sim_ctx*)user_ctx;
  FILE* f = fopen(s->state_file, "rb");
  if (f == NULL)
  {
    return 1; /* nothing persisted */
  }
  size_t r = fread(blob, 1, cap, f);
  int eof = feof(f);
  fclose(f);
  if (!eof)
  {
    return 1; /* blob did not fit in cap -> treat as no state */
  }
  *out_len = r;
  return 0;
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Indexed by az_iot_adu_state / az_iot_adu_operation. */
static const char* const k_adu_state_names[] = {
  "Idle",           "ManifestReceived", "VerifyingManifest", "DownloadStarted", "DownloadComplete",
  "BackupStarted",  "BackupComplete",   "InstallStarted",    "InstallComplete", "ApplyStarted",
  "RestoreStarted", "Failed",
};
static const char* const k_adu_operation_names[] = {
  "onboarding update check",
  "update check",
  "status report",
};

#define SAMPLE_NAME_OF(table, i) \
  (((size_t)(i) < sizeof(table) / sizeof((table)[0])) ? (table)[(size_t)(i)] : "?")

/* True when the event the SDK stamped is long enough to carry @p field.
 *
 * Events grow by APPENDING, so the test is against the last field this code
 * actually reads -- not sizeof(the whole struct), which would reject a usable
 * event from any library older than the newest field. */
#define SAMPLE_EVENT_HAS(ev, type, field) \
  ((ev)->_internal_size >= offsetof(type, field) + sizeof((ev)->field))

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo)
{
  (void)signo;
  g_stop = 1;
}

/* What the observers tell the pump loop. The SDK reports everything this
 * sample reacts to, so nothing here is polled or inferred. */
typedef struct
{
  az_iot_connection_state conn[AZ_IOT_CONN_SCOPE_COUNT];
  int faulted; /* a lifecycle settled at FAULTED: unrecoverable here */
  int check_abandoned; /* the update check gave up; see on_adu_event */
  int workflow_active; /* a deployment is in flight */
  int workflow_completed; /* one finished and returned to Idle */
  int announced; /* the "ready" line has been printed */
} sample_run;

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_adu_client_t adu_client;
  sim_ctx sim;
  sample_run run;
  /* Sized by the SDK's own default so raising it really does grow this cache;
   * the values it holds come from the environment. */
  AZ_IOT_ADU_DEVICE_PROPERTIES_STORAGE(dp_buffer);

  /* Device identity, kept past initialize() only so it can be printed. */
  char* manufacturer;
  char* model;
  char* installed_provider;
  char* installed_name;
  char* installed_version;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  az_iot_adu_client_destroy(&s->adu_client);

  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);

  free(s->sim.state_file);
  free(s->manufacturer);
  free(s->model);
  free(s->installed_provider);
  free(s->installed_name);
  free(s->installed_version);
}

/* Both lifecycles report here. `state` is meaningless without `scope`: a
 * provisioning session that is up says nothing about a hub, and on a
 * provision_only device the hub scope stays IDLE for good. */
static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_run* run = (sample_run*)user_ctx;

  /* `reason` is the last field read here, and `scope` indexes an array -- so an
   * event too short to carry them, or naming a scope this build does not know,
   * is ignored rather than read. */
  if (!SAMPLE_EVENT_HAS(event, az_iot_connection_state_event, reason)
      || (unsigned)event->scope >= AZ_IOT_CONN_SCOPE_COUNT)
  {
    return;
  }

  az_iot_connection_scope scope = event->scope;

  if (run->conn[scope] != event->state)
  {
    printf(
        "%s: %s -> %s (%s)\n",
        (scope == AZ_IOT_CONN_SCOPE_DPS) ? "Provisioning" : "Hub",
        sample_connection_state_name(run->conn[scope]),
        sample_connection_state_name(event->state),
        az_iot_result_to_string(event->reason));
    run->conn[scope] = event->state;
  }

  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    run->faulted = 1;
  }
}

static void on_adu_event(const az_iot_adu_event* event, void* user_ctx)
{
  sample_run* run = (sample_run*)user_ctx;

  /* `service_error` is the last field read here. */
  if (!SAMPLE_EVENT_HAS(event, az_iot_adu_event, service_error))
  {
    return;
  }

  switch (event->kind)
  {
    case AZ_IOT_ADU_EVENT_WORKFLOW_STATE_CHANGED:
      printf(
          "Update workflow: %s -> %s\n",
          SAMPLE_NAME_OF(k_adu_state_names, event->previous_state),
          SAMPLE_NAME_OF(k_adu_state_names, event->state));
      run->workflow_active = (event->state != AZ_IOT_ADU_STATE_IDLE);
      if (!run->workflow_active && event->previous_state != AZ_IOT_ADU_STATE_IDLE)
      {
        run->workflow_completed = 1;
      }
      break;

    case AZ_IOT_ADU_EVENT_OPERATION_ABANDONED:
      fprintf(
          stderr,
          "%s abandoned: %s. Service said: code=%d message=\"%s\" trackingId=\"%s\"\n",
          SAMPLE_NAME_OF(k_adu_operation_names, event->operation),
          az_iot_result_to_string(event->reason),
          (int)event->service_error.code,
          event->service_error.message,
          event->service_error.tracking_id);
      /* A status report is diagnostic: there is no public call to reissue one
       * and the client reports again at its next reporting point, so it is
       * logged and nothing more. A fetch giving up is what ends this sample --
       * the device asked and will not be told. */
      if (event->operation != AZ_IOT_ADU_OP_REPORT_STATUS)
      {
        run->check_abandoned = 1;
      }
      break;
  }
}

/* trace|debug|info|warn|error|off, in az_iot_log_level order. The default stays
 * INFO: the SDK's "adu:" and "dps:" protocol lines are DEBUG and would bury the
 * sample's own output. */
static az_iot_log_level sample_log_level_from_env(void)
{
  static const char* const k_names[] = { "trace", "debug", "info", "warn", "error", "off" };
  az_iot_log_level out = AZ_IOT_LOG_LEVEL_INFO;
  char* v = sample_env_dup("AZ_IOT_ADU_LOG_LEVEL", NULL);
  for (size_t i = 0; v != NULL && i < sizeof(k_names) / sizeof(k_names[0]); ++i)
  {
    if (strcmp(v, k_names[i]) == 0)
    {
      out = (az_iot_log_level)i;
      break;
    }
  }
  free(v);
  return out;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(sample_log_level_from_env());
  az_iot_log_set_global_sink(&log);

  signal(SIGINT, on_sigint);

  sample_state st = { 0 };

  /* Simulation knobs from the environment. */
  st.sim.fail_step = (int)sample_env_long("ADU_SIM_FAIL_STEP", 0);
  st.sim.hash_mismatch = sample_env_flag("ADU_SIM_HASH_MISMATCH");
  st.sim.reboot = sample_env_flag("ADU_SIM_REBOOT");
  st.sim.delay_ms = sample_env_long("ADU_SIM_DELAY_MS", 0);
  st.sim.state_file = sample_env_dup("ADU_SIM_STATE_FILE", "./adu_sim_state.blob");
  if (st.sim.state_file == NULL)
  {
    return 1;
  }

  if (sample_config_load(&st.config) != 0)
  {
    return 1;
  }

  int rc = 1;

  /* Certificate provider. */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = st.config.ca;
  pem.client_cert_pem_path = st.config.cert;
  pem.client_key_pem_path = st.config.key;
  if (az_iot_certificate_provider_pem_init(&st.certs, &pem) != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }

  /* Connection client. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &st.config);
  copts.certificate_provider = &st.certs.base;
  /* Every device-update operation runs on the provisioning session, before the
   * device registers, so by default this device declares it has no hub: the
   * session is kept up and pumped, registration never runs, and the hub scope
   * stays IDLE. Declared rather than inferred -- a hubless enrollment and a
   * misconfigured one both fail registration the same way, so inferring it
   * would hide real misconfiguration. */
  copts.dps.provision_only = !sample_env_flag("AZ_IOT_ADU_REGISTER_WITH_HUB");
  /* Reconnect with backoff + jitter so a long-running device rides out
   * transient drops. initial_delay_ms > 0 is what arms it. */
  copts.reconnection_policy.initial_delay_ms = 2000; /* first retry after 2s */
  copts.reconnection_policy.max_delay_ms = 60000; /* cap backoff at 60s */
  copts.reconnection_policy.max_attempts = 0; /* 0 = retry forever */
  copts.reconnection_policy.jitter_pct = 20; /* +/-20% jitter */
  if (az_iot_connection_client_init(&st.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }
  if (az_iot_connection_client_add_state_observer(&st.connection_client, on_conn_state, &st.run)
      != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }

  if (az_iot_connection_client_register_mqtt_factory(
          &st.connection_client, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(
             &st.connection_client, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }

  /* ADU client. */
  az_iot_adu_platform_hooks hooks = { 0 };
  hooks.download_fn = sim_download;
  hooks.read_file_fn = sim_read_file;
  hooks.install_fn = sim_install;
  hooks.apply_fn = sim_apply;
  hooks.backup_fn = sim_backup;
  hooks.restore_fn = sim_restore;
  hooks.is_installed_fn = sim_is_installed;
  hooks.persist_state_fn = sim_persist;
  hooks.load_state_fn = sim_load;
  hooks.user_ctx = &st.sim;

  az_iot_adu_crypto_hooks crypto = az_iot_adu_crypto_openssl_hooks();

  /* Microsoft's compiled-in ADU production root keys: anchors the trust chain
   * for updates signed by the real Device Update service. */
  size_t root_key_count = 0;
  const az_iot_adu_root_key* root_keys = az_iot_adu_microsoft_root_keys(&root_key_count);

  /* Compatibility properties: what the service matches a deployed update
   * against. Configurable because a hard-coded value that does not match the
   * imported update is answered "nothing to do" -- indistinguishable from
   * "nothing deployed" unless the device says what it asked with. */
  st.manufacturer = sample_env_dup("AZ_IOT_ADU_MANUFACTURER", "Contoso");
  st.model = sample_env_dup("AZ_IOT_ADU_MODEL", "ADU-Sim");
  st.installed_provider = sample_env_dup("AZ_IOT_ADU_INSTALLED_PROVIDER", "Contoso");
  st.installed_name = sample_env_dup("AZ_IOT_ADU_INSTALLED_NAME", "ADU-Sim");
  st.installed_version = sample_env_dup("AZ_IOT_ADU_INSTALLED_VERSION", "1.0.0");
  if (st.manufacturer == NULL || st.model == NULL || st.installed_provider == NULL
      || st.installed_name == NULL || st.installed_version == NULL)
  {
    sample_state_destroy(&st);
    return 1;
  }

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = st.manufacturer;
  dp.model = st.model;
  dp.installed_update_id.provider = st.installed_provider;
  dp.installed_update_id.name = st.installed_name;
  dp.installed_update_id.version = st.installed_version;

  /* Named, rather than a bare error out of initialize(): the cache is fixed
   * and these values now come from the environment. */
  size_t dp_needed = az_iot_adu_device_properties_buffer_size(&dp);
  if (dp_needed == 0)
  {
    fprintf(
        stderr,
        "Device properties are invalid or too long. Need 1-%d compatibility "
        "properties (nonempty, unique names; non-NULL values, empty allowed), and "
        "an installed update id that is unset or has nonempty provider, name and "
        "version.\n",
        AZ_IOT_ADU_MAX_COMPATIBILITY_PROPERTIES);
    sample_state_destroy(&st);
    return 1;
  }
  if (dp_needed > sizeof(st.dp_buffer))
  {
    fprintf(
        stderr,
        "Device properties need %zu bytes of cache; this sample has %zu. Shorten "
        "them or raise AZ_IOT_ADU_DEVICE_PROPERTIES_BUFFER_SIZE.\n",
        dp_needed,
        sizeof(st.dp_buffer));
    sample_state_destroy(&st);
    return 1;
  }

  az_iot_adu_client_config_options adu_opts = az_iot_adu_client_config_options_default();
  adu_opts.hooks = &hooks;
  adu_opts.crypto = &crypto;
  adu_opts.root_keys = root_keys;
  adu_opts.root_key_count = root_key_count;
  adu_opts.device_properties = &dp;
  adu_opts.device_properties_buffer = st.dp_buffer;
  adu_opts.device_properties_buffer_size = sizeof(st.dp_buffer);
  if (az_iot_adu_client_initialize(&st.adu_client, &st.connection_client, &adu_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "az_iot_adu_client_initialize failed\n");
    sample_state_destroy(&st);
    return 1;
  }

  /* Registered before resume(), which replays a persisted workflow state
   * through this same observer. */
  if (az_iot_adu_client_add_observer(&st.adu_client, on_adu_event, &st.run) != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }

  /* Resume any workflow persisted before a (simulated) reboot. A restored
   * state is reported through the observer registered above, which resume()
   * replays into; nothing to resume is not an error. */
  if (az_iot_adu_client_resume(&st.adu_client) != AZ_IOT_OK)
  {
    fprintf(stderr, "Could not resume the persisted workflow; starting from Idle.\n");
  }

  /* Ask for a day-0 onboarding update. Nothing is fetched unless the
   * application asks: only it knows whether it has a device record yet, and
   * the onboarding route is the one that needs none. A device that had already
   * provisioned would call az_iot_adu_client_request_update() instead.
   *
   * The timeout bounds how long the CLIENT keeps reissuing this check before
   * giving up and raising AZ_IOT_ADU_EVENT_OPERATION_ABANDONED with
   * AZ_IOT_ERR_TIMEOUT. Without it an unservable check -- no device record, no
   * linked hub -- is retried on every do_work() for the life of the client,
   * and looks exactly like "no update available".
   *
   * AZ_IOT_ADU_REQUEST_DEFAULT_TIMEOUT_MS is the default for an application
   * with no policy of its own. Pass your own value when you have one, or
   * AZ_IOT_ADU_REQUEST_NO_TIMEOUT to keep retrying indefinitely. */
  if (az_iot_adu_client_request_onboarding_update(
          &st.adu_client, AZ_IOT_ADU_REQUEST_DEFAULT_TIMEOUT_MS)
      != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }

  printf(
      "Matched against a deployed update: manufacturer=%s model=%s\n"
      "Reported only (not matched, and omitted on the onboarding route): "
      "installedUpdateId=%s/%s/%s\n",
      st.manufacturer,
      st.model,
      st.installed_provider,
      st.installed_name,
      st.installed_version);

  /* The application always opens the connection its feature clients use. On a
   * provision_only device this brings the provisioning session up and stops
   * there; that session IS the connection. */
  if (az_iot_connection_client_open(&st.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }

  /* Which lifecycle means "ready": the provisioning session on a hubless
   * device, the hub when one was asked for. */
  const az_iot_connection_scope ready_scope
      = copts.dps.provision_only ? AZ_IOT_CONN_SCOPE_DPS : AZ_IOT_CONN_SCOPE_HUB;

  /* One loop, from the moment open() returns. Nothing waits for a hub: the
   * update check runs on the provisioning session, so gating it on the hub
   * scope would leave it unissued on a device that has no hub. Both clients
   * are pumped -- the connection delivers, the ADU client advances -- and
   * everything this loop reacts to arrives through the two observers. */
  rc = 0; /* interrupted or asked to reboot is a normal exit */
  while (!g_stop)
  {
    (void)az_iot_connection_client_do_work(&st.connection_client, 50);
    (void)az_iot_adu_client_do_work(&st.adu_client);

    /* Says only which lifecycle came up, never whether the update check has
     * been answered. In hub mode this fires strictly AFTER that answer: a
     * successful verdict releases the provisioning hold, and only then does
     * registration and the hub connect run. The SDK raises no event for a
     * successful "no update available", so the sample cannot tell pending
     * from answered and does not claim to. */
    if (!st.run.announced && st.run.conn[ready_scope] == AZ_IOT_CONN_STATE_CONNECTED)
    {
      st.run.announced = 1;
      printf(
          "%s up. Running (Ctrl-C to exit)...\n",
          (ready_scope == AZ_IOT_CONN_SCOPE_DPS) ? "Provisioning session" : "Hub connection");
    }

    /* A workflow ran to completion and returned to Idle. The process stays
     * alive, but it asked once: nothing further arrives on this run. */
    if (st.run.workflow_completed)
    {
      st.run.workflow_completed = 0;
      printf("Deployment workflow complete. Restart the sample to ask again.\n");
      remove(st.sim.state_file); /* clear the resume blob */
    }

    /* A (simulated) reboot was requested: state is persisted; exit so the
     * operator can "reboot" and re-run to resume. */
    if (st.sim.reboot_signalled)
    {
      printf(
          "Reboot required. Workflow state persisted to %s.\n"
          "Re-run the sample (without ADU_SIM_REBOOT) to resume.\n",
          st.sim.state_file);
      break;
    }

    /* The check gave up, so nothing further can arrive for it. The observer
     * has already printed what the service said. */
    if (st.run.check_abandoned)
    {
      rc = 1;
      break;
    }

    /* FAULTED is settled: the SDK does not retry out of it. A workflow in
     * flight is never interrupted for it. */
    if (st.run.faulted && !st.run.workflow_active)
    {
      fprintf(stderr, "The connection faulted and will not recover on its own.\n");
      rc = 1;
      break;
    }
  }

  az_iot_connection_client_close(&st.connection_client);
  for (int i = 0; i < 100
       && (st.run.conn[AZ_IOT_CONN_SCOPE_DPS] != AZ_IOT_CONN_STATE_IDLE
           || st.run.conn[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_IDLE);
       ++i)
  {
    (void)az_iot_connection_client_do_work(&st.connection_client, 50);
  }

  sample_state_destroy(&st);
  return rc;
}
