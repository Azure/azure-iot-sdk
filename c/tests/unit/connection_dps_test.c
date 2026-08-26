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
      az_iot_connection_client_set_state_callback(fx->client, az_iot_test_on_state, &fx->log),
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
      az_iot_connection_client_set_state_callback(fx->client, az_iot_test_on_state, &fx->log),
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
      az_iot_connection_client_set_state_callback(&c, az_iot_test_on_state, &log), AZ_IOT_OK);

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
  az_iot_connection_client_options opts = dps_options();
  assert_int_equal(az_iot_connection_client_init(&pf->c, &opts), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_set_state_callback(&pf->c, az_iot_test_on_state, &pf->log),
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

static void profile_fixture_close(profile_fixture* pf) { az_iot_connection_client_destroy(&pf->c); }

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
          &pf.c, "$iothub/twin/res/#", AZ_IOT_MQTT_QOS_1, &owner),
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

  /* A per-feature filter of the shape gen2 feature clients register today,
   * underneath the presence wildcard. */
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          &pf.c, "ih/assigned-device/dev/twin/desired", AZ_IOT_MQTT_QOS_1, &owner),
      AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client__remove_subscriptions_for(&pf.c, &owner), 1);

  /* Exactly one UNSUBSCRIBE, and it is the feature filter -- never the
   * device-wide wildcard the presence handshake owns. */
  assert_int_equal(az_iot_mock_mqtt_client_count_of(hub, AZ_IOT_MOCK_CALL_UNSUBSCRIBE), 1);
  const az_iot_mock_call* uns
      = az_iot_mock_mqtt_client_last_of(hub, AZ_IOT_MOCK_CALL_UNSUBSCRIBE);
  assert_non_null(uns);
  assert_string_equal(uns->topic, "ih/assigned-device/dev/twin/desired");

  profile_fixture_close(&pf);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    /* endpoint + version selection */
    cmocka_unit_test_setup_teardown(
        dps_connects_to_the_global_endpoint_by_default, setup, teardown),
    cmocka_unit_test(dps_honors_the_configured_timings),
    cmocka_unit_test(dps_defaults_the_timings_when_unset),
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
        reprovisioning_connects_to_the_new_assignment, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        repeated_identity_rejection_still_honors_max_attempts, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(identity_rejection_without_a_policy_faults, setup, teardown),
    /* connection profile */
    cmocka_unit_test(dps_mqtt_v5_profile_connects_the_hub_over_v5),
    cmocka_unit_test(dps_classic_profile_connects_the_hub_over_v3_1_1),
    cmocka_unit_test(dps_absent_profile_defaults_to_classic),
    cmocka_unit_test(dps_null_profile_defaults_to_classic),
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
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
