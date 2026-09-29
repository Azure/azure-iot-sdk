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
#include "support/test_provider.h"
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
  az_iot_connection_client_options opts = az_iot_test_mqtt_v3_options();
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
  assert_int_equal(az_iot_test_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
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

/* max_attempts = 0 means "retry forever". */
static int setup_infinite(void** state) { return setup_with_policy(state, 0); }
static int setup_two_attempts(void** state) { return setup_with_policy(state, 2); }

/* As setup_two_attempts, but with the shortest gate deadline the seconds-scaled
 * option can express, so the never-acked path can be exercised on the wall clock
 * instead of only through the test seam. */
static int setup_short_gate_deadline(void** state)
{
  int rc = setup_with_policy(state, 2);
  if (rc == 0)
  {
    az_iot_test_conn* fx = (az_iot_test_conn*)*state;
    fx->client->opts.subscription_ack_timeout_seconds = 1;
  }
  return rc;
}

static int setup_no_reconnect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = az_iot_test_mqtt_v3_options();
  assert_int_equal(az_iot_test_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
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
    az_iot_connection_client_deinit(&fx->client_storage);
    if (!adopted)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
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
  {
    (void)az_iot_connection_client_do_work(fx->client, 0);
  }
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
          fx->client,
          "devices/ut-device/messages/devicebound/#",
          AZ_IOT_MQTT_QOS_1,
          fx,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
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
          fx->client,
          "devices/ut-device/messages/devicebound/#",
          AZ_IOT_MQTT_QOS_1,
          fx,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "$iothub/methods/POST/#",
          AZ_IOT_MQTT_QOS_0,
          fx,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
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

/* ---- persistent subscription registry exhaustion ------------------------- */

typedef struct log_capture
{
  int count;
  char last[AZ_IOT_LOG_MESSAGE_MAX];
} log_capture;

static void capture_error(
    void* user_ctx,
    az_iot_log_level level,
    const char* file,
    int line,
    const char* msg)
{
  log_capture* c = (log_capture*)user_ctx;
  (void)file;
  (void)line;
  if (level != AZ_IOT_LOG_LEVEL_ERROR || msg == NULL)
  {
    return;
  }
  c->count++;
  snprintf(c->last, sizeof(c->last), "%s", msg);
}

static void install_error_capture(log_capture* c)
{
  memset(c, 0, sizeof(*c));
  az_iot_log_sink sink;
  sink.sink = capture_error;
  sink.user_ctx = c;
  sink.min_level = AZ_IOT_LOG_LEVEL_ERROR;
  az_iot_log_set_global_sink(&sink);
}

static void persistent_subscription_registry_full_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;

  char filter[32];
  log_capture cap;
  install_error_capture(&cap);

  for (unsigned i = 0; i < AZ_IOT_MAX_PERSISTENT_SUBS; ++i)
  {
    filter[0] = 'f';
    filter[1] = (char)('0' + (int)i);
    filter[2] = '\0';
    assert_int_equal(
        az_iot_connection_client__add_subscription_on_connect(
            fx->client, filter, AZ_IOT_MQTT_QOS_0, fx, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL),
        AZ_IOT_OK);
  }
  assert_int_equal(cap.count, 0);

  /* A capacity failure rather than NOT_SUPPORTED: a larger registry would take
   * this filter, whereas nothing about a larger array makes a genuinely
   * unsupported operation work. The two need to be told apart by a caller
   * deciding whether to raise AZ_IOT_MAX_PERSISTENT_SUBS. */
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "one-too-many",
          AZ_IOT_MQTT_QOS_0,
          fx,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  az_iot_log_set_global_sink(NULL);

  /* Named, because the filter that gets refused is whichever one asked last --
   * which need not be the feature that consumed the slots. */
  assert_int_equal(cap.count, 1);
  assert_non_null(strstr(cap.last, "one-too-many"));
}

static void persistent_subscription_added_while_connected_subscribes_now(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);
  az_iot_mock_mqtt_client_clear_calls(m);

  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "late/filter/#",
          AZ_IOT_MQTT_QOS_1,
          fx,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_OK);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_string_equal(sub->topic, "late/filter/#");
}

/* ------------------------------------------------------------------------- */
/* withdrawing persistent subscriptions                                      */
/* ------------------------------------------------------------------------- */

/* Two feature clients share a connection; retiring one must not disturb the
 * other. Without an owner on the registry entry there is no way to tell them
 * apart, which is why removal is keyed by owner rather than by filter string. */
static void removing_one_owner_leaves_the_other_owners_filters(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  int owner_a = 0;
  int owner_b = 0;

  az_iot_mock_mqtt_client* m = open_to_connected(fx);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "$iothub/twin/res/#",
          AZ_IOT_MQTT_QOS_1,
          &owner_a,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "$iothub/methods/POST/#",
          AZ_IOT_MQTT_QOS_0,
          &owner_b,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client__remove_subscriptions_for(fx->client, &owner_a), 1);

  /* Only owner_a's filter is withdrawn on the wire ... */
  const az_iot_mock_call* uns = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_UNSUBSCRIBE);
  assert_non_null(uns);
  assert_string_equal(uns->topic, "$iothub/twin/res/#");
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_UNSUBSCRIBE), 1);

  /* ... and only owner_b's is restored on the next connect. */
  az_iot_mock_mqtt_client_clear_calls(m);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_test_wait_ms(RETRY_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* m2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m2);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_mock_mqtt_client_count_of(m2, AZ_IOT_MOCK_CALL_SUBSCRIBE), 1);
  const az_iot_mock_call* resub = az_iot_mock_mqtt_client_last_of(m2, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(resub);
  assert_string_equal(resub->topic, "$iothub/methods/POST/#");
}

/* The defect this closes: deinit() used to leave the filter registered, so it
 * came back on the next reconnect and kept consuming a registry slot forever. */
static void a_withdrawn_filter_is_not_restored_on_reconnect(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  int owner = 0;

  az_iot_mock_mqtt_client* m = open_to_connected(fx);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "$iothub/twin/res/#",
          AZ_IOT_MQTT_QOS_1,
          &owner,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__remove_subscriptions_for(fx->client, &owner), 1);

  az_iot_mock_mqtt_client_clear_calls(m);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_test_wait_ms(RETRY_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* m2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m2);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_mock_mqtt_client_count_of(m2, AZ_IOT_MOCK_CALL_SUBSCRIBE), 0);
}

/* Removing while disconnected still has to clear the registry, or the entry
 * would be resurrected by a reconnect the application never asked for. */
static void removing_while_disconnected_still_clears_the_registry(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  int owner = 0;

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "$iothub/twin/res/#",
          AZ_IOT_MQTT_QOS_1,
          &owner,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client__remove_subscriptions_for(fx->client, &owner), 1);

  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE), 0);
}

/* Withdrawing an owner that never registered anything is not an error -- a
 * feature client whose init failed part-way still runs its deinit(). */
static void removing_an_unknown_owner_removes_nothing(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  int registered = 0;
  int never_registered = 0;

  az_iot_mock_mqtt_client* m = open_to_connected(fx);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "$iothub/twin/res/#",
          AZ_IOT_MQTT_QOS_1,
          &registered,
          AZ_IOT_SUBSCRIPTION_FAILS_SESSION,
          NULL),
      AZ_IOT_OK);

  assert_int_equal(
      az_iot_connection_client__remove_subscriptions_for(fx->client, &never_registered), 0);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_UNSUBSCRIBE), 0);
}

/* --- what a refused subscription costs, by scope and by reason ------------ */

typedef struct sub_failure_probe
{
  int calls;
  az_iot_result reason;
  int32_t protocol_code;
  char topic[64];
} sub_failure_probe;

static void on_sub_failed(
    const char* topic_filter,
    az_iot_result reason,
    int32_t protocol_code,
    const void* owner)
{
  sub_failure_probe* p = (sub_failure_probe*)owner;
  p->calls++;
  p->reason = reason;
  p->protocol_code = protocol_code;
  snprintf(p->topic, sizeof(p->topic), "%s", topic_filter ? topic_filter : "");
}

/* Register one session-scoped filter and drive the connection to the point
 * where its SUBSCRIBE has been written and the ack is outstanding. */
static az_iot_mock_mqtt_client* open_to_pending_gate(az_iot_test_conn* fx, const char* filter)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, filter, AZ_IOT_MQTT_QOS_1, fx, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  return m;
}

/* The whole point of the gate: an application rebuilding feature clients from
 * the CONNECTED callback must not be told the session is live while the filter
 * carrying those clients' responses is still unacknowledged. */
static void connected_waits_for_the_suback(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_pending_gate(fx, "restored/#");

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_false(az_iot_connection_client__is_connected(fx->client));

  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* A filter the broker will refuse every time cannot be repaired by reconnecting:
 * the same SUBSCRIBE would be re-issued and refused again, so a device with a
 * policy configured would cycle forever without ever saying why. Terminal, and
 * deliberately so even though this fixture has reconnect enabled. */
static void a_refused_session_filter_faults_terminally(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_pending_gate(fx, "restored/#");

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(
      az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_ERR_SUBSCRIPTION_REFUSED));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_false(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_SUBSCRIPTION_REFUSED);
}

/* Quota exceeded or an unspecified error is how a passing service-side fault
 * presents, so the session retries rather than abandoning a filter the broker
 * may well grant on the next attempt. */
static void a_transient_suback_failure_reconnects(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_pending_gate(fx, "restored/#");

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_false(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
}

/* A SUBSCRIBE that could not even be written never reached a broker, so it
 * carries no verdict about the filter: transient, and retried. */
static void a_failed_subscribe_call_reconnects(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "restored/#", AZ_IOT_MQTT_QOS_1, fx, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  az_iot_mock_mqtt_client_set_next_result(m, AZ_IOT_MOCK_CALL_SUBSCRIBE, AZ_IOT_ERR_MQTT);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_false(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
}

/* A filter whose failure is scoped to itself still FAILS: its owner is told and
 * the entry is dropped, so a reconnect cannot silently re-issue it. What it must
 * not do is take telemetry and every other feature down with it. */
static void a_refused_self_scoped_filter_keeps_the_connection(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  sub_failure_probe probe = { 0 };

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "custom/topic/#",
          AZ_IOT_MQTT_QOS_1,
          &probe,
          AZ_IOT_SUBSCRIPTION_FAILS_SELF,
          on_sub_failed),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* Nothing gated is registered, so CONNECTED does not wait on this one. */
  assert_true(az_iot_connection_client__is_connected(fx->client));

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(
      az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_ERR_SUBSCRIPTION_REFUSED));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_CONNECTED);
  assert_int_equal(probe.calls, 1);
  assert_int_equal(probe.reason, AZ_IOT_ERR_SUBSCRIPTION_REFUSED);
  assert_string_equal(probe.topic, "custom/topic/#");

  /* Dropped from the registry, so the reconnect does not resurrect it. */
  az_iot_mock_mqtt_client_clear_calls(m);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_test_wait_ms(RETRY_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* m2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m2);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m2, AZ_IOT_MOCK_CALL_SUBSCRIBE), 0);
}

/* A broker that accepts the connection and then never answers the SUBSCRIBE
 * cannot be caught by keep-alive, because the link is alive. Silence is not a
 * refusal, so the deadline retries rather than faulting terminally. */
static void a_gate_that_is_never_acked_times_out(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_pending_gate(fx, "restored/#");
  assert_false(az_iot_connection_client__is_connected(fx->client));

  az_iot_connection_client__subscription_gate_force_timeout(fx->client);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_false(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_RECONNECTING), AZ_IOT_ERR_TIMEOUT);
}

/* Packet id of the SUBSCRIBE issued for `topic`, or 0 when there was none. */
static uint16_t subscribe_pid_for(const az_iot_mock_mqtt_client* m, const char* topic)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(m, i);
    if (call && call->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE && strcmp(call->topic, topic) == 0)
    {
      return call->packet_id;
    }
  }
  return 0;
}

/* The callback contract promises the verbatim wire code, not just a result.
 * Asserting only the result would still pass if every owner were handed 0. */
static void a_refused_self_scoped_filter_reports_the_wire_code(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  sub_failure_probe probe = { 0 };

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "custom/topic/#",
          AZ_IOT_MQTT_QOS_1,
          &probe,
          AZ_IOT_SUBSCRIPTION_FAILS_SELF,
          on_sub_failed),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* 0x87 Not authorized, as an MQTTv5 topic-space refusal would arrive. */
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
  ack.packet_id = subscribe_pid_for(m, "custom/topic/#");
  ack.status = AZ_IOT_ERR_SUBSCRIPTION_REFUSED;
  ack.protocol_code = 0x87;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &ack));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(probe.calls, 1);
  assert_int_equal(probe.protocol_code, 0x87);
  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* CONNECTED waits for EVERY gated filter, not merely the first to answer. */
static void connected_waits_for_all_gated_subacks(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "first/#", AZ_IOT_MQTT_QOS_1, fx, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "second/#", AZ_IOT_MQTT_QOS_1, fx, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_mock_mqtt_client_inject_suback(m, subscribe_pid_for(m, "first/#"), AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_false(az_iot_connection_client__is_connected(fx->client));

  assert_true(
      az_iot_mock_mqtt_client_inject_suback(m, subscribe_pid_for(m, "second/#"), AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* A self-scoped refusal must not release a gate a session-scoped filter is
 * still holding: the two scopes are tracked in one batch and must not bleed. */
static void a_self_scoped_refusal_does_not_release_the_gate(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  sub_failure_probe probe = { 0 };

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "gated/#", AZ_IOT_MQTT_QOS_1, fx, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client,
          "custom/#",
          AZ_IOT_MQTT_QOS_1,
          &probe,
          AZ_IOT_SUBSCRIPTION_FAILS_SELF,
          on_sub_failed),
      AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_mock_mqtt_client_inject_suback(
      m, subscribe_pid_for(m, "custom/#"), AZ_IOT_ERR_SUBSCRIPTION_REFUSED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(probe.calls, 1);
  assert_false(az_iot_connection_client__is_connected(fx->client));

  assert_true(az_iot_mock_mqtt_client_inject_suback(m, subscribe_pid_for(m, "gated/#"), AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* Withdrawing a gated filter while its ack is outstanding has to release the
 * gate: nothing is waiting on it any more, and the ack that would have
 * announced CONNECTED is never coming. */
static void withdrawing_a_gated_filter_releases_the_gate(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_pending_gate(fx, "restored/#");
  assert_false(az_iot_connection_client__is_connected(fx->client));

  assert_int_equal(az_iot_connection_client__remove_subscriptions_for(fx->client, fx), 1);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* And the ack that arrives afterwards must not be applied to whatever claimed
 * the freed registry slot -- a pending record holds an index, and an index can
 * be reused. Before this was handled, the refusal below faulted the session on
 * behalf of a filter nobody had asked for any more. */
static void a_late_ack_for_a_withdrawn_filter_is_ignored(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_pending_gate(fx, "restored/#");
  const uint16_t stale_pid = subscribe_pid_for(m, "restored/#");

  assert_int_equal(az_iot_connection_client__remove_subscriptions_for(fx->client, fx), 1);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));

  assert_true(az_iot_mock_mqtt_client_inject_suback(m, stale_pid, AZ_IOT_ERR_SUBSCRIPTION_REFUSED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* A filter registered while the session is up is subscribed immediately and its
 * ack correlated, so a refusal is not swallowed. This is the path P2's
 * rebuild-on-CONNECTED pattern uses, so a dead filter here would be invisible. */
static void a_filter_registered_while_connected_is_correlated(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  int owner = 0;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "late/#", AZ_IOT_MQTT_QOS_1, &owner, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL),
      AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_suback(
      m, subscribe_pid_for(m, "late/#"), AZ_IOT_ERR_SUBSCRIPTION_REFUSED));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_false(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
}

/* A SUBSCRIBE that cannot be written leaves nothing registered: reporting a
 * subscription the device does not have is worse than the error the caller
 * already gets back. */
static void a_failed_immediate_subscribe_registers_nothing(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  int owner = 0;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  az_iot_mock_mqtt_client_set_next_result(m, AZ_IOT_MOCK_CALL_SUBSCRIBE, AZ_IOT_ERR_MQTT);
  assert_int_equal(
      az_iot_connection_client__add_subscription_on_connect(
          fx->client, "late/#", AZ_IOT_MQTT_QOS_1, &owner, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL),
      AZ_IOT_ERR_MQTT);

  assert_int_equal(az_iot_connection_client__remove_subscriptions_for(fx->client, &owner), 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
}

/* A deadline must never act on a client the application has closed. close()
 * moves to DISCONNECTING but tears down only when the peer's DISCONNECT lands,
 * so the gate is still armed in between -- firing there would fault a
 * connection that was shutting down cleanly. */
static void a_gate_deadline_does_not_fault_a_closing_client(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_pending_gate(fx, "restored/#");
  assert_false(az_iot_connection_client__is_connected(fx->client));

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  az_iot_connection_client__subscription_gate_force_timeout(fx->client);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_not_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_not_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
}

/* The seam-driven test above proves the branch; this one proves the clock is
 * actually consulted. Nothing forces the deadline here -- it is configured to a
 * second and then allowed to pass. The intermediate assertion matters as much as
 * the final one: a deadline that fired immediately would satisfy the second
 * check on its own. */
static void a_gate_deadline_expires_on_the_clock(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_pending_gate(fx, "restored/#");
  assert_false(az_iot_connection_client__is_connected(fx->client));

  az_iot_test_wait_ms(250u);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_CONNECTING);

  az_iot_test_wait_ms(900u);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_false(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_RECONNECTING), AZ_IOT_ERR_TIMEOUT);
}

/* The gate belongs to the session. When that session dies its packet ids die
 * with it, so a deadline left armed could reconnect -- or fault -- a client on
 * behalf of acks that can never arrive. */
static void a_dropped_session_clears_the_gate(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_pending_gate(fx, "restored/#");
  assert_false(az_iot_connection_client__is_connected(fx->client));

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* Nothing to expire: the gate went with the session. */
  az_iot_connection_client__subscription_gate_force_timeout(fx->client);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_not_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);

  az_iot_test_wait_ms(RETRY_DELAY_MS + 5u);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  az_iot_mock_mqtt_client* m2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m2);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(
      az_iot_mock_mqtt_client_inject_suback(m2, subscribe_pid_for(m2, "restored/#"), AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
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
static void deinit_while_reconnect_is_scheduled(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_RECONNECTING);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));

  size_t transitions = fx->log.count;
  az_iot_connection_client_deinit(fx->client);
  assert_int_equal(fx->log.count, transitions);

  /* Neutralize the fixture teardown: already destroyed, factory adopted. */
  memset(&fx->client_storage, 0, sizeof(fx->client_storage));
  fx->factory = NULL;
}

/* deinit() is the one teardown that must stay silent: the application is
 * tearing the client down, so the context a publish callback closes over may
 * already be gone and calling into it would be a use-after-free. */
static void deinit_does_not_complete_pending_pubacks(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  puback_probe probe = { 0 };
  (void)publish_qos1(fx, m, &probe);

  az_iot_connection_client_deinit(fx->client);
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
        removing_one_owner_leaves_the_other_owners_filters, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_withdrawn_filter_is_not_restored_on_reconnect, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        removing_while_disconnected_still_clears_the_registry, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        removing_an_unknown_owner_removes_nothing, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(connected_waits_for_the_suback, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_refused_session_filter_faults_terminally, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_transient_suback_failure_reconnects, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_subscribe_call_reconnects, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_refused_self_scoped_filter_keeps_the_connection, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_gate_that_is_never_acked_times_out, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_refused_self_scoped_filter_reports_the_wire_code, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        connected_waits_for_all_gated_subacks, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_self_scoped_refusal_does_not_release_the_gate, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        withdrawing_a_gated_filter_releases_the_gate, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_late_ack_for_a_withdrawn_filter_is_ignored, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_filter_registered_while_connected_is_correlated, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_immediate_subscribe_registers_nothing, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_dropped_session_clears_the_gate, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_gate_deadline_does_not_fault_a_closing_client, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        a_gate_deadline_expires_on_the_clock, setup_short_gate_deadline, teardown),
    /* QoS-1 acknowledgements */
    cmocka_unit_test_setup_teardown(
        matching_puback_invokes_the_callback, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(unmatched_puback_is_ignored, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        pending_pubacks_are_completed_with_an_error_on_disconnect, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        all_pending_pubacks_are_completed_on_disconnect, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        deinit_does_not_complete_pending_pubacks, setup_two_attempts, teardown),
    cmocka_unit_test_setup_teardown(
        deinit_while_reconnect_is_scheduled, setup_two_attempts, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
