// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/twin_get_patch - sample.
 *
 * Issue a twin GET and a reported-properties PATCH on every connect, for ~60
 * seconds, on whichever hub DPS assigns: Classic (gen1) or AEG (gen2),
 * including after the device is moved to a hub of the other generation. The
 * AEG-only route is gen2/twin_get_patch. See unified/telemetry for the shape
 * every unified sample shares: build for an assumed generation before open(),
 * rebuild when DPS assigns the other one.
 *
 * The two twin clients differ in protocol, not only in type, so each has its
 * own callbacks:
 *
 *   - GET: Classic returns one document; AEG returns the desired and reported
 *     sections separately, each with its own authoritative version.
 *   - PATCH: Classic reports the new version; AEG also reports the service's
 *     verdict. An AEG patch can complete and still be refused -- another writer
 *     moved the version first -- so a run only succeeds when the verdict is OK.
 *   - The AEG client frames patches into protobuf in an application-supplied
 *     buffer, and needs its own do_work() in the pump.
 *
 * DPS is handled internally by the connection client when dps.id_scope is set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

/** @brief How long the sample runs. */
#define SAMPLE_RUN_MS 60000u

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  /* Only the one matching `profile` is ever initialized. */
  az_iot_gen1_twin_client gen1;
  az_iot_gen2_twin_client gen2;
  az_iot_connection_profile profile;
  int twin_initialized;
} sample_state;

typedef struct
{
  az_iot_connection_state conn_state;
  int connected_count;
  int faulted; /* terminal: nothing this sample can do about it */
  int rebuild; /* DPS assigned `assigned_profile`; rebuild for it */
  az_iot_connection_profile assigned_profile;
  int get_done;
  int patch_done;
  int desired_count;
  az_iot_result get_status;
  az_iot_result patch_status;
  /* AEG only: the service's verdict, separate from patch_status. Classic has
   * none, so its patch sets it to OK once the exchange completes. */
  az_iot_gen2_twin_patch_status patch_verdict;
  uint64_t patch_version;
} user_context;

static void clients_destroy(sample_state* s)
{
  if (!s->twin_initialized)
  {
    return;
  }
  if (s->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_gen2_twin_client_deinit(&s->gen2);
  }
  else
  {
    az_iot_gen1_twin_client_deinit(&s->gen1);
  }
  s->twin_initialized = 0;
}

static void sample_state_destroy(sample_state* s)
{
  clients_destroy(s);
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

/* AEG frames a reported patch into protobuf before it goes out, and the SDK does
 * not allocate, so the application supplies the scratch for it. */
static uint8_t s_twin_encode_buffer[256];

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;

  /* Only record it: feature clients must not be destroyed from inside the
   * callback, which runs nested in do_work(). */
  if (sample_event_is_profile_mismatch(event, &ctx->assigned_profile))
  {
    ctx->rebuild = 1;
    return;
  }

  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    ctx->faulted = 1;
    /* A generation newer than this SDK; the verbatim value is kept. */
    if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED && event->profile)
    {
      printf(
          "Unsupported hub generation \"%s\". Upgrade the SDK.\n",
          event->profile->connection_profile_raw);
    }
  }

  /* The provisioning lifecycle reports on its own scope; only the hub one says
   * whether the feature client can work. */
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      ctx->connected_count++;
    }
    ctx->conn_state = event->state;
  }
}

/* ---- Classic (gen1) callbacks ---------------------------------------------- */

static void on_desired_gen1(
    const uint8_t* patch,
    size_t patch_len,
    uint64_t version,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->desired_count++;
  printf(
      "twin desired (version %llu): %.*s\n",
      (unsigned long long)version,
      (int)patch_len,
      (const char*)patch);
}

static void on_get_gen1(az_iot_result status, const uint8_t* body, size_t len, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->get_status = status;
  ctx->get_done = 1;
  if (status == AZ_IOT_OK)
  {
    printf("twin GET: %.*s\n", (int)len, (const char*)body);
  }
}

static void on_patch_gen1(az_iot_result status, uint64_t version, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->patch_status = status;
  ctx->patch_verdict = AZ_IOT_GEN2_TWIN_PATCH_OK;
  ctx->patch_version = version;
  ctx->patch_done = 1;
}

/* ---- AEG (gen2) callbacks -------------------------------------------------- */

/* A SNAPSHOT replaces local desired state; a PATCH merges onto it. */
static void on_desired_gen2(
    az_iot_gen2_twin_desired_kind kind,
    uint64_t version,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->desired_count++;
  printf(
      "twin desired %s (version %llu): %.*s\n",
      kind == AZ_IOT_GEN2_TWIN_DESIRED_SNAPSHOT ? "snapshot" : "patch",
      (unsigned long long)version,
      (int)payload_len,
      (const char*)payload);
}

static void on_get_gen2(az_iot_result status, const az_iot_gen2_twin_state* twin, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->get_status = status;
  ctx->get_done = 1;
  if (status == AZ_IOT_OK && twin)
  {
    printf(
        "twin GET desired (version %llu): %.*s\n",
        (unsigned long long)twin->desired.version,
        (int)twin->desired.payload_len,
        twin->desired.payload ? (const char*)twin->desired.payload : "");
    printf(
        "twin GET reported (version %llu): %.*s\n",
        (unsigned long long)twin->reported.version,
        (int)twin->reported.payload_len,
        twin->reported.payload ? (const char*)twin->reported.payload : "");
  }
}

static void on_patch_gen2(
    az_iot_result status,
    const az_iot_gen2_twin_patch_result* result,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->patch_status = status;
  ctx->patch_verdict = result ? result->status : AZ_IOT_GEN2_TWIN_PATCH_UNSPECIFIED;
  ctx->patch_version = result ? result->version : 0;
  ctx->patch_done = 1;
  if (status == AZ_IOT_OK && result && result->status != AZ_IOT_GEN2_TWIN_PATCH_OK)
  {
    printf(
        "twin patch refused (status %d, current version %llu)\n",
        (int)result->status,
        (unsigned long long)result->version);
  }
}

/* Pins the connection to `profile`: an assignment to the other generation is
 * then refused with AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH. */
static az_iot_result clients_build(
    sample_state* s,
    az_iot_connection_profile profile,
    user_context* ctx)
{
  az_iot_result result;
  switch (profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      result = az_iot_gen2_twin_client_init(&s->gen2, &s->connection_client);
      if (result != AZ_IOT_OK)
      {
        return result;
      }
      s->profile = profile;
      s->twin_initialized = 1;
      result = az_iot_gen2_twin_client_set_encode_buffer(
          &s->gen2, AZ_SPAN_FROM_BUFFER(s_twin_encode_buffer));
      if (result == AZ_IOT_OK)
      {
        result = az_iot_gen2_twin_client_set_desired_handler(&s->gen2, on_desired_gen2, ctx);
      }
      return result;
    case AZ_IOT_CONNECTION_PROFILE_CLASSIC:
      result = az_iot_gen1_twin_client_init(&s->gen1, &s->connection_client);
      if (result != AZ_IOT_OK)
      {
        return result;
      }
      s->profile = profile;
      s->twin_initialized = 1;
      return az_iot_gen1_twin_client_set_desired_handler(&s->gen1, on_desired_gen1, ctx);
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
    default:
      return AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED;
  }
}

/* One pump step. The AEG twin client has timers of its own to drive. */
static void pump(sample_state* s, uint32_t timeout_ms)
{
  (void)az_iot_connection_client_do_work(&s->connection_client, timeout_ms);
  if (s->twin_initialized && s->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    (void)az_iot_gen2_twin_client_do_work(&s->gen2);
  }
}

/* GET + PATCH, answered through the callbacks. A request that was never queued
 * has no callback coming, so it completes here. */
static void start_round(sample_state* s, user_context* ctx)
{
  static const uint8_t patch[] = "{\"sample\":\"hello\"}";
  az_iot_result get_rc;
  az_iot_result patch_rc;

  ctx->get_done = 0;
  ctx->patch_done = 0;
  if (s->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    get_rc = az_iot_gen2_twin_client_get(&s->gen2, on_get_gen2, ctx);
    patch_rc = az_iot_gen2_twin_client_patch_reported(
        &s->gen2, patch, sizeof(patch) - 1, on_patch_gen2, ctx);
  }
  else
  {
    get_rc = az_iot_gen1_twin_client_get(&s->gen1, on_get_gen1, ctx);
    patch_rc = az_iot_gen1_twin_client_patch_reported(
        &s->gen1, patch, sizeof(patch) - 1, on_patch_gen1, ctx);
  }
  if (get_rc != AZ_IOT_OK)
  {
    ctx->get_status = get_rc;
    ctx->get_done = 1;
  }
  if (patch_rc != AZ_IOT_OK)
  {
    ctx->patch_status = patch_rc;
    ctx->patch_done = 1;
  }
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sample_state state = { 0 };
  if (sample_config_load(&state.config) != 0)
  {
    return 1;
  }

  user_context user_ctx = { 0 };

  /* Certificate provider */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = state.config.ca;
  pem.client_cert_pem_path = state.config.cert;
  pem.client_key_pem_path = state.config.key;

  if (az_iot_certificate_provider_pem_init(&state.certs, &pem) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Connection client (DPS provisioning is internal). The default reconnection
   * policy is what re-provisions a device its hub no longer accepts. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state.config);
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  /* Both adapters: v3.1.1 serves DPS and a Classic hub, v5 serves an AEG hub. */
  if (az_iot_connection_client_register_mqtt_factory(
          &state.connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  if (az_iot_connection_client_register_mqtt_factory(
          &state.connection_client, az_iot_paho_factory_create_v5())
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Assume Classic until DPS says otherwise; see unified/telemetry. */
  if (clients_build(&state, AZ_IOT_CONNECTION_PROFILE_CLASSIC, &user_ctx) != AZ_IOT_OK
      || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  uint64_t end_ms = sample_now_ms() + SAMPLE_RUN_MS;
  int rounds = 0; /* connects already served */
  int round_active = 0;
  int rounds_ok = 0;
  while (!user_ctx.faulted && sample_now_ms() < end_ms)
  {
    pump(&state, 100);

    if (user_ctx.rebuild)
    {
      user_ctx.rebuild = 0;
      printf(
          "DPS assigned %s; rebuilding the feature client.\n",
          sample_connection_profile_name(user_ctx.assigned_profile));
      clients_destroy(&state);
      round_active = 0; /* lost with the old client */
      az_iot_connection_client_close(&state.connection_client);
      if (clients_build(&state, user_ctx.assigned_profile, &user_ctx) != AZ_IOT_OK
          || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
      {
        break;
      }
      continue;
    }

    /* Every connect, including one to a rebuilt client, refreshes the twin. */
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED && user_ctx.connected_count > rounds)
    {
      rounds = user_ctx.connected_count;
      round_active = 1;
      start_round(&state, &user_ctx);
    }

    if (round_active && user_ctx.get_done && user_ctx.patch_done)
    {
      round_active = 0;
      printf(
          "twin_get: %s, patch_reported: %s version=%llu\n",
          az_iot_result_to_string(user_ctx.get_status),
          az_iot_result_to_string(user_ctx.patch_status),
          (unsigned long long)user_ctx.patch_version);
      /* A refused write is a failure: the exchange completing is not the same
       * as the twin being updated. */
      if (user_ctx.get_status == AZ_IOT_OK && user_ctx.patch_status == AZ_IOT_OK
          && user_ctx.patch_verdict == AZ_IOT_GEN2_TWIN_PATCH_OK)
      {
        rounds_ok++;
      }
    }
  }

  int rc = (rounds_ok > 0 && !user_ctx.faulted) ? 0 : 1;

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    pump(&state, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
