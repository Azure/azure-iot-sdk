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
 * See the companion README.md and docs/eng/su-client-design.md.
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
 *                                   60); 0 checks once, at startup.
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

/* Microsoft's software updates production root keys, compiled into the SDK,
 * anchor manifest trust. To accept updates signed by your OWN root, build an
 * az_iot_su_root_key array (kid + big-endian modulus/exponent) and pass it to
 * az_iot_su_client_initialize() instead. */

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo)
{
  (void)signo;
  g_stop = 1;
}

/* What the observers tell the pump loop. */
typedef struct
{
  az_iot_connection_state conn[AZ_IOT_CONN_SCOPE_COUNT];
  int faulted; /* a lifecycle settled at FAULTED: unrecoverable here */
  int workflow_active; /* a deployment is in flight */
  int workflow_completed; /* one finished and returned to Idle */
  int workflow_succeeded; /* ...and it was applied successfully */
  uint32_t retry_after_ms; /* the service asked to wait before asking again */
  int registered; /* the hub connected at least once, so a device record exists */
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
  AZ_IOT_SU_DEVICE_PROPERTIES_STORAGE(dp_buffer);

  char* manufacturer;
  char* model;
  /* What is installed now: sent on every check and read by is_installed_fn. */
  char installed_provider[SU_SIM_ID_PART_SIZE];
  char installed_name[SU_SIM_ID_PART_SIZE];
  char installed_version[SU_SIM_ID_PART_SIZE];
  az_iot_su_report_update_id installed;
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
}

/* Copies @p src into @p dst; 0 when it does not fit. */
static int copy_str(char* dst, size_t dst_size, const char* src)
{
  size_t n = strlen(src);
  if (n >= dst_size)
  {
    return 0;
  }
  memcpy(dst, src, n + 1);
  return 1;
}

/* Reads @p name, or @p fallback when unset, into @p dst; 0 when out of memory
 * or too long. */
static int env_to_buffer(const char* name, const char* fallback, char* dst, size_t dst_size)
{
  char* v = sample_env_dup(name, fallback);
  int ok = (v != NULL) && copy_str(dst, dst_size, v);
  if (v != NULL && !ok)
  {
    fprintf(stderr, "%s is too long (max %zu characters).\n", name, dst_size - 1);
  }
  free(v);
  return ok;
}

static void fill_device_properties(const sample_state* s, az_iot_su_device_properties* dp)
{
  memset(dp, 0, sizeof(*dp));
  dp->manufacturer = s->manufacturer;
  dp->model = s->model;
  dp->installed_update_id.provider = s->installed_provider;
  dp->installed_update_id.name = s->installed_name;
  dp->installed_update_id.version = s->installed_version;
}

/* Both lifecycles report here. `state` is meaningless without `scope`. */
static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_run* run = (sample_run*)user_ctx;

  /* `reason` is the last field read here, and `scope` indexes an array. */
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

  if (scope == AZ_IOT_CONN_SCOPE_HUB && event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    run->registered = 1;
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
        /* The engine returns to Idle from ApplyStarted only when the last step
         * applied; failures pass through Failed, a skip leaves from
         * VerifyingManifest. */
        run->workflow_succeeded = (event->previous_state == AZ_IOT_SU_STATE_APPLY_STARTED);
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
        if (!run->registered)
        {
          fprintf(
              stderr,
              "A device that has never registered has no device record yet; it "
              "registers now, and a later check can succeed.\n");
        }
        run->retry_after_ms = event->service_error.retry_after_ms;
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

  if (su_sim_init(&st.sim) != 0)
  {
    return 1;
  }

  long poll_interval_s = su_sample_env_long("AZ_IOT_SU_POLL_INTERVAL_S", 60);
  if (poll_interval_s < 0)
  {
    poll_interval_s = 60;
  }

  if (sample_config_load(&st.config) != 0)
  {
    su_sim_deinit(&st.sim);
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

  /* Connection client. No dps.provision_only: the device registers and
   * connects to its hub, which gives it the device record the regular route
   * needs. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &st.config);
  copts.certificate_provider = &st.certs.base;
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

  /* Device identity. */
  st.manufacturer = sample_env_dup("AZ_IOT_SU_MANUFACTURER", "Contoso");
  st.model = sample_env_dup("AZ_IOT_SU_MODEL", "SU-Sim");
  if (st.manufacturer == NULL || st.model == NULL
      || !env_to_buffer(
          "AZ_IOT_SU_INSTALLED_PROVIDER",
          "Contoso",
          st.installed_provider,
          sizeof(st.installed_provider))
      || !env_to_buffer(
          "AZ_IOT_SU_INSTALLED_NAME", "SU-Sim", st.installed_name, sizeof(st.installed_name))
      || !env_to_buffer(
          "AZ_IOT_SU_INSTALLED_VERSION",
          "1.0.0",
          st.installed_version,
          sizeof(st.installed_version)))
  {
    sample_state_destroy(&st);
    return 1;
  }
  st.installed.provider = st.installed_provider;
  st.installed.name = st.installed_name;
  st.installed.version = st.installed_version;
  st.sim.installed = &st.installed;

  az_iot_su_device_properties dp;
  fill_device_properties(&st, &dp);

  size_t dp_needed = az_iot_su_device_properties_buffer_size(&dp);
  if (dp_needed == 0 || dp_needed > sizeof(st.dp_buffer))
  {
    fprintf(
        stderr,
        "Device properties are invalid or too long for the %zu-byte cache. Need "
        "1-%d compatibility properties and a nonempty installed provider, name and "
        "version.\n",
        sizeof(st.dp_buffer),
        AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES);
    sample_state_destroy(&st);
    return 1;
  }

  /* Software updates client. */
  az_iot_su_platform_hooks hooks = su_sim_hooks(&st.sim);
  az_iot_su_crypto_hooks crypto = az_iot_su_crypto_openssl_hooks();
  size_t root_key_count = 0;
  const az_iot_su_root_key* root_keys = az_iot_su_microsoft_root_keys(&root_key_count);

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

  if (az_iot_su_client_resume(&st.su_client) != AZ_IOT_OK)
  {
    fprintf(stderr, "Could not resume the persisted workflow; starting from Idle.\n");
  }

  /* First check, asked before open(): the SDK holds registration until the
   * check on the first provisioning session reaches a verdict, so asking now
   * lets the device register without waiting out that hold. Later checks run
   * on a provisioning session the SDK reopens on demand. */
  if (az_iot_su_client_request_update(&st.su_client, AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS)
      != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }

  printf(
      "Matched against a deployed update: manufacturer=%s model=%s\n"
      "Installed: %s/%s/%s\n",
      st.manufacturer,
      st.model,
      st.installed_provider,
      st.installed_name,
      st.installed_version);
  if (poll_interval_s > 0)
  {
    printf("Checking for updates every %ld s.\n", poll_interval_s);
  }

  if (az_iot_connection_client_open(&st.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&st);
    return 1;
  }

  uint64_t next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;

  rc = 0; /* interrupted or asked to reboot is a normal exit */
  while (!g_stop)
  {
    (void)az_iot_connection_client_do_work(&st.connection_client, 50);
    (void)az_iot_su_client_do_work(&st.su_client);

    if (!st.run.announced && st.run.conn[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTED)
    {
      st.run.announced = 1;
      printf("Hub connection up. Running (Ctrl-C to exit)...\n");
    }

    if (st.run.workflow_completed)
    {
      st.run.workflow_completed = 0;
      remove(st.sim.state_file); /* clear the resume blob */

      /* Report the applied update as installed. Without this the next check
       * still sends the previous id, and is offered the same update again. */
      if (st.run.workflow_succeeded && st.sim.applied_valid)
      {
        az_iot_su_device_properties next;
        fill_device_properties(&st, &next);
        next.installed_update_id.provider = st.sim.applied_provider;
        next.installed_update_id.name = st.sim.applied_name;
        next.installed_update_id.version = st.sim.applied_version;
        /* applied_* and installed_* have the same capacity, so the copies fit. */
        if (az_iot_su_client_update_device_properties(&st.su_client, &next) == AZ_IOT_OK
            && copy_str(
                st.installed_provider, sizeof(st.installed_provider), st.sim.applied_provider)
            && copy_str(st.installed_name, sizeof(st.installed_name), st.sim.applied_name)
            && copy_str(st.installed_version, sizeof(st.installed_version), st.sim.applied_version))
        {
          printf(
              "Update applied. Installed: %s/%s/%s\n",
              st.installed_provider,
              st.installed_name,
              st.installed_version);
        }
        else
        {
          fprintf(stderr, "Could not record the applied update as installed.\n");
        }
      }
      st.run.workflow_succeeded = 0;
      printf("Deployment workflow complete.\n");
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

    /* FAULTED is settled: the SDK does not retry out of it. A workflow in
     * flight is never interrupted for it. */
    if (st.run.faulted && !st.run.workflow_active)
    {
      fprintf(stderr, "The connection faulted and will not recover on its own.\n");
      rc = 1;
      break;
    }

    /* The service's requested delay, when longer, pushes the next check. */
    if (st.run.retry_after_ms > 0)
    {
      uint64_t earliest = sample_now_ms() + st.run.retry_after_ms;
      if (earliest > next_check_ms)
      {
        next_check_ms = earliest;
      }
      st.run.retry_after_ms = 0;
    }

    /* Poll, while no deployment is in flight. The SDK raises no event for a
     * successful "no update available", so nothing here waits on one. */
    if (poll_interval_s > 0 && !st.run.workflow_active && sample_now_ms() >= next_check_ms)
    {
      next_check_ms = sample_now_ms() + (uint64_t)poll_interval_s * 1000u;
      if (az_iot_su_client_request_update(&st.su_client, AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS)
          != AZ_IOT_OK)
      {
        fprintf(stderr, "Could not request an update check.\n");
      }
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
