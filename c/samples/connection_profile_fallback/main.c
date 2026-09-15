// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* connection_profile_fallback - sample.
 *
 * For an application that must serve BOTH hub generations from one binary,
 * because it does not control which one its devices are provisioned to.
 *
 * Every other sample picks a side. telemetry_gen1 declares itself Classic
 * before it opens the connection and fails fast if the device turns out to be
 * on AEG; telemetry_gen2 does the mirror image. That is the right shape when
 * you know the answer, and it is why those samples are short.
 *
 * This one does not know. So it registers both adapters, opens the connection,
 * asks az_iot_connection_client_get_hub_profile() what it actually reached, and
 * builds the matching feature client only then. That costs the branch the other
 * samples were split to remove -- which is the point: this is what you take on
 * when one binary has to serve both, and what you get to drop when it does not.
 *
 * Telemetry is the feature used here only because its two clients have the same
 * API shape, so nothing distracts from the profile query itself.
 *
 * Provision via DPS, open, send one message, close. DPS is handled internally by
 * the connection client when host == NULL and dps.id_scope is set.
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
  /* Both, because which one is needed is not known until CONNECTED. Only one is
   * ever initialized; `profile` says which. */
  az_iot_gen1_telemetry_client gen1;
  az_iot_gen2_telemetry_client gen2;
  az_iot_connection_profile profile;
  int initialized;
} sample_state;

static void telemetry_destroy(sample_state* s)
{
  if (!s->initialized)
  {
    return;
  }
  if (s->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_gen2_telemetry_client_destroy(&s->gen2);
  }
  else
  {
    az_iot_gen1_telemetry_client_destroy(&s->gen1);
  }
  s->initialized = 0;
}

static void sample_state_destroy(sample_state* s)
{
  telemetry_destroy(s);
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  az_iot_connection_state conn_state;
  az_iot_result reason;
} user_context;

/* Build the client that matches what the connection actually reached. Called
 * once the profile is knowable, never before. */
static az_iot_result build_for_profile(sample_state* s, az_iot_connection_profile profile)
{
  telemetry_destroy(s);

  az_iot_result result;
  if (profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    result = az_iot_gen2_telemetry_client_init(&s->gen2, &s->connection_client);
  }
  else if (profile == AZ_IOT_CONNECTION_PROFILE_CLASSIC)
  {
    result = az_iot_gen1_telemetry_client_init(&s->gen1, &s->connection_client);
  }
  else
  {
    /* A profile this SDK does not implement. Nothing to build; the connection
     * has already failed, and the raw value is reported by the caller. */
    return AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED;
  }

  if (result == AZ_IOT_OK)
  {
    s->profile = profile;
    s->initialized = 1;
  }
  return result;
}

/* Read the profile and report what the service actually sent, whether or not
 * this SDK understood it. *out_profile is set only on AZ_IOT_OK.
 *
 * The CONNECTED event carries the same answer, but only this query is available
 * where the decision belongs: the callback runs nested inside do_work(), while
 * the code that owns the feature clients is still waiting to get control back. */
static az_iot_result report_profile(
    const az_iot_connection_client* conn,
    az_iot_connection_profile* out_profile)
{
  /* Not `= {0}`: the SDK stamps a size into this struct so it can tell which
   * version of it the application was compiled against, and an unstamped one is
   * rejected with AZ_IOT_ERR_INVALID_ARG. */
  az_iot_hub_profile profile = AZ_IOT_HUB_PROFILE_INIT;

  az_iot_result result = az_iot_connection_client_get_hub_profile(conn, &profile);
  if (result != AZ_IOT_OK)
  {
    printf("profile not available: %s\n", az_iot_result_to_string(result));
    return result;
  }

  const char* name = profile.connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
      ? "AEG (MQTT v5)"
      : profile.connection_profile == AZ_IOT_CONNECTION_PROFILE_CLASSIC ? "Classic (MQTT v3.1.1)"
                                                                        : "not known to this SDK";

  printf(
      "hub profile: %s, service sent \"%s\"%s\n",
      name,
      profile.connection_profile_raw ? profile.connection_profile_raw : "(none)",
      profile.connection_profile_raw_truncated ? " (truncated)" : "");

  /* The verbatim string is kept even for a value this SDK has no enum for, so
   * an older device can at least say what it was offered. Only trust it fully
   * when it was not truncated -- the buffer is
   * AZ_IOT_CONNECTION_PROFILE_RAW_BUF bytes. */
  if (profile.connection_profile == AZ_IOT_CONNECTION_PROFILE_UNKNOWN)
  {
    printf(
        "This build predates that profile. Upgrade the SDK, or pin the device to one it knows.\n");
  }

  *out_profile = profile.connection_profile;
  return AZ_IOT_OK;
}

/* Records where the connection ended up. Nothing is decided here. */
static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;
  ctx->reason = event->reason;
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  az_iot_result* out = (az_iot_result*)user_ctx;
  *out = status;
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
  copts.dps.id_scope = state.config.id_scope;
  copts.dps.registration_id = state.config.reg_id;
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_set_state_callback(&state.connection_client, on_conn_state, &user_ctx);

  /* Both adapters, because either could be the one needed. A single-generation
   * application registers only what its hub speaks -- one factory for Classic,
   * two for AEG, since the DPS leg is v3.1.1 either way. Here neither can be
   * left out. */
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

  /* No feature client is created here. That is the whole difference: a
   * single-generation application would init one now and let the connection
   * fail if the device turned out to be on the other hub. This one cannot
   * decide until it has connected. */
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

  /* The point of the sample: the connection is up, nothing is bound to it yet,
   * and only now does the application ask what it reached and build to match. */
  az_iot_connection_profile profile = AZ_IOT_CONNECTION_PROFILE_UNKNOWN;
  az_iot_result build_status = AZ_IOT_ERR_NOT_INITIALIZED;

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    if (report_profile(&state.connection_client, &profile) == AZ_IOT_OK)
    {
      build_status = build_for_profile(&state, profile);
    }
  }
  else if (user_ctx.reason == AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED)
  {
    /* The connection failed because the service named a generation this SDK
     * does not implement. The profile stays readable precisely so this can be
     * logged rather than guessed at. */
    printf("Connection refused: unsupported hub generation.\n");
    (void)report_profile(&state.connection_client, &profile);
  }

  if (build_status == AZ_IOT_OK)
  {
    static const uint8_t payload[] = "{\"temp\":23}";
    az_iot_telemetry_property props[] = {
      { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
      { AZ_IOT_MSG_PROP_MESSAGE_ID, "fallback-1" },
    };
    az_iot_telemetry_message msg = { 0 };
    msg.payload = payload;
    msg.payload_len = sizeof(payload) - 1;
    msg.properties = props;
    msg.properties_count = sizeof(props) / sizeof(props[0]);

    /* The one branch this sample exists to show. The message is identical
     * either way -- only the client it is handed to differs, and the SDK puts
     * those same properties on two different wires. */
    az_iot_result send_status = AZ_IOT_ERR_NOT_INITIALIZED;
    az_iot_result sent = state.profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
        ? az_iot_gen2_telemetry_client_send(&state.gen2, &msg, on_send_done, &send_status)
        : az_iot_gen1_telemetry_client_send(&state.gen1, &msg, on_send_done, &send_status);

    if (sent == AZ_IOT_OK)
    {
      for (int i = 0; i < 600 && send_status == AZ_IOT_ERR_NOT_INITIALIZED; ++i)
      {
        (void)az_iot_connection_client_do_work(&state.connection_client, 50);
      }
      if (send_status == AZ_IOT_OK)
      {
        rc = 0;
      }
    }
  }

  az_iot_connection_client_close(&state.connection_client);

  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
