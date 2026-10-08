// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Recovery after the hub refuses the device identity (opts.identity_recovery):
 * mqttv3 and mqttv5, CONNACK rejection and mid-session DISCONNECT, direct and
 * DPS-provisioned, the bounds of the ladder, explicit re-provisioning, and what
 * the state events report. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "support/test_provider.h"

#include "internal/connection_client_internal.h"

#include "support/connection_test_harness.h"

#define HUB_HOST "myhub.azure-devices.net"
#define DPS_HOST "global.azure-devices-provisioning.net"

/* Distinct delays, so the ladder a retry climbed is visible in the event. */
#define RECONNECT_MS 20u
#define IDENTITY_MS 30u

#define DPS_RESPONSE_TOPIC_ASSIGNED "$dps/registrations/res/200/?$rid=1"

static const char k_assigned_body[]
    = "{\"operationId\":\"op-1\",\"status\":\"assigned\","
      "\"registrationState\":{\"registrationId\":\"ut-device\","
      "\"assignedHub\":\"" HUB_HOST "\",\"deviceId\":\"assigned-device\"}}";

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

static az_iot_connection_client_options base_options(void)
{
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  opts.reconnection_policy.initial_delay_ms = RECONNECT_MS;
  opts.reconnection_policy.max_delay_ms = RECONNECT_MS;
  opts.reconnection_policy.jitter_pct = 0;
  opts.identity_recovery.policy.initial_delay_ms = IDENTITY_MS;
  opts.identity_recovery.policy.max_delay_ms = IDENTITY_MS;
  opts.identity_recovery.policy.jitter_pct = 0;
  opts.identity_recovery.mode = AZ_IOT_IDENTITY_RECOVERY_RETRY_HUB;
  return opts;
}

static int setup_opts(
    void** state,
    const az_iot_connection_client_options* opts,
    az_iot_mqtt_version v)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);
  assert_int_equal(az_iot_test_connection_client_init(&fx->client_storage, opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);
  az_iot_connection_client__seed_rng(fx->client, 0xC0FFEEFEEDFACEull);
  fx->factory = az_iot_mock_mqtt_factory_create(v);
  assert_non_null(fx->factory);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  *state = fx;
  return 0;
}

static int setup_v3(void** state)
{
  az_iot_connection_client_options opts = base_options();
  opts.host = HUB_HOST;
  return setup_opts(state, &opts, AZ_IOT_MQTT_VERSION_3_1_1);
}

static int setup_v5(void** state)
{
  az_iot_connection_client_options opts = base_options();
  opts.host = HUB_HOST;
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  return setup_opts(state, &opts, AZ_IOT_MQTT_VERSION_5);
}

static int setup_dps(void** state)
{
  az_iot_connection_client_options opts = base_options();
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  return setup_opts(state, &opts, AZ_IOT_MQTT_VERSION_3_1_1);
}

static int teardown(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  if (fx)
  {
    az_iot_connection_client_deinit(&fx->client_storage);
    free(fx);
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void pump(az_iot_test_conn* fx, int n)
{
  for (int i = 0; i < n; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
}

static const char* last_connect_host(az_iot_test_conn* fx)
{
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  return c->connect.host;
}

/* Index of the newest event for (scope, state). */
static size_t last_index_of(
    const az_iot_test_state_log* log,
    az_iot_connection_scope scope,
    az_iot_connection_state state)
{
  size_t found = SIZE_MAX;
  for (size_t i = log->count; i > 0 && found == SIZE_MAX; --i)
  {
    if (log->scopes[i - 1] == scope && log->states[i - 1] == state)
    {
      found = i - 1;
    }
  }
  assert_int_not_equal(found, SIZE_MAX);
  return found;
}

static void reject_identity(az_iot_test_conn* fx)
{
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_IDENTITY_REJECTED));
  pump(fx, 2);
}

/* Wait out the pending retry and fire it. */
static void fire_retry(az_iot_test_conn* fx)
{
  assert_int_not_equal(fx->client->reconnect_due_ms, 0);
  az_iot_test_wait_until_ms(fx->client->reconnect_due_ms);
  pump(fx, 1);
}

/* The HUB:RETRY_PENDING event the newest refusal produced. */
static const az_iot_connection_recovery_info* last_hub_recovery(
    az_iot_test_conn* fx,
    az_iot_connection_state state)
{
  size_t i = last_index_of(&fx->log, AZ_IOT_CONN_SCOPE_HUB, state);
  assert_true(fx->log.recovery_present[i]);
  return &fx->log.recovery[i];
}

static void provision(az_iot_test_conn* fx)
{
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(dps);
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps, AZ_IOT_OK));
  pump(fx, 1);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(dps, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(dps, sub->packet_id, AZ_IOT_OK));
  pump(fx, 1);
  assert_true(az_iot_mock_mqtt_client_inject_message(
      dps,
      DPS_RESPONSE_TOPIC_ASSIGNED,
      (const uint8_t*)k_assigned_body,
      strlen(k_assigned_body),
      AZ_IOT_MQTT_QOS_1));
  pump(fx, 5);
  assert_string_equal(last_connect_host(fx), HUB_HOST);
}

/* mqttv5: CONNACK -> presence SUBACK -> birth-ack -> HUB:CONNECTED. */
static void connect_v5(az_iot_test_conn* fx)
{
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  pump(fx, 1);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  pump(fx, 1);
  const az_iot_mock_call* birth = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(birth);

  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/ut-device/dev/presence";
  ack_msg.correlation_data = nonce;
  ack_msg.correlation_data_len = sizeof(nonce);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &ack));
  pump(fx, 1);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);
}

static void inject_v5_disconnect(az_iot_test_conn* fx, az_iot_result status, int32_t reason_code)
{
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  az_iot_mqtt_event evt;
  memset(&evt, 0, sizeof(evt));
  evt.kind = AZ_IOT_MQTT_EVT_DISCONNECTED;
  evt.status = status;
  evt.protocol_code = reason_code;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &evt));
  pump(fx, 1);
}

/* ------------------------------------------------------------------------- */
/* mqttv3 / mqttv5: the cached hub is retried on the identity ladder          */
/* ------------------------------------------------------------------------- */

static void mqttv3_connack_rejection_retries_the_hub_on_the_identity_ladder(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  reject_identity(fx);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RETRY_PENDING);
  const az_iot_connection_recovery_info* r = last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(r->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(r->attempt, 1);
  assert_int_equal(r->next_attempt_delay_ms, IDENTITY_MS);
  assert_false(r->next_attempt_reprovisions);
  size_t i = last_index_of(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_string_equal(fx->log.recovery_endpoint[i], HUB_HOST);
  assert_int_equal(fx->log.reasons[i], AZ_IOT_ERR_IDENTITY_REJECTED);

  fire_retry(fx);
  assert_string_equal(last_connect_host(fx), HUB_HOST);
}

static void mqttv5_connack_rejection_retries_the_hub_on_the_identity_ladder(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  reject_identity(fx);
  const az_iot_connection_recovery_info* r = last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(r->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(r->next_attempt_delay_ms, IDENTITY_MS);

  fire_retry(fx);
  assert_string_equal(last_connect_host(fx), HUB_HOST);
}

/* A mid-session Not authorized DISCONNECT is the same refusal as a CONNACK one. */
static void mqttv5_mid_session_not_authorized_uses_the_identity_ladder(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  connect_v5(fx);

  inject_v5_disconnect(fx, az_iot_mqtt_disconnect_result(AZ_IOT_MQTT_VERSION_5, 0x87), 0x87);
  size_t i = last_index_of(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(fx->log.reasons[i], AZ_IOT_ERR_AUTH);
  assert_true(fx->log.error_present[i]);
  assert_int_equal(fx->log.error_codes[i], 0x87);
  assert_int_equal(fx->log.recovery[i].classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(fx->log.recovery[i].next_attempt_delay_ms, IDENTITY_MS);

  fire_retry(fx);
  assert_string_equal(last_connect_host(fx), HUB_HOST);
}

static void mqttv5_mid_session_server_busy_uses_the_reconnection_policy(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  connect_v5(fx);

  inject_v5_disconnect(fx, az_iot_mqtt_disconnect_result(AZ_IOT_MQTT_VERSION_5, 0x89), 0x89);
  const az_iot_connection_recovery_info* r = last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(r->classification, AZ_IOT_CONN_FAILURE_TRANSIENT);
  assert_int_equal(r->next_attempt_delay_ms, RECONNECT_MS);
  assert_int_equal(fx->client->identity_retry_attempt, 0);
}

/* With retries disabled a mid-session refusal is a failure, not a clean end. */
static void mqttv5_mid_session_not_authorized_faults_without_a_policy(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.reconnection_policy = az_iot_connection_client_get_disabled_retry_policy();
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  connect_v5(fx);

  inject_v5_disconnect(fx, az_iot_mqtt_disconnect_result(AZ_IOT_MQTT_VERSION_5, 0x87), 0x87);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  const az_iot_connection_recovery_info* r = last_hub_recovery(fx, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(r->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_AUTH);
}

/* ------------------------------------------------------------------------- */
/* bounds                                                                    */
/* ------------------------------------------------------------------------- */

static void identity_backoff_grows_until_max_attempts_faults(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.identity_recovery.policy.initial_delay_ms = 10;
  fx->client->opts.identity_recovery.policy.max_delay_ms = 40;
  fx->client->opts.identity_recovery.policy.max_attempts = 3;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  static const uint32_t expected[] = { 10, 20, 40 };
  for (size_t n = 0; n < 3; ++n)
  {
    reject_identity(fx);
    const az_iot_connection_recovery_info* r
        = last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING);
    assert_int_equal(r->attempt, n + 1);
    assert_int_equal(r->next_attempt_delay_ms, expected[n]);
    fire_retry(fx);
  }
  reject_identity(fx);

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  size_t i = last_index_of(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->log.reasons[i], AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_true(fx->log.recovery_present[i]);
  assert_int_equal(fx->log.recovery[i].classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(fx->log.recovery[i].next_attempt_delay_ms, 0);
  assert_int_equal(fx->client->reconnect_due_ms, 0);
}

/* No attempt is scheduled to start at or after max_duration_seconds: the
 * refusal whose next retry would land there stops recovery, before the
 * deadline. */
static void identity_recovery_stops_at_max_duration(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.identity_recovery.policy.initial_delay_ms = 400;
  fx->client->opts.identity_recovery.policy.max_delay_ms = 400;
  fx->client->opts.identity_recovery.max_duration_seconds = 1;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  for (int n = 0; n < 6 && az_iot_test_last_state(&fx->log) != AZ_IOT_CONN_STATE_FAULTED; ++n)
  {
    reject_identity(fx);
    if (az_iot_test_last_state(&fx->log) == AZ_IOT_CONN_STATE_RETRY_PENDING)
    {
      assert_true(fx->client->reconnect_due_ms < fx->client->identity_recovery_started_ms + 1000u);
      fire_retry(fx);
    }
  }
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_int_equal(fx->client->reconnect_due_ms, 0);
}

/* A due retry is not started when do_work() first runs past the deadline. */
static void a_retry_due_before_the_deadline_does_not_start_after_it(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.identity_recovery.max_duration_seconds = 1;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  reject_identity(fx);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RETRY_PENDING);

  az_iot_test_wait_until_ms(fx->client->identity_recovery_started_ms + 1000u);
  pump(fx, 1);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
  assert_int_equal(fx->client->reconnect_due_ms, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  const az_iot_connection_recovery_info* r = last_hub_recovery(fx, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(r->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_IDENTITY_REJECTED);
}

static void mode_none_faults_on_the_first_refusal(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.identity_recovery.mode = AZ_IOT_IDENTITY_RECOVERY_NONE;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  reject_identity(fx);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  const az_iot_connection_recovery_info* r = last_hub_recovery(fx, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(r->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(r->next_attempt_delay_ms, 0);
}

/* Disabling the reconnection policy stops the SDK acting on its own, identity
 * recovery included. */
static void a_disabled_reconnection_policy_faults_on_the_first_refusal(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.reconnection_policy = az_iot_connection_client_get_disabled_retry_policy();
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  reject_identity(fx);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_IDENTITY_REJECTED);
}

/* max_duration_seconds bounds the whole episode: transient failures after a
 * refusal cannot outlive it, and the fault reports the refusal. */
static void max_duration_bounds_transient_retries_after_a_refusal(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.identity_recovery.max_duration_seconds = 1;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  reject_identity(fx);
  fire_retry(fx);

  for (int n = 0; n < 200
       && az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB)
           != AZ_IOT_CONN_STATE_FAULTED;
       ++n)
  {
    /* Keep only this round's events; the log is bounded. */
    memset(&fx->log, 0, sizeof(fx->log));
    az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(m);
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
    pump(fx, 2);
    if (az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB)
        == AZ_IOT_CONN_STATE_RETRY_PENDING)
    {
      fire_retry(fx);
    }
  }
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  const az_iot_connection_recovery_info* r = last_hub_recovery(fx, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(r->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  size_t i = last_index_of(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->log.reasons[i], AZ_IOT_ERR_IDENTITY_REJECTED);
  /* Nothing is attempted after the fault. */
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
  pump(fx, 3);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

/* Only HUB:CONNECTED restarts the ladder; a transient failure in between does
 * not. */
static void only_a_hub_connection_resets_the_identity_ladder(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  reject_identity(fx);
  fire_retry(fx);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  pump(fx, 2);
  assert_int_equal(
      last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING)->classification,
      AZ_IOT_CONN_FAILURE_TRANSIENT);
  fire_retry(fx);
  reject_identity(fx);
  assert_int_equal(last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING)->attempt, 2);

  fire_retry(fx);
  m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  pump(fx, 1);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_CONNECTED);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  pump(fx, 1);
  fire_retry(fx);
  reject_identity(fx);
  assert_int_equal(last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING)->attempt, 1);
}

/* ------------------------------------------------------------------------- */
/* DPS-provisioned devices                                                   */
/* ------------------------------------------------------------------------- */

/* Default: no DPS registration, and so no certificate request, on a refusal. */
static void a_refusal_retries_the_assigned_hub_without_dps(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);

  for (int n = 0; n < 3; ++n)
  {
    reject_identity(fx);
    assert_false(fx->client->needs_reprovision);
    assert_false(last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING)->next_attempt_reprovisions);
    fire_retry(fx);
    assert_string_equal(last_connect_host(fx), HUB_HOST);
    assert_null(fx->client->dps_mqtt);
  }
}

/* A hub that answers with a refusal is reachable, so refusals neither count
 * towards nor complete max_hub_connect_attempts_before_reprovision. */
static void refusals_do_not_trigger_the_unreachable_hub_threshold(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.dps.max_hub_connect_attempts_before_reprovision = 2;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  pump(fx, 2);
  fire_retry(fx);
  for (int n = 0; n < 3; ++n)
  {
    reject_identity(fx);
    fire_retry(fx);
    assert_string_equal(last_connect_host(fx), HUB_HOST);
  }
  /* The transient failure before the refusals was forgotten. */
  m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  pump(fx, 2);
  fire_retry(fx);
  assert_string_equal(last_connect_host(fx), HUB_HOST);
}

/* REPROVISION mode: DPS assigns, the hub refuses again. The cycle keeps its
 * backoff and stops at max_attempts instead of re-registering forever. */
static void reprovision_cycles_are_bounded(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.identity_recovery.mode = AZ_IOT_IDENTITY_RECOVERY_REPROVISION;
  fx->client->opts.identity_recovery.policy.initial_delay_ms = 10;
  fx->client->opts.identity_recovery.policy.max_delay_ms = 40;
  fx->client->opts.identity_recovery.policy.max_attempts = 3;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);

  static const uint32_t expected[] = { 10, 20, 40 };
  for (size_t n = 0; n < 3; ++n)
  {
    reject_identity(fx);
    const az_iot_connection_recovery_info* r
        = last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING);
    assert_int_equal(r->attempt, n + 1);
    assert_int_equal(r->next_attempt_delay_ms, expected[n]);
    assert_true(r->next_attempt_reprovisions);
    fire_retry(fx);
    assert_string_equal(last_connect_host(fx), DPS_HOST);
    provision(fx);
  }
  reject_identity(fx);

  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->client->reconnect_due_ms, 0);
  /* Nothing further is attempted: no adapter is created after the fault. */
  pump(fx, 5);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

/* ------------------------------------------------------------------------- */
/* az_iot_connection_client_request_reprovision()                            */
/* ------------------------------------------------------------------------- */

static void request_reprovision_runs_the_pending_retry_through_dps_now(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.identity_recovery.policy.initial_delay_ms = 3600000u;
  fx->client->opts.identity_recovery.policy.max_delay_ms = 3600000u;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);

  reject_identity(fx);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(az_iot_connection_client_request_reprovision(fx->client), AZ_IOT_OK);
  pump(fx, 1);
  assert_string_equal(last_connect_host(fx), DPS_HOST);

  /* The re-provision completes and the refusal ladder carries on from it. */
  provision(fx);
  reject_identity(fx);
  assert_int_equal(last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING)->attempt, 2);
}

static void close_and_open_after_a_fault_returns_to_the_cached_hub(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.identity_recovery.mode = AZ_IOT_IDENTITY_RECOVERY_NONE;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);
  reject_identity(fx);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  assert_string_equal(last_connect_host(fx), HUB_HOST);

  /* Unless the application asks for a re-provision. */
  reject_identity(fx);
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_request_reprovision(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  assert_string_equal(last_connect_host(fx), DPS_HOST);
}

/* A pending registration retry keeps its DPS backoff (and any retry-after). */
static void request_reprovision_keeps_a_pending_registration_schedule(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.reconnection_policy.initial_delay_ms = 3600000u;
  fx->client->opts.reconnection_policy.max_delay_ms = 3600000u;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps, AZ_IOT_ERR_MQTT));
  pump(fx, 3);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_DPS),
      AZ_IOT_CONN_STATE_RETRY_PENDING);
  uint64_t due = fx->client->reconnect_due_ms;
  uint32_t attempts = fx->client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS];

  assert_int_equal(az_iot_connection_client_request_reprovision(fx->client), AZ_IOT_OK);
  assert_int_equal(fx->client->reconnect_due_ms, due);
  assert_int_equal(fx->client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS], attempts);
}

/* A request made while a registration runs is satisfied by it; it does not
 * linger and divert a later, unrelated reconnect to DPS. */
static void request_reprovision_during_a_registration_does_not_linger(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);
  reject_identity(fx);
  assert_int_equal(az_iot_connection_client_request_reprovision(fx->client), AZ_IOT_OK);
  pump(fx, 1);
  assert_string_equal(last_connect_host(fx), DPS_HOST);

  assert_int_equal(az_iot_connection_client_request_reprovision(fx->client), AZ_IOT_OK);
  provision(fx);
  assert_false(fx->client->needs_reprovision);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  pump(fx, 1);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  pump(fx, 1);
  fire_retry(fx);
  assert_string_equal(last_connect_host(fx), HUB_HOST);
}

/* DPS refusing the bootstrap identity is retried on reconnection_policy, not
 * the identity ladder, which is the hub's. The cause is not retriable. */
static void a_dps_refusal_is_not_on_the_identity_ladder(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  reject_identity(fx);
  size_t i = last_index_of(&fx->log, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_true(fx->log.recovery_present[i]);
  assert_int_equal(fx->log.recovery[i].classification, AZ_IOT_CONN_FAILURE_TERMINAL);
  assert_int_equal(fx->log.recovery[i].next_attempt_delay_ms, RECONNECT_MS);
  assert_string_equal(fx->log.recovery_endpoint[i], DPS_HOST);
  assert_int_equal(fx->client->identity_retry_attempt, 0);
}

/* Hub refuses in REPROVISION mode with a 1 s bound; the retry registers and
 * returns the DPS adapter with the register PUBLISH issued. */
static az_iot_mock_mqtt_client* reprovision_after_a_bounded_refusal(az_iot_test_conn* fx)
{
  fx->client->opts.identity_recovery.mode = AZ_IOT_IDENTITY_RECOVERY_REPROVISION;
  fx->client->opts.identity_recovery.max_duration_seconds = 1;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);
  reject_identity(fx);
  fire_retry(fx);
  assert_string_equal(last_connect_host(fx), DPS_HOST);
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps, AZ_IOT_OK));
  pump(fx, 1);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(dps, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(dps, sub->packet_id, AZ_IOT_OK));
  pump(fx, 1);
  return dps;
}

static void assert_identity_recovery_stopped(az_iot_test_conn* fx)
{
  assert_int_equal(fx->client->reconnect_due_ms, 0);
  size_t i = last_index_of(&fx->log, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->log.reasons[i], AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_true(fx->log.recovery_present[i]);
  assert_int_equal(fx->log.recovery[i].classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  /* The hub, waiting since the refusal, is told recovery ended too. */
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_FAULTED);
  i = last_index_of(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->log.reasons[i], AZ_IOT_ERR_IDENTITY_REJECTED);
  assert_true(fx->log.recovery_present[i]);
  assert_int_equal(fx->log.recovery[i].classification, AZ_IOT_CONN_FAILURE_IDENTITY);
}

/* A failed registration whose retry would land past the deadline faults DPS
 * and the hub waiting on it. */
static void a_failed_registration_past_the_identity_deadline_faults_both_scopes(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.reconnection_policy.initial_delay_ms = 2000u;
  fx->client->opts.reconnection_policy.max_delay_ms = 2000u;
  az_iot_mock_mqtt_client* dps = reprovision_after_a_bounded_refusal(fx);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_RETRY_PENDING);

  static const char k_failed[]
      = "{\"operationId\":\"op-1\",\"status\":\"failed\","
        "\"registrationState\":{\"errorCode\":500000,\"errorMessage\":\"x\"}}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      dps,
      DPS_RESPONSE_TOPIC_ASSIGNED,
      (const uint8_t*)k_failed,
      strlen(k_failed),
      AZ_IOT_MQTT_QOS_1));
  pump(fx, 3);
  assert_identity_recovery_stopped(fx);
}

/* A due registration retry reached past the deadline faults both scopes. */
static void a_delayed_registration_retry_past_the_deadline_faults_both_scopes(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* dps = reprovision_after_a_bounded_refusal(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps, AZ_IOT_ERR_MQTT));
  pump(fx, 3);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_DPS),
      AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_not_equal(fx->client->reconnect_due_ms, 0);

  az_iot_test_wait_until_ms(fx->client->identity_recovery_started_ms + 1000u);
  pump(fx, 1);
  assert_identity_recovery_stopped(fx);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

/* A DPS retry-after that lands past max_duration_seconds stops recovery. */
static void a_retry_after_past_the_identity_deadline_stops_recovery(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* dps = reprovision_after_a_bounded_refusal(fx);

  static const char k_throttled[] = "{\"errorCode\":429001,\"message\":\"Too many requests.\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      dps,
      "$dps/registrations/res/429/?$rid=1&retry-after=30",
      (const uint8_t*)k_throttled,
      strlen(k_throttled),
      AZ_IOT_MQTT_QOS_1));
  pump(fx, 3);
  assert_identity_recovery_stopped(fx);
}

/* A registration still polling past max_duration_seconds is abandoned instead
 * of publishing its status query. */
static void a_registration_polling_past_the_identity_deadline_is_abandoned(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* dps = reprovision_after_a_bounded_refusal(fx);

  static const char k_assigning[] = "{\"operationId\":\"op-1\",\"status\":\"assigning\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      dps,
      "$dps/registrations/res/202/?$rid=1&retry-after=1",
      (const uint8_t*)k_assigning,
      strlen(k_assigning),
      AZ_IOT_MQTT_QOS_1));
  pump(fx, 1);

  az_iot_test_wait_until_ms(fx->client->identity_recovery_started_ms + 1000u);
  pump(fx, 3);
  assert_null(fx->client->dps_mqtt);
  assert_identity_recovery_stopped(fx);
}

/* Register on the DPS adapter the last attempt opened, and answer with @p body. */
static void answer_registration(az_iot_test_conn* fx, const char* body)
{
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(dps);
  assert_string_equal(last_connect_host(fx), DPS_HOST);
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps, AZ_IOT_OK));
  pump(fx, 1);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(dps, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(dps, sub->packet_id, AZ_IOT_OK));
  pump(fx, 1);
  assert_true(az_iot_mock_mqtt_client_inject_message(
      dps, DPS_RESPONSE_TOPIC_ASSIGNED, (const uint8_t*)body, strlen(body), AZ_IOT_MQTT_QOS_1));
  pump(fx, 5);
}

static const char k_assigned_unknown_profile[]
    = "{\"operationId\":\"op-1\",\"status\":\"assigned\","
      "\"registrationState\":{\"registrationId\":\"ut-device\","
      "\"assignedHub\":\"" HUB_HOST "\",\"connectionProfile\":\"mqttV9\","
      "\"deviceId\":\"assigned-device\"}}";

/* Both scopes settled at FAULTED with @p reason, and nothing left to retry. */
static void assert_both_scopes_faulted(az_iot_test_conn* fx, az_iot_result reason)
{
  assert_int_equal(fx->client->reconnect_due_ms, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_DPS),
      AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_FAULTED);
  size_t i = last_index_of(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->log.reasons[i], reason);
  assert_true(fx->log.recovery_present[i]);
  assert_int_equal(fx->log.recovery[i].classification, AZ_IOT_CONN_FAILURE_TERMINAL);
}

/* A re-registration after a refusal returns an assignment the client cannot
 * use: the hub waiting on it is faulted too, not left RETRY_PENDING. */
static void a_rejected_reassignment_after_a_refusal_faults_the_hub(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.identity_recovery.mode = AZ_IOT_IDENTITY_RECOVERY_REPROVISION;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);
  reject_identity(fx);
  fire_retry(fx);

  answer_registration(fx, k_assigned_unknown_profile);
  assert_both_scopes_faulted(fx, AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED);
  /* Nothing further is attempted. */
  pump(fx, 3);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

/* Same when the re-registration came from the unreachable-hub threshold. */
static void a_rejected_reassignment_after_an_unreachable_hub_faults_the_hub(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.dps.max_hub_connect_attempts_before_reprovision = 1;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_MQTT));
  pump(fx, 2);
  fire_retry(fx);

  answer_registration(fx, k_assigned_unknown_profile);
  assert_both_scopes_faulted(fx, AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED);
}

/* On a first registration no hub is waiting, so only DPS faults. */
static void a_rejected_first_assignment_leaves_the_hub_idle(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  answer_registration(fx, k_assigned_unknown_profile);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_DPS),
      AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_IDLE);
}

static void request_reprovision_needs_a_dps_client(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(az_iot_connection_client_request_reprovision(NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_connection_client_request_reprovision(fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_false(fx->client->needs_reprovision);
}

/* ------------------------------------------------------------------------- */
/* defaults                                                                  */
/* ------------------------------------------------------------------------- */

/* A zeroed identity_recovery keeps the earlier behaviour: a CONNACK refusal
 * re-provisions, paced by reconnection_policy. */
static void zeroed_identity_recovery_reprovisions_on_the_reconnection_schedule(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  memset(&fx->client->opts.identity_recovery, 0, sizeof(fx->client->opts.identity_recovery));
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  provision(fx);

  reject_identity(fx);
  const az_iot_connection_recovery_info* r = last_hub_recovery(fx, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_equal(r->classification, AZ_IOT_CONN_FAILURE_IDENTITY);
  assert_int_equal(r->next_attempt_delay_ms, RECONNECT_MS);
  assert_true(r->next_attempt_reprovisions);
  fire_retry(fx);
  assert_string_equal(last_connect_host(fx), DPS_HOST);
}

static void options_default_enables_slow_hub_recovery_without_dps(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  assert_int_equal(
      opts.identity_recovery.policy.initial_delay_ms,
      AZ_IOT_DEFAULT_IDENTITY_RECOVERY_INITIAL_DELAY_MS);
  assert_int_equal(
      opts.identity_recovery.policy.max_delay_ms, AZ_IOT_DEFAULT_IDENTITY_RECOVERY_MAX_DELAY_MS);
  assert_int_equal(opts.identity_recovery.policy.max_attempts, 0u);
  assert_int_equal(
      opts.identity_recovery.policy.jitter_pct, AZ_IOT_DEFAULT_IDENTITY_RECOVERY_JITTER_PCT);
  assert_int_equal(opts.identity_recovery.max_duration_seconds, 0u);
  assert_int_equal(opts.identity_recovery.mode, AZ_IOT_IDENTITY_RECOVERY_RETRY_HUB);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(
        mqttv3_connack_rejection_retries_the_hub_on_the_identity_ladder, setup_v3, teardown),
    cmocka_unit_test_setup_teardown(
        mqttv5_connack_rejection_retries_the_hub_on_the_identity_ladder, setup_v5, teardown),
    cmocka_unit_test_setup_teardown(
        mqttv5_mid_session_not_authorized_uses_the_identity_ladder, setup_v5, teardown),
    cmocka_unit_test_setup_teardown(
        mqttv5_mid_session_server_busy_uses_the_reconnection_policy, setup_v5, teardown),
    cmocka_unit_test_setup_teardown(
        identity_backoff_grows_until_max_attempts_faults, setup_v3, teardown),
    cmocka_unit_test_setup_teardown(identity_recovery_stops_at_max_duration, setup_v3, teardown),
    cmocka_unit_test_setup_teardown(
        a_retry_due_before_the_deadline_does_not_start_after_it, setup_v3, teardown),
    cmocka_unit_test_setup_teardown(mode_none_faults_on_the_first_refusal, setup_v3, teardown),
    cmocka_unit_test_setup_teardown(
        a_disabled_reconnection_policy_faults_on_the_first_refusal, setup_v3, teardown),
    cmocka_unit_test_setup_teardown(
        only_a_hub_connection_resets_the_identity_ladder, setup_v3, teardown),
    cmocka_unit_test_setup_teardown(
        a_refusal_retries_the_assigned_hub_without_dps, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        refusals_do_not_trigger_the_unreachable_hub_threshold, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(reprovision_cycles_are_bounded, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        request_reprovision_runs_the_pending_retry_through_dps_now, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        close_and_open_after_a_fault_returns_to_the_cached_hub, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        a_retry_after_past_the_identity_deadline_stops_recovery, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_registration_past_the_identity_deadline_faults_both_scopes, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        a_delayed_registration_retry_past_the_deadline_faults_both_scopes, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        a_registration_polling_past_the_identity_deadline_is_abandoned, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        a_rejected_reassignment_after_a_refusal_faults_the_hub, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        a_rejected_reassignment_after_an_unreachable_hub_faults_the_hub, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        a_rejected_first_assignment_leaves_the_hub_idle, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(request_reprovision_needs_a_dps_client, setup_v3, teardown),
    cmocka_unit_test_setup_teardown(
        request_reprovision_keeps_a_pending_registration_schedule, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        request_reprovision_during_a_registration_does_not_linger, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        a_dps_refusal_is_not_on_the_identity_ladder, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        mqttv5_mid_session_not_authorized_faults_without_a_policy, setup_v5, teardown),
    cmocka_unit_test_setup_teardown(
        zeroed_identity_recovery_reprovisions_on_the_reconnection_schedule, setup_dps, teardown),
    cmocka_unit_test_setup_teardown(
        max_duration_bounds_transient_retries_after_a_refusal, setup_v3, teardown),
    cmocka_unit_test(options_default_enables_slow_hub_recovery_without_dps),
  };
  return cmocka_run_group_tests_name("connection_identity_recovery", tests, NULL, NULL);
}
