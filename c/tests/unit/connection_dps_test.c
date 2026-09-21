// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* ConnectionClient DPS-before-connect unit tests: the provisioning leg that
 * runs when opts.host is NULL and opts.dps is configured -- endpoint/version
 * selection, the register/poll handshake, the handoff to the assigned hub, and
 * the failure paths.
 *
 * The CSR-enrollment variant of this flow is covered in connection_client_test.c
 * (dps_csr_flow_sends_csr_and_stores_issued_chain); this suite is the plain
 * (no operational certificate) flow and its error handling. */
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

#include "internal/connection_client_internal.h"

#include "support/connection_test_harness.h"

#define DPS_RESPONSE_TOPIC_ASSIGNED "$dps/registrations/res/200/?$rid=1"
#define DPS_RESPONSE_TOPIC_ACCEPTED "$dps/registrations/res/202/?$rid=1&retry-after=1"
#define DPS_RESPONSE_TOPIC_ACCEPTED_NOW "$dps/registrations/res/202/?$rid=1&retry-after=0"
#define DPS_PROFILE_OVERRIDE_ENV "AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE"

static void set_dps_profile_override(const char* value)
{
#ifdef _WIN32
  assert_int_equal(_putenv_s(DPS_PROFILE_OVERRIDE_ENV, value ? value : ""), 0);
#else
  if (value)
  {
    assert_int_equal(setenv(DPS_PROFILE_OVERRIDE_ENV, value, 1), 0);
  }
  else
  {
    assert_int_equal(unsetenv(DPS_PROFILE_OVERRIDE_ENV), 0);
  }
#endif
}

static const char k_assigned_body[]
    = "{\"operationId\":\"op-1\",\"status\":\"assigned\","
      "\"registrationState\":{\"registrationId\":\"ut-device\","
      "\"assignedHub\":\"myhub.azure-devices.net\",\"deviceId\":\"assigned-device\"}}";

static const char k_assigning_body[] = "{\"operationId\":\"op-1\",\"status\":\"assigning\"}";
static const char k_failed_body[]
    = "{\"operationId\":\"op-1\",\"status\":\"failed\","
      "\"registrationState\":{\"errorCode\":400207,\"errorMessage\":\"Custom allocation failed\"}}";
static const char k_disabled_body[] = "{\"operationId\":\"op-1\",\"status\":\"disabled\"}";

/* connectionProfile variants. The property is a readOnly string on
 * DeviceRegistrationResult (api-version 2026-11-02-preview) and an extensible
 * union, so "absent", "null" and "a value we have never seen" are all real
 * cases the service is allowed to produce. */
#define ASSIGNED_BODY_WITH_PROFILE(profile_json)             \
  "{\"operationId\":\"op-1\",\"status\":\"assigned\","       \
  "\"registrationState\":{\"registrationId\":\"ut-device\"," \
  "\"assignedHub\":\"myhub.azure-devices.net\","             \
  "\"connectionProfile\":" profile_json ","                  \
  "\"deviceId\":\"assigned-device\"}}"

static const char k_assigned_classic[] = ASSIGNED_BODY_WITH_PROFILE("\"classic\"");
static const char k_assigned_mqtt_v5[] = ASSIGNED_BODY_WITH_PROFILE("\"mqttV5\"");
static const char k_assigned_profile_null[] = ASSIGNED_BODY_WITH_PROFILE("null");
static const char k_assigned_profile_unknown[] = ASSIGNED_BODY_WITH_PROFILE("\"mqttV9-quantum\"");
/* Longer than AZ_IOT_CONNECTION_PROFILE_RAW_BUF (64), so the raw string cannot
 * be reported whole and the caller has to be told so. */
static const char k_assigned_profile_overlong[] = ASSIGNED_BODY_WITH_PROFILE(
    "\"mqttV5-with-an-absurdly-long-forward-compatible-suffix-that-will-not-fit\"");

/* ------------------------------------------------------------------------- */
/* fixture                                                                   */
/* ------------------------------------------------------------------------- */

static az_iot_connection_client_options dps_options(void)
{
  az_iot_connection_client_options opts = { 0 };
  opts.host = NULL; /* DPS mode */
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  return opts;
}

static int setup(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = dps_options();
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  if (fx)
  {
    bool adopted = (fx->client->factory_count > 0);
    az_iot_connection_client_destroy(&fx->client_storage);
    if (!adopted)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
    free(fx);
  }
  return 0;
}

/* open() -> the DPS adapter instance, still awaiting CONNACK. */
static az_iot_mock_mqtt_client* dps_open(az_iot_test_conn* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  return m;
}

/* Drive CONNACK -> SUBACK so the register PUBLISH has been issued. */
static az_iot_mock_mqtt_client* dps_open_to_registering(az_iot_test_conn* fx)
{
  az_iot_mock_mqtt_client* m = dps_open(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  return m;
}

static bool inject_dps_response(az_iot_mock_mqtt_client* m, const char* topic, const char* body)
{
  return az_iot_mock_mqtt_client_inject_message(
      m, topic, (const uint8_t*)body, strlen(body), AZ_IOT_MQTT_QOS_1);
}

/* ------------------------------------------------------------------------- */
/* re-provisioning after an identity rejection                                */
/* ------------------------------------------------------------------------- */

/* Fixture with a reconnection policy so the retry path is live. Backoff is
 * short but long enough that a single do_work() cannot cross the deadline in
 * the call that schedules it. */
#define REPROVISION_DELAY_MS 20u

static int setup_with_reconnect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = dps_options();
  opts.reconnection_policy.initial_delay_ms = REPROVISION_DELAY_MS;
  opts.reconnection_policy.max_delay_ms = REPROVISION_DELAY_MS;
  opts.reconnection_policy.max_attempts = 3;
  opts.reconnection_policy.jitter_pct = 0;
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);
  az_iot_connection_client__seed_rng(fx->client, 0xC0FFEEFEEDFACEull);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

/* Provision, then hand back the hub adapter sitting in CONNECTING. */
static az_iot_mock_mqtt_client* provision_to_hub_connecting(az_iot_test_conn* fx)
{
  az_iot_mock_mqtt_client* dps = dps_open_to_registering(fx);
  assert_true(inject_dps_response(dps, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_body));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(hub);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(hub, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "myhub.azure-devices.net");
  return hub;
}

/* An identity rejection is not a transient transport failure -- the broker has
 * refused this credential, so retrying it cannot succeed. A DPS-provisioned
 * device must go back to DPS for a fresh assignment. */
static void hub_identity_rejection_reprovisions_through_dps(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);

  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* The retry targets DPS, not the hub that just refused us. */
  az_iot_mock_mqtt_client* retry = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(retry);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(retry, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "global.azure-devices-provisioning.net");
}

/* A transport failure says nothing about the identity, so the cached hub
 * assignment stays valid and DPS must not be involved. */
static void hub_transport_error_reconnects_without_reprovisioning(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);

  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* retry = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(retry);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(retry, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "myhub.azure-devices.net");
}

/* A hub that has been vacated service-side may simply stop answering rather
 * than rejecting the identity, so a threshold on consecutive hub failures is
 * the only thing that would ever send the device back to DPS. */
#define REPROVISION_THRESHOLD 2u

static int setup_with_reprovision_threshold(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = dps_options();
  opts.reconnection_policy.initial_delay_ms = REPROVISION_DELAY_MS;
  opts.reconnection_policy.max_delay_ms = REPROVISION_DELAY_MS;
  opts.reconnection_policy.max_attempts = 0; /* the threshold is what is under test */
  opts.reconnection_policy.jitter_pct = 0;
  opts.dps.max_hub_connect_attempts_before_reprovision = REPROVISION_THRESHOLD;
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);
  az_iot_connection_client__seed_rng(fx->client, 0xC0FFEEFEEDFACEull);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

static const char* last_connect_host(az_iot_mock_mqtt_client* m)
{
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  return c->connect.host;
}

/* Fail the attempt `m` is carrying and pump through to the next one. */
static az_iot_mock_mqtt_client* fail_hub_attempt(az_iot_test_conn* fx, az_iot_mock_mqtt_client* m)
{
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* next = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(next);
  return next;
}

static void hub_unreachable_past_the_threshold_reprovisions(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);

  /* The first failure still trusts the cached assignment. */
  az_iot_mock_mqtt_client* second = fail_hub_attempt(fx, hub);
  assert_string_equal(last_connect_host(second), "myhub.azure-devices.net");

  /* The second crosses the threshold, so the next attempt asks DPS where the
   * device lives now instead of retrying a host that never answers. */
  az_iot_mock_mqtt_client* third = fail_hub_attempt(fx, second);
  assert_string_equal(last_connect_host(third), "global.azure-devices-provisioning.net");
}

/* The hub-failure counter must be reset by a SUCCESSFUL connection, whether or
 * not anyone is watching the connection state.
 *
 * It used to be reset inside the `if (state_cb)` branch of the transition, so a
 * client with no observer registered never cleared it: every successful
 * reconnect left the count standing, and after enough intermittent outages --
 * separated by working connections -- the device would re-provision as though
 * the hub had been unreachable throughout. Observability must not change
 * behaviour, which is what this pins.
 *
 * The fixture's own observer is withdrawn first, because with it registered the
 * old code path happened to do the right thing. */
static void a_successful_connect_clears_the_hub_failure_count_without_an_observer(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_remove_state_observer(fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);

  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);

  az_iot_mock_mqtt_client* second = fail_hub_attempt(fx, hub);
  assert_string_equal(last_connect_host(second), "myhub.azure-devices.net");

  assert_true(az_iot_mock_mqtt_client_inject_connected(second, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  /* Read the state directly: there is deliberately no observer to log it. */
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);

  /* Without the reset this drop would be failure number two and divert to DPS.
   * a_successful_hub_connection_resets_the_failure_count proves the same thing
   * WITH an observer registered, which is why it could not catch the reset
   * living inside the observer branch. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(second));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* third = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(third);
  assert_string_equal(last_connect_host(third), "myhub.azure-devices.net");
}

static void a_zero_threshold_never_reprovisions(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.dps.max_hub_connect_attempts_before_reprovision = 0;

  az_iot_mock_mqtt_client* m = provision_to_hub_connecting(fx);
  for (int i = 0; i < 4; ++i)
  {
    m = fail_hub_attempt(fx, m);
    assert_string_equal(last_connect_host(m), "myhub.azure-devices.net");
  }
}

static void a_successful_hub_connection_resets_the_failure_count(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);

  az_iot_mock_mqtt_client* second = fail_hub_attempt(fx, hub);
  assert_string_equal(last_connect_host(second), "myhub.azure-devices.net");

  assert_true(az_iot_mock_mqtt_client_inject_connected(second, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_CONNECTED);

  /* Without the reset this drop would be failure number two and divert to DPS;
   * an intermittent link must not accumulate its way into re-provisioning. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(second));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* third = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(third);
  assert_string_equal(last_connect_host(third), "myhub.azure-devices.net");
}

/* Re-provisioning completes end to end: DPS answers with a new assignment and
 * the client connects to it. */
static void reprovisioning_connects_to_the_new_assignment(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* dps2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(dps2, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(dps2, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  static const char k_reassigned[]
      = "{\"operationId\":\"op-2\",\"status\":\"assigned\","
        "\"registrationState\":{\"registrationId\":\"ut-device\","
        "\"assignedHub\":\"otherhub.azure-devices.net\",\"deviceId\":\"assigned-device\"}}";
  assert_true(inject_dps_response(dps2, DPS_RESPONSE_TOPIC_ASSIGNED, k_reassigned));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  az_iot_mock_mqtt_client* hub2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(hub2);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(hub2, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "otherhub.azure-devices.net");

  assert_true(az_iot_mock_mqtt_client_inject_connected(hub2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* Re-provisioning runs through the reconnection policy, so a device whose
 * enrollment has genuinely been deleted stops instead of hammering DPS. */
static void repeated_identity_rejection_still_honors_max_attempts(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = provision_to_hub_connecting(fx);

  for (int i = 0; i < 6 && !az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED); ++i)
  {
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_IDENTITY_REJECTED));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    (void)az_iot_connection_client_do_work(fx->client, 0);
    az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
    (void)az_iot_connection_client_do_work(fx->client, 0);
    m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    if (!m)
    {
      break;
    }
  }
  assert_true(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
}

/* Without a reconnection policy there is no retry to carry a re-provision, so
 * the rejection must still fault rather than silently restart DPS. */
static void identity_rejection_without_a_policy_faults(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_IDENTITY_REJECTED));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
}

/* ...but the verdict is not thrown away with the retry. Disabling retries
 * stops the SDK acting on its own; it does not make the client forget that the
 * hub refused this identity. Without this the next open() would see the cached
 * host and walk straight back into the hub that just rejected it. */
static void identity_rejection_without_a_policy_still_reprovisions_on_reopen(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);
  assert_string_equal(fx->client->opts.host, "myhub.azure-devices.net");

  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_IDENTITY_REJECTED));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_true(fx->client->needs_reprovision);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* next = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(next);
  assert_string_equal(last_connect_host(next), "global.azure-devices-provisioning.net");
}

/* ------------------------------------------------------------------------- */
/* endpoint + version selection                                              */
/* ------------------------------------------------------------------------- */

static void dps_connects_to_the_global_endpoint_by_default(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open(fx);

  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "global.azure-devices-provisioning.net");
}

static void dps_honors_a_custom_global_endpoint(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();
  opts.dps.global_endpoint = "my-dps.example.net";

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(factory);
  const az_iot_mock_call* call = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(call);
  assert_string_equal(call->connect.host, "my-dps.example.net");

  az_iot_connection_client_destroy(&c);
}

/* Provisioning is the one connect that happens unattended on a first boot, so
 * an application that tuned the timings for its link needs them to apply there
 * too. They used to be hardcoded on this path. */
static void dps_honors_the_configured_timings(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();
  opts.keep_alive_seconds = 120;
  opts.connect_timeout_seconds = 7;

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(factory);
  const az_iot_mock_call* call = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(call);
  assert_int_equal(call->connect.keep_alive_seconds, 120);
  assert_int_equal(call->connect.connect_timeout_seconds, 7);

  az_iot_connection_client_destroy(&c);
}

static void dps_defaults_the_timings_when_unset(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(factory);
  const az_iot_mock_call* call = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(call);
  assert_int_equal(call->connect.keep_alive_seconds, AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS);
  assert_int_equal(call->connect.connect_timeout_seconds, AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS);

  az_iot_connection_client_destroy(&c);
}

/* The proxy and the transport apply to provisioning too. A device that can only
 * reach the network through a proxy, or only over 443, cannot reach DPS either
 * -- and DPS is the FIRST connect it makes, so getting this wrong means the
 * device never provisions at all. */
static void dps_carries_the_proxy_and_transport(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();
  opts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(factory);
  const az_iot_mock_call* call = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(call);
  assert_int_equal(call->connect.transport, AZ_IOT_MQTT_TRANSPORT_WEBSOCKET);
  assert_int_equal(call->connect.port, 443);
  assert_string_equal(call->connect.proxy_host, "proxy.corp.example");
  assert_int_equal(call->connect.proxy_port, 3128);

  az_iot_connection_client_destroy(&c);
}

/* DPS speaks MQTT v3.1.1 only. Even when the device is headed for a v5
 * Hub-Next endpoint, the provisioning leg must pick the v3.1.1 factory. */
static void dps_uses_v3_1_1_even_when_the_hub_is_next(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_mqtt_factory* v5 = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  az_iot_mqtt_factory* v3 = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, v5), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, v3), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);
  assert_null(az_iot_mock_mqtt_factory_last_client(v5));
  assert_non_null(az_iot_mock_mqtt_factory_last_client(v3));

  az_iot_connection_client_destroy(&c);
}

static void dps_without_a_v3_1_1_factory_is_not_supported(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_mqtt_factory* v5 = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, v5), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_connection_client_destroy(&c);
}

/* ------------------------------------------------------------------------- */
/* register handshake                                                        */
/* ------------------------------------------------------------------------- */

static void dps_subscribes_the_registration_response_topic(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_string_equal(sub->topic, "$dps/registrations/res/#");
}

static void dps_publishes_register_only_after_the_suback(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  /* Registering before the subscription is confirmed would race the response. */
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_PUBLISH), 0);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_non_null(strstr(pub->topic, "$dps/registrations/PUT/iotdps-register"));
}

/* ------------------------------------------------------------------------- */
/* polling                                                                   */
/* ------------------------------------------------------------------------- */

static void dps_polls_operation_status_after_an_assigning_response(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);
  az_iot_mock_mqtt_client_clear_calls(m);

  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ACCEPTED_NOW, k_assigning_body));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  /* retry-after 0: the very next pump issues the status query. */
  (void)az_iot_connection_client_do_work(fx->client, 0);

  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_non_null(strstr(pub->topic, "$dps/registrations/GET/iotdps-get-operationstatus"));
}

/* The service tells the device how long to back off; polling sooner is a
 * throttling risk, so the deadline must actually gate the query. */
static void dps_honors_the_retry_after_delay(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);
  az_iot_mock_mqtt_client_clear_calls(m);

  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ACCEPTED, k_assigning_body));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_PUBLISH), 0);

  az_iot_test_wait_ms(1050);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_PUBLISH), 1);
}

/* ------------------------------------------------------------------------- */
/* assignment handoff                                                        */
/* ------------------------------------------------------------------------- */

static void dps_assignment_connects_to_the_assigned_hub(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* dps = dps_open_to_registering(fx);

  assert_true(inject_dps_response(dps, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_body));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(hub);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(hub, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "myhub.azure-devices.net");
}

/* The hub identity is the DPS-assigned deviceId, not the registrationId the
 * device happened to be configured with. */
static void dps_assignment_uses_the_assigned_device_id(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* dps = dps_open_to_registering(fx);

  assert_true(inject_dps_response(dps, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_body));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(fx->factory);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(hub, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.client_id, "assigned-device");
}

static void dps_session_reaches_connected_after_assignment(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* dps = dps_open_to_registering(fx);

  assert_true(inject_dps_response(dps, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_body));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* ------------------------------------------------------------------------- */
/* failure paths                                                             */
/* ------------------------------------------------------------------------- */

/* With no reconnection policy there is no retry to carry a re-registration, so
 * the failure is terminal. The retrying counterpart is
 * dps_failed_status_retries_under_the_policy(). */
static void dps_failed_status_faults_with_a_dps_error(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_failed_body));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_DPS);
}

static void dps_disabled_status_faults_with_a_dps_error(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_disabled_body));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_DPS);
}

/* A registration failure is the most transient failure a device meets: the
 * enrollment may not exist yet, or the DPS may have no linked hub yet. It must
 * go through the reconnection policy like every other failure, not straight to
 * a terminal fault -- a device configured to retry forever must retry. */
static void dps_failed_status_retries_under_the_policy(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_failed_body));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_RECONNECTING), AZ_IOT_ERR_DPS);
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
}

/* And the retry is a re-REGISTRATION, not a connect to a host the client was
 * never assigned. */
static void dps_failed_status_retries_against_dps(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_failed_body));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* second = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(second);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(second, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "global.azure-devices-provisioning.net");
}

/* max_attempts still bounds it, so an enrollment that really is absent stops
 * instead of hammering the service. */
/* A DPS retry whose dps_start() fails must stay on the DPS path when there is
 * no cached assignment to fall back to.
 *
 * do_work() clears needs_reprovision before the attempt, deliberately, so a
 * failing dps_start() falls back to an ordinary retry rather than looping
 * through provisioning forever. That fallback is right for a device that HAS
 * an assignment -- it still has a hub to try. For a device that has never
 * registered, opts.host is NULL, so the hub path has no endpoint at all and
 * the client would never reach DPS again. */
static void a_failed_dps_retry_stays_on_dps_when_no_hub_is_known(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  /* No assignment has ever been made. */
  assert_null(fx->client->opts.host);

  /* Registration fails -> retry scheduled on the DPS ladder. */
  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_failed_body));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  assert_true(fx->client->needs_reprovision);

  /* Make the next connect fail synchronously, so dps_start() itself fails. */
  az_iot_mock_mqtt_factory_fail_next_connect(fx->factory, AZ_IOT_ERR_MQTT);
  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* The demand must survive: there is no hub to fall back to. */
  assert_true(fx->client->needs_reprovision);

  /* And the next retry really does go to DPS rather than a NULL endpoint. */
  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_mock_mqtt_client* next = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(next);
  assert_string_equal(last_connect_host(next), "global.azure-devices-provisioning.net");
}

static void dps_failed_status_still_honors_max_attempts(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  for (int i = 0; i < 6 && !az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED); ++i)
  {
    assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_failed_body));
    for (int j = 0; j < 3; ++j)
    {
      (void)az_iot_connection_client_do_work(fx->client, 0);
    }
    az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
    (void)az_iot_connection_client_do_work(fx->client, 0);

    m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    if (!m)
    {
      break;
    }
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
    if (!sub)
    {
      break;
    }
    assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_true(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
  assert_int_equal(az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_DPS);
  /* It got there by exhausting the policy, not by faulting on the first
   * failure. */
  assert_true(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_RECONNECTING));
  assert_int_equal(az_iot_test_count_state(&fx->log, AZ_IOT_CONN_STATE_RECONNECTING), 3);
}

/* dps_apply_deferred() also finalizes AUXILIARY sessions -- the ones a feature
 * client opens for itself alongside a live hub connection. Those can only
 * reach it by failing, and the retry added above must not apply to them:
 * schedule_reconnect() calls teardown_active(), which would destroy a hub
 * session that is up and healthy. */
static void a_failing_auxiliary_session_does_not_tear_down_the_hub(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);

  /* Stand in for a live hub connection. */
  az_iot_mqtt_client* hub = fx->factory->create(fx->factory->factory_ctx);
  assert_non_null(hub);
  fx->client->active_client = hub;

  assert_int_equal(az_iot_connection_client__dps_user_acquire(fx->client), AZ_IOT_OK);
  fx->client->dps_phase = AZ_IOT_DPS_PHASE_DONE;
  assert_int_equal(az_iot_connection_client__dps_session_ensure(fx->client), AZ_IOT_ERR_BUSY);
  assert_true(fx->client->dps_session_auxiliary);

  az_iot_mock_mqtt_client* aux = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(aux);
  assert_true(az_iot_mock_mqtt_client_inject_connected(aux, AZ_IOT_ERR_MQTT));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  /* The auxiliary session is gone; the hub is untouched and no retry was
   * scheduled for the application's connection. */
  assert_false(fx->client->dps_session_auxiliary);
  assert_ptr_equal(fx->client->active_client, hub);
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_RECONNECTING));
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));

  /* Destroyed explicitly: the mock factory frees only its LAST client, and the
   * auxiliary session created one after this stand-in. */
  fx->client->active_client = NULL;
  hub->iface->destroy(hub);
  az_iot_connection_client__dps_user_release(fx->client);
}

/* A throttle or a server error carries a retry-after, and that is the service
 * telling the device when it may come back. It must win over the policy's own
 * backoff, or routing this failure through the policy (which this change does)
 * would let a throttled device retry sooner than it was asked to. */
static void a_service_retry_after_outranks_the_policy_backoff(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  /* A request-level throttle: 4xx/5xx response topic with retry-after, and a
   * body carrying errorCode/message and no operationId. The parser reports it
   * as FAILED, the same branch an operation-level failure takes. */
  static const char k_throttled_body[]
      = "{\"errorCode\":429001,\"trackingId\":\"t-1\",\"message\":\"Too many requests.\"}";
  assert_true(inject_dps_response(
      m, "$dps/registrations/res/429/?$rid=1&retry-after=30", k_throttled_body));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);

  /* The policy in this fixture would have retried after REPROVISION_DELAY_MS
   * (20ms). The service asked for 30s, so the deadline must be far beyond it. */
  uint64_t now = az_iot_time_mono_ms();
  assert_true(fx->client->reconnect_due_ms > now + 20000ull);

  /* And no new provisioning session is opened when the policy's own delay
   * elapses. The failed session was destroyed, which clears the mock's cached
   * last client, so a non-NULL one here would mean a retry was issued. */
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
}

static void dps_connack_failure_faults(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_IDENTITY_REJECTED));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_IDENTITY_REJECTED);
}

static void dps_suback_failure_faults(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);

  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_ERR_MQTT));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_MQTT);
}

static void dps_disconnect_midflow_faults(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  /* The link drops between REGISTER and the assignment response. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_NOT_CONNECTED);
}

/* A registration response the SDK cannot parse has no knowable outcome, and
 * waiting longer cannot produce one. Failing the attempt (with the body in the
 * log) is recoverable; sitting in CONNECTING with no fault and no diagnostic is
 * indistinguishable from a hang. */
static void dps_malformed_response_faults_with_a_protocol_error(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, "{ not json"));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_PROTOCOL);
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_CONNECTED));
}

/* MQTT permits an empty payload, and an adapter may legally report that as a
 * NULL pointer with zero length. An empty body cannot parse, so the diagnostic
 * path above runs -- and it must not hand that NULL to the logger's "%.*s",
 * which is undefined behaviour even at precision 0. The fault must still be
 * reported normally. */
static void dps_empty_response_body_faults_without_a_null_deref(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  assert_true(az_iot_mock_mqtt_client_inject_message(
      m, DPS_RESPONSE_TOPIC_ASSIGNED, NULL, 0, AZ_IOT_MQTT_QOS_1));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_PROTOCOL);
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_CONNECTED));
}

/* A DPS identity the SDK cannot use must come back as an error, not as a hang.
 * These spans are handed to az_core, whose precondition handler in this build is
 * an infinite loop, so the check has to happen before that call: an empty or
 * missing registration id used to wedge the calling thread inside open(). */
static void dps_rejects_a_null_registration_id(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = NULL;

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_INVALID_ARG);
  az_iot_connection_client_destroy(&c);
}

static void dps_rejects_an_empty_registration_id(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "";

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_INVALID_ARG);
  az_iot_connection_client_destroy(&c);
}

/* A rejected open() must leave the client reusable, not stuck mid-provisioning. */
static void dps_rejected_identity_leaves_the_client_idle(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "";

  az_iot_test_state_log log = { 0 };
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&c, az_iot_test_on_state, &log), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_INVALID_ARG);
  assert_false(az_iot_test_saw_state(&log, AZ_IOT_CONN_STATE_CONNECTING));
  assert_false(az_iot_test_saw_state(&log, AZ_IOT_CONN_STATE_FAULTED));

  /* Still IDLE, so a corrected configuration can be opened on this instance. */
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_INVALID_ARG);
  az_iot_connection_client_destroy(&c);
}

/* ------------------------------------------------------------------------- */
/* connection profile                                                        */
/* ------------------------------------------------------------------------- */

/* A DPS client with both factories registered, so the assignment is free to
 * land on either generation -- which is the whole point of these tests. */
typedef struct profile_fixture
{
  az_iot_connection_client c;
  az_iot_mqtt_factory* v3;
  az_iot_mqtt_factory* v5;
  az_iot_test_state_log log;
} profile_fixture;

static void profile_fixture_open(profile_fixture* pf)
{
  set_dps_profile_override(NULL);
  az_iot_connection_client_options opts = dps_options();
  assert_int_equal(az_iot_connection_client_init(&pf->c, &opts), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&pf->c, az_iot_test_on_state, &pf->log),
      AZ_IOT_OK);
  pf->v5 = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  pf->v3 = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&pf->c, pf->v5), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&pf->c, pf->v3), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&pf->c), AZ_IOT_OK);
}

/* Drive the provisioning leg through CONNACK/SUBACK and deliver `body` as the
 * ASSIGNED response. */
static void profile_assign(profile_fixture* pf, const char* body)
{
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_factory_last_client(pf->v3);
  assert_non_null(dps);
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&pf->c, 0);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(dps, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(dps, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&pf->c, 0);

  assert_true(inject_dps_response(dps, DPS_RESPONSE_TOPIC_ASSIGNED, body));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(&pf->c, 0);
  }
}

/* The hub leg is what proves the profile was acted on rather than merely
 * recorded: the SDK picks the MQTT version from it. */
static void assert_hub_leg_used(profile_fixture* pf, az_iot_mqtt_factory* expected)
{
  az_iot_mqtt_factory* other = (expected == pf->v3) ? pf->v5 : pf->v3;
  az_iot_mock_mqtt_client* used = az_iot_mock_mqtt_factory_last_client(expected);
  assert_non_null(used);
  const az_iot_mock_call* call = az_iot_mock_mqtt_client_last_of(used, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(call);
  assert_string_equal(call->connect.host, "myhub.azure-devices.net");
  /* v3.1.1 also carried the provisioning leg, so it is only the v5 factory that
   * can be asserted untouched. */
  if (other == pf->v5)
  {
    assert_null(az_iot_mock_mqtt_factory_last_client(other));
  }
}

static void profile_fixture_close(profile_fixture* pf)
{
  az_iot_connection_client_destroy(&pf->c);
  set_dps_profile_override(NULL);
}

static void profile_fixture_open_with_override(profile_fixture* pf, const char* profile)
{
  profile_fixture_open(pf);
  set_dps_profile_override(profile);
}

/* Carry the hub leg all the way to CONNECTED, which is what makes the profile
 * readable. Classic needs only a CONNACK; Hub-Next additionally has to complete
 * the presence handshake, since there it is the birth-ack -- not the CONNACK --
 * that announces the session. */
static void profile_finish_hub_leg(profile_fixture* pf, bool next)
{
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(next ? pf->v5 : pf->v3);
  assert_non_null(hub);
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&pf->c, 0);

  if (!next)
  {
    return;
  }

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(hub, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(hub, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&pf->c, 0);

  const az_iot_mock_call* birth = az_iot_mock_mqtt_client_last_of(hub, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(birth);
  assert_int_equal(birth->correlation_data_len, 16);

  /* nonce and ack_type must outlive the do_work that delivers the event. */
  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/assigned-device/dev/presence";
  ack_msg.correlation_data = nonce;
  ack_msg.correlation_data_len = sizeof(nonce);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(hub, &ack));
  (void)az_iot_connection_client_do_work(&pf->c, 0);
}

/* "mqttV5" is the whole reason the field exists: it is what routes a
 * DPS-provisioned device onto an AEG/IoT Hub endpoint instead of Classic. */
static void dps_mqtt_v5_profile_connects_the_hub_over_v5(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);
  profile_assign(&pf, k_assigned_mqtt_v5);

  assert_hub_leg_used(&pf, pf.v5);
  profile_finish_hub_leg(&pf, true);
  assert_int_equal(az_iot_test_last_state(&pf.log), AZ_IOT_CONN_STATE_CONNECTED);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&pf.c, &hp), AZ_IOT_OK);
  assert_int_equal(hp.connection_profile, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  assert_string_equal(hp.connection_profile_raw, "mqttV5");

  profile_fixture_close(&pf);
}

static void dps_classic_profile_connects_the_hub_over_v3_1_1(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);
  profile_assign(&pf, k_assigned_classic);

  assert_hub_leg_used(&pf, pf.v3);
  profile_finish_hub_leg(&pf, false);
  assert_int_equal(az_iot_test_last_state(&pf.log), AZ_IOT_CONN_STATE_CONNECTED);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&pf.c, &hp), AZ_IOT_OK);
  assert_int_equal(hp.connection_profile, AZ_IOT_CONNECTION_PROFILE_CLASSIC);
  assert_string_equal(hp.connection_profile_raw, "classic");

  profile_fixture_close(&pf);
}

static void assert_state_event_contract(
    const az_iot_test_state_log* log,
    az_iot_connection_profile expected_profile,
    const char* expected_raw)
{
  bool saw_connected = false;
  for (size_t i = 0; i < log->count; ++i)
  {
    assert_int_equal(log->event_sizes[i], sizeof(az_iot_connection_state_event));
    if (log->states[i] == AZ_IOT_CONN_STATE_CONNECTED)
    {
      saw_connected = true;
      assert_true(log->profile_present[i]);
      assert_int_equal(log->profile_sizes[i], sizeof(az_iot_hub_profile));
      assert_int_equal(log->profiles[i], expected_profile);
      assert_string_equal(log->profile_raw[i], expected_raw);
    }
    else
    {
      assert_false(log->profile_present[i]);
    }
  }
  assert_true(saw_connected);
}

static void classic_state_events_are_stamped_and_profile_only_on_connected(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);
  profile_assign(&pf, k_assigned_classic);
  profile_finish_hub_leg(&pf, false);

  assert_state_event_contract(&pf.log, AZ_IOT_CONNECTION_PROFILE_CLASSIC, "classic");

  profile_fixture_close(&pf);
}

static void mqtt_v5_state_events_are_stamped_and_profile_only_on_connected(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);
  profile_assign(&pf, k_assigned_mqtt_v5);
  profile_finish_hub_leg(&pf, true);

  assert_state_event_contract(&pf.log, AZ_IOT_CONNECTION_PROFILE_MQTT_V5, "mqttV5");

  profile_fixture_close(&pf);
}

/* Absent is not an error. The service contract documents it as meaning
 * "classic", which is also what every hub predating the field will send. */
static void dps_absent_profile_defaults_to_classic(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);
  profile_assign(&pf, k_assigned_body); /* no connectionProfile at all */

  assert_hub_leg_used(&pf, pf.v3);
  profile_finish_hub_leg(&pf, false);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&pf.c, &hp), AZ_IOT_OK);
  assert_int_equal(hp.connection_profile, AZ_IOT_CONNECTION_PROFILE_CLASSIC);
  assert_string_equal(hp.connection_profile_raw, "classic");

  profile_fixture_close(&pf);
}

/* Explicit null resolves the same way as absent -- the contract lists both. */
static void dps_null_profile_defaults_to_classic(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);
  profile_assign(&pf, k_assigned_profile_null);

  assert_hub_leg_used(&pf, pf.v3);
  profile_finish_hub_leg(&pf, false);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&pf.c, &hp), AZ_IOT_OK);
  assert_int_equal(hp.connection_profile, AZ_IOT_CONNECTION_PROFILE_CLASSIC);

  profile_fixture_close(&pf);
}

/* Development can exercise a real AEG assignment before the DPS api-version
 * carrying connectionProfile ships: keep the assigned host and identity, but
 * synthesize only the missing profile at the same parser boundary. */
static void dps_absent_profile_can_be_overridden_to_mqtt_v5(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open_with_override(&pf, "mqttV5");
  profile_assign(&pf, k_assigned_body);

  assert_hub_leg_used(&pf, pf.v5);
  profile_finish_hub_leg(&pf, true);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&pf.c, &hp), AZ_IOT_OK);
  assert_int_equal(hp.connection_profile, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  assert_string_equal(hp.connection_profile_raw, "mqttV5");

  profile_fixture_close(&pf);
}

static void dps_null_profile_can_be_overridden_to_mqtt_v5(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open_with_override(&pf, "mqttV5");
  profile_assign(&pf, k_assigned_profile_null);

  assert_hub_leg_used(&pf, pf.v5);
  profile_finish_hub_leg(&pf, true);

  profile_fixture_close(&pf);
}

/* Once DPS sends the property, the wire is authoritative. Leaving an override
 * in a developer's environment must not mask service rollout or a real profile. */
static void dps_wire_profile_wins_over_the_development_override(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  /* Deliberately invalid: proving a wire value wins means proving the
   * environment is not even validated once that value exists. */
  profile_fixture_open_with_override(&pf, "not-a-profile");
  profile_assign(&pf, k_assigned_classic);

  assert_hub_leg_used(&pf, pf.v3);
  profile_finish_hub_leg(&pf, false);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&pf.c, &hp), AZ_IOT_OK);
  assert_int_equal(hp.connection_profile, AZ_IOT_CONNECTION_PROFILE_CLASSIC);
  assert_string_equal(hp.connection_profile_raw, "classic");

  profile_fixture_close(&pf);
}

static void dps_classic_development_override_is_accepted(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open_with_override(&pf, "classic");
  profile_assign(&pf, k_assigned_body);

  assert_hub_leg_used(&pf, pf.v3);
  profile_finish_hub_leg(&pf, false);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&pf.c, &hp), AZ_IOT_OK);
  assert_int_equal(hp.connection_profile, AZ_IOT_CONNECTION_PROFILE_CLASSIC);
  assert_string_equal(hp.connection_profile_raw, "classic");

  profile_fixture_close(&pf);
}

/* A typo must fail at the assignment instead of silently selecting Classic,
 * which would connect with the wrong MQTT version and surface much later. */
static void dps_invalid_development_override_faults_before_the_hub(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open_with_override(&pf, "mqtt-v5");
  profile_assign(&pf, k_assigned_body);

  assert_true(az_iot_test_saw_state(&pf.log, AZ_IOT_CONN_STATE_FAULTED));
  assert_int_equal(
      az_iot_test_reason_for(&pf.log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_INVALID_ARG);
  assert_null(az_iot_mock_mqtt_factory_last_client(pf.v5));

  profile_fixture_close(&pf);
}

static void dps_overlong_development_override_faults_before_the_hub(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open_with_override(
      &pf, "mqttV5-with-an-absurdly-long-development-override-that-will-not-fit-in-the-buffer");
  profile_assign(&pf, k_assigned_body);

  assert_true(az_iot_test_saw_state(&pf.log, AZ_IOT_CONN_STATE_FAULTED));
  assert_int_equal(
      az_iot_test_reason_for(&pf.log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_INVALID_ARG);
  assert_null(az_iot_mock_mqtt_factory_last_client(pf.v5));

  profile_fixture_close(&pf);
}

/* An unrecognised profile means the SDK does not know which wire protocol to
 * speak. Guessing would produce a device that appears to connect and then
 * misbehaves; failing closed produces one clear error. */
static void dps_unknown_profile_faults_the_connection(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);
  profile_assign(&pf, k_assigned_profile_unknown);

  assert_true(az_iot_test_saw_state(&pf.log, AZ_IOT_CONN_STATE_FAULTED));
  assert_int_equal(
      az_iot_test_reason_for(&pf.log, AZ_IOT_CONN_STATE_FAULTED),
      AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED);
  assert_false(az_iot_test_saw_state(&pf.log, AZ_IOT_CONN_STATE_CONNECTED));
  /* No hub leg was attempted on either generation. */
  assert_null(az_iot_mock_mqtt_factory_last_client(pf.v5));

  profile_fixture_close(&pf);
}

/* The value must survive the trip into C even when the enum cannot hold it.
 * Discarding it would turn a deliberately forward-compatible wire format into a
 * lossy one at the library boundary -- and this is exactly the case where a
 * support engineer needs to see what the service actually said. */
static void dps_unknown_profile_is_still_reported_verbatim(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);
  profile_assign(&pf, k_assigned_profile_unknown);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&pf.c, &hp), AZ_IOT_OK);
  assert_int_equal(hp.connection_profile, AZ_IOT_CONNECTION_PROFILE_UNKNOWN);
  assert_string_equal(hp.connection_profile_raw, "mqttV9-quantum");
  assert_false(hp.connection_profile_raw_truncated);

  profile_fixture_close(&pf);
}

/* A value too long for the buffer cannot be reported verbatim, so the contract
 * is that it is reported as a PREFIX and says so. Silently handing back a
 * shortened string as if it were what the service sent would be the one
 * genuinely misleading outcome -- an operator would chase a profile name that
 * was never on the wire. */
static void dps_overlong_profile_is_flagged_as_truncated(void** state)
{
  (void)state;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);
  profile_assign(&pf, k_assigned_profile_overlong);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&pf.c, &hp), AZ_IOT_OK);
  assert_true(hp.connection_profile_raw_truncated);
  /* Unknown, so the connection fails closed rather than guessing a protocol. */
  assert_int_equal(hp.connection_profile, AZ_IOT_CONNECTION_PROFILE_UNKNOWN);
  assert_int_equal(
      az_iot_test_reason_for(&pf.log, AZ_IOT_CONN_STATE_FAULTED),
      AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED);
  /* What was kept is a genuine prefix of what was sent, not arbitrary bytes. */
  assert_int_equal(strlen(hp.connection_profile_raw), AZ_IOT_CONNECTION_PROFILE_RAW_BUF - 1);
  assert_memory_equal(
      hp.connection_profile_raw,
      "mqttV5-with-an-absurdly-long",
      strlen("mqttV5-with-an-absurdly-long"));

  profile_fixture_close(&pf);
}

/* ---- get_hub_profile() contract ----------------------------------------- */

/* On the DPS path the profile is genuinely unknown until provisioning finishes,
 * so reading it early has to fail rather than report a plausible default. */
static void get_hub_profile_before_connected_is_rejected(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(&c, &hp), AZ_IOT_ERR_NOT_CONNECTED);

  az_iot_connection_client_destroy(&c);
}

/* The size stamp is what will let this struct grow without breaking callers, so
 * an unstamped `= {0}` has to be refused now -- while there are no shipped
 * callers -- rather than silently misread later. */
static void get_hub_profile_rejects_an_unstamped_struct(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);

  az_iot_hub_profile hp = { 0 }; /* the mistake this guard exists for */
  assert_int_equal(az_iot_connection_client_get_hub_profile(&c, &hp), AZ_IOT_ERR_INVALID_ARG);

  az_iot_connection_client_destroy(&c);
}

/* A caller built against a newer header expects fields this build never writes;
 * reporting success would leave them reading uninitialized memory. */
static void get_hub_profile_rejects_a_newer_caller_struct(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);

  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  hp._internal_size = (uint32_t)(sizeof(az_iot_hub_profile) + 8u);
  assert_int_equal(az_iot_connection_client_get_hub_profile(&c, &hp), AZ_IOT_ERR_NOT_SUPPORTED);

  az_iot_connection_client_destroy(&c);
}

static void get_hub_profile_rejects_null_arguments(void** state)
{
  (void)state;
  az_iot_hub_profile hp = AZ_IOT_HUB_PROFILE_INIT;
  assert_int_equal(az_iot_connection_client_get_hub_profile(NULL, &hp), AZ_IOT_ERR_INVALID_ARG);

  az_iot_connection_client_options opts = dps_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_get_hub_profile(&c, NULL), AZ_IOT_ERR_INVALID_ARG);
  az_iot_connection_client_destroy(&c);
}

/* UNKNOWN only ever comes back FROM the service. A caller declaring it is asking
 * the SDK to speak a protocol it has no implementation for, so it is refused at
 * the boundary rather than silently falling through to classic. */
static void init_rejects_a_connection_profile_the_sdk_cannot_speak(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = dps_options();
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_UNKNOWN;

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_ERR_INVALID_ARG);
}

/* A reassignment can move a device to a different generation. Filters registered
 * for the old one must not be re-issued at the new hub -- AEG does not grant
 * $iothub/..., and once CONNECTED is gated on those SUBACKs (P1c) a session
 * carrying them could never come up. Dropping them is what keeps the two P1c
 * fixes from deadlocking each other. */
static void reassignment_to_another_generation_drops_the_old_filters(void** state)
{
  (void)state;
  int owner = 0;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);

  /* A Classic-shaped filter, registered while the client still defaults to
   * classic -- exactly what a gen1 feature client would have left behind. */
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          &pf.c,
          "$iothub/twin/res/#",
          AZ_IOT_MQTT_QOS_1,
          &owner,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_OK);

  profile_assign(&pf, k_assigned_mqtt_v5);
  assert_hub_leg_used(&pf, pf.v5);

  /* Drive the presence handshake to completion, because it is announce_connected
   * -- reached only on the birth-ack -- that re-issues the persistent filters.
   * Stopping at CONNACK would make this test pass whether or not the stale
   * filter was dropped. */
  profile_finish_hub_leg(&pf, true);
  assert_int_equal(az_iot_test_last_state(&pf.log), AZ_IOT_CONN_STATE_CONNECTED);

  /* The only SUBSCRIBE on a gen2 session is the presence handshake's own
   * ih/{id}/dev/#; the Classic filter must not have come along. */
  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(pf.v5);
  assert_non_null(hub);
  assert_int_not_equal(az_iot_mock_mqtt_client_count_of(hub, AZ_IOT_MOCK_CALL_SUBSCRIBE), 0);
  for (size_t i = 0; i < az_iot_mock_mqtt_client_call_count(hub); ++i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(hub, i);
    assert_non_null(call);
    if (call->kind != AZ_IOT_MOCK_CALL_SUBSCRIBE)
    {
      continue;
    }
    assert_null(strstr(call->topic, "$iothub/"));
  }

  profile_fixture_close(&pf);
}

/* Removal withdraws the filter from the broker on gen2 as well, not just on
 * Classic. The device-wide ih/{id}/dev/# subscription is not endangered by
 * that: the presence handshake issues it directly rather than through the
 * persistent-subscription registry, so it has no owner and removal can never
 * select it. Anything a gen2 feature client did register is its own, and
 * leaving it live until the session ends would be the very slot leak this
 * change exists to close. */
static void removal_on_gen2_unsubscribes_only_the_owners_filter(void** state)
{
  (void)state;
  int owner = 0;
  profile_fixture pf = { 0 };
  profile_fixture_open(&pf);

  profile_assign(&pf, k_assigned_mqtt_v5);
  assert_hub_leg_used(&pf, pf.v5);
  profile_finish_hub_leg(&pf, true);
  assert_int_equal(az_iot_test_last_state(&pf.log), AZ_IOT_CONN_STATE_CONNECTED);

  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(pf.v5);
  assert_non_null(hub);

  /* A filter registered underneath the presence wildcard. No gen2 feature
   * client does this any more -- the wildcard covers them -- but an application
   * custom topic will, and the property being pinned is about the registry, not
   * about which caller filled it. */
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          &pf.c,
          "ih/assigned-device/dev/twin/desired",
          AZ_IOT_MQTT_QOS_1,
          &owner,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client__remove_subscriptions_for(&pf.c, &owner), 1);

  /* Exactly one UNSUBSCRIBE, and it is the feature filter -- never the
   * device-wide wildcard the presence handshake owns. */
  assert_int_equal(az_iot_mock_mqtt_client_count_of(hub, AZ_IOT_MOCK_CALL_UNSUBSCRIBE), 1);
  const az_iot_mock_call* uns = az_iot_mock_mqtt_client_last_of(hub, AZ_IOT_MOCK_CALL_UNSUBSCRIBE);
  assert_non_null(uns);
  assert_string_equal(uns->topic, "ih/assigned-device/dev/twin/desired");

  profile_fixture_close(&pf);
}

/* ------------------------------------------------------------------------- */
/* close() during provisioning                                               */
/* ------------------------------------------------------------------------- */

/* Before provisioning completes there is a DPS session but no hub adapter, so
 * close() has to cancel that session itself. Without this it reported
 * NOT_INITIALIZED and left the client in CONNECTING with nothing able to take
 * it out -- the same shape as a fault. */
static void close_during_provisioning_returns_to_idle(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)dps_open(fx);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_DPS),
      AZ_IOT_CONN_STATE_CONNECTING);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);
  assert_false(az_iot_connection_client__dps_session_ready(fx->client));

  /* And the client is reusable. */
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_CONNECTING);
}

/* A registration response that arrived just before the close must not move a
 * client the application has already closed. */
static void close_during_provisioning_drops_the_pending_outcome(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);

  /* Queued, not yet applied: the deferred finalize runs from do_work(). */
  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_failed_body));
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);

  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
}

/* An assignment the SDK cannot speak must not be adopted. If opts.host were
 * rewritten before the profile was checked, the next open() would see a host,
 * skip DPS and connect to that hub with the pre-provisioning session role --
 * speaking a protocol the service just said this hub does not use. */
static void an_unsupported_profile_does_not_adopt_the_assigned_hub(void** state)
{
  (void)state;
  set_dps_profile_override(NULL);

  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);
  az_iot_connection_client_options opts = dps_options();
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);
  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);
  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_profile_unknown));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED),
      AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED);

  /* The rejected assignment was not adopted. */
  assert_null(fx->client->opts.host);

  /* So the retry asks DPS again instead of connecting to the rejected hub. */
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* second = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(second);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(second, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "global.azure-devices-provisioning.net");

  az_iot_connection_client_destroy(&fx->client_storage);
  free(fx);
}

/* ------------------------------------------------------------------------- */
/* a rejected or unusable assignment must not leave a reusable cached one     */
/*                                                                           */
/* These all exist because FAULTED is now recoverable: close() + open() is a  */
/* supported retry, and open() skips DPS whenever opts.host is set. Anything  */
/* that faults while an assignment is cached therefore has to say explicitly  */
/* that the cache is no good, or the retry walks straight back into it.       */
/* ------------------------------------------------------------------------- */

/* A device that has already provisioned, re-provisions, and is handed a
 * profile this SDK cannot speak. The new assignment is refused -- but the
 * PREVIOUS one is still cached, so the retry must still go to DPS. */
static void a_rejected_reprovision_does_not_reuse_the_cached_hub(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  set_dps_profile_override(NULL);

  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);
  assert_string_equal(fx->client->opts.host, "myhub.azure-devices.net");

  /* Hub refuses the identity, so the client goes back to DPS. */
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* dps2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(dps2);
  assert_string_equal(last_connect_host(dps2), "global.azure-devices-provisioning.net");
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(dps2, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(dps2, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* The new assignment carries a profile the SDK cannot speak. */
  assert_true(inject_dps_response(dps2, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_profile_unknown));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED),
      AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED);

  /* The retry must re-provision, NOT reconnect to the hub still in opts.host. */
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* next = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(next);
  assert_string_equal(last_connect_host(next), "global.azure-devices-provisioning.net");
}

/* The response parser accepts an empty device id -- it rejects only a negative
 * or oversized one -- so an assignment can be half usable. Neither half may be
 * adopted, or the client would hold the new hub with the previous device id. */
static void a_half_usable_assignment_is_not_partially_adopted(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  set_dps_profile_override(NULL);

  static const char k_assigned_no_device_id[]
      = "{\"operationId\":\"op-1\",\"status\":\"assigned\","
        "\"registrationState\":{\"registrationId\":\"ut-device\","
        "\"assignedHub\":\"myhub.azure-devices.net\",\"deviceId\":\"\"}}";

  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);
  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_no_device_id));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  /* The hub half was NOT committed on the way to the fault. */
  assert_null(fx->client->opts.host);

  /* And the retry provisions again rather than connecting to a half-assignment. */
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* next = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(next);
  assert_string_equal(last_connect_host(next), "global.azure-devices-provisioning.net");
}

/* An identity rejection records that the device must re-provision, and the
 * same failure can exhaust the policy and fault. close() must not throw that
 * intent away: the cached host would otherwise send open() back to the hub
 * that just rejected this identity. */
static void the_reprovision_demand_survives_close_and_open(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  fx->client->opts.reconnection_policy.max_attempts = 1;

  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);

  /* Identity rejected: sets the re-provision demand and starts the DPS ladder.
   * It does NOT fault here -- the DPS ladder has its own budget, which is the
   * point of the per-scope counters. */
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);

  az_iot_test_wait_ms(REPROVISION_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_mock_mqtt_client* dps2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(dps2);
  assert_string_equal(last_connect_host(dps2), "global.azure-devices-provisioning.net");

  /* Now exhaust the DPS ladder, so the client faults still holding the demand. */
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps2, AZ_IOT_ERR_MQTT));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* next = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(next);
  assert_string_equal(last_connect_host(next), "global.azure-devices-provisioning.net");
}

/* ------------------------------------------------------------------------- */
/* the two retry ladders are independent                                     */
/* ------------------------------------------------------------------------- */

/* A real exponential ladder, unlike setup_with_reconnect's deliberately flat
 * one, so a position on it is observable in the scheduled delay. */
#define LADDER_INITIAL_MS 10u

static int setup_with_exponential_reconnect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = dps_options();
  opts.reconnection_policy.initial_delay_ms = LADDER_INITIAL_MS;
  opts.reconnection_policy.max_delay_ms = 10000u;
  opts.reconnection_policy.max_attempts = 0; /* forever */
  opts.reconnection_policy.jitter_pct = 0; /* deterministic */
  opts.dps.max_hub_connect_attempts_before_reprovision = 3;
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);
  az_iot_connection_client__seed_rng(fx->client, 0xC0FFEEFEEDFACEull);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

/* Crossing into provisioning must start the DPS ladder at initial_delay_ms.
 * With one shared counter the first registration retry inherited the hub's
 * exhausted backoff, so a device that had been failing against a dead hub for
 * a while waited at the cap before asking DPS where it actually lives. */
/* ------------------------------------------------------------------------- */
/* scoped state: DPS and the hub are two independent lifecycles              */
/* ------------------------------------------------------------------------- */

/* dps_start() announces DPS:CONNECTING before it calls connect(), so a
 * SYNCHRONOUS connect failure has to settle the lifecycle again -- there is no
 * inbound event coming to do it later.
 *
 * Exercised through dps_session_ensure(), the path a FEATURE client uses.
 * open() happens to settle the scope on its own failure path, so a test that
 * went through open() would pass whether or not dps_start() cleaned up after
 * itself -- and the feature-client path would still leak.
 *
 * Left pinned at CONNECTING, two things break: the next dps_start() announces
 * nothing, because the value is unchanged, so a retry is invisible; and open()
 * would see a DPS lifecycle it cannot explain. */
static void a_synchronous_dps_connect_failure_settles_the_scope(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  /* Stand in for a feature client holding an interest in the session. */
  assert_int_equal(az_iot_connection_client__dps_user_acquire(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_factory_fail_next_connect(fx->factory, AZ_IOT_ERR_MQTT);
  assert_int_not_equal(az_iot_connection_client__dps_session_ensure(fx->client), AZ_IOT_ERR_BUSY);

  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_DPS),
      AZ_IOT_CONN_STATE_IDLE);
  /* The hub lifecycle was never involved. */
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_IDLE);
}
/* The headline change. A DPS-provisioned device runs provisioning and then the
 * hub; before scopes both collapsed into one enum, so the whole provisioning
 * phase was invisible -- set_state_to() suppressed the hub's CONNECTING because
 * the VALUE already matched the DPS one, and a device could sit in CONNECTING
 * for a minute with no way to tell which half it was in. */
static void a_dps_run_reports_both_lifecycles_in_order(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  size_t dps_connecting
      = az_iot_test_index_of(&fx->log, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_CONNECTING);
  size_t hub_connecting
      = az_iot_test_index_of(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_CONNECTING);
  size_t hub_connected
      = az_iot_test_index_of(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_CONNECTED);

  assert_int_not_equal(dps_connecting, SIZE_MAX);
  assert_int_not_equal(hub_connecting, SIZE_MAX);
  assert_int_not_equal(hub_connected, SIZE_MAX);
  /* Provisioning first, then the hub. The hub's CONNECTING is the one the old
   * single-state machine swallowed. */
  assert_true(dps_connecting < hub_connecting);
  assert_true(hub_connecting < hub_connected);
}

/* The per-scope comparison in set_state_to(). With one shared `state` the
 * second CONNECTING is dropped as a no-op; this is the regression that would
 * reintroduce the invisible provisioning phase. */
static void the_hub_connecting_is_not_swallowed_by_the_dps_one(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)provision_to_hub_connecting(fx);

  assert_int_equal(
      az_iot_test_count_for(&fx->log, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_CONNECTING), 1);
  assert_int_equal(
      az_iot_test_count_for(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_CONNECTING), 1);
}

/* Same (scope, state) twice in a row is still one event: the suppression is
 * per scope, not abandoned. */
static void a_repeated_state_in_one_scope_is_still_suppressed(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  /* A second CONNACK on a session already CONNECTED must not re-announce. */
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(
      az_iot_test_count_for(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_CONNECTED), 1);
}

/* A registration that fails is a DPS-scope event. The hub never started, so its
 * lifecycle must never have left IDLE -- an application waiting on
 * HUB:CONNECTED must not be told the hub faulted when there was no hub. */
static void a_registration_failure_is_reported_on_the_dps_scope_only(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* dps = dps_open_to_registering(fx);
  assert_true(inject_dps_response(dps, DPS_RESPONSE_TOPIC_ASSIGNED, k_failed_body));
  for (int i = 0; i < 3; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_IDLE);
  assert_int_equal(
      az_iot_test_count_for(&fx->log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED), 0);
  assert_int_not_equal(
      az_iot_test_last_state_for(&fx->log, AZ_IOT_CONN_SCOPE_DPS), AZ_IOT_CONN_STATE_IDLE);
}

/* The scoped getter is the supported way to ask, and the two lifecycles
 * genuinely differ: the provisioning session is torn down at registration, so
 * its scope settles to IDLE while the hub is CONNECTED.
 *
 * Asserted as IDLE, not merely "not CONNECTED": leaving the DPS scope pinned at
 * CONNECTING after its session died would satisfy the weaker form, and would
 * then silently suppress the CONNECTING of any later re-provisioning run. */
static void the_scoped_getter_reports_each_lifecycle_separately(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_CONNECTED);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_DPS),
      AZ_IOT_CONN_STATE_IDLE);
  /* And the teardown was announced, not just applied. */
  assert_int_equal(
      az_iot_test_count_for(&fx->log, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_DISCONNECTING), 1);
}

/* A hub failure that diverts to re-provisioning must fire the retry ONCE.
 *
 * `reconnect_due_ms` is the pending-retry token and firing consumes it. Gating
 * the retry on the STATE alone is not enough once the scopes are independent:
 * a hub failure whose recovery is a re-registration leaves HUB in RECONNECTING
 * while the attempt runs on DPS, so a state-only gate re-fires dps_start() on
 * every tick for as long as the hub stays down.
 *
 * Asserted on the token rather than on the state log on purpose: a repeated
 * dps_start() re-announces nothing, because DPS is already CONNECTING and
 * set_state_to() suppresses it -- so the storm is invisible in the log and a
 * log-based assertion would pass while the device hammered the service. */
static void a_diverted_retry_consumes_its_pending_token(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* A retry really is pending, and it is the hub scope that is waiting even
   * though the attempt will be a registration. */
  assert_int_not_equal(fx->client->reconnect_due_ms, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_RECONNECTING);

  az_iot_test_wait_until_ms(fx->client->reconnect_due_ms);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* Fired, and the token is spent: further ticks cannot start another attempt
   * while this one is in flight. */
  assert_int_equal(fx->client->reconnect_due_ms, 0);
  for (int i = 0; i < 20; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  assert_int_equal(fx->client->reconnect_due_ms, 0);
}

/* close() is a statement about the whole client: whichever scope was waiting or
 * faulted, both settle. */
static void close_settles_both_lifecycles(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* hub = provision_to_hub_connecting(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_IDLE);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_DPS),
      AZ_IOT_CONN_STATE_IDLE);
}

static void the_dps_ladder_does_not_inherit_the_hub_backoff(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;

  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);
  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_body));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  /* Climb the hub ladder: 10, 20, 40 ms. The third failure crosses
   * max_hub_connect_attempts_before_reprovision and switches to DPS. */
  uint64_t last_delay = 0;
  for (int i = 0; i < 3; ++i)
  {
    az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(hub);
    uint64_t before = az_iot_time_mono_ms();
    assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_MQTT));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    (void)az_iot_connection_client_do_work(fx->client, 0);
    assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
    last_delay = fx->client->reconnect_due_ms - before;
    if (i < 2)
    {
      az_iot_test_wait_ms((unsigned)last_delay + 5u);
      (void)az_iot_connection_client_do_work(fx->client, 0);
    }
  }

  /* The hub ladder had reached 40 ms; the DPS one starts over at 10 ms. */
  assert_true(fx->client->needs_reprovision);
  assert_true(last_delay <= (uint64_t)LADDER_INITIAL_MS + 2u);
  assert_int_equal(fx->client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS], 1);

  /* And the attempt that deadline schedules really is a registration -- the
   * observable fact the short delay is only evidence for. */
  az_iot_test_wait_ms((unsigned)last_delay + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_mock_mqtt_client* next = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(next);
  assert_string_equal(last_connect_host(next), "global.azure-devices-provisioning.net");
}

/* max_attempts is a budget PER ladder. With one shared counter a long hub
 * outage could leave a registration that would have succeeded first try with
 * no attempts left at all. */
static void the_hub_ladder_cannot_spend_the_dps_budget(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  /* Two hub retries is the whole budget; the fixture crosses to DPS on the
   * third consecutive hub failure. */
  fx->client->opts.reconnection_policy.max_attempts = 2;

  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);
  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_body));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  for (int i = 0; i < 3; ++i)
  {
    az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(hub);
    assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_MQTT));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    (void)az_iot_connection_client_do_work(fx->client, 0);
    assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
    az_iot_test_wait_until_ms(fx->client->reconnect_due_ms);
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  /* The hub ladder is at its limit -- one more hub retry would fault -- yet a
   * registration attempt was still made on a budget of its own. */
  assert_int_equal(
      fx->client->retry_attempt[AZ_IOT_CONN_SCOPE_HUB],
      fx->client->opts.reconnection_policy.max_attempts);
  assert_int_equal(fx->client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS], 1);
  az_iot_mock_mqtt_client* dps2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(dps2);
  assert_string_equal(last_connect_host(dps2), "global.azure-devices-provisioning.net");
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
}

/* A successful registration clears both ladders: the new assignment has not
 * failed, so its first connect must not back off for the old hub's failures. */
static void a_successful_registration_clears_both_ladders(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;

  az_iot_mock_mqtt_client* m = dps_open_to_registering(fx);
  assert_true(inject_dps_response(m, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_body));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  /* Two hub failures, then an identity rejection that sends us back to DPS. */
  for (int i = 0; i < 2; ++i)
  {
    az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(hub);
    assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_MQTT));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    (void)az_iot_connection_client_do_work(fx->client, 0);
    az_iot_test_wait_until_ms(fx->client->reconnect_due_ms);
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
  assert_true(fx->client->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] > 0);

  az_iot_mock_mqtt_client* hub = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(hub);
  assert_true(az_iot_mock_mqtt_client_inject_connected(hub, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_test_wait_until_ms(fx->client->reconnect_due_ms);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* Re-register successfully. */
  az_iot_mock_mqtt_client* dps2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(dps2);
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(dps2, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(dps2, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(inject_dps_response(dps2, DPS_RESPONSE_TOPIC_ASSIGNED, k_assigned_body));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }

  assert_int_equal(fx->client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS], 0);
  assert_int_equal(fx->client->retry_attempt[AZ_IOT_CONN_SCOPE_HUB], 0);
}

/* close() is legal from inside the state callback, and the CONNECTING
 * announcement in dps_start() runs that callback synchronously -- before
 * dps_start() has finished with the adapter it just stored. A close() there
 * destroys that adapter, so dps_start() must notice rather than keep using it. */
typedef struct
{
  az_iot_connection_client* client;
  az_iot_test_state_log* log;
  int closed;
} close_from_callback_ctx;

static void close_on_connecting(const az_iot_connection_state_event* event, void* user_ctx)
{
  close_from_callback_ctx* ctx = (close_from_callback_ctx*)user_ctx;
  az_iot_test_on_state(event, ctx->log);
  if (event->state == AZ_IOT_CONN_STATE_CONNECTING && ctx->closed == 0)
  {
    ctx->closed = 1;
    (void)az_iot_connection_client_close(ctx->client);
  }
}

static void closing_from_the_connecting_callback_abandons_the_session(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  close_from_callback_ctx ctx = { fx->client, &fx->log, 0 };
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, close_on_connecting, &ctx),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);

  /* Must not touch the adapter the callback already destroyed. */
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_NOT_CONNECTED);

  assert_int_equal(ctx.closed, 1);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_IDLE);
  assert_false(az_iot_connection_client__dps_session_ready(fx->client));

  /* The pump must be safe afterwards too. */
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(fx->client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_IDLE);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    /* endpoint + version selection */
    cmocka_unit_test_setup_teardown(
        dps_connects_to_the_global_endpoint_by_default, setup, teardown),
    cmocka_unit_test(dps_honors_the_configured_timings),
    cmocka_unit_test(dps_defaults_the_timings_when_unset),
    cmocka_unit_test(dps_carries_the_proxy_and_transport),
    cmocka_unit_test(dps_honors_a_custom_global_endpoint),
    cmocka_unit_test(dps_uses_v3_1_1_even_when_the_hub_is_next),
    cmocka_unit_test(dps_without_a_v3_1_1_factory_is_not_supported),
    /* register handshake */
    cmocka_unit_test_setup_teardown(
        dps_subscribes_the_registration_response_topic, setup, teardown),
    cmocka_unit_test_setup_teardown(dps_publishes_register_only_after_the_suback, setup, teardown),
    /* polling */
    cmocka_unit_test_setup_teardown(
        dps_polls_operation_status_after_an_assigning_response, setup, teardown),
    cmocka_unit_test_setup_teardown(dps_honors_the_retry_after_delay, setup, teardown),
    /* assignment handoff */
    cmocka_unit_test_setup_teardown(dps_assignment_connects_to_the_assigned_hub, setup, teardown),
    cmocka_unit_test_setup_teardown(dps_assignment_uses_the_assigned_device_id, setup, teardown),
    cmocka_unit_test_setup_teardown(
        dps_session_reaches_connected_after_assignment, setup, teardown),
    /* failure paths */
    cmocka_unit_test_setup_teardown(dps_failed_status_faults_with_a_dps_error, setup, teardown),
    cmocka_unit_test_setup_teardown(dps_disabled_status_faults_with_a_dps_error, setup, teardown),
    cmocka_unit_test_setup_teardown(
        dps_failed_status_retries_under_the_policy, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        dps_failed_status_retries_against_dps, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        dps_failed_status_still_honors_max_attempts, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_dps_retry_stays_on_dps_when_no_hub_is_known, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        a_service_retry_after_outranks_the_policy_backoff, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        a_failing_auxiliary_session_does_not_tear_down_the_hub, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(dps_connack_failure_faults, setup, teardown),
    cmocka_unit_test_setup_teardown(dps_suback_failure_faults, setup, teardown),
    cmocka_unit_test_setup_teardown(dps_disconnect_midflow_faults, setup, teardown),
    cmocka_unit_test_setup_teardown(
        dps_malformed_response_faults_with_a_protocol_error, setup, teardown),
    cmocka_unit_test_setup_teardown(
        dps_empty_response_body_faults_without_a_null_deref, setup, teardown),
    /* identity validation */
    cmocka_unit_test(dps_rejects_a_null_registration_id),
    cmocka_unit_test(dps_rejects_an_empty_registration_id),
    cmocka_unit_test(dps_rejected_identity_leaves_the_client_idle),
    /* re-provisioning after an identity rejection */
    cmocka_unit_test_setup_teardown(
        hub_identity_rejection_reprovisions_through_dps, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        hub_transport_error_reconnects_without_reprovisioning, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        hub_unreachable_past_the_threshold_reprovisions,
        setup_with_reprovision_threshold,
        teardown),
    cmocka_unit_test_setup_teardown(
        a_zero_threshold_never_reprovisions, setup_with_reprovision_threshold, teardown),
    cmocka_unit_test_setup_teardown(
        a_successful_connect_clears_the_hub_failure_count_without_an_observer,
        setup_with_reprovision_threshold,
        teardown),
    cmocka_unit_test_setup_teardown(
        a_successful_hub_connection_resets_the_failure_count,
        setup_with_reprovision_threshold,
        teardown),
    cmocka_unit_test_setup_teardown(
        reprovisioning_connects_to_the_new_assignment, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        repeated_identity_rejection_still_honors_max_attempts, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(identity_rejection_without_a_policy_faults, setup, teardown),
    cmocka_unit_test_setup_teardown(
        identity_rejection_without_a_policy_still_reprovisions_on_reopen, setup, teardown),
    /* connection profile */
    cmocka_unit_test(dps_mqtt_v5_profile_connects_the_hub_over_v5),
    cmocka_unit_test(dps_classic_profile_connects_the_hub_over_v3_1_1),
    cmocka_unit_test(classic_state_events_are_stamped_and_profile_only_on_connected),
    cmocka_unit_test(mqtt_v5_state_events_are_stamped_and_profile_only_on_connected),
    cmocka_unit_test(dps_absent_profile_defaults_to_classic),
    cmocka_unit_test(dps_null_profile_defaults_to_classic),
    cmocka_unit_test(dps_absent_profile_can_be_overridden_to_mqtt_v5),
    cmocka_unit_test(dps_null_profile_can_be_overridden_to_mqtt_v5),
    cmocka_unit_test(dps_wire_profile_wins_over_the_development_override),
    cmocka_unit_test(dps_classic_development_override_is_accepted),
    cmocka_unit_test(dps_invalid_development_override_faults_before_the_hub),
    cmocka_unit_test(dps_overlong_development_override_faults_before_the_hub),
    cmocka_unit_test(dps_unknown_profile_faults_the_connection),
    cmocka_unit_test(dps_unknown_profile_is_still_reported_verbatim),
    cmocka_unit_test(dps_overlong_profile_is_flagged_as_truncated),
    cmocka_unit_test(get_hub_profile_before_connected_is_rejected),
    cmocka_unit_test(get_hub_profile_rejects_an_unstamped_struct),
    cmocka_unit_test(get_hub_profile_rejects_a_newer_caller_struct),
    cmocka_unit_test(get_hub_profile_rejects_null_arguments),
    cmocka_unit_test(init_rejects_a_connection_profile_the_sdk_cannot_speak),
    cmocka_unit_test(reassignment_to_another_generation_drops_the_old_filters),
    cmocka_unit_test(removal_on_gen2_unsubscribes_only_the_owners_filter),
    /* close() during provisioning */
    cmocka_unit_test_setup_teardown(close_during_provisioning_returns_to_idle, setup, teardown),
    cmocka_unit_test_setup_teardown(
        close_during_provisioning_drops_the_pending_outcome, setup, teardown),
    cmocka_unit_test(an_unsupported_profile_does_not_adopt_the_assigned_hub),
    /* a rejected or unusable assignment must not leave a reusable cached one */
    cmocka_unit_test_setup_teardown(
        a_rejected_reprovision_does_not_reuse_the_cached_hub, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        a_half_usable_assignment_is_not_partially_adopted, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_reprovision_demand_survives_close_and_open, setup_with_reconnect, teardown),
    /* scoped state: the two lifecycles are independent */
    cmocka_unit_test_setup_teardown(
        a_synchronous_dps_connect_failure_settles_the_scope, setup, teardown),
    cmocka_unit_test_setup_teardown(a_dps_run_reports_both_lifecycles_in_order, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_hub_connecting_is_not_swallowed_by_the_dps_one, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_repeated_state_in_one_scope_is_still_suppressed, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_registration_failure_is_reported_on_the_dps_scope_only, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_scoped_getter_reports_each_lifecycle_separately, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_diverted_retry_consumes_its_pending_token, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(close_settles_both_lifecycles, setup_with_reconnect, teardown),
    /* the two retry ladders are independent */
    cmocka_unit_test_setup_teardown(
        the_dps_ladder_does_not_inherit_the_hub_backoff,
        setup_with_exponential_reconnect,
        teardown),
    cmocka_unit_test_setup_teardown(
        the_hub_ladder_cannot_spend_the_dps_budget, setup_with_exponential_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        a_successful_registration_clears_both_ladders, setup_with_exponential_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        closing_from_the_connecting_callback_abandons_the_session, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
