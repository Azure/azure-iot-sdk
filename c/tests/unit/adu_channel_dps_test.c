// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Device-update channel over the provisioning session, at the MQTT level.
 *
 * adu_protocol_test.c pins the wire format as pure functions. This suite pins
 * the part that can only go wrong once a real connection client is underneath:
 * what actually reaches the broker, how a response is correlated back, and the
 * paths that can fault provisioning or silently lose a report --
 *
 *   - the publish and its topic/request id, and the one-at-a-time pending slot
 *   - the observer running BEFORE the provisioning parser, and a response for
 *     the channel never reaching that parser
 *   - a late or unmatched request id being consumed rather than handed on
 *   - the session going away with a request outstanding
 *   - an error response producing a verdict the engine can act on
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_version.h"

#include "internal/connection_client_internal.h"

#include "adu_channel_internal.h"

#include "support/connection_test_harness.h"
#include "support/mock_mqtt_iface.h"

/* A registration response the provisioning parser accepts, used to prove the
 * channel's observer does not disturb the provisioning flow. */
static const char k_assigned_body[]
    = "{\"operationId\":\"op-1\",\"status\":\"assigned\","
      "\"registrationState\":{\"registrationId\":\"ut-device\","
      "\"assignedHub\":\"myhub.azure-devices.net\",\"deviceId\":\"assigned-device\"}}";

typedef struct
{
  az_iot_connection_client client;
  az_iot_mqtt_factory* factory;
  az_iot_test_state_log log;

  az_iot_adu_channel_dps channel_state;
  az_iot_adu_channel channel;

  /* What the engine would have been told. */
  size_t update_count;
  char last_update[512];

  size_t result_count;
  az_iot_adu_operation last_op;
  az_iot_result last_result;
  az_iot_adu_error_action last_action;
} fixture;

static void on_update(const uint8_t* payload, size_t payload_len, void* engine_ctx)
{
  fixture* fx = (fixture*)engine_ctx;
  fx->update_count++;
  size_t n = payload_len < sizeof(fx->last_update) - 1 ? payload_len : sizeof(fx->last_update) - 1;
  memcpy(fx->last_update, payload, n);
  fx->last_update[n] = '\0';
}

static void on_result(
    az_iot_adu_operation operation,
    az_iot_result result,
    az_iot_adu_error_action action,
    void* engine_ctx)
{
  fixture* fx = (fixture*)engine_ctx;
  fx->result_count++;
  fx->last_op = operation;
  fx->last_result = result;
  fx->last_action = action;
}

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = NULL; /* DPS mode */
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  /* Short so the advisory-expiry case is testable without a long wait. */
  opts.dps_hold_timeout_ms = 50;
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);

  assert_int_equal(
      az_iot_connection_client_set_state_callback(&fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Foobar";
  dp.installed_update_id.provider = "Contoso";
  dp.installed_update_id.name = "Foobar";
  dp.installed_update_id.version = "1.0";
  assert_int_equal(
      az_iot_adu_channel_dps_init(&fx->channel_state, &fx->client, &dp, &fx->channel), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    fx->channel.vtable->close(fx->channel.ctx);
    bool adopted = (fx->client.factory_count > 0);
    az_iot_connection_client_destroy(&fx->client);
    if (!adopted)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
    free(fx);
  }
  return 0;
}

/* Drive an ALREADY-CREATED session (one dps_session_ensure() started) up to
 * usable. Distinct from open_to_registering(), which calls
 * az_iot_connection_client_open() and so cannot be used when a session object
 * already exists. */
static az_iot_mock_mqtt_client* drive_existing_session(fixture* fx)
{
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  return m;
}

/* CONNACK -> SUBACK, which is what makes the provisioning session usable. */
static az_iot_mock_mqtt_client* open_to_registering(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  return m;
}

/* Bind the channel BEFORE the session comes up, which is the supported order:
 * binding takes the hold, so the session stops for the check instead of
 * registering straight through. Binding afterwards cannot take a hold and the
 * channel then defers its operations to the next session, so a helper that
 * bound late would not be able to publish at all. */
static az_iot_mock_mqtt_client* open_and_bind(fixture* fx)
{
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  return open_to_registering(fx);
}

static bool inject_raw(az_iot_mock_mqtt_client* m, const char* topic, const char* body)
{
  return az_iot_mock_mqtt_client_inject_message(
      m, topic, (const uint8_t*)body, strlen(body), AZ_IOT_MQTT_QOS_1);
}

/* Inject and pump: an injected message is only dispatched on the next tick. */
static bool inject(fixture* fx, az_iot_mock_mqtt_client* m, const char* topic, const char* body)
{
  bool ok = inject_raw(m, topic, body);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  return ok;
}

/* The request id the channel just used, read off the PUBLISH it issued. */
static void last_rid(az_iot_mock_mqtt_client* m, char* out, size_t out_size)
{
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  const char* p = strstr(pub->topic, "$rid=");
  assert_non_null(p);
  p += 5;
  size_t n = strlen(p);
  assert_true(n < out_size);
  memcpy(out, p, n + 1);
}

/* ------------------------------------------------------------------------- */

/* The fetch goes out on the provisioning topic, carrying a request id. */
static void request_update_publishes_on_the_dps_topic(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);

  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_non_null(strstr(pub->topic, "$dps/registrations/POST/"));
  assert_non_null(strstr(pub->topic, "deviceupdate"));
  assert_non_null(strstr(pub->topic, "$rid="));
  /* The compatibility properties are what the service matches on. */
  assert_non_null(strstr((const char*)pub->payload, "Contoso"));
}

/* One operation at a time: the slot is held until the response arrives. */
static void a_second_request_is_refused_while_one_is_outstanding(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_ERR_BUSY);

  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);
  assert_true(inject(fx, m, topic, "{\"updateAvailable\":false}"));

  /* The slot is free again -- the answer retired it, so this is no longer BUSY.
   * It is refused for the other reason: that response also ended the exchange
   * and released the hold, so registration is about to go out and this session
   * can no longer carry a reply. */
  assert_false(fx->channel_state.request_pending);
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_ERR_NOT_CONNECTED);
}

/* A response addressed to the channel must not reach the provisioning parser,
 * which would read it as a malformed registration response and fault the whole
 * attempt. */
static void a_channel_response_does_not_disturb_provisioning(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);
  assert_true(inject(fx, m, topic, "{\"updateAvailable\":false}"));

  /* Provisioning is still live: the registration response is still accepted. */
  assert_true(inject(fx, m, "$dps/registrations/res/200/?$rid=1", k_assigned_body));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
}

/* A reply we are no longer waiting for is consumed and dropped, not delivered
 * and not handed to the provisioning parser. */
static void a_late_response_is_consumed_and_dropped(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);

  assert_true(inject(fx, m, topic, "{\"updateAvailable\":false}"));
  size_t results_after_first = fx->result_count;

  /* The same reply again: already retired, so it yields nothing. */
  assert_true(inject(fx, m, topic, "{\"updateAvailable\":false}"));
  assert_int_equal(fx->result_count, results_after_first);
  assert_int_equal(fx->update_count, 0);

  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
}

/* An update document reaches the engine, and the operation is reported OK. */
static void an_available_update_is_delivered_to_the_engine(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);

  assert_true(inject(fx, m, topic, "{\"updateMetadata\":{\"manifestVersion\":\"5\"}}"));

  assert_int_equal(fx->update_count, 1);
  assert_int_equal(fx->result_count, 1);
  assert_int_equal(fx->last_result, AZ_IOT_OK);
  assert_int_equal(fx->last_action, AZ_IOT_ADU_ERROR_ACTION_NONE);
}

/* An error response produces a verdict, so the engine can retry rather than
 * treating a rejected operation as delivered. */
static void an_error_response_reports_an_action(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/500/?$rid=%s", rid);

  assert_true(inject(fx, m, topic, "{\"errorCode\":500000,\"message\":\"server error\"}"));

  assert_int_equal(fx->result_count, 1);
  assert_int_not_equal(fx->last_result, AZ_IOT_OK);
  assert_int_equal(fx->last_action, AZ_IOT_ADU_ERROR_ACTION_RETRY);
  assert_int_equal(fx->update_count, 0);

  /* The slot was released, so the retry can actually be issued. */
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
}

/* A report is published and, once accepted, reported as delivered. */
static void a_report_is_published_and_acknowledged(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  az_iot_adu_report report = { 0 };
  report.workflow_id = "wf-1";
  report.extended_result_codes = "00000000";
  assert_int_equal(fx->channel.vtable->report(fx->channel.ctx, &report), AZ_IOT_OK);

  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_non_null(strstr(pub->topic, "deviceupdatestatus"));

  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);
  assert_true(inject(fx, m, topic, "{}"));

  assert_int_equal(fx->result_count, 1);
  assert_int_equal(fx->last_op, AZ_IOT_ADU_OP_REPORT_STATUS);
  assert_int_equal(fx->last_result, AZ_IOT_OK);
}

/* The reply can only come back on the session the request went out on. When
 * that session ends the slot must be released, with a retry verdict, or the
 * channel would wait forever for an answer that can never arrive. */
static void losing_the_session_releases_the_pending_request(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_ERR_BUSY);

  /* The check is never answered, so the hold expires and the device registers
   * with the request still outstanding. Provisioning then completes, which
   * tears the session down underneath it. */
  az_iot_test_wait_ms(60); /* opts.dps_hold_timeout_ms is 50 */
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_true(inject(fx, m, "$dps/registrations/res/200/?$rid=1", k_assigned_body));
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  /* Not BUSY: the dead request was abandoned rather than held forever. The
   * session is gone, so the answer is "no session", and the engine was told to
   * retry rather than left believing the operation was delivered. */
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(fx->result_count, 1);
  assert_int_equal(fx->last_action, AZ_IOT_ADU_ERROR_ACTION_RETRY);
  assert_int_not_equal(fx->last_result, AZ_IOT_OK);
}

/* Refreshing device properties must reach the channel: otherwise every later
 * fetch still carries the identity captured at startup. */
static void updated_device_properties_change_what_is_sent(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_non_null(fx->channel.vtable->set_device_properties);

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Fabrikam";
  dp.model = "Gizmo";
  dp.installed_update_id.provider = "Fabrikam";
  dp.installed_update_id.name = "Gizmo";
  dp.installed_update_id.version = "2.0";
  assert_int_equal(fx->channel.vtable->set_device_properties(fx->channel.ctx, &dp), AZ_IOT_OK);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);

  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  /* The onboarding request carries the compatibility properties (the installed
   * update id rides the later get-update request), so that is what proves the
   * refresh reached the channel. */
  assert_non_null(strstr((const char*)pub->payload, "Fabrikam"));
  assert_non_null(strstr((const char*)pub->payload, "Gizmo"));
  /* The startup identity is gone, not merely appended to. */
  assert_null(strstr((const char*)pub->payload, "Contoso"));
}

/* Compatibility properties are a bounded set that manufacturer and model
 * already occupy two slots of, so a caller within the engine's own
 * custom-property limit can still overflow the channel. Dropping the excess
 * silently would change the device class the service computes, and the device
 * never sees that class -- the only symptom would be updates quietly never
 * arriving. */
static void too_many_compatibility_properties_are_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)open_and_bind(fx);

  static const az_iot_adu_custom_property customs[]
      = { { "a", "1" }, { "b", "2" }, { "c", "3" }, { "d", "4" } };
  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Tractor";
  dp.custom_properties = customs;
  dp.custom_properties_count = sizeof(customs) / sizeof(customs[0]);

  /* manufacturer + model + 4 custom = 6, one past the bound. */
  assert_int_equal(
      fx->channel.vtable->set_device_properties(fx->channel.ctx, &dp), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

/* Exactly at the bound is accepted: the refusal above must be the overflow,
 * not an off-by-one that also rejects a legal set. */
static void compatibility_properties_at_the_bound_are_accepted(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)open_and_bind(fx);

  static const az_iot_adu_custom_property customs[] = { { "a", "1" }, { "b", "2" }, { "c", "3" } };
  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Tractor";
  dp.custom_properties = customs;
  dp.custom_properties_count = sizeof(customs) / sizeof(customs[0]);

  assert_int_equal(fx->channel.vtable->set_device_properties(fx->channel.ctx, &dp), AZ_IOT_OK);
}

/* The agent reports this SDK. Falling back to the vendored upstream's ADUv1
 * constant made every device claim to be "DU;agent/1.0.0" whatever was running.
 * Asserted as the whole property rather than the absence of that literal, which
 * would become a false failure the day this SDK is itself version 1.0.0. */
static void the_fetch_reports_this_sdk_version(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);

  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  char expected[96];
  snprintf(
      expected, sizeof(expected), "\"agentSdkVersion\":\"DU;agent/%s\"", AZ_IOT_VERSION_STRING);
  assert_non_null(strstr((const char*)pub->payload, expected));
}

/* A rejected refresh must leave the previous identity in place. Copying first
 * and failing part-way would advertise a truncated prefix of the new set --
 * the same silent misdescription the bound exists to prevent, reached through
 * the error path instead. */
static void a_rejected_property_set_leaves_the_previous_identity(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  static const az_iot_adu_custom_property too_many[]
      = { { "a", "1" }, { "b", "2" }, { "c", "3" }, { "d", "4" } };
  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Fabrikam";
  dp.model = "Gizmo";
  dp.custom_properties = too_many;
  dp.custom_properties_count = sizeof(too_many) / sizeof(too_many[0]);

  assert_int_equal(
      fx->channel.vtable->set_device_properties(fx->channel.ctx, &dp), AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  /* The fixture's startup identity, not the rejected set's accepted prefix. */
  assert_non_null(strstr((const char*)pub->payload, "Contoso"));
  assert_null(strstr((const char*)pub->payload, "Fabrikam"));
  assert_null(strstr((const char*)pub->payload, "Gizmo"));
}

/* Without a usable provisioning session there is nothing to publish onto. */
static void a_request_before_the_session_is_ready_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_ERR_NOT_CONNECTED);
}

/* ------------------------------------------------------------------------- */
/* the pre-registration hold                                                 */
/* ------------------------------------------------------------------------- */

/* Registration is issued from the SUBACK handler and the session is torn down
 * on its response, so without a hold the first update check never has a
 * session to run on. */
static void the_channel_holds_registration_so_bootstrap_can_run(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = open_to_registering(fx);

  /* Held at the SUBACK: no registration went out. */
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));
  assert_int_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_HOLD);
  assert_null(az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH));

  /* And the session is usable, which is the whole point of holding it. */
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_non_null(strstr(pub->topic, "deviceupdate"));
}

/* Finishing the exchange lets provisioning continue. */
static void finishing_the_check_releases_the_hold_and_registration_follows(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = open_to_registering(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);
  assert_true(inject(fx, m, topic, "{\"updateMetadata\":null}"));

  /* The holder let go; the phase still says held until the next tick, because
   * registration is published from the pump rather than from inside a message
   * callback. */
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));

  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_false(az_iot_connection_client__dps_hold_is_active(&fx->client));
  assert_int_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_REGISTERING);

  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_non_null(strstr(pub->topic, "$dps/registrations/PUT/iotdps-register"));
}

/* An OFFERED update does not finish the exchange. The bootstrap loop installs
 * it, reports the outcome and checks again, registering only once the service
 * answers "no update". Releasing on the offer let registration tear the session
 * down first, and every later publish was then refused for want of a hold --
 * so the device installed an update the service was never told about. */
static void an_offered_update_keeps_the_hold_so_the_result_can_be_reported(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = open_to_registering(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);
  assert_true(inject(
      fx,
      m,
      topic,
      "{\"updateMetadata\":{\"workflowId\":\"wf-1\",\"updateManifest\":\"{}\","
      "\"updateManifestSignature\":\"sig\",\"fileUrls\":{}}}"));

  /* Still held, and it survives the pump that would otherwise register. */
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));
  assert_int_not_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_REGISTERING);

  /* Which is the point: the install can now be reported. */
  az_iot_adu_report report = { 0 };
  report.workflow_id = "wf-1";
  report.extended_result_codes = "00000000";
  assert_int_equal(fx->channel.vtable->report(fx->channel.ctx, &report), AZ_IOT_OK);
}

/* The load-bearing rule: a stalled or unavailable device-update service must
 * never stop a device from provisioning. */
static void the_hold_expires_and_registration_proceeds_anyway(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = open_to_registering(fx);

  /* Never answered: the hold is still held. */
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));

  az_iot_test_wait_ms(60); /* opts.dps_hold_timeout_ms is 50 */
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  assert_int_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_REGISTERING);
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_non_null(strstr(pub->topic, "$dps/registrations/PUT/iotdps-register"));
}

/* A client with no device update behaves exactly as before. */
static void without_a_holder_registration_is_not_delayed(void** state)
{
  fixture* fx = (fixture*)*state;
  /* Channel never opened, so no hold is taken. */
  az_iot_mock_mqtt_client* m = open_to_registering(fx);

  assert_false(az_iot_connection_client__dps_hold_is_active(&fx->client));
  assert_int_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_REGISTERING);
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_non_null(strstr(pub->topic, "$dps/registrations/PUT/iotdps-register"));
}

/* Closing releases the hold, so destroying the ADU client cannot strand a
 * device short of registration. */
static void closing_the_channel_releases_the_hold(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  (void)open_to_registering(fx);
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));

  fx->channel.vtable->close(fx->channel.ctx);
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  assert_int_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_REGISTERING);
}

/* Asking for a hold once registration is under way is refused rather than
 * silently doing nothing. */
static void a_hold_taken_too_late_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)open_to_registering(fx); /* no holder: goes straight to REGISTERING */

  assert_int_equal(
      az_iot_connection_client__dps_hold_acquire(&fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
}

/* A request outstanding when the session goes away can never be answered. The
 * engine cleared its pending flag when the request was accepted, so nothing
 * would ever retire it and every later automatic check would be suppressed.
 * The channel's tick is what reports the loss. */
static void a_request_lost_to_teardown_is_reported_on_the_next_tick(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = open_to_registering(fx);

  /* Held, and a check goes out that the service never answers. */
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  size_t results_before = fx->result_count;

  /* The hold expires, the device registers, and the response tears the
   * provisioning session down underneath the outstanding request. */
  az_iot_test_wait_ms(60);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_true(inject(fx, m, "$dps/registrations/res/200/?$rid=1", k_assigned_body));

  /* Nothing has told the engine yet. */
  assert_int_equal(fx->result_count, results_before);

  assert_non_null(fx->channel.vtable->do_work);
  assert_int_equal(fx->channel.vtable->do_work(fx->channel.ctx), AZ_IOT_OK);

  assert_int_equal(fx->result_count, results_before + 1);
  assert_int_equal(fx->last_action, AZ_IOT_ADU_ERROR_ACTION_RETRY);
  assert_int_not_equal(fx->last_result, AZ_IOT_OK);
}

/* Binding after registration has started cannot take a hold, but the interest
 * must survive so the next session stops for the check rather than racing it. */
static void a_late_bind_still_holds_the_next_session(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)open_to_registering(fx); /* registration already in flight */

  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  assert_false(az_iot_connection_client__dps_hold_is_active(&fx->client));

  /* A later session -- a reprovision -- is early enough, and the tick takes the
   * hold that could not be taken at bind time. */
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_CONNECTING;
  assert_int_equal(fx->channel.vtable->do_work(fx->channel.ctx), AZ_IOT_OK);
  assert_int_equal(fx->client.dps_hold_count, 1);
}

/* Releasing on a retryable failure would race registration, so it must not. */
static void a_retryable_failure_keeps_the_hold(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = open_to_registering(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/500/?$rid=%s", rid);
  assert_true(inject(fx, m, topic, "{\"errorCode\":500000,\"message\":\"server error\"}"));

  assert_int_equal(fx->last_action, AZ_IOT_ADU_ERROR_ACTION_RETRY);
  /* Still held: the retry is meant to happen on this session. */
  assert_int_equal(fx->client.dps_hold_count, 1);
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));

  /* A success then releases it and registration follows. */
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  last_rid(m, rid, sizeof(rid));
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);
  assert_true(inject(fx, m, topic, "{\"updateMetadata\":null}"));
  assert_int_equal(fx->client.dps_hold_count, 0);
}

/* The holder count is a uint8_t; refuse rather than wrap. */
static void the_hold_count_refuses_to_overflow(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_CONNECTING;
  fx->client.dps_hold_count = UINT8_MAX;

  assert_int_equal(
      az_iot_connection_client__dps_hold_acquire(&fx->client), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(fx->client.dps_hold_count, UINT8_MAX);

  fx->client.dps_hold_count = 0; /* so teardown is not left holding */
}

/* Binding to a session that is already registering cannot take a hold. The
 * session still reports ready, so without gating the very first tick would
 * publish onto a registration already in flight -- whose response tears the
 * session down before any reply could arrive. */
static void a_late_bind_defers_operations_to_the_next_session(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_to_registering(fx); /* registers straight through */
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);

  /* The session is ready by the readiness test, but the hold is owed. */
  assert_true(az_iot_connection_client__dps_session_ready(&fx->client));
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_ERR_NOT_CONNECTED);

  az_iot_adu_report report;
  memset(&report, 0, sizeof(report));
  report.workflow_id = "wf-1";
  report.extended_result_codes = "00000000";
  assert_int_equal(fx->channel.vtable->report(fx->channel.ctx, &report), AZ_IOT_ERR_NOT_CONNECTED);

  /* Nothing of ours went onto the registration in flight. */
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_null(strstr(pub->topic, "deviceupdate"));
}

/* The standing interest outlives the exchange. A device that reprovisions opens
 * a new session, and that session must be held for its own check rather than
 * registering straight through. */
static void a_reprovision_is_held_again_after_a_completed_check(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  /* First session: the check runs and releases the hold. */
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);
  assert_true(inject(fx, m, topic, "{\"updateMetadata\":null}"));
  assert_int_equal(fx->client.dps_hold_count, 0);

  /* The session ends. */
  assert_true(inject(fx, m, "$dps/registrations/res/200/?$rid=1", k_assigned_body));
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_false(az_iot_connection_client__dps_session_ready(&fx->client));

  /* The channel's tick notices and re-arms for the next session. */
  assert_int_equal(fx->channel.vtable->do_work(fx->channel.ctx), AZ_IOT_OK);
  assert_int_equal(fx->client.dps_hold_count, 1);
}

/* Closing ends the standing interest, so a destroyed ADU client cannot hold a
 * later provisioning session back. */
static void closing_ends_the_standing_interest(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  fx->channel.vtable->close(fx->channel.ctx);

  assert_int_equal(fx->client.dps_hold_count, 0);
  /* A tick after close must not re-acquire. */
  assert_int_equal(fx->channel.vtable->do_work(fx->channel.ctx), AZ_IOT_OK);
  assert_int_equal(fx->client.dps_hold_count, 0);
}

/* The hold is advisory, so it can expire while the channel still believes it
 * holds one. The connection registers, and until that registration is answered
 * the session still reports ready -- so a late operation would publish onto a
 * session about to be torn down and lose its reply. */
static void an_operation_after_hold_expiry_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));

  /* Nothing answers, so the hold times out and the device registers anyway. */
  az_iot_test_wait_ms(60); /* opts.dps_hold_timeout_ms is 50 */
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_int_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_REGISTERING);

  /* The channel still thinks it holds one, and the session still reports
   * ready -- neither on its own is enough to make publishing safe. */
  assert_true(fx->channel_state.holds_registration);
  assert_true(az_iot_connection_client__dps_session_ready(&fx->client));
  assert_false(az_iot_connection_client__dps_hold_is_active(&fx->client));

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_ERR_NOT_CONNECTED);

  /* And nothing of ours reached the wire after the registration. */
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_null(strstr(pub->topic, "deviceupdate"));
}

/* "No update service is configured for this device" is a terminal answer, not a
 * failure: the hold must be released so the device registers, and no retry may
 * be scheduled -- a retry would fire onto the registration session. */
static void a_not_linked_response_releases_the_hold_without_retrying(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/409/?$rid=%s", rid);
  assert_true(inject(fx, m, topic, "{\"errorCode\":409000,\"message\":\"not linked\"}"));

  assert_int_equal(fx->last_action, AZ_IOT_ADU_ERROR_ACTION_PROCEED);
  /* Released, so registration is free to proceed. */
  assert_int_equal(fx->client.dps_hold_count, 0);
  assert_false(fx->channel_state.holds_registration);

  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_int_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_REGISTERING);
}

/* A zero timeout selects the compiled-in default. Asserted by showing the hold
 * is still held well past the 50 ms the other tests use, without waiting out
 * the full default. */
static void a_zero_hold_timeout_selects_the_default(void** state)
{
  (void)state;
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = NULL;
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  opts.dps_hold_timeout_ms = 0; /* -> AZ_IOT_DPS_HOLD_TIMEOUT_MS */
  assert_int_equal(az_iot_connection_client_init(&fx->client, &opts), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Contoso";
  dp.model = "Foobar";
  assert_int_equal(
      az_iot_adu_channel_dps_init(&fx->channel_state, &fx->client, &dp, &fx->channel), AZ_IOT_OK);
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  (void)open_to_registering(fx);

  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));
  /* Far past the 50 ms the other tests configure, and nowhere near the default. */
  az_iot_test_wait_ms(120);
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));
  assert_int_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_HOLD);

  fx->channel.vtable->close(fx->channel.ctx);
  az_iot_connection_client_destroy(&fx->client);
  free(fx);
}

/* The deadline is only a real bound if the pump cannot sleep past it. A caller
 * asking for a long wait must not be able to defer registration: the DPS pump
 * caps what it hands the adapter at the time remaining. */
static void the_dps_pump_caps_its_wait_at_the_hold_deadline(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->client));

  /* opts.dps_hold_timeout_ms is 50, so a 10s request must be cut down. */
  (void)az_iot_connection_client_do_work(&fx->client, 10000);

  const az_iot_mock_call* loop = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP);
  assert_non_null(loop);
  assert_true(loop->timeout_ms <= 50);
}

/* DONE is where a provisioned client waits until it reprovisions, so a holder
 * must be able to reserve the next session from there. Refusing would leave the
 * reprovision running from SUBACK straight into registration, unheld. */
static void a_hold_can_be_reserved_once_provisioning_is_done(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_DONE;
  fx->client.dps_hold_count = 0;

  assert_int_equal(az_iot_connection_client__dps_hold_acquire(&fx->client), AZ_IOT_OK);
  assert_int_equal(fx->client.dps_hold_count, 1);

  /* Still refused where registration is actually in flight. */
  fx->client.dps_hold_count = 0;
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_REGISTERING;
  assert_int_equal(
      az_iot_connection_client__dps_hold_acquire(&fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_POLLING;
  assert_int_equal(
      az_iot_connection_client__dps_hold_acquire(&fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_NONE;
}

/* Releasing the hold does not make the session usable again. Registration goes
 * out on the next pump, so an operation issued in between -- the engine
 * reporting its state on the following tick, typically -- would be accepted onto
 * a session about to be torn down and never hear back. */
static void a_report_after_the_exchange_is_refused_not_lost(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = open_and_bind(fx);

  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  char rid[64];
  last_rid(m, rid, sizeof(rid));
  char topic[256];
  snprintf(topic, sizeof(topic), "$dps/registrations/res/200/?$rid=%s", rid);
  assert_true(inject(fx, m, topic, "{\"updateMetadata\":null}"));

  /* Hold released, but registration has not been published yet, so the session
   * still looks usable. */
  assert_int_equal(fx->client.dps_hold_count, 0);
  assert_true(az_iot_connection_client__dps_session_ready(&fx->client));

  az_iot_adu_report report;
  memset(&report, 0, sizeof(report));
  report.workflow_id = "wf-1";
  report.extended_result_codes = "00000000";
  assert_int_equal(fx->channel.vtable->report(fx->channel.ctx, &report), AZ_IOT_ERR_NOT_CONNECTED);

  /* Nothing was put on the wire, so there is no accepted operation to lose. */
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_null(strstr(pub->topic, "deviceupdatestatus"));
}

/* ------------------------------------------------------------------------- */
/* the operational session                                                   */
/* ------------------------------------------------------------------------- */

/* After the device provisions, the ordinary flow has torn its session down, so
 * an operation has nothing to publish on. The channel asks for one. */
static void a_session_is_opened_on_demand_after_provisioning(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);

  /* Standing interest is taken at bind. */
  assert_int_equal(fx->client.dps_user_count, 1);

  /* Provisioning is over and nothing is open. The factory has to be registered
   * for a session to be creatable at all. */
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_DONE;
  assert_false(az_iot_connection_client__dps_session_ready(&fx->client));

  /* BUSY, not OK: the session has to come up first. */
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_true(fx->client.dps_session_auxiliary);
}

/* A caller with no standing interest must not be able to open one. */
static void without_interest_no_session_is_opened(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_DONE;
  assert_int_equal(fx->client.dps_user_count, 0);

  assert_int_equal(
      az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_false(fx->client.dps_session_auxiliary);
}

/* The whole point of the auxiliary session: it must not register. Registering
 * would take the assignment path, which rewrites the host and role and
 * reconnects -- destroying the hub connection it is meant to sit beside.
 *
 * No ADU channel here on purpose. The channel also holds registration back for
 * its pre-registration check, and that hold would stop the session first --
 * proving nothing about the auxiliary guard. This drives the connection client
 * directly so the guard is the only thing that can prevent a register. */
static void an_auxiliary_session_never_registers(void** state)
{
  fixture* fx = (fixture*)*state;

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_DONE;
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_true(fx->client.dps_session_auxiliary);
  assert_int_equal(fx->client.dps_hold_count, 0); /* nothing else is holding it */

  az_iot_mock_mqtt_client* m = drive_existing_session(fx);

  /* Usable for our traffic ... */
  assert_true(az_iot_connection_client__dps_session_ready(&fx->client));
  /* ... and nothing was published. Asserted as "no publish at all": checking
   * only for the register topic passes vacuously if nothing publishes. */
  assert_null(az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH));
  assert_int_not_equal(fx->client.dps_phase, AZ_IOT_DPS_PHASE_REGISTERING);
  assert_true(fx->client.dps_session_auxiliary);

  az_iot_connection_client__dps_user_release(&fx->client);
}

/* Releasing the last interest closes the session -- from the pump, not from the
 * release call, which may run inside a callback. */
static void the_session_closes_when_the_last_user_lets_go(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_DONE;
  (void)az_iot_connection_client__dps_session_ensure(&fx->client);
  (void)drive_existing_session(fx);
  assert_true(az_iot_connection_client__dps_session_ready(&fx->client));

  /* Still open while the interest is held. */
  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_true(fx->client.dps_session_auxiliary);

  fx->channel.vtable->close(fx->channel.ctx);
  assert_int_equal(fx->client.dps_user_count, 0);

  (void)az_iot_connection_client_do_work(&fx->client, 0);
  assert_false(fx->client.dps_session_auxiliary);
  assert_false(az_iot_connection_client__dps_session_ready(&fx->client));
}

/* A session is not held between polls. Idle beyond the linger closes it. */
static void an_idle_session_is_closed_after_the_linger(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_DONE;
  (void)az_iot_connection_client__dps_session_ensure(&fx->client);
  (void)drive_existing_session(fx);
  assert_true(fx->client.dps_session_auxiliary);

  /* Force the deadline into the past rather than waiting out the real linger. */
  fx->client.dps_aux_idle_deadline_ms = 1;
  (void)az_iot_connection_client_do_work(&fx->client, 0);

  assert_false(fx->client.dps_session_auxiliary);
  assert_int_equal(fx->client.dps_user_count, 1); /* interest survives the session */
}

/* The interest count is a uint8_t; refuse rather than wrap. */
static void the_user_count_refuses_to_overflow(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->client.dps_user_count = UINT8_MAX;

  assert_int_equal(
      az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(fx->client.dps_user_count, UINT8_MAX);

  fx->client.dps_user_count = 0;
}

/* An auxiliary session runs ALONGSIDE the hub connection, so the hub must keep
 * being serviced while it is open. If the DPS pump returned early -- as it
 * rightly does for an ordinary provisioning run, which owns the client -- a
 * device that opened one would stop servicing telemetry, twin and method
 * traffic for as long as the session lasted. */
static void the_hub_is_still_pumped_while_an_auxiliary_session_is_open(void** state)
{
  fixture* fx = (fixture*)*state;

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);

  /* Stand in for a live hub connection: the pump must reach this client. */
  az_iot_mqtt_client* hub = fx->factory->create(fx->factory->factory_ctx);
  assert_non_null(hub);
  fx->client.active_client = hub;
  az_iot_mock_mqtt_client* hub_mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(hub_mock);

  assert_int_equal(az_iot_connection_client__dps_user_acquire(&fx->client), AZ_IOT_OK);
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_DONE;
  assert_int_equal(az_iot_connection_client__dps_session_ensure(&fx->client), AZ_IOT_ERR_BUSY);
  assert_true(fx->client.dps_session_auxiliary);

  size_t before = (size_t)az_iot_mock_mqtt_client_count_of(hub_mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP);

  (void)az_iot_connection_client_do_work(&fx->client, 0);

  size_t after = (size_t)az_iot_mock_mqtt_client_count_of(hub_mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP);
  assert_true(after > before);

  /* Destroyed explicitly. The mock factory frees only its LAST client, and the
   * auxiliary session created one after this stand-in, so nothing else would
   * ever free it -- confirmed by LeakSanitizer, which reports it otherwise. */
  fx->client.active_client = NULL;
  hub->iface->destroy(hub);
  az_iot_connection_client__dps_user_release(&fx->client);
}

/* The path that makes an operation possible after the device has provisioned:
 * a request is refused because no session is up, the tick notices and opens
 * one, and the retry then publishes.
 *
 * This is what the first version got wrong. The tick tested request_pending,
 * but the call immediately above it clears request_pending precisely when the
 * session is gone -- so the condition could never be true and the session was
 * never reopened. Every post-provisioning operation would have failed forever.
 */
static void a_refused_request_causes_a_session_to_be_opened(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->client, fx->factory), AZ_IOT_OK);

  /* Provisioned, and the ordinary flow has taken its session away. */
  fx->client.dps_phase = AZ_IOT_DPS_PHASE_DONE;
  assert_false(az_iot_connection_client__dps_session_ready(&fx->client));

  /* Refused -- and the demand is recorded. */
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_ERR_NOT_CONNECTED);
  assert_true(fx->channel_state.wants_session);

  /* The tick acts on it. */
  assert_int_equal(fx->channel.vtable->do_work(fx->channel.ctx), AZ_IOT_OK);
  assert_true(fx->client.dps_session_auxiliary);

  az_iot_mock_mqtt_client* m = drive_existing_session(fx);
  assert_true(az_iot_connection_client__dps_session_ready(&fx->client));

  /* And the retry now reaches the wire -- the publish gate must not demand a
   * hold on a session that never registers. */
  assert_int_equal(fx->channel.vtable->request_update(fx->channel.ctx), AZ_IOT_OK);
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_non_null(strstr(pub->topic, "deviceupdate"));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(request_update_publishes_on_the_dps_topic, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_second_request_is_refused_while_one_is_outstanding, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_channel_response_does_not_disturb_provisioning, setup, teardown),
    cmocka_unit_test_setup_teardown(a_late_response_is_consumed_and_dropped, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_available_update_is_delivered_to_the_engine, setup, teardown),
    cmocka_unit_test_setup_teardown(an_error_response_reports_an_action, setup, teardown),
    cmocka_unit_test_setup_teardown(a_report_is_published_and_acknowledged, setup, teardown),
    cmocka_unit_test_setup_teardown(
        losing_the_session_releases_the_pending_request, setup, teardown),
    cmocka_unit_test_setup_teardown(updated_device_properties_change_what_is_sent, setup, teardown),
    cmocka_unit_test_setup_teardown(too_many_compatibility_properties_are_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        compatibility_properties_at_the_bound_are_accepted, setup, teardown),
    cmocka_unit_test_setup_teardown(the_fetch_reports_this_sdk_version, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_rejected_property_set_leaves_the_previous_identity, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_request_before_the_session_is_ready_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_channel_holds_registration_so_bootstrap_can_run, setup, teardown),
    cmocka_unit_test_setup_teardown(
        finishing_the_check_releases_the_hold_and_registration_follows, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_offered_update_keeps_the_hold_so_the_result_can_be_reported, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_hold_expires_and_registration_proceeds_anyway, setup, teardown),
    cmocka_unit_test_setup_teardown(without_a_holder_registration_is_not_delayed, setup, teardown),
    cmocka_unit_test_setup_teardown(closing_the_channel_releases_the_hold, setup, teardown),
    cmocka_unit_test_setup_teardown(a_hold_taken_too_late_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_request_lost_to_teardown_is_reported_on_the_next_tick, setup, teardown),
    cmocka_unit_test_setup_teardown(a_late_bind_still_holds_the_next_session, setup, teardown),
    cmocka_unit_test_setup_teardown(a_retryable_failure_keeps_the_hold, setup, teardown),
    cmocka_unit_test_setup_teardown(the_hold_count_refuses_to_overflow, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_late_bind_defers_operations_to_the_next_session, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_reprovision_is_held_again_after_a_completed_check, setup, teardown),
    cmocka_unit_test_setup_teardown(closing_ends_the_standing_interest, setup, teardown),
    cmocka_unit_test_setup_teardown(an_operation_after_hold_expiry_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_report_after_the_exchange_is_refused_not_lost, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_not_linked_response_releases_the_hold_without_retrying, setup, teardown),
    cmocka_unit_test(a_zero_hold_timeout_selects_the_default),
    cmocka_unit_test_setup_teardown(
        a_hold_can_be_reserved_once_provisioning_is_done, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_session_is_opened_on_demand_after_provisioning, setup, teardown),
    cmocka_unit_test_setup_teardown(without_interest_no_session_is_opened, setup, teardown),
    cmocka_unit_test_setup_teardown(an_auxiliary_session_never_registers, setup, teardown),
    cmocka_unit_test_setup_teardown(the_session_closes_when_the_last_user_lets_go, setup, teardown),
    cmocka_unit_test_setup_teardown(an_idle_session_is_closed_after_the_linger, setup, teardown),
    cmocka_unit_test_setup_teardown(the_user_count_refuses_to_overflow, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_refused_request_causes_a_session_to_be_opened, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_hub_is_still_pumped_while_an_auxiliary_session_is_open, setup, teardown),
    cmocka_unit_test_setup_teardown(
        the_dps_pump_caps_its_wait_at_the_hold_deadline, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
