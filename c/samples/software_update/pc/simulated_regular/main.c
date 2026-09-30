// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* software_update/pc/simulated_regular - Software Update regular (operational)
 * update sample.
 *
 * Portable PC sample (Linux + Windows). Runs the ENTIRE software updates workflow end to end
 * against a real Device Update instance, but with SIMULATED download/install
 * hooks so it is safe to run on a dev box (it never touches real firmware).
 * See the companion README.md and docs/eng/software-updates.md.
 *
 * REGULAR ROUTE: a device that has registered, and so has a device record,
 * asks with az_iot_su_client_request_update(), which sends installedUpdateId.
 * A day-0 device uses the onboarding route instead; see ../simulated_onboarding.
 *
 * AN IOT HUB IS REQUIRED. The device registers through DPS and connects to its
 * assigned hub; registration is what creates the device record. The update
 * checks and status reports still run on the provisioning session, which the
 * SDK reopens on demand after registration.
 *
 * It checks once at startup, then every AZ_IOT_SU_POLL_INTERVAL_S while no
 * deployment is in flight. After an update succeeds it reports the applied
 * update as installed, so the next check asks for what comes after it.
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
 * INSTALLED: the update id installed at startup. Sent on every check, so the
 * service knows what to offer next; an offered update with this id is
 * skipped as already installed. Replaced in memory after a successful update
 * only: a restart starts from these values again.
 *   AZ_IOT_SU_INSTALLED_PROVIDER=<s>  default "Contoso"
 *   AZ_IOT_SU_INSTALLED_NAME=<s>      default "SU-Sim"
 *   AZ_IOT_SU_INSTALLED_VERSION=<s>   default "1.0.0"
 *
 * Other knobs (environment variables, all optional):
 *   AZ_IOT_SU_POLL_INTERVAL_S=<s>  seconds between update checks (default
 *                                   60); 0 checks once, at startup. A
 *                                   check not answered within half the
 *                                   interval (at most 60 s) is abandoned.
 *   AZ_IOT_SU_LOG_LEVEL=<lvl>      trace|debug|info|warn|error|off (default
 *                                   info). The SDK's "su:" and "dps:"
 *                                   protocol lines are emitted at debug.
 *   SU_SIM_FAIL_STEP=<n>     force install_fn to fail at 1-based step n
 *   SU_SIM_HASH_MISMATCH=1   corrupt the synthesized payload (hash failure)
 *   SU_SIM_REBOOT=1          install returns REBOOT_REQUIRED; state is
 *                             persisted and the sample exits. Re-run (without
 *                             this knob) to resume() and finish the workflow.
 *   SU_SIM_DELAY_MS=<ms>     per-download delay so progress is observable
 *   SU_SIM_STATE_FILE=<path> resume blob path (default ./su_sim_regular_state.blob)
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

/* Microsoft's software updates production root keys, compiled into the SDK,
 * anchor manifest trust. To accept updates signed by your OWN root, build an
 * az_iot_su_root_key array (kid + big-endian modulus/exponent) and pass it to
 * az_iot_su_client_init() instead. */

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

  struct su_device_properties
  {
    char* manufacturer;
    char* model;
    /* What is installed now: sent on every check and read by su_is_installed(). */
    char installed_provider[SU_SIM_ID_PART_SIZE];
    char installed_name[SU_SIM_ID_PART_SIZE];
    char installed_version[SU_SIM_ID_PART_SIZE];
    az_iot_su_report_update_id installed_update_id;
  } su_device_properties;

  su_simulation_control simulation_control;

  /* Set by the observers. */
  az_iot_connection_state connection_state[AZ_IOT_CONN_SCOPE_COUNT];
  int connection_is_faulted; /* a lifecycle settled at FAULTED: unrecoverable here */
  int device_registered; /* the hub connected at least once, so a device record exists */
  int su_workflow_active; /* a deployment is in flight */
  int su_workflow_completed; /* one finished and returned to Idle */
  int su_workflow_succeeded; /* ...and it was applied successfully */
  uint32_t su_retry_after_ms; /* the service asked to wait before asking again */

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
}

static void fill_device_properties(const sample_state* s, az_iot_su_device_properties* dp)
{
  memset(dp, 0, sizeof(*dp));
  dp->manufacturer = s->su_device_properties.manufacturer;
  dp->model = s->su_device_properties.model;
  dp->installed_update_id.provider = s->su_device_properties.installed_provider;
  dp->installed_update_id.name = s->su_device_properties.installed_name;
  dp->installed_update_id.version = s->su_device_properties.installed_version;
}

/* Both lifecycles report here. `state` is meaningless without `scope`. */
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

    if (scope == AZ_IOT_CONN_SCOPE_HUB && event->state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      state->device_registered = 1;
      printf("Hub connection up. Running (Ctrl-C to exit)...\n");
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
        /* The engine returns to Idle from ApplyStarted only when the last step
         * applied; failures pass through Failed, a skip leaves from
         * VerifyingManifest. */
        state->su_workflow_succeeded = (event->previous_state == AZ_IOT_SU_STATE_APPLY_STARTED);
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
      /* A long-running agent asks again at its next poll. A status report has
       * no public reissue; the client reports again at its next reporting
       * point. */
      if (event->operation == AZ_IOT_SU_OP_GET_UPDATE)
      {
        if (!state->device_registered && event->reason != AZ_IOT_ERR_TIMEOUT)
        {
          fprintf(
              stderr,
              "A device that has never registered has no device record yet; it "
              "registers now, and a later check can succeed.\n");
        }
        state->su_retry_after_ms = event->service_error.retry_after_ms;
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
 * @brief Create the certificate provider and the connection client, observed
 * by on_connection_state_event_received() and with both MQTT factories registered.
 *
 * No dps.provision_only: the device registers and connects to its hub, which
 * gives it the device record the regular route needs. The reconnection policy
 * is the default from az_iot_connection_client_options_default().
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

  signal(SIGINT, on_sigint);

  sample_state state = { 0 };

  /* Simulation knobs from the environment. */
  state.simulation_control.fail_step = (int)su_sample_env_long("SU_SIM_FAIL_STEP", 0);
  state.simulation_control.hash_mismatch = su_sample_env_flag("SU_SIM_HASH_MISMATCH");
  state.simulation_control.reboot = su_sample_env_flag("SU_SIM_REBOOT");
  state.simulation_control.delay_ms = su_sample_env_long("SU_SIM_DELAY_MS", 0);
  state.simulation_control.state_file
      = sample_env_dup("SU_SIM_STATE_FILE", "./su_sim_regular_state.blob");
  if (state.simulation_control.state_file == NULL)
  {
    return 1;
  }

  long poll_interval_s = su_sample_env_long("AZ_IOT_SU_POLL_INTERVAL_S", 60);
  if (poll_interval_s < 0)
  {
    poll_interval_s = 60;
  }
  /* Each check is bounded to half the poll interval: a new request resets the
   * deadline, so a longer bound would let an unanswered check outlive every
   * poll and never be abandoned. */
  uint32_t request_timeout_ms = AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS;
  if (poll_interval_s > 0 && (uint64_t)poll_interval_s * 500u < request_timeout_ms)
  {
    request_timeout_ms = (uint32_t)poll_interval_s * 500u;
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

  /* Device identity. */
  state.su_device_properties.manufacturer = sample_env_dup("AZ_IOT_SU_MANUFACTURER", "Contoso");
  state.su_device_properties.model = sample_env_dup("AZ_IOT_SU_MODEL", "SU-Sim");
  if (state.su_device_properties.manufacturer == NULL || state.su_device_properties.model == NULL
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_PROVIDER",
          "Contoso",
          state.su_device_properties.installed_provider,
          sizeof(state.su_device_properties.installed_provider))
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_NAME",
          "SU-Sim",
          state.su_device_properties.installed_name,
          sizeof(state.su_device_properties.installed_name))
      || !sample_env_to_buffer(
          "AZ_IOT_SU_INSTALLED_VERSION",
          "1.0.0",
          state.su_device_properties.installed_version,
          sizeof(state.su_device_properties.installed_version)))
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.su_device_properties.installed_update_id.provider
      = state.su_device_properties.installed_provider;
  state.su_device_properties.installed_update_id.name = state.su_device_properties.installed_name;
  state.su_device_properties.installed_update_id.version
      = state.su_device_properties.installed_version;
  state.simulation_control.installed = &state.su_device_properties.installed_update_id;

  az_iot_su_device_properties dp;
  fill_device_properties(&state, &dp);

  size_t dp_needed = az_iot_su_device_properties_buffer_size(&dp);
  if (dp_needed == 0 || dp_needed > sizeof(state.dp_buffer))
  {
    fprintf(
        stderr,
        "Device properties are invalid or too long for the %zu-byte cache. Need "
        "1-%d compatibility properties and a nonempty installed provider, name and "
        "version.\n",
        sizeof(state.dp_buffer),
        AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES);
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
  size_t root_key_count = 0;
  const az_iot_su_root_key* root_keys = az_iot_su_microsoft_root_keys(&root_key_count);

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

  if (az_iot_su_client_resume(&state.su_client) != AZ_IOT_OK)
  {
    fprintf(stderr, "Could not resume the persisted workflow; starting from Idle.\n");
  }

  /* First check, asked before open(): the SDK holds registration until the
   * check on the first provisioning session reaches a verdict, so asking now
   * lets the device register without waiting out that hold. Later checks run
   * on a provisioning session the SDK reopens on demand.
   *
   * Only from Idle: a workflow restored by resume() is finished first, since a
   * check now could deliver a different workflow, which supersedes it. The poll
   * asks once it is done; registration stays held until that check's verdict
   * (at most AZ_IOT_DPS_HOLD_TIMEOUT_MS). */
  if (az_iot_su_client_get_state(&state.su_client) == AZ_IOT_SU_STATE_IDLE
      && az_iot_su_client_request_update(&state.su_client, request_timeout_ms) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  printf(
      "Matched against a deployed update: manufacturer=%s model=%s\n"
      "Installed: %s/%s/%s\n",
      state.su_device_properties.manufacturer,
      state.su_device_properties.model,
      state.su_device_properties.installed_provider,
      state.su_device_properties.installed_name,
      state.su_device_properties.installed_version);
  if (poll_interval_s > 0)
  {
    printf("Checking for updates every %ld s.\n", poll_interval_s);
  }

  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  uint64_t next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;

  rc = 0; /* interrupted or asked to reboot is a normal exit */
  while (!g_stop)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    (void)az_iot_su_client_do_work(&state.su_client);

    if (state.su_workflow_completed)
    {
      state.su_workflow_completed = 0;
      remove(state.simulation_control.state_file); /* clear the resume blob */

      /* Report the applied update as installed. Without this the next check
       * still sends the previous id, and is offered the same update again. */
      if (state.su_workflow_succeeded && state.simulation_control.applied_valid)
      {
        az_iot_su_device_properties next;
        fill_device_properties(&state, &next);
        next.installed_update_id.provider = state.simulation_control.applied_provider;
        next.installed_update_id.name = state.simulation_control.applied_name;
        next.installed_update_id.version = state.simulation_control.applied_version;
        /* applied_* and installed_* have the same capacity, so the copies fit. */
        if (az_iot_su_client_update_device_properties(&state.su_client, &next) == AZ_IOT_OK
            && sample_copy_str(
                state.su_device_properties.installed_provider,
                sizeof(state.su_device_properties.installed_provider),
                state.simulation_control.applied_provider)
            && sample_copy_str(
                state.su_device_properties.installed_name,
                sizeof(state.su_device_properties.installed_name),
                state.simulation_control.applied_name)
            && sample_copy_str(
                state.su_device_properties.installed_version,
                sizeof(state.su_device_properties.installed_version),
                state.simulation_control.applied_version))
        {
          printf(
              "Update applied. Installed: %s/%s/%s\n",
              state.su_device_properties.installed_provider,
              state.su_device_properties.installed_name,
              state.su_device_properties.installed_version);
        }
        else
        {
          fprintf(stderr, "Could not record the applied update as installed.\n");
        }
      }
      state.su_workflow_succeeded = 0;
      printf("Deployment workflow complete.\n");
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

    /* FAULTED is settled: the SDK does not retry out of it. A workflow in
     * flight is never interrupted for it. */
    if (state.connection_is_faulted && !state.su_workflow_active)
    {
      fprintf(stderr, "The connection faulted and will not recover on its own.\n");
      rc = 1;
      break;
    }

    /* The service's requested delay, when longer, pushes the next check. */
    if (state.su_retry_after_ms > 0)
    {
      uint64_t earliest = sample_now_ms() + state.su_retry_after_ms;
      if (earliest > next_check_ms)
      {
        next_check_ms = earliest;
      }
      state.su_retry_after_ms = 0;
    }

    /* Poll, while no deployment is in flight. The SDK raises no event for a
     * successful "no update available", so nothing here waits on one. */
    if (poll_interval_s > 0 && !state.su_workflow_active && sample_now_ms() >= next_check_ms)
    {
      next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
      if (az_iot_su_client_request_update(&state.su_client, request_timeout_ms) != AZ_IOT_OK)
      {
        fprintf(stderr, "Could not request an update check.\n");
      }
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
