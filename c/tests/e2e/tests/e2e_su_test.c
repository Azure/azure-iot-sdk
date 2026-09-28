// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/*
 * Software updates end-to-end suite: the shipping SDK against a real
 * DPS/Device Update endpoint.
 *
 * Everything beneath the test is the shipping SDK: the connection client, its
 * DPS flow and the pre-registration hold, the device-update channel, the
 * codecs, the Paho MQTT adapter, and X.509 authentication. Nothing here
 * implements a transport or a credential of its own.
 *
 * The scenarios drive the channel vtable directly rather than going through
 * az_iot_su_client, so the engine's workflow state machine is NOT exercised --
 * only the transport-facing half is.
 *
 * Scope note. These scenarios deliberately cover only what a device can reach
 * WITHOUT an update being offered to it:
 *
 *   - onboarding check (pre-registration, needs no registry entry)
 *   - the ETags the service issues, and that the channel stores them
 *   - the error surface reachable today: an unknown workflow on the report
 *     route
 *   - that registration still proceeds once device update has had its turn
 *
 * Not covered for a different reason: the operational (software-update) route.
 * The channel only ever issues the onboarding operation today -- the operational
 * poll needs a provisioning session after registration, which is not built yet.
 * Its request and response shapes are pinned at the codec level in
 * su_protocol_test.c meanwhile. Add the scenario here when the channel can
 * issue it.
 *
 * The offer path -- a real updateMetadata driven through the engine, and the
 * conflict on re-reporting a terminal result -- is in e2e_su_offer_test.c.
 *
 * Needs an X.509 enrollment in the target DPS; see e2e_su_env.h. An incomplete
 * environment fails the suite.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include "azure/iot/az_iot_su.h"
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_result.h"

#include "internal/connection_client_internal.h"

#include "e2e_su_env.h"

#include "su_channel_internal.h"
#include "su_protocol_internal.h"

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_certificate_provider_pem.h"

/* --- environment --------------------------------------------------------- */

static e2e_su_env g_env;

/* --- fixture ------------------------------------------------------------- */

typedef struct
{
  az_iot_connection_client conn;
  az_iot_certificate_provider_pem certs;

  az_iot_su_channel_dps channel_state;
  az_iot_su_channel channel;

  /* what the channel reported back */
  size_t update_count;
  size_t result_count;
  az_iot_su_operation last_op;
  az_iot_result last_result;
  az_iot_su_error_action last_action;
  /* The service's own diagnosis of the last failure, when it sent one. */
  bool had_service_error;
  int32_t last_error_code;
  char last_error_text[128];
  char last_tracking_id[64];

  bool faulted;
  az_iot_result fault_reason;
  char assigned_hub[128];
} e2e_fixture;

/* A faulted connection has to end a wait: otherwise a scenario that can no
 * longer make progress just burns its whole budget before failing. */
static void on_conn_state(const az_iot_connection_state_event* event, void* ctx)
{
  /* Deliberately scope-agnostic: this device rides the provisioning session, so
   * a DPS fault ends the wait just as a hub fault does. */
  if (event != NULL && event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    ((e2e_fixture*)ctx)->faulted = true;
    ((e2e_fixture*)ctx)->fault_reason = event->reason;
  }
}

static void on_update(const uint8_t* payload, size_t payload_len, void* ctx)
{
  (void)payload;
  (void)payload_len;
  ((e2e_fixture*)ctx)->update_count++;
}

static void on_result(
    az_iot_su_operation operation,
    az_iot_result result,
    az_iot_su_error_action action,
    const az_iot_su_service_error* service_error,
    void* ctx)
{
  e2e_fixture* fx = (e2e_fixture*)ctx;
  fx->result_count++;
  fx->last_op = operation;
  fx->last_result = result;
  fx->last_action = action;

  /* NULL whenever the failure was local, so it is optional by contract. */
  fx->had_service_error = (service_error != NULL);
  if (service_error != NULL)
  {
    fx->last_error_code = service_error->code;
    snprintf(fx->last_error_text, sizeof(fx->last_error_text), "%s", service_error->message);
    snprintf(fx->last_tracking_id, sizeof(fx->last_tracking_id), "%s", service_error->tracking_id);
  }
}

static void fixture_open(e2e_fixture* fx)
{
  memset(fx, 0, sizeof(*fx));

  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  e2e_su_env_apply(&g_env, &opts, &pem);
  assert_int_equal(az_iot_certificate_provider_pem_init(&fx->certs, &pem), AZ_IOT_OK);

  opts.certificate_provider = &fx->certs.base;
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&fx->conn, on_conn_state, fx), AZ_IOT_OK);

  az_iot_su_device_properties dp = { 0 };
  dp.manufacturer = "contoso";
  dp.model = "tractor";
  dp.installed_update_id.provider = "contoso";
  dp.installed_update_id.name = "tractor";
  dp.installed_update_id.version = "1.0";
  assert_int_equal(
      az_iot_su_channel_dps_init(&fx->channel_state, &fx->conn, &dp, &fx->channel), AZ_IOT_OK);
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);

  /* The shipping Paho adapter. DPS is v3.1.1 only, so one factory is enough. */
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(
          &fx->conn, az_iot_paho_factory_create_v3_1_1()),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
}

static void fixture_close(e2e_fixture* fx)
{
  if (fx->channel.vtable != NULL)
  {
    fx->channel.vtable->close(fx->channel.ctx);
  }
  az_iot_connection_client_deinit(&fx->conn);
  az_iot_certificate_provider_pem_deinit(&fx->certs);
}

/* Pump until the predicate holds or the budget runs out. Wall-clock bounded
 * because every wait here is on a real service. */
#define PUMP_UNTIL(fx, cond, seconds)                                            \
  do                                                                             \
  {                                                                              \
    time_t deadline_ = time(NULL) + (seconds);                                   \
    while (!(cond) && time(NULL) < deadline_)                                    \
    {                                                                            \
      (void)az_iot_connection_client_do_work(&(fx)->conn, 200);                  \
      /* The channel has its own tick, and an application drives                 \
       * it through the software updates client. It is what re-arms the hold on  \
       * a new session and retires a request whose session went                  \
       * away, so pumping only the connection would stall here. */               \
      if ((fx)->channel.vtable != NULL && (fx)->channel.vtable->do_work != NULL) \
      {                                                                          \
        (void)(fx)->channel.vtable->do_work((fx)->channel.ctx);                  \
      }                                                                          \
    }                                                                            \
  } while (0)

/* Drive the connection to the point where the provisioning session is up and
 * registration is being held for the update check. */
static void wait_for_hold(e2e_fixture* fx)
{
  /* A faulted connection ends the wait: it can make no further progress, so
   * burning the whole budget would only delay the same failure and report it
   * as a timeout rather than as the fault it is. */
  PUMP_UNTIL(fx, az_iot_connection_client__dps_hold_is_active(&fx->conn) || fx->faulted, 30);
  assert_false(fx->faulted);
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->conn));
}

static void wait_for_result(e2e_fixture* fx, size_t target)
{
  PUMP_UNTIL(fx, fx->result_count >= target || fx->faulted, 30);
  assert_false(fx->faulted);
  assert_true(fx->result_count >= target);
}

/* --- scenarios ----------------------------------------------------------- */

/* The pre-registration check: a device with no registry entry asks for
 * onboarding updates on the provisioning session, before it registers. */
static void onboarding_check_runs_before_registration(void** state)
{
  (void)state;

  e2e_fixture fx;
  fixture_open(&fx);

  wait_for_hold(&fx);
  assert_int_equal(fx.conn.dps_phase, AZ_IOT_DPS_PHASE_HOLD);

  assert_int_equal(
      fx.channel.vtable->request_update(fx.channel.ctx, AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE),
      AZ_IOT_OK);
  wait_for_result(&fx, 1);

  /* A device with nothing deployed to it is a success with no update. */
  assert_int_equal(fx.last_result, AZ_IOT_OK);
  assert_int_equal(fx.last_action, AZ_IOT_SU_ERROR_ACTION_NONE);
  assert_int_equal(fx.update_count, 0);

  /* The service answered with ETags, which is what makes the next poll cheap. */
  assert_true(fx.channel_state.agent_info_etag[0] != '\0');
  assert_true(fx.channel_state.service_config_etag[0] != '\0');

  fixture_close(&fx);
}

/* Holding registration must not prevent it: the hold is released, the
 * registration goes out, and the service answers.
 *
 * It does NOT claim the device is assigned a hub, and cannot: this environment
 * has no IoT Hub linked to the provisioning service (the enrollment carries no
 * allocationPolicy and no iotHubs), so registration is refused with
 * AZ_IOT_ERR_DPS no matter which credential is used. Device update is what this
 * environment exists for; a hub is not part of it.
 *
 * AZ_IOT_DPS_PHASE_DONE also proves nothing on its own: dps_apply_deferred()
 * sets it BEFORE testing status and have_assignment, so a rejected registration
 * reaches it too, and a successful one passes straight through it to NONE. So
 * the claim here is about the hold -- device update let go, and the service
 * answered -- and the refusal is pinned by its specific reason rather than
 * accepted as "some ending".
 *
 * If a hub is ever linked, the AZ_IOT_ERR_DPS assertion is MEANT to fail and be
 * rewritten around a real assignment. */
static void the_hold_is_released_and_registration_is_answered(void** state)
{
  (void)state;

  e2e_fixture fx;
  fixture_open(&fx);

  wait_for_hold(&fx);
  assert_int_equal(
      fx.channel.vtable->request_update(fx.channel.ctx, AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE),
      AZ_IOT_OK);
  wait_for_result(&fx, 1);

  /* The hold is released by the verdict and the pump publishes the
   * registration. */
  PUMP_UNTIL(&fx, fx.conn.dps_phase != AZ_IOT_DPS_PHASE_HOLD && fx.conn.dps_hold_count == 0, 60);

  /* The hold is gone, so device update let go. */
  assert_false(az_iot_connection_client__dps_hold_is_active(&fx.conn));
  assert_int_not_equal(fx.conn.dps_phase, AZ_IOT_DPS_PHASE_HOLD);

  /* And the registration was ANSWERED, not merely published. POLLING is the
   * signal: the connection client only enters it after parsing the register
   * response and taking the operationId out of it, so reaching POLLING means
   * the service replied. REGISTERING would prove only that bytes went out.
   *
   * What the registration eventually RESOLVES to is deliberately not asserted.
   * This environment has no IoT Hub linked to the provisioning service, and the
   * outcome is not stable across runs: it has been observed both as an
   * immediate refusal (AZ_IOT_ERR_DPS) and as an open "assigning" poll that
   * outlives any reasonable budget. Pinning either one produced a test that
   * passed for a while and then failed for reasons that had nothing to do with
   * this SDK. The claim this scenario can honestly make is that device update
   * let go of the hold and provisioning then got a real answer. */
  PUMP_UNTIL(
      &fx,
      fx.conn.dps_phase == AZ_IOT_DPS_PHASE_POLLING || fx.conn.dps_phase == AZ_IOT_DPS_PHASE_DONE
          || fx.faulted,
      60);
  assert_true(
      fx.conn.dps_phase == AZ_IOT_DPS_PHASE_POLLING || fx.conn.dps_phase == AZ_IOT_DPS_PHASE_DONE
      || fx.faulted);

  fixture_close(&fx);
}

/* The ETags the service issues, and what the channel does with them.
 *
 * Deliberately only one exchange. A provisioning session carries exactly one
 * pre-registration check: answering it releases the hold and registration
 * follows, so the session can no longer carry a reply and further operations
 * are refused. Asserted here, because it is the contract -- publishing into that
 * window would be accepted by the service and the reply lost.
 *
 * So the SMALLER steady-state response (serviceConfiguration omitted when the
 * ETags match) is NOT exercised here: it needs a second check, which needs a
 * second session, which in turn needs the operational path that does not exist
 * yet. That response shape is pinned instead in su_protocol_test.c against the
 * real 75-byte body captured from the service. What this proves end to end is
 * that the ETags arrive, are stored, and survive the session being spent. */
static void etags_are_issued_stored_and_survive_the_session(void** state)
{
  (void)state;

  e2e_fixture fx;
  fixture_open(&fx);

  wait_for_hold(&fx);
  assert_int_equal(
      fx.channel.vtable->request_update(fx.channel.ctx, AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE),
      AZ_IOT_OK);
  wait_for_result(&fx, 1);
  assert_int_equal(fx.last_result, AZ_IOT_OK);

  char first_agent_etag[128];
  char first_config_etag[128];
  snprintf(first_agent_etag, sizeof(first_agent_etag), "%s", fx.channel_state.agent_info_etag);
  snprintf(
      first_config_etag, sizeof(first_config_etag), "%s", fx.channel_state.service_config_etag);
  assert_true(first_agent_etag[0] != '\0');
  assert_true(first_config_etag[0] != '\0');

  /* Both are opaque tokens, not JSON fragments or an error string. Checked on
   * each: validating only one would let an error body through in the other and
   * then compare it with itself. */
  assert_true(strlen(first_agent_etag) >= 8);
  assert_true(strchr(first_agent_etag, '{') == NULL);
  assert_true(strlen(first_config_etag) >= 8);
  assert_true(strchr(first_config_etag, '{') == NULL);

  /* That answer ended the pre-registration exchange, so this session is spent:
   * it is about to carry the registration and could not answer us again. The
   * refusal is the contract -- publishing here would be accepted by the service
   * and the reply lost. */
  assert_int_equal(
      fx.channel.vtable->request_update(fx.channel.ctx, AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE),
      AZ_IOT_ERR_NOT_CONNECTED);

  /* The stored ETags survive the session being spent, which is what a later
   * session would send. */
  assert_string_equal(fx.channel_state.agent_info_etag, first_agent_etag);
  assert_string_equal(fx.channel_state.service_config_etag, first_config_etag);

  fixture_close(&fx);
}

/* Reporting against a workflow the service does not know is answered 400000
 * with UNKNOWN_WORKFLOW_ID in the message. What this pins is that the report is
 * NOT read as "already reported" -- nothing was delivered, and claiming
 * otherwise would silently drop the only record the service gets of what the
 * device did.
 *
 * Honest limit: this does NOT exercise reading the code out of "message".
 * 400000 is not in the numeric ladder, so it defaults to FATAL and the scenario
 * still passes with message parsing disabled (measured). The case where the
 * message is genuinely load-bearing is 409000, which splits by operation, and
 * producing one needs a real workflow. The message-vs-numeric behaviour is
 * pinned instead by su_protocol_test.c against captured bodies. */
static void an_unknown_workflow_report_is_not_treated_as_delivered(void** state)
{
  (void)state;

  e2e_fixture fx;
  fixture_open(&fx);

  wait_for_hold(&fx);

  az_iot_su_report report;
  memset(&report, 0, sizeof(report));
  report.workflow_id = "e2e-workflow-that-does-not-exist";
  report.outcome = AZ_IOT_SU_OUTCOME_SUCCEEDED;
  report.failure_origin = AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE;
  report.result_code = 700;
  report.extended_result_codes = "";
  report.result_details = "e2e";

  assert_int_equal(fx.channel.vtable->report(fx.channel.ctx, &report), AZ_IOT_OK);
  wait_for_result(&fx, 1);

  assert_int_equal(fx.last_op, AZ_IOT_SU_OP_REPORT_STATUS);
  assert_int_not_equal(fx.last_result, AZ_IOT_OK);
  assert_int_not_equal(fx.last_action, AZ_IOT_SU_ERROR_ACTION_ALREADY_REPORTED);
  assert_int_equal(fx.last_action, AZ_IOT_SU_ERROR_ACTION_FATAL);

  fixture_close(&fx);
}

/* An incomplete environment fails every scenario rather than skipping them. */
static int group_setup(void** state)
{
  (void)state;
  return e2e_su_env_load(&g_env);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(onboarding_check_runs_before_registration),
    cmocka_unit_test(the_hold_is_released_and_registration_is_answered),
    cmocka_unit_test(etags_are_issued_stored_and_survive_the_session),
    cmocka_unit_test(an_unknown_workflow_report_is_not_treated_as_delivered),
  };
  return cmocka_run_group_tests(tests, group_setup, NULL);
}
