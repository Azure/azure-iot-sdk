// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* adu/pc - Azure Device Update (ADU) on-device workflow sample.
 *
 * Portable PC sample (Linux + Windows). Runs the ENTIRE ADU workflow end to end
 * against a real IoT Hub + Device Update instance, but with SIMULATED
 * download/install hooks so it is safe to run on a dev box (it never touches
 * real firmware). See the companion README.md and docs/azure-device-update.md.
 *
 * Real:      connection, twin, manifest receipt, JWS verification (OpenSSL),
 *            per-file SHA-256 hash check, state reporting.
 * Simulated: download_fn (synthesizes deterministic payload bytes),
 *            install/apply/backup/restore (log only, optional forced failure
 *            or reboot), persist/load (a temp file so resume() works).
 *
 * Simulation knobs (environment variables, all optional):
 *   ADU_SIM_FAIL_STEP=<n>     force install_fn to fail at 1-based step n
 *   ADU_SIM_HASH_MISMATCH=1   corrupt the synthesized payload (hash failure)
 *   ADU_SIM_REBOOT=1          install returns REBOOT_REQUIRED; state is
 *                             persisted and the sample exits. Re-run (without
 *                             this knob) to resume() and finish the workflow.
 *   ADU_SIM_DELAY_MS=<ms>     per-download delay so progress is observable
 *   ADU_SIM_STATE_FILE=<path> resume blob path (default ./adu_sim_state.blob)
 */
#include <signal.h>
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
 * roots, compiled into the SDK — so updates imported through the real Device
 * Update service (which signs every manifest with Microsoft's signing service)
 * verify out of the box.
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
  const char* state_file;

  int reboot_signalled; /* set by install_fn when it returns REBOOT_REQUIRED */
} sim_ctx;

static void sleep_ms(long ms)
{
  if (ms <= 0)
  {
    return;
  }
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  (void)nanosleep(&ts, NULL);
#endif
}

/* Portable getenv (MSVC flags plain getenv as unsafe under -Werror). Read once
 * at startup; the small one-time Windows duplicate is acceptable for a sample. */
static char* sample_getenv(const char* name)
{
#if defined(_WIN32)
  char* v = NULL;
  size_t n = 0;
  if (_dupenv_s(&v, &n, name) != 0)
  {
    return NULL;
  }
  return v;
#else
  return getenv(name);
#endif
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
  sleep_ms(s->delay_ms);
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

static const char* adu_state_name(az_iot_adu_state s)
{
  switch (s)
  {
    case AZ_IOT_ADU_STATE_IDLE:
      return "Idle";
    case AZ_IOT_ADU_STATE_MANIFEST_RECEIVED:
      return "ManifestReceived";
    case AZ_IOT_ADU_STATE_VERIFYING_MANIFEST:
      return "VerifyingManifest";
    case AZ_IOT_ADU_STATE_DOWNLOAD_STARTED:
      return "DownloadStarted";
    case AZ_IOT_ADU_STATE_DOWNLOAD_COMPLETE:
      return "DownloadComplete";
    case AZ_IOT_ADU_STATE_BACKUP_STARTED:
      return "BackupStarted";
    case AZ_IOT_ADU_STATE_BACKUP_COMPLETE:
      return "BackupComplete";
    case AZ_IOT_ADU_STATE_INSTALL_STARTED:
      return "InstallStarted";
    case AZ_IOT_ADU_STATE_INSTALL_COMPLETE:
      return "InstallComplete";
    case AZ_IOT_ADU_STATE_APPLY_STARTED:
      return "ApplyStarted";
    case AZ_IOT_ADU_STATE_RESTORE_STARTED:
      return "RestoreStarted";
    case AZ_IOT_ADU_STATE_FAILED:
      return "Failed";
    default:
      return "?";
  }
}

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo)
{
  (void)signo;
  g_stop = 1;
}

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_twin_client twin_client;
  az_iot_adu_client_t adu_client;
  sim_ctx sim;
  uint8_t dp_buffer[512];
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  az_iot_adu_client_destroy(&s->adu_client);
  az_iot_twin_client_destroy(&s->twin_client);
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

static az_iot_connection_state g_conn_state = AZ_IOT_CONN_STATE_IDLE;
static const char* conn_state_name(az_iot_connection_state s)
{
  switch (s)
  {
    case AZ_IOT_CONN_STATE_IDLE:
      return "Idle";
    case AZ_IOT_CONN_STATE_CONNECTING:
      return "Connecting";
    case AZ_IOT_CONN_STATE_CONNECTED:
      return "Connected";
    case AZ_IOT_CONN_STATE_RECONNECTING:
      return "Reconnecting";
    case AZ_IOT_CONN_STATE_DISCONNECTING:
      return "Disconnecting";
    case AZ_IOT_CONN_STATE_FAULTED:
      return "Faulted";
    default:
      return "?";
  }
}
static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_connection_state st = event->state;
  az_iot_result reason = event->reason;
  (void)user_ctx;
  if (st != g_conn_state)
  {
    printf(
        "Connection: %s -> %s (reason=0x%08x)\n",
        conn_state_name(g_conn_state),
        conn_state_name(st),
        (unsigned)reason);
  }
  g_conn_state = st;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  signal(SIGINT, on_sigint);

  sample_state st = { 0 };

  /* Simulation knobs from the environment. */
  const char* env;
  st.sim.fail_step = (env = sample_getenv("ADU_SIM_FAIL_STEP")) ? atoi(env) : 0;
  st.sim.hash_mismatch = sample_getenv("ADU_SIM_HASH_MISMATCH") ? 1 : 0;
  st.sim.reboot = sample_getenv("ADU_SIM_REBOOT") ? 1 : 0;
  st.sim.delay_ms = (env = sample_getenv("ADU_SIM_DELAY_MS")) ? atol(env) : 0;
  st.sim.state_file = (env = sample_getenv("ADU_SIM_STATE_FILE")) ? env : "./adu_sim_state.blob";

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

  /* Connection client (DPS provisioning is internal when host == NULL). */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = st.config.id_scope;
  copts.dps.registration_id = st.config.reg_id;
  copts.certificate_provider = &st.certs.base;
  /* Announce the Device Update PnP model id at connection. Device Update
   * imports and classifies a device ONLY if it advertises a model id as part
   * of the MQTT CONNECT; without it the device never lands in the ADU
   * instance and no device group ever forms. This value matches the
   * contractModelId the ADU agent reports in its twin. */
  copts.model_id = "dtmi:azure:iot:deviceUpdateContractModel;2";
  /* Enable automatic reconnect with exponential backoff + jitter so the
   * long-running sample recovers transparently from transient drops (e.g.
   * a duplicate-connection eviction or a network blip) while it waits for a
   * deployment. initial_delay_ms > 0 is what arms the reconnect machinery. */
  copts.reconnection_policy.initial_delay_ms = 2000; /* first retry after 2s */
  copts.reconnection_policy.max_delay_ms = 60000; /* cap backoff at 60s */
  copts.reconnection_policy.max_attempts = 0; /* 0 = retry forever */
  copts.reconnection_policy.jitter_pct = 20; /* +/-20% jitter */
  if (az_iot_connection_client_init(&st.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }
  az_iot_connection_client_set_state_callback(&st.connection_client, on_conn_state, NULL);

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

  /* Twin client (ADU registers as a desired-property subscriber on it). */
  if (az_iot_twin_client_init(&st.twin_client, &st.connection_client) != AZ_IOT_OK)
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

  static const az_iot_adu_custom_property customs[] = {
    { "environment", "sim" },
  };
  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "ADU-Sim";
  dp.installed_update_id.provider = "Contoso";
  dp.installed_update_id.name = "ADU-Sim";
  dp.installed_update_id.version = "1.0.0";
  dp.custom_properties = customs;
  dp.custom_properties_count = sizeof(customs) / sizeof(customs[0]);

  az_iot_adu_client_config_options adu_opts = az_iot_adu_client_config_options_default();
  adu_opts.hooks = &hooks;
  adu_opts.crypto = &crypto;
  adu_opts.root_keys = root_keys;
  adu_opts.root_key_count = root_key_count;
  adu_opts.device_props = &dp;
  adu_opts.device_props_buffer = st.dp_buffer;
  adu_opts.device_props_buffer_size = sizeof(st.dp_buffer);
  if (az_iot_adu_client_initialize(&st.adu_client, &st.twin_client, &adu_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "az_iot_adu_client_initialize failed\n");
    sample_state_destroy(&st);
    return 1;
  }

  /* Resume any workflow persisted before a (simulated) reboot. */
  if (az_iot_adu_client_resume(&st.adu_client) == AZ_IOT_OK
      && az_iot_adu_client_get_state(&st.adu_client) != AZ_IOT_ADU_STATE_IDLE)
  {
    printf(
        "Resumed persisted workflow at state: %s\n",
        adu_state_name(az_iot_adu_client_get_state(&st.adu_client)));
  }

  /* Open (internally provisions via DPS then connects to the assigned hub). */
  if (az_iot_connection_client_open(&st.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }
  for (int i = 0; i < 1200 && g_conn_state != AZ_IOT_CONN_STATE_CONNECTED && !g_stop; ++i)
  {
    (void)az_iot_connection_client_do_work(&st.connection_client, 50);
    if (g_conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (g_conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    printf("Connected. Device reports manufacturer=Contoso model=ADU-Sim "
           "installedUpdateId=1.0.0.\n");
    printf("Waiting for a deployment (Ctrl-C to exit)...\n");

    az_iot_adu_state prev = az_iot_adu_client_get_state(&st.adu_client);
    int saw_active = (prev != AZ_IOT_ADU_STATE_IDLE);

    while (!g_stop)
    {
      (void)az_iot_connection_client_do_work(&st.connection_client, 50);
      (void)az_iot_adu_client_do_work(&st.adu_client);

      az_iot_adu_state cur = az_iot_adu_client_get_state(&st.adu_client);
      if (cur != prev)
      {
        printf("ADU state: %s -> %s\n", adu_state_name(prev), adu_state_name(cur));
        prev = cur;
      }
      if (cur != AZ_IOT_ADU_STATE_IDLE)
      {
        saw_active = 1;
      }

      /* A (simulated) reboot was requested: state is persisted; exit so
       * the operator can "reboot" and re-run to resume. */
      if (st.sim.reboot_signalled)
      {
        printf(
            "Reboot required. Workflow state persisted to %s.\n"
            "Re-run the sample (without ADU_SIM_REBOOT) to resume.\n",
            st.sim.state_file);
        rc = 0;
        break;
      }

      /* Workflow ran to completion and returned to Idle. */
      if (saw_active && cur == AZ_IOT_ADU_STATE_IDLE)
      {
        printf("Deployment workflow complete.\n");
        remove(st.sim.state_file); /* clear resume blob */
        saw_active = 0;
        rc = 0;
      }
    }
  }
  else
  {
    fprintf(stderr, "Failed to connect (state=%d).\n", (int)g_conn_state);
  }

  az_iot_connection_client_close(&st.connection_client);
  for (int i = 0; i < 100 && g_conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&st.connection_client, 50);
  }

  sample_state_destroy(&st);
  return rc;
}
