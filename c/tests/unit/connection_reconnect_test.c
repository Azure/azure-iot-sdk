// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* ConnectionClient reconnection unit tests: what schedules a retry, how the
 * backoff deadline gates it, how the attempt counter behaves across outages,
 * and what the core does with persistent subscriptions and in-flight QoS-1
 * acknowledgements when a session is replaced.
 *
 * The backoff MATH lives in reconnect_policy_test.c; this suite is about the
 * state machine that consumes it. */
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
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "internal/connection_client_internal.h"

#include "support/connection_test_harness.h"

/* Backoff long enough that a single do_work() cannot cross the deadline in the
 * same call that schedules it (which would hide the RECONNECTING state), yet
 * short enough to keep the suite fast. No jitter: timing must be exact. */
#define RETRY_DELAY_MS 20u

static az_iot_connection_client_options reconnect_options(uint32_t max_attempts)
{
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  opts.reconnection_policy.initial_delay_ms = RETRY_DELAY_MS;
  opts.reconnection_policy.max_delay_ms = RETRY_DELAY_MS;
  opts.reconnection_policy.max_attempts = max_attempts;
  opts.reconnection_policy.jitter_pct = 0;
  return opts;
}

static int setup_with_policy(void** state, uint32_t max_attempts)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = reconnect_options(max_attempts);
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

/* max_attempts = 0 means "retry forever". */
static int setup_infinite(void** state) { return setup_with_policy(state, 0); }
static int setup_two_attempts(void** state) { return setup_with_policy(state, 2); }

static int setup_no_reconnect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = az_iot_test_classic_options();
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
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    free(fx);
  }
  return 0;
}

static az_iot_mock_mqtt_client* open_to_connected(az_iot_test_conn* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
  return m;
}

/* Let the backoff elapse and pump until the retry CONNECT has been issued;
 * returns the adapter instance created for the new attempt. */
static az_iot_mock_mqtt_client* advance_to_retry(az_iot_test_conn* fx)
{
  az_iot_test_wait_ms(RETRY_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  return m;
}

/* ------------------------------------------------------------------------- */
/* what schedules a retry                                                    */
/* ------------------------------------------------------------------------- */

static void adapter_error_event_schedules_a_retry(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  /* An ERROR event is the adapter reporting a socket/TLS/library failure
   * without a clean DISCONNECT. It must retry like a peer disconnect. */
  assert_true(az_iot_mock_mqtt_client_inject_error(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_RECONNECTING), AZ_IOT_ERR_MQTT);
}

static void adapter_error_event_faults_without_a_policy(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_true(az_iot_mock_mqtt_client_inject_error(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
}

/* A keep-alive violation reaches the core as a plain DISCONNECTED from the
 * adapter -- there is no distinct event kind for it. */
static void keep_alive_drop_is_retried_like_any_disconnect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  /* A DISCONNECTED carrying no status is reported as NOT_CONNECTED, not OK:
   * "the link went away" is a failure reason even when the frame was clean. */
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_RECONNECTING), AZ_IOT_ERR_NOT_CONNECTED);
}

static void user_close_disconnect_goes_idle_not_reconnecting(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_RECONNECTING));
}

/* ------------------------------------------------------------------------- */
/* the backoff deadline actually gates the retry                             */
/* ------------------------------------------------------------------------- */

static void retry_waits_for_the_backoff_deadline(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);

  /* Pumping before the deadline must not produce a new adapter/CONNECT. */
  for (int i = 0; i < 5; ++i)
    (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);

  az_iot_mock_mqtt_client* retry = advance_to_retry(fx);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(retry, AZ_IOT_MOCK_CALL_CONNECT), 1);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_CONNECTING);
}

static void retry_destroys_the_old_adapter_and_builds_a_new_one(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* first = open_to_connected(fx);
  assert_true(az_iot_mock_mqtt_client_count_of(first, AZ_IOT_MOCK_CALL_CONNECT) == 1);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(first));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  /* The old adapter is destroyed as soon as the retry is scheduled -- the
   * client does not hold a dead session open across the backoff window. */
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));

  /* The replacement starts from a clean slate: exactly the retry's CONNECT and
   * nothing inherited from the previous session. (Pointer identity proves
   * nothing here -- the allocator may hand back the same address.) */
  az_iot_mock_mqtt_client* second = advance_to_retry(fx);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(second, AZ_IOT_MOCK_CALL_CONNECT), 1);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(second, AZ_IOT_MOCK_CALL_DISCONNECT), 0);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(second, AZ_IOT_MOCK_CALL_DESTROY), 0);
}

/* ------------------------------------------------------------------------- */
/* attempt accounting                                                        */
/* ------------------------------------------------------------------------- */

static void successful_reconnect_resets_the_attempt_counter(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  /* Outage 1: burn the single retry allowed before FAULTED (max_attempts = 2),
   * then succeed. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  m = advance_to_retry(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));

  /* Outage 2: if the counter had NOT been reset, this second outage would
   * exhaust max_attempts immediately and FAULT. It must retry instead. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
}

/* max_attempts = 0 is documented as "infinite". Drive more consecutive
 * failures than any finite cap in the fixture would allow. */
static void zero_max_attempts_never_gives_up(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  for (int attempt = 0; attempt < 6; ++attempt)
  {
    m = advance_to_retry(fx);
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    (void)az_iot_connection_client_do_work(fx->client, 0);
    assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  }
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));
}

static void full_outage_recovers_to_connected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_true(az_iot_mock_mqtt_client_inject_error(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  m = advance_to_retry(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_count_state(&fx->log, AZ_IOT_CONN_STATE_CONNECTED), 2);
}

/* ------------------------------------------------------------------------- */
/* persistent subscriptions                                                  */
/* ------------------------------------------------------------------------- */

static void persistent_subscription_is_issued_on_connect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "devices/ut-device/messages/devicebound/#", AZ_IOT_MQTT_QOS_1),
      AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_string_equal(sub->topic, "devices/ut-device/messages/devicebound/#");
  assert_int_equal(sub->qos, AZ_IOT_MQTT_QOS_1);
}

static void persistent_subscriptions_are_reissued_after_a_reconnect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "devices/ut-device/messages/devicebound/#", AZ_IOT_MQTT_QOS_1),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "$iothub/methods/POST/#", AZ_IOT_MQTT_QOS_0),
      AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE), 2);

  /* Drop and come back: the new session must re-issue BOTH filters, because
   * the broker-side subscription died with the old session. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  m = advance_to_retry(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE), 2);
}

static void persistent_subscription_registry_full_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;

  char filter[32];
  for (unsigned i = 0; i < AZ_IOT_MAX_PERSISTENT_SUBS; ++i)
  {
    filter[0] = 'f';
    filter[1] = (char)('0' + (int)i);
    filter[2] = '\0';
    assert_int_equal(
        az_iot_connection_client__add_subscription_on_connect(
            fx->client, filter, AZ_IOT_MQTT_QOS_0),
        AZ_IOT_OK);
  }
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "one-too-many", AZ_IOT_MQTT_QOS_0),
      AZ_IOT_ERR_NOT_SUPPORTED);
}

static void persistent_subscription_added_while_connected_subscribes_now(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);
  az_iot_mock_mqtt_client_clear_calls(m);

  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "late/filter/#", AZ_IOT_MQTT_QOS_1),
      AZ_IOT_OK);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_string_equal(sub->topic, "late/filter/#");
}

/* ---- the service's five-subscription limit ------------------------------- */

typedef struct warning_capture
{
  int count;
  char last[AZ_IOT_LOG_MESSAGE_MAX];
} warning_capture;

static void capture_warning(
    void* user_ctx,
    az_iot_log_level level,
    const char* file,
    int line,
    const char* msg)
{
  warning_capture* c = (warning_capture*)user_ctx;
  (void)file;
  (void)line;
  if (level != AZ_IOT_LOG_LEVEL_WARN || msg == NULL)
  {
    return;
  }
  c->count++;
  snprintf(c->last, sizeof(c->last), "%s", msg);
}

static void install_warning_capture(warning_capture* c)
{
  memset(c, 0, sizeof(*c));
  az_iot_log_sink sink;
  sink.sink = capture_warning;
  sink.user_ctx = c;
  sink.min_level = AZ_IOT_LOG_LEVEL_WARN;
  az_iot_log_set_global_sink(&sink);
}

static void the_hub_subscription_limit_is_diagnosed(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;

  /* IoT Hub Classic allows a device five topic subscriptions, and the registry
   * here is deliberately larger because Hub-Next needs the slots. A sixth
   * filter is one the service refuses at SUBACK time -- which does not fail the
   * connection, it just means those messages never arrive. Nothing said so
   * before; now it names the filter that caused it. */
  char filter[32];
  warning_capture cap;
  install_warning_capture(&cap);

  for (unsigned i = 0; i < AZ_IOT_HUB_MAX_SUBSCRIPTIONS; ++i)
  {
    snprintf(filter, sizeof(filter), "within/%u/#", i);
    assert_int_equal(
        az_iot_connection_client__add_subscription_on_connect(
            fx->client, filter, AZ_IOT_MQTT_QOS_0),
        AZ_IOT_OK);
  }
  assert_int_equal(cap.count, 0);

  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "one/past/the/limit/#", AZ_IOT_MQTT_QOS_0),
      AZ_IOT_OK);
  az_iot_log_set_global_sink(NULL);

  /* Still accepted locally -- refusing it would be worse than reporting it,
   * since the registry is shared with a flavor that allows more. */
  assert_int_equal(cap.count, 1);
  assert_non_null(strstr(cap.last, "one/past/the/limit/#"));
}

static void the_hub_subscription_limit_does_not_apply_to_dps(void** state)
{
  (void)state;
  /* The five-topic rule is an IoT Hub Classic rule. DPS has its own, much
   * smaller topic set and no published equivalent, so warning there would
   * quote a limit that does not apply to the session in hand.
   *
   * The role is set explicitly: a DPS-configured client is labelled
   * HUB_CLASSIC until open() starts provisioning, because that is what it
   * becomes once it is assigned. */
  az_iot_connection_client_options opts = { 0 };
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__set_session_role(&c, AZ_IOT_MQTT_ROLE_DPS), AZ_IOT_OK);

  char filter[32];
  warning_capture cap;
  install_warning_capture(&cap);
  for (unsigned i = 0; i < AZ_IOT_HUB_MAX_SUBSCRIPTIONS + 2; ++i)
  {
    snprintf(filter, sizeof(filter), "dps/%u/#", i);
    assert_int_equal(
        az_iot_connection_client__add_subscription_on_connect(&c, filter, AZ_IOT_MQTT_QOS_0),
        AZ_IOT_OK);
  }
  az_iot_log_set_global_sink(NULL);

  assert_int_equal(cap.count, 0);
  az_iot_connection_client_destroy(&c);
}

/* A broker that refuses one filter must not take the whole connection down:
 * the other features on the same connection are still usable. */
static void a_failed_subscription_restore_keeps_the_connection(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "restored/#", AZ_IOT_MQTT_QOS_1),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  az_iot_mock_mqtt_client_set_next_result(m, AZ_IOT_MOCK_CALL_SUBSCRIBE, AZ_IOT_ERR_MQTT);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_CONNECTED);
}

static void a_failed_suback_keeps_the_connection(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "restored/#", AZ_IOT_MQTT_QOS_1),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_CONNECTED);
}

/* ------------------------------------------------------------------------- */
/* in-flight QoS-1 acknowledgements across a session change                   */
/* ------------------------------------------------------------------------- */

typedef struct puback_probe
{
  int calls;
  az_iot_result last_status;
} puback_probe;

static void on_puback(az_iot_result status, void* user_ctx)
{
  puback_probe* p = (puback_probe*)user_ctx;
  p->calls++;
  p->last_status = status;
}

static uint16_t publish_qos1(az_iot_test_conn* fx, az_iot_mock_mqtt_client* m, puback_probe* probe)
{
  az_iot_mqtt_message msg = { 0 };
  msg.topic = "devices/ut-device/messages/events/";
  msg.payload = (const uint8_t*)"x";
  msg.payload_len = 1;
  msg.qos = AZ_IOT_MQTT_QOS_1;
  assert_int_equal(
      az_iot_connection_client__publish(fx->client, &msg, on_puback, probe), AZ_IOT_OK);

  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  return pub->packet_id;
}

static void matching_puback_invokes_the_callback(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  puback_probe probe = { 0 };
  uint16_t pid = publish_qos1(fx, m, &probe);

  assert_true(az_iot_mock_mqtt_client_inject_puback(m, pid, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(probe.calls, 1);
  assert_int_equal(probe.last_status, AZ_IOT_OK);
}

static void unmatched_puback_is_ignored(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  puback_probe probe = { 0 };
  uint16_t pid = publish_qos1(fx, m, &probe);

  assert_true(az_iot_mock_mqtt_client_inject_puback(m, (uint16_t)(pid + 1), AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(probe.calls, 0);
}

/* A publish that was in flight when the session died can never be
 * acknowledged. Completing the callback with an error is what lets the caller
 * decide to resend; dropping it silently would leave the app tracking a
 * publish that can no longer finish either way. */
static void pending_pubacks_are_completed_with_an_error_on_disconnect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  puback_probe probe = { 0 };
  uint16_t pid = publish_qos1(fx, m, &probe);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(probe.calls, 1);
  assert_int_equal(probe.last_status, AZ_IOT_ERR_NOT_CONNECTED);

  /* The slot is genuinely released, so a late ack from the dead session on a
   * recycled packet id cannot fire the callback a second time. */
  m = advance_to_retry(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_mock_mqtt_client_inject_puback(m, pid, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(probe.calls, 1);
}

/* Every outstanding publish must be reported, not just the first. */
static void all_pending_pubacks_are_completed_on_disconnect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  puback_probe a = { 0 };
  puback_probe b = { 0 };
  puback_probe c = { 0 };
  (void)publish_qos1(fx, m, &a);
  (void)publish_qos1(fx, m, &b);
  (void)publish_qos1(fx, m, &c);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(a.calls, 1);
  assert_int_equal(b.calls, 1);
  assert_int_equal(c.calls, 1);
  assert_int_equal(a.last_status, AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(b.last_status, AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(c.last_status, AZ_IOT_ERR_NOT_CONNECTED);
}

/* Destroying inside the backoff window is the awkward case: there is no
 * adapter to tear down (it was destroyed when the retry was scheduled) but a
 * deadline is still armed. Nothing must be left pointing at freed memory, and
 * no retry may fire afterwards. */
static void destroy_while_reconnect_is_scheduled(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));

  size_t transitions = fx->log.count;
  az_iot_connection_client_destroy(fx->client);
  assert_int_equal(fx->log.count, transitions);

  /* Neutralize the fixture teardown: already destroyed, factory adopted. */
  memset(&fx->client_storage, 0, sizeof(fx->client_storage));
  fx->factory = NULL;
}

/* destroy() is the one teardown that must stay silent: the application is
 * tearing the client down, so the context a publish callback closes over may
 * already be gone and calling into it would be a use-after-free. */
static void destroy_does_not_complete_pending_pubacks(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  puback_probe probe = { 0 };
  (void)publish_qos1(fx, m, &probe);

  az_iot_connection_client_destroy(fx->client);
  assert_int_equal(probe.calls, 0);

  /* Neutralize the fixture teardown: already destroyed, factory adopted. */
  memset(&fx->client_storage, 0, sizeof(fx->client_storage));
  fx->factory = NULL;
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    /* what schedules a retry */
    cmocka_unit_test_setup_teardown(
        adapter_error_event_schedules_a_retry, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        adapter_error_event_faults_without_a_policy, setup_no_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        keep_alive_drop_is_retried_like_any_disconnect, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        user_close_disconnect_goes_idle_not_reconnecting, setup_two_attempts, teardown),
    /* backoff deadline */
    cmocka_unit_test_setup_teardown(
        retry_waits_for_the_backoff_deadline, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        retry_destroys_the_old_adapter_and_builds_a_new_one, setup_two_attempts, teardown),
    /* attempt accounting */
    cmocka_unit_test_setup_teardown(
        successful_reconnect_resets_the_attempt_counter, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(zero_max_attempts_never_gives_up, setup_infinite, teardown),
    cmocka_unit_test_setup_teardown(
        full_outage_recovers_to_connected, setup_two_attempts, teardown),
    /* persistent subscriptions */
    cmocka_unit_test_setup_teardown(
        persistent_subscription_is_issued_on_connect, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        persistent_subscriptions_are_reissued_after_a_reconnect, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        persistent_subscription_registry_full_is_rejected, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        persistent_subscription_added_while_connected_subscribes_now, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        the_hub_subscription_limit_is_diagnosed, setup_two_attempts, teardown),
    cmocka_unit_test(the_hub_subscription_limit_does_not_apply_to_dps),
    cmocka_unit_test_setup_teardown(
        a_failed_subscription_restore_keeps_the_connection, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_suback_keeps_the_connection, setup_two_attempts, teardown),
    /* QoS-1 acknowledgements */
    cmocka_unit_test_setup_teardown(
        matching_puback_invokes_the_callback, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(unmatched_puback_is_ignored, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        pending_pubacks_are_completed_with_an_error_on_disconnect, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        all_pending_pubacks_are_completed_on_disconnect, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        destroy_does_not_complete_pending_pubacks, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        destroy_while_reconnect_is_scheduled, setup_two_attempts, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
