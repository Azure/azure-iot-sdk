// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* twin_get_patch_gen2 - sample.
 *
 * Issue a twin GET and a reported-properties PATCH against an AEG (Hub-Next)
 * hub over MQTT v5. The Classic route is a separate sample,
 * twin_get_patch_gen1; an application that must serve either hub picks at
 * runtime -- see connection_profile_fallback.
 *
 * The client is created before the connection opens, because
 * az_iot_gen2_twin_client_init() records the generation it needs rather than
 * reading one off a live connection. A sample that served both generations
 * could not do that: it would have to wait for CONNECTED, read the resolved
 * profile, and only then build the right client -- and rebuild it whenever a
 * reconnect landed on the other generation.
 *
 * Two adapters are registered, which the Classic sample does not need: the hub
 * leg is MQTT v5, but the DPS leg is still v3.1.1. Registering only the v5
 * factory makes provisioning fail with AZ_IOT_ERR_NOT_SUPPORTED before the hub
 * is ever reached.
 *
 * Two things differ from gen1 beyond the client type, and both are protocol
 * rather than API taste:
 *
 *   - GET returns the desired and reported sections separately, each with its
 *     own authoritative version, so it has its own callback shape.
 *   - PATCH reports the service's verdict as well as a transport status. A
 *     patch can complete and still be refused -- another writer moved the
 *     version first -- so a run is only successful when the verdict is OK.
 *
 * Provision via DPS, open, GET + PATCH, close. DPS is handled internally by the
 * connection client when host == NULL and dps.id_scope is set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_gen2_twin_client twin;
  int twin_initialized;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  if (s->twin_initialized)
  {
    az_iot_gen2_twin_client_deinit(&s->twin);
    s->twin_initialized = 0;
  }
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  az_iot_connection_state conn_state;
  int get_done;
  int patch_done;
  int desired_count;
  az_iot_result get_status;
  az_iot_result patch_status;
  /* The service's verdict, which is separate from patch_status: the exchange
   * can complete (AZ_IOT_OK) while the write itself is refused. */
  az_iot_gen2_twin_patch_status patch_verdict;
  uint64_t patch_version;
} user_context;

/* A reported patch is framed into protobuf before it goes out, and the SDK does
 * not allocate, so the application supplies the scratch for it. */
static uint8_t s_twin_encode_buffer[256];

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  /* Hub lifecycle only: the provisioning session reports on its own scope,
   * and storing its state here would overwrite the hub state this code acts
   * on. */
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }

  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;

  /* Pinned to AEG, so an assignment to the other generation is terminal here
   * rather than something to rebuild for: the twin client cannot be re-pinned
   * without being destroyed, and this sample has nothing else to be. An
   * application that must survive a reassignment is connection_profile_fallback. */
  if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH)
  {
    printf("Provisioned to a Classic hub; this AEG-only sample cannot serve it. "
           "Use twin_get_patch_gen1.\n");
  }
}

static void on_desired(const uint8_t* patch, size_t patch_len, uint64_t version, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->desired_count++;
  printf(
      "twin desired (version %llu): %.*s\n",
      (unsigned long long)version,
      (int)patch_len,
      (const char*)patch);
}

/* gen2 returns the two twin sections separately, each with its own
 * authoritative version. */
static void on_get(az_iot_result status, const az_iot_gen2_twin_state* twin, void* user_ctx)
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

/* gen2 reports the service's verdict rather than just a version: a patch can
 * complete yet be refused, when another writer moved the version first. */
static void on_patch(
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

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sample_state state = { 0 };
  if (sample_config_load(&state.config) != 0)
  {
    return 1;
  }

  int rc = 1;
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

  /* Connection client (DPS provisioning is internal when host==NULL) */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state.config);
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  /* Both legs: the hub is v5, but DPS is still v3.1.1. */
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

  /* Before open(): this records the generation the sample needs, so an
   * assignment to the other one is refused rather than discovered later. */
  if (az_iot_gen2_twin_client_init(&state.twin, &state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.twin_initialized = 1;

  if (az_iot_gen2_twin_client_set_desired_handler(&state.twin, on_desired, &user_ctx) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  if (az_iot_gen2_twin_client_set_encode_buffer(
          &state.twin, s_twin_encode_buffer, sizeof(s_twin_encode_buffer))
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Open (internally provisions via DPS then connects to assigned hub) */
  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    /* The return is the send/queue status; the twin data arrives
     * asynchronously via on_get / on_patch, checked below. */
    static const uint8_t patch[] = "{\"sample\":\"hello\"}";
    az_iot_result get_rc = az_iot_gen2_twin_client_get(&state.twin, on_get, &user_ctx);
    az_iot_result patch_rc = az_iot_gen2_twin_client_patch_reported(
        &state.twin, patch, sizeof(patch) - 1, on_patch, &user_ctx);
    (void)get_rc;
    (void)patch_rc;

    /* Pump until both responses arrive */
    for (int i = 0; i < 600 && (!user_ctx.get_done || !user_ctx.patch_done); ++i)
    {
      (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    }

    /* A refused write is a failed sample run: the exchange completing is not
     * the same as the twin being updated. */
    if (user_ctx.get_done && user_ctx.get_status == AZ_IOT_OK && user_ctx.patch_done
        && user_ctx.patch_status == AZ_IOT_OK
        && user_ctx.patch_verdict == AZ_IOT_GEN2_TWIN_PATCH_OK)
    {
      rc = 0;
    }

    printf(
        "twin_get:       done=%d status=%s\n",
        user_ctx.get_done,
        az_iot_result_to_string(user_ctx.get_status));
    printf(
        "patch_reported: done=%d status=%s verdict=%d version=%llu\n",
        user_ctx.patch_done,
        az_iot_result_to_string(user_ctx.patch_status),
        (int)user_ctx.patch_verdict,
        (unsigned long long)user_ctx.patch_version);
  }

  /* Close connection */
  az_iot_connection_client_close(&state.connection_client);

  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
