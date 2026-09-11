// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* twin_get_patch - sample.
 *
 * Provision via DPS, open connection, issue twin GET + PATCH reported, close.
 * DPS is handled internally by the connection client when host == NULL and
 * dps.id_scope is set.
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
  az_iot_gen1_twin_client gen1_twin;
  az_iot_gen2_twin_client gen2_twin;
  az_iot_connection_profile twin_profile;
  int twin_initialized;
} sample_state;

static void twin_destroy(sample_state* s)
{
  if (!s->twin_initialized)
  {
    return;
  }
  if (s->twin_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_gen2_twin_client_destroy(&s->gen2_twin);
  }
  else
  {
    az_iot_gen1_twin_client_destroy(&s->gen1_twin);
  }
  s->twin_initialized = 0;
}

static void sample_state_destroy(sample_state* s)
{
  twin_destroy(s);
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  sample_state* state;
  az_iot_connection_state conn_state;
  az_iot_result twin_status;
  int get_done;
  int patch_done;
  az_iot_result get_status;
  az_iot_result patch_status;
  uint64_t patch_version;
} user_context;

/* The generation is only known once CONNECTED reports the resolved profile,
 * and a reconnect can land on the other one -- so the client is built from the
 * transition rather than constructed once up front. */
static az_iot_result twin_rebuild(sample_state* s, az_iot_connection_profile profile)
{
  twin_destroy(s);

  az_iot_result result;
  if (profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    result = az_iot_gen2_twin_client_init(&s->gen2_twin, &s->connection_client);
  }
  else if (profile == AZ_IOT_CONNECTION_PROFILE_CLASSIC)
  {
    result = az_iot_gen1_twin_client_init(&s->gen1_twin, &s->connection_client);
  }
  else
  {
    return AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED;
  }

  if (result == AZ_IOT_OK)
  {
    s->twin_profile = profile;
    s->twin_initialized = 1;
  }
  return result;
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;
  if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    ctx->twin_status = event->profile ? twin_rebuild(ctx->state, event->profile->connection_profile)
                                      : AZ_IOT_ERR_INTERNAL;
  }
}

static void on_get(az_iot_result status, const uint8_t* body, size_t len, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->get_status = status;
  ctx->get_done = 1;
  if (status == AZ_IOT_OK)
  {
    printf("twin GET: %.*s\n", (int)len, (const char*)body);
  }
}

static void on_patch(az_iot_result status, uint64_t version, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->patch_status = status;
  ctx->patch_version = version;
  ctx->patch_done = 1;
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
  user_ctx.state = &state;

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
  copts.dps.id_scope = state.config.id_scope;
  copts.dps.registration_id = state.config.reg_id;
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_set_state_callback(&state.connection_client, on_conn_state, &user_ctx);

  /* MQTT adapters: register both v3.1.1 (DPS + Classic) and v5 (Next).
   * The connection client selects the appropriate factory based on the
   * session role. Both may be backed by different MQTT libraries. */
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

  /* The twin client is built from the CONNECTED transition, once the resolved
   * profile says which generation this hub speaks. */

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

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED && state.twin_initialized)
  {
    /* Issue twin GET (the return is the send/queue status; the twin data
     * arrives asynchronously via on_get, checked below). */
    static const uint8_t patch[] = "{\"sample\":\"hello\"}";
    az_iot_result get_rc;
    az_iot_result patch_rc;

    if (state.twin_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
    {
      get_rc = az_iot_gen2_twin_client_get(&state.gen2_twin, on_get, &user_ctx);
      patch_rc = az_iot_gen2_twin_client_patch_reported(
          &state.gen2_twin, patch, sizeof(patch) - 1, on_patch, &user_ctx);
    }
    else
    {
      get_rc = az_iot_gen1_twin_client_get(&state.gen1_twin, on_get, &user_ctx);
      patch_rc = az_iot_gen1_twin_client_patch_reported(
          &state.gen1_twin, patch, sizeof(patch) - 1, on_patch, &user_ctx);
    }
    (void)get_rc;
    (void)patch_rc;

    /* Pump until both responses arrive */
    for (int i = 0; i < 600 && (!user_ctx.get_done || !user_ctx.patch_done); ++i)
    {
      (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    }

    if (user_ctx.get_done && user_ctx.get_status == AZ_IOT_OK && user_ctx.patch_done
        && user_ctx.patch_status == AZ_IOT_OK)
    {
      rc = 0;
    }

    printf(
        "twin_get:       done=%d status=%s\n",
        user_ctx.get_done,
        az_iot_result_to_string(user_ctx.get_status));
    printf(
        "patch_reported: done=%d status=%s version=%llu\n",
        user_ctx.patch_done,
        az_iot_result_to_string(user_ctx.patch_status),
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
