// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* software_update/pc/simulated_onboarding - Software Update onboarding sample.
 *
 * Portable PC sample (Linux + Windows). Runs the ENTIRE software updates workflow end to end
 * against a real Device Update instance, but with SIMULATED download/install
 * hooks so it is safe to run on a dev box (it never touches real firmware).
 * See the companion README.md and docs/eng/software-updates.md.
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
 * to az_iot_su_client_init() in place of the Microsoft keys.
 */

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
  az_iot_su_client su_client;

  /* Device identity, kept past init() only so it can be printed. */
  struct su_device_properties
  {
    char* manufacturer;
    char* model;
    char* installed_provider;
    char* installed_name;
    char* installed_version;
  } su_device_properties;

  su_simulation_control simulation_control;

  /* Set by the observers; the SDK reports everything this sample reacts to, so
   * nothing here is polled or inferred. */
  az_iot_connection_state connection_state[AZ_IOT_CONN_SCOPE_COUNT];
  int connection_is_faulted; /* a lifecycle settled at FAULTED: unrecoverable here */
  int check_su_request_abandoned; /* the update check gave up; see on_su_event */
  int su_workflow_active; /* a deployment is in flight */
  int su_workflow_completed; /* one finished and returned to Idle */

  /* Sized by the SDK's own default so raising it really does grow this cache;
   * the values it holds come from the environment. */
  AZ_IOT_SU_DEVICE_PROPERTIES_STORAGE(dp_buffer);
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  az_iot_su_client_deinit(&s->su_client);

  az_iot_connection_client_deinit(&s->connection_client);
  az_iot_certificate_provider_pem_deinit(&s->certs);
  sample_config_release(&s->config);

  free(s->simulation_control.state_file);
  free(s->su_device_properties.manufacturer);
  free(s->su_device_properties.model);
  free(s->su_device_properties.installed_provider);
  free(s->su_device_properties.installed_name);
  free(s->su_device_properties.installed_version);
}

/* Both lifecycles report here. `state` is meaningless without `scope`: a
 * provisioning session that is up says nothing about a hub, and on a
 * provision_only device the hub scope stays IDLE for good. */
static void on_connection_state_event_received(
    const az_iot_connection_state_event* event,
    void* user_ctx)
{
  sample_state* state = (sample_state*)user_ctx;

  /* `scope` indexes an array: ignore one this build does not know. */
  if ((unsigned)event->scope >= AZ_IOT_CONN_SCOPE_COUNT)
  {
    return;
  }

  az_iot_connection_scope scope = event->scope;

  if (state->connection_state[scope] != event->state)
  {
    printf(
        "%s: %s -> %s (%s)\n",
        (scope == AZ_IOT_CONN_SCOPE_DPS) ? "Provisioning" : "Hub",
        sample_connection_state_name(state->connection_state[scope]),
        sample_connection_state_name(event->state),
        az_iot_result_to_string(event->reason));
    state->connection_state[scope] = event->state;

    /* Says only that the provisioning session came up, never whether the
     * update check has been answered. The SDK raises no event for a
     * successful "no update available", so the sample cannot tell pending
     * from answered and does not claim to. */
    if (scope == AZ_IOT_CONN_SCOPE_DPS && event->state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      printf("Provisioning session up. Running (Ctrl-C to exit)...\n");
    }
  }

  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    state->connection_is_faulted = 1;
  }
}

static void on_su_event(const az_iot_su_event* event, void* user_ctx)
{
  sample_state* state = (sample_state*)user_ctx;

  switch (event->kind)
  {
    case AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED:
      printf(
          "Update workflow: %s -> %s\n",
          su_sample_state_name(event->previous_state),
          su_sample_state_name(event->state));
      state->su_workflow_active = (event->state != AZ_IOT_SU_STATE_IDLE);
      if (!state->su_workflow_active && event->previous_state != AZ_IOT_SU_STATE_IDLE)
      {
        state->su_workflow_completed = 1;
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
        state->check_su_request_abandoned = 1;
      }
      break;
    case AZ_IOT_SU_EVENT_PERSIST_FAILED:
    case AZ_IOT_SU_EVENT_PERSIST_RECOVERED:
      /* While a write is failing the workflow is held: do not reboot. */
      fprintf(
          stderr,
          "Update state storage %s after %u failed write(s)%s\n",
          event->kind == AZ_IOT_SU_EVENT_PERSIST_FAILED ? "failing" : "recovered",
          (unsigned)event->persist_attempts,
          (event->kind == AZ_IOT_SU_EVENT_PERSIST_FAILED && !event->persist_retrying)
              ? "; giving up"
              : "");
      break;
  }
}

/**
 * @brief Create the certificate provider and the provision_only connection
 * client, observed by on_connection_state_event_received() and with both MQTT factories registered.
 *
 * The reconnection policy is the default from
 * az_iot_connection_client_options_default().
 *
 * @param[in,out] state Sample state; its config must be loaded.
 * @return 0 on success, nonzero on failure. sample_state_destroy() releases
 * what was created either way.
 */
static int initialize_connection_client(sample_state* state)
{
  /* Certificate provider. */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = state->config.ca;
  pem.client_cert_pem_path = state->config.cert;
  pem.client_key_pem_path = state->config.key;
  if (az_iot_certificate_provider_pem_init(&state->certs, &pem) != AZ_IOT_OK)
  {
    return 1;
  }

  /* Connection client. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state->config);
  copts.certificate_provider = &state->certs.base;
  /* Every device-update operation runs on the provisioning session, before the
   * device registers, so this device declares it has no hub: the session is
   * kept up and pumped, registration never runs, and the hub scope stays IDLE.
   * Declared rather than inferred -- a hubless enrollment and a misconfigured
   * one both fail registration the same way, so inferring it would hide real
   * misconfiguration. */
  copts.dps.provision_only = true;
  if (az_iot_connection_client_init(&state->connection_client, &copts) != AZ_IOT_OK)
  {
    return 1;
  }
  if (az_iot_connection_client_add_state_observer(
          &state->connection_client, on_connection_state_event_received, state)
      != AZ_IOT_OK)
  {
    return 1;
  }

  if (az_iot_connection_client_register_mqtt_factory(
          &state->connection_client, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(
             &state->connection_client, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK)
  {
    return 1;
  }

  return 0;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(su_sample_log_level_from_env());
  az_iot_log_set_global_sink(&log);

  (void)signal(SIGINT, on_sigint); /* Ctrl+C handling is a convenience */

  sample_state state = { 0 };

  /* Simulation knobs from the environment. */
  state.simulation_control.fail_step = (int)su_sample_env_long("SU_SIM_FAIL_STEP", 0);
  state.simulation_control.hash_mismatch = su_sample_env_flag("SU_SIM_HASH_MISMATCH");
  state.simulation_control.reboot = su_sample_env_flag("SU_SIM_REBOOT");
  state.simulation_control.delay_ms = su_sample_env_long("SU_SIM_DELAY_MS", 0);
  state.simulation_control.state_file = sample_env_dup("SU_SIM_STATE_FILE", "./su_sim_state.blob");
  if (state.simulation_control.state_file == NULL)
  {
    return 1;
  }

  if (sample_config_load(&state.config) != 0)
  {
    free(state.simulation_control.state_file);
    return 1;
  }

  int rc = 1;

  if (initialize_connection_client(&state) != 0)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Software updates client. The platform hooks do the device-side work of a
   * deployment; here each one is simulated (see ../common/su_sim.c). */
  az_iot_su_platform_hooks hooks = { 0 };
  hooks.download_fn = su_download;
  hooks.read_file_fn = su_read_file;
  hooks.is_installed_fn = su_is_installed;
  hooks.backup_fn = su_backup;
  hooks.install_fn = su_install;
  hooks.apply_fn = su_apply;
  hooks.restore_fn = su_restore;
  hooks.persist_state_fn = su_persist_state;
  hooks.load_state_fn = su_load_state;
  hooks.user_ctx = &state.simulation_control;

  az_iot_su_crypto_hooks crypto = az_iot_su_crypto_openssl_hooks();

  /* Microsoft's compiled-in software updates production root keys: anchors the trust chain
   * for updates signed by the real Device Update service. */
  size_t root_key_count = 0;
  const az_iot_su_root_key* root_keys = az_iot_su_microsoft_root_keys(&root_key_count);

  /* Compatibility properties: what the service matches a deployed update
   * against. Configurable because a hard-coded value that does not match the
   * imported update is answered "nothing to do" -- indistinguishable from
   * "nothing deployed" unless the device says what it asked with. */
  state.su_device_properties.manufacturer = sample_env_dup("AZ_IOT_SU_MANUFACTURER", "Contoso");
  state.su_device_properties.model = sample_env_dup("AZ_IOT_SU_MODEL", "SU-Sim");
  state.su_device_properties.installed_provider
      = sample_env_dup("AZ_IOT_SU_INSTALLED_PROVIDER", "Contoso");
  state.su_device_properties.installed_name = sample_env_dup("AZ_IOT_SU_INSTALLED_NAME", "SU-Sim");
  state.su_device_properties.installed_version
      = sample_env_dup("AZ_IOT_SU_INSTALLED_VERSION", "1.0.0");
  if (state.su_device_properties.manufacturer == NULL || state.su_device_properties.model == NULL
      || state.su_device_properties.installed_provider == NULL
      || state.su_device_properties.installed_name == NULL
      || state.su_device_properties.installed_version == NULL)
  {
    sample_state_destroy(&state);
    return 1;
  }

  az_iot_su_device_properties dp = { 0 };
  dp.manufacturer = state.su_device_properties.manufacturer;
  dp.model = state.su_device_properties.model;
  dp.installed_update_id.provider = state.su_device_properties.installed_provider;
  dp.installed_update_id.name = state.su_device_properties.installed_name;
  dp.installed_update_id.version = state.su_device_properties.installed_version;

  /* Named, rather than a bare error out of init(): the cache is fixed
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
    sample_state_destroy(&state);
    return 1;
  }
  if (dp_needed > sizeof(state.dp_buffer))
  {
    fprintf(
        stderr,
        "Device properties need %zu bytes of cache; this sample has %zu. Shorten "
        "them or raise AZ_IOT_SU_DEVICE_PROPERTIES_BUFFER_SIZE.\n",
        dp_needed,
        sizeof(state.dp_buffer));
    sample_state_destroy(&state);
    return 1;
  }

  az_iot_su_client_config_options su_opts = az_iot_su_client_config_options_default();
  su_opts.hooks = &hooks;
  su_opts.crypto = &crypto;
  su_opts.root_keys = root_keys;
  su_opts.root_key_count = root_key_count;
  su_opts.device_properties = &dp;
  su_opts.device_properties_buffer = state.dp_buffer;
  su_opts.device_properties_buffer_size = sizeof(state.dp_buffer);
  if (az_iot_su_client_init(&state.su_client, &state.connection_client, &su_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "az_iot_su_client_init failed\n");
    sample_state_destroy(&state);
    return 1;
  }

  /* Registered before resume(), which replays a persisted workflow state
   * through this same observer. */
  if (az_iot_su_client_add_observer(&state.su_client, on_su_event, &state) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Resume any workflow persisted before a (simulated) reboot. A restored
   * state is reported through the observer registered above, which resume()
   * replays into; nothing to resume is not an error. */
  if (az_iot_su_client_resume(&state.su_client) != AZ_IOT_OK)
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
  /* Only from Idle: a workflow restored by resume() is finished first, since a
   * check now could deliver a different workflow, which supersedes it. */
  if (az_iot_su_client_get_state(&state.su_client) == AZ_IOT_SU_STATE_IDLE
      && az_iot_su_client_request_onboarding_update(
             &state.su_client, AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS)
          != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  printf(
      "Matched against a deployed update: manufacturer=%s model=%s\n"
      "Reported only (not matched, and omitted on the onboarding route): "
      "installedUpdateId=%s/%s/%s\n",
      state.su_device_properties.manufacturer,
      state.su_device_properties.model,
      state.su_device_properties.installed_provider,
      state.su_device_properties.installed_name,
      state.su_device_properties.installed_version);

  /* The application always opens the connection its feature clients use. On a
   * provision_only device this brings the provisioning session up and stops
   * there; that session IS the connection. */
  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
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
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    (void)az_iot_su_client_do_work(&state.su_client);

    /* A workflow ran to completion and returned to Idle. The process stays
     * alive, but it asked once: nothing further arrives on this run. */
    if (state.su_workflow_completed)
    {
      state.su_workflow_completed = 0;
      printf("Deployment workflow complete. Restart the sample to ask again.\n");
      (void)remove(state.simulation_control.state_file); /* clear the resume blob */
    }

    /* A (simulated) reboot was requested. Exit so the operator can "reboot"
     * and re-run to resume, but only once the state is persisted: while the
     * write fails the client retries it, and if it gives up it rolls the
     * update back (clearing reboot_pending) and reports the failure. */
    if (state.simulation_control.reboot_pending && !state.simulation_control.persist_failed)
    {
      printf(
          "Reboot required. Workflow state persisted to %s.\n"
          "Re-run the sample (without SU_SIM_REBOOT) to resume.\n",
          state.simulation_control.state_file);
      break;
    }

    /* The check gave up, so nothing further can arrive for it. The observer
     * has already printed what the service said. */
    if (state.check_su_request_abandoned)
    {
      rc = 1;
      break;
    }

    /* FAULTED is settled: the SDK does not retry out of it. A workflow in
     * flight is never interrupted for it. */
    if (state.connection_is_faulted && !state.su_workflow_active)
    {
      fprintf(stderr, "The connection faulted and will not recover on its own.\n");
      rc = 1;
      break;
    }
  }

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100
       && (state.connection_state[AZ_IOT_CONN_SCOPE_DPS] != AZ_IOT_CONN_STATE_IDLE
           || state.connection_state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_IDLE);
       ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);
  return rc;
}
