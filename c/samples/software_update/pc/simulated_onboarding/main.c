// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* software_update/pc/simulated_onboarding - Software Update onboarding sample.
 *
 * Portable PC sample (Linux + Windows). Runs the ENTIRE software updates workflow end to end
 * against a real Device Update instance, but with SIMULATED download/install
 * hooks so it is safe to run on a dev box (it never touches real firmware).
 * See the companion README.md and docs/eng/su-client-design.md.
 *
 * ONBOARDING ROUTE ONLY: a day-0 device, with no device record yet, asks with
 * az_iot_su_client_request_onboarding_update(). A device that has already
 * registered uses the regular route instead; see ../simulated_regular.
 *
 * NO IOT HUB IS REQUIRED. Every device-update operation runs on the
 * provisioning session, before the device registers, so the sample declares
 * dps.provision_only: keep that session up, never register, never connect to a
 * hub.
 *
 * Real:      connection, update request/response, manifest receipt, JWS
 *            verification (OpenSSL), per-file SHA-256 hash check, status
 *            reporting.
 * Simulated: download_fn (synthesizes deterministic payload bytes),
 *            install/apply/backup/restore (log only, optional forced failure
 *            or reboot), persist/load (a temp file so resume() works). See
 *            ../common/su_sim.c.
 *
 * Device identity (environment variables, all optional).
 *
 * MATCHED: manufacturer and model are the COMPATIBILITY PROPERTIES the
 * service matches an update against. If they do not match the imported
 * update, the device is answered "nothing to do" and is never offered
 * anything, so the sample prints what it reported.
 *   AZ_IOT_SU_MANUFACTURER=<s>        default "Contoso"
 *   AZ_IOT_SU_MODEL=<s>               default "SU-Sim"
 *
 * REPORTED, NOT MATCHED: the installed update id says what is on the device
 * now. It takes no part in matching, and the onboarding route this sample uses
 * omits it entirely -- changing it cannot make an update eligible.
 *   AZ_IOT_SU_INSTALLED_PROVIDER=<s>  default "Contoso"
 *   AZ_IOT_SU_INSTALLED_NAME=<s>      default "SU-Sim"
 *   AZ_IOT_SU_INSTALLED_VERSION=<s>   default "1.0.0"
 *
 * Other knobs (environment variables, all optional):
 *   AZ_IOT_SU_LOG_LEVEL=<lvl>      trace|debug|info|warn|error|off (default
 *                                   info). The SDK's "su:" and "dps:"
 *                                   protocol lines are emitted at debug.
 *   SU_SIM_FAIL_STEP=<n>     force install_fn to fail at 1-based step n
 *   SU_SIM_HASH_MISMATCH=1   corrupt the synthesized payload (hash failure)
 *   SU_SIM_REBOOT=1          install returns REBOOT_REQUIRED; state is
 *                             persisted and the sample exits. Re-run (without
 *                             this knob) to resume() and finish the workflow.
 *   SU_SIM_DELAY_MS=<ms>     per-download delay so progress is observable
 *   SU_SIM_STATE_FILE=<path> resume blob path (default ./su_sim_state.blob)
 */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_su.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "az_iot_su_crypto_openssl.h"

#include "sample_utils.h"
#include "su_sim.h"

/* ------------------------------------------------------------------------- */
/* root keys                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Software updates anchors manifest trust in one or more RSA root public keys. This sample
 * uses az_iot_su_microsoft_root_keys() — Microsoft's published software updates production
 * roots, compiled into the SDK — so manifests signed under those roots
 * verify out of the box. Other issuers require explicitly trusted keys.
 *
 * To accept updates signed by your OWN root instead, build your own
 * az_iot_su_root_key array (kid + big-endian modulus/exponent) and pass it
 * to az_iot_su_client_initialize() in place of the Microsoft keys.
 */

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
  int check_abandoned; /* the update check gave up; see on_su_event */
  int workflow_active; /* a deployment is in flight */
  int workflow_completed; /* one finished and returned to Idle */
  int announced; /* the "ready" line has been printed */
} sample_run;

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_su_client su_client;
  su_sim sim;
  sample_run run;
  /* Sized by the SDK's own default so raising it really does grow this cache;
   * the values it holds come from the environment. */
  AZ_IOT_SU_DEVICE_PROPERTIES_STORAGE(dp_buffer);

  /* Device identity, kept past initialize() only so it can be printed. */
  char* manufacturer;
  char* model;
  char* installed_provider;
  char* installed_name;
  char* installed_version;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  az_iot_su_client_destroy(&s->su_client);

  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);

  su_sim_deinit(&s->sim);
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
  if (!SU_SAMPLE_EVENT_HAS(event, az_iot_connection_state_event, reason)
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

static void on_su_event(const az_iot_su_event* event, void* user_ctx)
{
  sample_run* run = (sample_run*)user_ctx;

  /* `service_error` is the last field read here. */
  if (!SU_SAMPLE_EVENT_HAS(event, az_iot_su_event, service_error))
  {
    return;
  }

  switch (event->kind)
  {
    case AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED:
      printf(
          "Update workflow: %s -> %s\n",
          su_sample_state_name(event->previous_state),
          su_sample_state_name(event->state));
      run->workflow_active = (event->state != AZ_IOT_SU_STATE_IDLE);
      if (!run->workflow_active && event->previous_state != AZ_IOT_SU_STATE_IDLE)
      {
        run->workflow_completed = 1;
      }
      break;

    case AZ_IOT_SU_EVENT_OPERATION_ABANDONED:
      fprintf(
          stderr,
          "%s abandoned: %s. Service said: code=%d message=\"%s\" trackingId=\"%s\"\n",
          su_sample_operation_name(event->operation),
          az_iot_result_to_string(event->reason),
          (int)event->service_error.code,
          event->service_error.message,
          event->service_error.tracking_id);
      /* A status report is diagnostic: there is no public call to reissue one
       * and the client reports again at its next reporting point, so it is
       * logged and nothing more. A fetch giving up is what ends this sample --
       * the device asked and will not be told. */
      if (event->operation != AZ_IOT_SU_OP_REPORT_STATUS)
      {
        run->check_abandoned = 1;
      }
      break;
  }
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(su_sample_log_level_from_env());
  az_iot_log_set_global_sink(&log);

  signal(SIGINT, on_sigint);

  sample_state st = { 0 };

  /* Simulation knobs from the environment. */
  if (su_sim_init(&st.sim) != 0)
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
   * device registers, so this device declares it has no hub: the session is
   * kept up and pumped, registration never runs, and the hub scope stays IDLE.
   * Declared rather than inferred -- a hubless enrollment and a misconfigured
   * one both fail registration the same way, so inferring it would hide real
   * misconfiguration. */
  copts.dps.provision_only = true;
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

  /* Software updates client. */
  az_iot_su_platform_hooks hooks = su_sim_hooks(&st.sim);

  az_iot_su_crypto_hooks crypto = az_iot_su_crypto_openssl_hooks();

  /* Microsoft's compiled-in software updates production root keys: anchors the trust chain
   * for updates signed by the real Device Update service. */
  size_t root_key_count = 0;
  const az_iot_su_root_key* root_keys = az_iot_su_microsoft_root_keys(&root_key_count);

  /* Compatibility properties: what the service matches a deployed update
   * against. Configurable because a hard-coded value that does not match the
   * imported update is answered "nothing to do" -- indistinguishable from
   * "nothing deployed" unless the device says what it asked with. */
  st.manufacturer = sample_env_dup("AZ_IOT_SU_MANUFACTURER", "Contoso");
  st.model = sample_env_dup("AZ_IOT_SU_MODEL", "SU-Sim");
  st.installed_provider = sample_env_dup("AZ_IOT_SU_INSTALLED_PROVIDER", "Contoso");
  st.installed_name = sample_env_dup("AZ_IOT_SU_INSTALLED_NAME", "SU-Sim");
  st.installed_version = sample_env_dup("AZ_IOT_SU_INSTALLED_VERSION", "1.0.0");
  if (st.manufacturer == NULL || st.model == NULL || st.installed_provider == NULL
      || st.installed_name == NULL || st.installed_version == NULL)
  {
    sample_state_destroy(&st);
    return 1;
  }

  az_iot_su_device_properties dp = { 0 };
  dp.manufacturer = st.manufacturer;
  dp.model = st.model;
  dp.installed_update_id.provider = st.installed_provider;
  dp.installed_update_id.name = st.installed_name;
  dp.installed_update_id.version = st.installed_version;

  /* Named, rather than a bare error out of initialize(): the cache is fixed
   * and these values now come from the environment. */
  size_t dp_needed = az_iot_su_device_properties_buffer_size(&dp);
  if (dp_needed == 0)
  {
    fprintf(
        stderr,
        "Device properties are invalid or too long. Need 1-%d compatibility "
        "properties (nonempty, unique names; non-NULL values, empty allowed), and "
        "an installed update id that is unset or has nonempty provider, name and "
        "version.\n",
        AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES);
    sample_state_destroy(&st);
    return 1;
  }
  if (dp_needed > sizeof(st.dp_buffer))
  {
    fprintf(
        stderr,
        "Device properties need %zu bytes of cache; this sample has %zu. Shorten "
        "them or raise AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE.\n",
        dp_needed,
        sizeof(st.dp_buffer));
    sample_state_destroy(&st);
    return 1;
  }

  az_iot_su_client_config_options su_opts = az_iot_su_client_config_options_default();
  su_opts.hooks = &hooks;
  su_opts.crypto = &crypto;
  su_opts.root_keys = root_keys;
  su_opts.root_key_count = root_key_count;
  su_opts.device_properties = &dp;
  su_opts.device_properties_buffer = st.dp_buffer;
  su_opts.device_properties_buffer_size = sizeof(st.dp_buffer);
  if (az_iot_su_client_initialize(&st.su_client, &st.connection_client, &su_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "az_iot_su_client_initialize failed\n");
    sample_state_destroy(&st);
    return 1;
  }

  /* Registered before resume(), which replays a persisted workflow state
   * through this same observer. */
  if (az_iot_su_client_add_observer(&st.su_client, on_su_event, &st.run) != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }

  /* Resume any workflow persisted before a (simulated) reboot. A restored
   * state is reported through the observer registered above, which resume()
   * replays into; nothing to resume is not an error. */
  if (az_iot_su_client_resume(&st.su_client) != AZ_IOT_OK)
  {
    fprintf(stderr, "Could not resume the persisted workflow; starting from Idle.\n");
  }

  /* Ask for a day-0 onboarding update. Nothing is fetched unless the
   * application asks: only it knows whether it has a device record yet, and
   * the onboarding route is the one that needs none. A registered device calls
   * az_iot_su_client_request_update() instead; see ../simulated_regular.
   *
   * The timeout bounds how long the CLIENT keeps reissuing this check before
   * giving up and raising AZ_IOT_SU_EVENT_OPERATION_ABANDONED with
   * AZ_IOT_ERR_TIMEOUT. Without it an unservable check -- no device record, no
   * linked hub -- is retried on every do_work() for the life of the client,
   * and looks exactly like "no update available".
   *
   * AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS is the default for an application
   * with no policy of its own. Pass your own value when you have one, or
   * AZ_IOT_SU_REQUEST_NO_TIMEOUT to keep retrying indefinitely. */
  if (az_iot_su_client_request_onboarding_update(
          &st.su_client, AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS)
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

  /* One loop, from the moment open() returns. Nothing waits for a hub: the
   * update check runs on the provisioning session, so gating it on the hub
   * scope would leave it unissued on a device that has no hub. Both clients
   * are pumped -- the connection delivers, the software updates client advances -- and
   * everything this loop reacts to arrives through the two observers. */
  rc = 0; /* interrupted or asked to reboot is a normal exit */
  while (!g_stop)
  {
    (void)az_iot_connection_client_do_work(&st.connection_client, 50);
    (void)az_iot_su_client_do_work(&st.su_client);

    /* Says only that the provisioning session came up, never whether the
     * update check has been answered. The SDK raises no event for a
     * successful "no update available", so the sample cannot tell pending
     * from answered and does not claim to. */
    if (!st.run.announced && st.run.conn[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_CONNECTED)
    {
      st.run.announced = 1;
      printf("Provisioning session up. Running (Ctrl-C to exit)...\n");
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
          "Re-run the sample (without SU_SIM_REBOOT) to resume.\n",
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
