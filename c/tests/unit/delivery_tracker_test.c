// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_iot_delivery_tracker: send, confirm, resend, expire, over a connection with a mock adapter. */
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
#include "internal/connection_client_internal.h"
#include "internal/deadline.h"
#include "internal/delivery_tracker.h"
#include "internal/mono_time.h"

#include "support/connection_test_harness.h"
#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"

#define ENTRY_COUNT 4u
#define RESERVED_PUBACKS 2u

typedef struct completion
{
  size_t entry_index;
  az_iot_delivery_tracker_outcome outcome;
} completion;

typedef struct fixture
{
  az_iot_connection_client connection;
  az_iot_mqtt_factory* factory;
  az_iot_mock_mqtt_client* mock;
  az_iot_delivery_tracker tracker;
  az_iot_delivery_tracker_entry entries[ENTRY_COUNT];
  char topics[ENTRY_COUNT][8];
  az_iot_mqtt_qos qos;
  az_iot_result encode_result;
  int failing_sends; /* Sends the adapter fails with failing_send_result. */
  az_iot_result failing_send_result;
  completion completions[16];
  size_t completion_count;
  bool unbind_on_complete;
  /* Once, when set: submit two messages with this profile (after unbind, init, bind when
   * rebind_first). */
  const az_iot_delivery_tracker_profile* submit_two_on_complete;
  bool rebind_first;
} fixture;

static const uint8_t k_guaranteed[] = { 1u }; /* One slot kept for priority 0. */

static az_iot_result encode(void* owner_context, size_t entry_index, az_iot_mqtt_message* out)
{
  fixture* fx = (fixture*)owner_context;
  (void)snprintf(
      fx->topics[entry_index], sizeof(fx->topics[entry_index]), "t/%u", (unsigned)entry_index);
  out->topic = fx->topics[entry_index];
  out->qos = fx->qos;
  if (fx->failing_sends > 0)
  {
    --fx->failing_sends;
    az_iot_mock_mqtt_client_set_next_result(
        fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, fx->failing_send_result);
  }
  return fx->encode_result;
}

static void on_complete(
    void* owner_context,
    size_t entry_index,
    az_iot_delivery_tracker_outcome outcome,
    const az_iot_publish_result* detail)
{
  (void)detail;
  fixture* fx = (fixture*)owner_context;
  assert_true(fx->completion_count < sizeof(fx->completions) / sizeof(fx->completions[0]));
  fx->completions[fx->completion_count].entry_index = entry_index;
  fx->completions[fx->completion_count].outcome = outcome;
  fx->completion_count++;
  if (fx->submit_two_on_complete != NULL)
  {
    const az_iot_delivery_tracker_profile* profile = fx->submit_two_on_complete;
    fx->submit_two_on_complete = NULL;
    if (fx->rebind_first)
    {
      az_iot_delivery_tracker__unbind(&fx->tracker);
      assert_int_equal(
          az_iot_delivery_tracker__init(
              &fx->tracker, fx->entries, ENTRY_COUNT, RESERVED_PUBACKS, k_guaranteed, 1u, fx),
          AZ_IOT_OK);
      assert_int_equal(az_iot_delivery_tracker__bind(&fx->tracker, &fx->connection), AZ_IOT_OK);
    }
    for (uint8_t id = 7u; id <= 8u; ++id)
    {
      size_t index = 0;
      assert_int_equal(
          az_iot_delivery_tracker__submit(&fx->tracker, profile, &id, 1u, 0u, &index), AZ_IOT_OK);
    }
  }
  if (fx->unbind_on_complete)
  {
    az_iot_delivery_tracker__unbind(&fx->tracker);
    memset(&fx->tracker, 0xA5, sizeof(fx->tracker)); /* The owner reuses the storage. */
  }
}

#define PROFILE(name, confirm, policy, prio)        \
  static const az_iot_delivery_tracker_profile name \
      = {                                           \
          #name,                                    \
          AZ_IOT_DELIVERY_TRACKER_##confirm,        \
          AZ_IOT_DELIVERY_TRACKER_##policy,         \
          prio,                                     \
          encode,                                   \
          on_complete,                              \
        }
PROFILE(k_resend, CONFIRM_BY_PUBACK, ON_SESSION_LOSS_RESEND, 0);
PROFILE(k_fail, CONFIRM_BY_PUBACK, ON_SESSION_LOSS_FAIL, 0);
PROFILE(k_response, CONFIRM_BY_RESPONSE, ON_SESSION_LOSS_FAIL, 0);
PROFILE(k_response_resend, CONFIRM_BY_RESPONSE, ON_SESSION_LOSS_RESEND, 0);
PROFILE(k_low_resend, CONFIRM_BY_PUBACK, ON_SESSION_LOSS_RESEND, 1);
PROFILE(k_low_drop, CONFIRM_BY_PUBACK, ON_SESSION_LOSS_DROP, 1);

static void do_work(fixture* fx) { (void)az_iot_connection_client_do_work(&fx->connection, 0); }

/** @brief Open and reach CONNECTED on a new mock session. */
static void connect(fixture* fx)
{
  assert_int_equal(az_iot_connection_client_open(&fx->connection), AZ_IOT_OK);
  fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(fx->mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
  do_work(fx);
  assert_true(az_iot_connection_client__is_connected(&fx->connection));
}

/** @brief The link drops (FAULTED with no reconnection policy), then close() settles to IDLE. */
static void lose_session(fixture* fx)
{
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(fx->mock));
  do_work(fx);
  assert_int_equal(az_iot_connection_client_close(&fx->connection), AZ_IOT_OK);
  fx->mock = NULL;
}

static size_t publish_count(const fixture* fx)
{
  return az_iot_mock_mqtt_client_count_of(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH);
}

static const az_iot_mock_call* last_publish(const fixture* fx)
{
  const az_iot_mock_call* call
      = az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(call);
  return call;
}

/** @brief The last PUBLISH to @p topic. */
static const az_iot_mock_call* find_publish(const fixture* fx, const char* topic)
{
  const az_iot_mock_call* found = NULL;
  for (size_t i = 0; i < az_iot_mock_mqtt_client_call_count(fx->mock); ++i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(fx->mock, i);
    found = (call->kind == AZ_IOT_MOCK_CALL_PUBLISH && strcmp(call->topic, topic) == 0) ? call
                                                                                        : found;
  }
  assert_non_null(found);
  return found;
}

static void puback(fixture* fx, uint16_t packet_id, az_iot_result status)
{
  assert_true(az_iot_mock_mqtt_client_inject_puback(fx->mock, packet_id, status));
  do_work(fx);
}

static size_t submit(
    fixture* fx,
    const az_iot_delivery_tracker_profile* profile,
    uint8_t id,
    uint32_t timeout_ms)
{
  size_t index = SIZE_MAX;
  assert_int_equal(
      az_iot_delivery_tracker__submit(&fx->tracker, profile, &id, 1u, timeout_ms, &index),
      AZ_IOT_OK);
  return index;
}

static void assert_completed(
    const fixture* fx,
    size_t nth,
    size_t entry_index,
    az_iot_delivery_tracker_outcome outcome)
{
  assert_true(fx->completion_count > nth);
  assert_int_equal(fx->completions[nth].entry_index, entry_index);
  assert_int_equal(fx->completions[nth].outcome, outcome);
}

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);
  az_iot_connection_client_options options = az_iot_test_mqtt_v3_options();
  assert_int_equal(az_iot_test_connection_client_init(&fx->connection, &options), AZ_IOT_OK);
  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->connection, fx->factory), AZ_IOT_OK);
  fx->qos = AZ_IOT_MQTT_QOS_1;
  fx->failing_send_result = AZ_IOT_ERR_BUSY;
  assert_int_equal(
      az_iot_delivery_tracker__init(
          &fx->tracker, fx->entries, ENTRY_COUNT, RESERVED_PUBACKS, k_guaranteed, 1u, fx),
      AZ_IOT_OK);
  assert_int_equal(az_iot_delivery_tracker__bind(&fx->tracker, &fx->connection), AZ_IOT_OK);
  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_delivery_tracker__unbind(&fx->tracker);
  az_iot_connection_client_deinit(&fx->connection);
  az_iot_mock_mqtt_factory_destroy(fx->factory);
  free(fx);
  return 0;
}

/* Sent at submit while CONNECTED; delivered once at its PUBACK. */
static void a_message_is_delivered_at_its_puback(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  size_t index = submit(fx, &k_resend, 1u, 0u);
  assert_int_equal(publish_count(fx), 1);
  assert_string_equal(last_publish(fx)->topic, "t/0");
  assert_int_equal(fx->completion_count, 0);
  puback(fx, last_publish(fx)->packet_id, AZ_IOT_OK);
  assert_int_equal(fx->completion_count, 1);
  assert_completed(fx, 0, index, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED);
  do_work(fx);
  assert_int_equal(publish_count(fx), 1);
}

/* Submitted before CONNECTED: held, then sent on CONNECTED. */
static void a_message_submitted_offline_is_sent_on_connected(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)submit(fx, &k_resend, 1u, 0u);
  connect(fx);
  assert_int_equal(publish_count(fx), 1);
}

/* QoS 0 confirmed by PUBACK is done at send; QoS 0 confirmed by response waits for it. */
static void qos0_messages_are_untracked(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->qos = AZ_IOT_MQTT_QOS_0;
  connect(fx);
  size_t index = submit(fx, &k_resend, 1u, 0u);
  assert_completed(fx, 0, index, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED);
  index = submit(fx, &k_response, 2u, 0u);
  assert_int_equal(fx->completion_count, 1);
  uint8_t const id = 2u;
  assert_int_equal(az_iot_delivery_tracker__complete(&fx->tracker, &id, 1u), AZ_IOT_OK);
  assert_completed(fx, 1, index, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED);
}

/* Confirmed by response: after the PUBACK, or before it (the late PUBACK is then ignored). */
static void a_response_confirms_before_or_after_the_puback(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  uint8_t const first = 1u;
  uint8_t const second = 2u;
  size_t index = submit(fx, &k_response, first, 0u);
  puback(fx, last_publish(fx)->packet_id, AZ_IOT_OK);
  assert_int_equal(fx->completion_count, 0);
  assert_int_equal(az_iot_delivery_tracker__complete(&fx->tracker, &first, 1u), AZ_IOT_OK);
  assert_completed(fx, 0, index, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED);

  index = submit(fx, &k_response, second, 0u);
  uint16_t const packet_id = last_publish(fx)->packet_id;
  assert_int_equal(az_iot_delivery_tracker__complete(&fx->tracker, &second, 1u), AZ_IOT_OK);
  assert_completed(fx, 1, index, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED);
  puback(fx, packet_id, AZ_IOT_OK);
  assert_int_equal(fx->completion_count, 2);

  assert_int_equal(
      az_iot_delivery_tracker__complete(&fx->tracker, &second, 1u), AZ_IOT_ERR_NOT_FOUND);
}

/* A refused PUBLISH completes as REFUSED and is not resent. */
static void a_refused_publish_is_not_resent(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  size_t index = submit(fx, &k_resend, 1u, 0u);
  puback(fx, last_publish(fx)->packet_id, AZ_IOT_ERR_PUBLISH_REFUSED);
  assert_completed(fx, 0, index, AZ_IOT_DELIVERY_TRACKER_OUTCOME_REFUSED);
  do_work(fx);
  assert_int_equal(publish_count(fx), 1);
}

/* A lost session: RESEND publishes again on the next session; FAIL completes as SESSION_LOST,
 * whether it was waiting for its PUBACK or for its response. */
static void a_lost_session_resends_or_fails_per_policy(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  size_t const resent = submit(fx, &k_resend, 1u, 0u);
  size_t const failed = submit(fx, &k_fail, 2u, 0u);
  (void)resent;
  assert_int_equal(publish_count(fx), 2);
  lose_session(fx);
  assert_int_equal(fx->completion_count, 1);
  assert_completed(fx, 0, failed, AZ_IOT_DELIVERY_TRACKER_OUTCOME_SESSION_LOST);

  connect(fx);
  assert_int_equal(publish_count(fx), 1);
  size_t const awaiting = submit(fx, &k_response, 3u, 0u);
  size_t const awaiting_resend = submit(fx, &k_response_resend, 4u, 0u);
  az_iot_mock_mqtt_client* m = fx->mock;
  for (size_t i = 0; i < az_iot_mock_mqtt_client_call_count(m); ++i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(m, i);
    if (call->kind == AZ_IOT_MOCK_CALL_PUBLISH && strcmp(call->topic, "t/0") != 0)
    {
      assert_true(az_iot_mock_mqtt_client_inject_puback(m, call->packet_id, AZ_IOT_OK));
    }
  }
  do_work(fx);
  /* t/2 went out when t/1 freed its slot; its PUBACK leaves it awaiting the response. */
  assert_string_equal(last_publish(fx)->topic, "t/2");
  puback(fx, last_publish(fx)->packet_id, AZ_IOT_OK);
  assert_int_equal(
      fx->entries[awaiting_resend]._internal.state, fx->entries[awaiting]._internal.state);
  lose_session(fx);
  assert_completed(fx, 1, awaiting, AZ_IOT_DELIVERY_TRACKER_OUTCOME_SESSION_LOST);
  assert_int_equal(fx->completion_count, 2);
  connect(fx);
  assert_int_equal(publish_count(fx), 2); /* t/0 again, and the resent response request. */
}

/* A PUBACK error (not a refusal) requeues it; that same PUBACK event does not resend it, the
 * next tick does. */
static void a_failed_puback_is_resent_from_a_later_event(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  (void)submit(fx, &k_resend, 1u, 0u);
  assert_true(az_iot_mock_mqtt_client_inject_puback(
      fx->mock, last_publish(fx)->packet_id, AZ_IOT_ERR_MQTT));
  do_work(fx);
  assert_int_equal(publish_count(fx), 1);
  do_work(fx);
  assert_int_equal(publish_count(fx), 2);
  assert_int_equal(fx->completion_count, 0);
}

/* The time limit expires a message on the tick, sent or not; 0 uses the connection's
 * max_unconfirmed_delivery_timeout_ms. */
static void messages_expire_at_their_limit(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->connection.opts.max_unconfirmed_delivery_timeout_ms = 20u;
  size_t const unbounded = submit(fx, &k_resend, 1u, 0u);
  connect(fx);
  size_t const bounded = submit(fx, &k_fail, 2u, 10u);
  az_iot_test_wait_ms(50u);
  do_work(fx);
  assert_int_equal(fx->completion_count, 2);
  assert_completed(fx, 0, unbounded, AZ_IOT_DELIVERY_TRACKER_OUTCOME_EXPIRED);
  assert_completed(fx, 1, bounded, AZ_IOT_DELIVERY_TRACKER_OUTCOME_EXPIRED);
}

/* A message expired while offline is not sent on CONNECTED. */
static void an_expired_message_is_not_sent(void** state)
{
  fixture* fx = (fixture*)*state;
  size_t const index = submit(fx, &k_resend, 1u, 60000u);
  az_iot_deadline__expire_now(&fx->entries[index]._internal.deadline);
  connect(fx);
  assert_int_equal(publish_count(fx), 0);
  assert_completed(fx, 0, index, AZ_IOT_DELIVERY_TRACKER_OUTCOME_EXPIRED);
}

/* Held messages go out by priority (0 first), then in submit order. */
static void held_messages_go_out_by_priority_then_age(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->qos = AZ_IOT_MQTT_QOS_0;
  size_t const low = submit(fx, &k_low_resend, 1u, 0u);
  size_t const high_first = submit(fx, &k_resend, 2u, 0u);
  size_t const high_second = submit(fx, &k_resend, 3u, 0u);
  connect(fx);
  assert_int_equal(fx->completion_count, 3);
  assert_completed(fx, 0, high_first, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED);
  assert_completed(fx, 1, high_second, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED);
  assert_completed(fx, 2, low, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED);
}

/* With 2 slots and 1 kept for priority 0, priority 1 holds at most one; priority 0 still gets
 * one; a DROP message with no slot goes out untracked. */
static void priority_zero_keeps_its_guaranteed_slot(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  (void)submit(fx, &k_low_resend, 1u, 0u);
  (void)submit(fx, &k_low_resend, 2u, 0u);
  assert_int_equal(publish_count(fx), 1);
  (void)submit(fx, &k_resend, 3u, 0u);
  assert_int_equal(publish_count(fx), 2);
  size_t const notice = submit(fx, &k_low_drop, 4u, 0u);
  assert_int_equal(publish_count(fx), 3);
  assert_completed(fx, 0, notice, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED);
  /* Priority 0 freeing its slot does not admit the held priority-1 message (that slot stays
   * kept); priority 1 freeing its own does. */
  puback(fx, find_publish(fx, "t/2")->packet_id, AZ_IOT_OK);
  assert_int_equal(publish_count(fx), 3);
  puback(fx, find_publish(fx, "t/0")->packet_id, AZ_IOT_OK);
  assert_int_equal(publish_count(fx), 4);
  assert_string_equal(last_publish(fx)->topic, "t/1");
}

/* Deliver IDENTITY_CHANGED to the tracker's observer, as dps_apply_deferred() does. */
static void signal_identity_change(fixture* fx)
{
  const az_iot_connection_feature_observer* observer = NULL;
  for (size_t i = 0; i < fx->connection.opts.limits.max_feature_observers; ++i)
  {
    observer = fx->connection.feature_observers[i].user_ctx == &fx->tracker
        ? &fx->connection.feature_observers[i]
        : observer;
  }
  assert_non_null(observer);
  assert_true(observer->events & AZ_IOT_CONNECTION_FEATURE_EVENT_IDENTITY_CHANGED);
  az_iot_connection_feature_event const event = {
    AZ_IOT_CONNECTION_FEATURE_EVENT_IDENTITY_CHANGED, NULL, AZ_IOT_CONNECTION_SESSION_END_LOST, NULL
  };
  observer->cb(&event, observer->user_ctx);
}

/* An identity change discards every message. */
static void an_identity_change_discards_every_message(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)submit(fx, &k_resend, 1u, 0u);
  (void)submit(fx, &k_response, 2u, 0u);
  signal_identity_change(fx);
  assert_int_equal(fx->completion_count, 2);
  assert_int_equal(fx->completions[0].outcome, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DISCARDED);
  assert_int_equal(fx->completions[1].outcome, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DISCARDED);
}

/* An on_complete that re-initializes and rebinds to the same connection stops the walk: the
 * messages it submits are kept. */
static void a_completion_may_rebind_the_tracker(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)submit(fx, &k_resend, 1u, 0u);
  (void)submit(fx, &k_resend, 2u, 0u);
  fx->submit_two_on_complete = &k_resend;
  fx->rebind_first = true;
  signal_identity_change(fx);
  assert_int_equal(fx->completion_count, 1);
  assert_non_null(fx->entries[0]._internal.profile);
  assert_non_null(fx->entries[1]._internal.profile);
}
/* An on_complete that unbinds the tracker stops the walk: nothing else is completed, and the
 * tracker storage is not touched again. */
static void a_completion_may_unbind_the_tracker(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_deadline__expire_now(&fx->entries[submit(fx, &k_resend, 1u, 60000u)]._internal.deadline);
  az_iot_deadline__expire_now(&fx->entries[submit(fx, &k_resend, 2u, 60000u)]._internal.deadline);
  fx->unbind_on_complete = true;
  do_work(fx);
  assert_int_equal(fx->completion_count, 1);
  assert_int_equal(fx->tracker._internal.tick_count, 0xA5A5A5A5u);
  memset(&fx->tracker, 0, sizeof(fx->tracker));
  do_work(fx);
  assert_int_equal(fx->completion_count, 1);
}

/* Cancel drops a message silently; its late PUBACK reaches nobody. encode() failing discards. */
static void cancel_and_encode_failure(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  size_t index = submit(fx, &k_resend, 1u, 0u);
  uint16_t const packet_id = last_publish(fx)->packet_id;
  az_iot_delivery_tracker__cancel(&fx->tracker, index);
  puback(fx, packet_id, AZ_IOT_OK);
  assert_int_equal(fx->completion_count, 0);

  fx->encode_result = AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  index = submit(fx, &k_resend, 2u, 0u);
  assert_completed(fx, 0, index, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DISCARDED);
  assert_int_equal(publish_count(fx), 1);
}

/* Unbinding forgets shared-pool publishes too: their PUBACK, or the session teardown, reaches
 * nobody. */
static void unbinding_forgets_shared_pool_publishes(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_delivery_tracker shared;
  az_iot_delivery_tracker_entry shared_entries[1];
  assert_int_equal(
      az_iot_delivery_tracker__init(&shared, shared_entries, 1u, 0u, NULL, 0u, fx), AZ_IOT_OK);
  assert_int_equal(az_iot_delivery_tracker__bind(&shared, &fx->connection), AZ_IOT_OK);
  connect(fx);
  size_t index = 0;
  uint8_t const id = 1u;
  assert_int_equal(
      az_iot_delivery_tracker__submit(&shared, &k_resend, &id, 1u, 0u, &index), AZ_IOT_OK);
  uint16_t const packet_id = last_publish(fx)->packet_id;
  az_iot_delivery_tracker__unbind(&shared);
  memset(shared_entries, 0xA5, sizeof(shared_entries)); /* The owner reuses the storage. */
  puback(fx, packet_id, AZ_IOT_OK);
  (void)submit(fx, &k_resend, 2u, 0u);
  lose_session(fx);
  assert_int_equal(fx->completion_count, 0);
}

static az_iot_result encode_second(
    void* owner_context,
    size_t entry_index,
    az_iot_mqtt_message* out)
{
  (void)owner_context;
  (void)entry_index;
  out->topic = "second";
  out->qos = AZ_IOT_MQTT_QOS_1;
  return AZ_IOT_OK;
}

static void submit_to_first_on_complete(
    void* owner_context,
    size_t entry_index,
    az_iot_delivery_tracker_outcome outcome,
    const az_iot_publish_result* detail)
{
  (void)entry_index;
  (void)outcome;
  (void)detail;
  (void)submit((fixture*)owner_context, &k_resend, 9u, 0u);
}

static const az_iot_delivery_tracker_profile k_second = {
  "second",
  AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_PUBACK,
  AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_FAIL,
  0u,
  encode_second,
  submit_to_first_on_complete,
};

/* A submit from a later observer's TICK does not resend, in the same do_work(), a message a
 * PUBACK error requeued. */
static void a_later_observer_submit_does_not_resend_a_requeued_message(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_delivery_tracker second;
  az_iot_delivery_tracker_entry second_entries[1];
  assert_int_equal(
      az_iot_delivery_tracker__init(&second, second_entries, 1u, 0u, NULL, 0u, fx), AZ_IOT_OK);
  assert_int_equal(az_iot_delivery_tracker__bind(&second, &fx->connection), AZ_IOT_OK);
  size_t index = 0;
  uint8_t const id = 1u;
  assert_int_equal(
      az_iot_delivery_tracker__submit(&second, &k_second, &id, 1u, 60000u, &index), AZ_IOT_OK);
  connect(fx);
  (void)submit(fx, &k_resend, 1u, 0u);
  size_t const sent = publish_count(fx);
  assert_true(az_iot_mock_mqtt_client_inject_puback(
      fx->mock, last_publish(fx)->packet_id, AZ_IOT_ERR_MQTT));
  az_iot_deadline__expire_now(&second_entries[0]._internal.deadline);
  do_work(fx); /* The second tracker expires its message and submits t/1 to the first. */
  assert_int_equal(publish_count(fx), sent + 1u);
  assert_string_equal(last_publish(fx)->topic, "t/1");
  do_work(fx);
  assert_int_equal(publish_count(fx), sent + 2u);
  assert_string_equal(last_publish(fx)->topic, "t/0");
  az_iot_delivery_tracker__unbind(&second);
}

/* A message the adapter cannot send (AZ_IOT_ERR_INVALID_ARG, or AZ_IOT_ERR_NOT_ENOUGH_SPACE when
 * it is too large) is discarded; the ones after it still go, and the wait is not shortened. */
static void an_unsendable_message_is_discarded(void** state)
{
  fixture* fx = (fixture*)*state;
  size_t const unsendable = submit(fx, &k_resend, 1u, 0u);
  (void)submit(fx, &k_resend, 2u, 0u);
  fx->failing_sends = 1;
  fx->failing_send_result = AZ_IOT_ERR_INVALID_ARG;
  connect(fx);
  assert_int_equal(publish_count(fx), 2);
  assert_string_equal(last_publish(fx)->topic, "t/1");
  assert_int_equal(fx->completion_count, 1);
  assert_completed(fx, 0, unsendable, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DISCARDED);

  fx->failing_sends = 1;
  fx->failing_send_result = AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  size_t const too_large = submit(fx, &k_resend, 3u, 60000u);
  assert_completed(fx, 1, too_large, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DISCARDED);
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_true(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms > 1000u);
}

/* Any other send error refuses like BUSY: the wait is not shortened, and the TICK of the next
 * do_work() sends again, whether the error came at submit or at a TICK-time resend. */
static void a_send_error_is_retried_by_the_next_do_work(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  fx->failing_send_result = AZ_IOT_ERR_MQTT;
  fx->failing_sends = 1;
  (void)submit(fx, &k_resend, 1u, 60000u);
  assert_int_equal(publish_count(fx), 1);
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_true(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms > 1000u);
  assert_int_equal(publish_count(fx), 2);

  assert_true(az_iot_mock_mqtt_client_inject_puback(
      fx->mock, last_publish(fx)->packet_id, AZ_IOT_ERR_MQTT));
  do_work(fx);
  fx->failing_sends = 1; /* The TICK-time resend fails. */
  do_work(fx);
  assert_int_equal(publish_count(fx), 3);
  do_work(fx);
  assert_int_equal(publish_count(fx), 4);
  assert_int_equal(fx->completion_count, 0);
}

/* do_work() waits no longer than the earliest message time limit. */
static void the_wait_ends_by_the_earliest_time_limit(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  az_iot_mock_mqtt_client_set_next_result(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_BUSY);
  (void)submit(fx, &k_resend, 1u, 60000u);
  (void)submit(fx, &k_fail, 2u, 5000u);
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  uint32_t const waited
      = az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms;
  assert_true(waited <= 5000u && waited >= 4000u);
}

/* The do_work() after a PUBACK error requeued a message does not wait: its TICK resends it. */
static void a_requeued_message_does_not_wait_out_the_adapter(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  (void)submit(fx, &k_resend, 1u, 60000u);
  assert_true(az_iot_mock_mqtt_client_inject_puback(
      fx->mock, last_publish(fx)->packet_id, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_true(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms > 1000u);
  assert_int_equal(publish_count(fx), 1);
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_int_equal(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms, 0u);
  assert_int_equal(publish_count(fx), 2);
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_true(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms > 1000u);
}

/* Cancel and a response that free a PUBACK slot send the message queued behind them. */
static void freed_capacity_sends_the_queued_message(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  size_t const cancelled = submit(fx, &k_resend, 1u, 0u);
  (void)submit(fx, &k_response, 2u, 0u);
  (void)submit(fx, &k_resend, 3u, 0u);
  (void)submit(fx, &k_resend, 4u, 0u);
  assert_int_equal(publish_count(fx), RESERVED_PUBACKS);
  az_iot_delivery_tracker__cancel(&fx->tracker, cancelled);
  assert_int_equal(publish_count(fx), RESERVED_PUBACKS + 1u);
  assert_string_equal(last_publish(fx)->topic, "t/2");
  uint8_t const id = 2u;
  assert_int_equal(az_iot_delivery_tracker__complete(&fx->tracker, &id, 1u), AZ_IOT_OK);
  assert_int_equal(publish_count(fx), RESERVED_PUBACKS + 2u);
  assert_string_equal(last_publish(fx)->topic, "t/3");
}

/* An identity change discards only the messages held when it came: those an on_complete
 * submits for the new identity are kept, whatever entry they land in. */
static void an_identity_change_keeps_messages_submitted_during_it(void** state)
{
  fixture* fx = (fixture*)*state;
  (void)submit(fx, &k_resend, 1u, 0u);
  size_t const freed = submit(fx, &k_resend, 2u, 0u);
  (void)submit(fx, &k_resend, 3u, 0u);
  az_iot_delivery_tracker__cancel(&fx->tracker, freed);
  fx->submit_two_on_complete = &k_resend; /* Into entry 0 (just freed) and entry 1. */
  signal_identity_change(fx);
  assert_int_equal(fx->completion_count, 2);
  assert_non_null(fx->entries[0]._internal.profile);
  assert_non_null(fx->entries[1]._internal.profile);
  assert_null(fx->entries[2]._internal.profile);
}

static void ignore_publish_result(const az_iot_publish_result* result, void* user_ctx)
{
  (void)result;
  (void)user_ctx;
}

/* A PUBACK slot another tracker frees makes the next do_work() not wait, and its TICK sends
 * the message queued for it. After a refused send the wait is not shortened. */
static void a_slot_freed_elsewhere_is_used_without_waiting(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_delivery_tracker other;
  az_iot_delivery_tracker_entry other_entries[1];
  az_iot_delivery_tracker shared;
  az_iot_delivery_tracker_entry shared_entries[1];
  assert_int_equal(
      az_iot_delivery_tracker__init(&other, other_entries, 1u, 0u, NULL, 0u, fx), AZ_IOT_OK);
  assert_int_equal(az_iot_delivery_tracker__bind(&other, &fx->connection), AZ_IOT_OK);
  assert_int_equal(
      az_iot_delivery_tracker__init(&shared, shared_entries, 1u, 0u, NULL, 0u, fx), AZ_IOT_OK);
  assert_int_equal(az_iot_delivery_tracker__bind(&shared, &fx->connection), AZ_IOT_OK);
  connect(fx);
  az_iot_mqtt_message filler = { 0 };
  filler.topic = "filler";
  filler.qos = AZ_IOT_MQTT_QOS_1;
  while (az_iot_connection_client__can_track_publish(&fx->connection, &shared))
  {
    az_iot_publish_handle handle = { 0 };
    assert_int_equal(
        az_iot_connection_client__publish_with_result(
            &fx->connection, fx, &filler, ignore_publish_result, NULL, &handle),
        AZ_IOT_OK);
    if (!az_iot_connection_client__can_track_publish(&fx->connection, &shared))
    {
      az_iot_connection_client__forget_publish(&fx->connection, handle); /* Leave one. */
      break;
    }
  }
  size_t index = 0;
  uint8_t const id = 1u;
  assert_int_equal(
      az_iot_delivery_tracker__submit(&other, &k_resend, &id, 1u, 0u, &index), AZ_IOT_OK);
  size_t const sent = publish_count(fx);
  assert_int_equal(
      az_iot_delivery_tracker__submit(&shared, &k_resend, &id, 1u, 0u, &index), AZ_IOT_OK);
  assert_int_equal(publish_count(fx), sent); /* No slot left. */
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_true(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms > 1000u);

  az_iot_delivery_tracker__cancel(&other, 0u);
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_int_equal(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms, 0u);
  assert_int_equal(publish_count(fx), sent + 1u);

  /* A PUBACK error holds it back; another owner takes the slot, so it misses its first eligible
   * TICK; when that slot frees again, the next do_work() does not wait. */
  assert_true(az_iot_mock_mqtt_client_inject_puback(
      fx->mock, last_publish(fx)->packet_id, AZ_IOT_ERR_MQTT));
  do_work(fx);
  assert_int_equal(
      az_iot_delivery_tracker__submit(&other, &k_resend, &id, 1u, 0u, &index), AZ_IOT_OK);
  assert_int_equal(publish_count(fx), sent + 2u);
  do_work(fx);
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_true(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms > 1000u);
  assert_int_equal(publish_count(fx), sent + 2u);
  az_iot_delivery_tracker__cancel(&other, 0u);
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_int_equal(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms, 0u);
  assert_int_equal(publish_count(fx), sent + 3u);

  az_iot_delivery_tracker__cancel(&shared, 0u);
  fx->failing_sends = 1;
  fx->failing_send_result = AZ_IOT_ERR_BUSY;
  (void)submit(fx, &k_low_resend, 2u, 0u);
  (void)az_iot_connection_client_do_work(&fx->connection, UINT32_MAX);
  assert_true(
      az_iot_mock_mqtt_client_last_of(fx->mock, AZ_IOT_MOCK_CALL_PROCESS_LOOP)->timeout_ms > 1000u);
  az_iot_delivery_tracker__unbind(&shared);
  az_iot_delivery_tracker__unbind(&other);
}
/* A send the adapter refuses does not hold back the message a PUBACK error requeued: it goes out
 * from the next do_work(). */
static void a_busy_adapter_does_not_hold_back_a_requeued_message(void** state)
{
  fixture* fx = (fixture*)*state;
  connect(fx);
  (void)submit(fx, &k_resend, 1u, 0u);
  uint16_t const packet_id = last_publish(fx)->packet_id;
  fx->failing_sends = 1;
  (void)submit(fx, &k_resend, 2u, 0u);
  assert_true(az_iot_mock_mqtt_client_inject_puback(fx->mock, packet_id, AZ_IOT_ERR_MQTT));
  fx->failing_sends = 2; /* Both the PUBACK and the tick flush. */
  do_work(fx);
  assert_int_equal(fx->failing_sends, 0);
  size_t const before = publish_count(fx);
  do_work(fx);
  assert_int_equal(publish_count(fx), before + 2u);
  assert_int_equal(fx->completion_count, 0);
}
/* Arguments, a full tracker, an unbound one, and rebinding after unbind. */
static void arguments_capacity_and_binding(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_delivery_tracker other;
  assert_int_equal(
      az_iot_delivery_tracker__init(&other, NULL, 1u, 0u, NULL, 0u, NULL), AZ_IOT_ERR_INVALID_ARG);
  uint8_t const too_many[] = { 3u };
  assert_int_equal(
      az_iot_delivery_tracker__init(&other, fx->entries, 1u, 2u, too_many, 1u, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  size_t index = 0;
  uint8_t id[AZ_IOT_DELIVERY_TRACKER_CORRELATION_ID_MAX + 1u] = { 0 };
  assert_int_equal(
      az_iot_delivery_tracker__submit(&fx->tracker, &k_resend, id, sizeof(id), 0u, &index),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_delivery_tracker__submit(&fx->tracker, &k_response, NULL, 0u, 0u, &index),
      AZ_IOT_ERR_INVALID_ARG);

  for (uint8_t i = 0; i < ENTRY_COUNT; ++i)
  {
    (void)submit(fx, &k_resend, i, 0u);
  }
  assert_int_equal(
      az_iot_delivery_tracker__submit(&fx->tracker, &k_resend, id, 1u, 0u, &index),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  az_iot_connection_client other_connection;
  memset(&other_connection, 0, sizeof(other_connection));
  assert_int_equal(az_iot_delivery_tracker__bind(&fx->tracker, &fx->connection), AZ_IOT_OK);
  assert_int_equal(
      az_iot_delivery_tracker__bind(&fx->tracker, &other_connection),
      AZ_IOT_ERR_ALREADY_INITIALIZED);

  az_iot_delivery_tracker__unbind(&fx->tracker);
  assert_int_equal(
      az_iot_delivery_tracker__submit(&fx->tracker, &k_resend, id, 1u, 0u, &index),
      AZ_IOT_ERR_NOT_INITIALIZED);
  for (size_t i = 0; i < fx->connection.opts.limits.max_publish_reservations; ++i)
  {
    assert_ptr_not_equal(fx->connection.puback_reservations[i].owner, &fx->tracker);
  }
  assert_int_equal(az_iot_delivery_tracker__bind(&fx->tracker, &fx->connection), AZ_IOT_OK);
  assert_int_equal(fx->completion_count, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(a_message_is_delivered_at_its_puback, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_message_submitted_offline_is_sent_on_connected, setup, teardown),
    cmocka_unit_test_setup_teardown(qos0_messages_are_untracked, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_response_confirms_before_or_after_the_puback, setup, teardown),
    cmocka_unit_test_setup_teardown(a_refused_publish_is_not_resent, setup, teardown),
    cmocka_unit_test_setup_teardown(a_lost_session_resends_or_fails_per_policy, setup, teardown),
    cmocka_unit_test_setup_teardown(a_failed_puback_is_resent_from_a_later_event, setup, teardown),
    cmocka_unit_test_setup_teardown(messages_expire_at_their_limit, setup, teardown),
    cmocka_unit_test_setup_teardown(an_expired_message_is_not_sent, setup, teardown),
    cmocka_unit_test_setup_teardown(held_messages_go_out_by_priority_then_age, setup, teardown),
    cmocka_unit_test_setup_teardown(priority_zero_keeps_its_guaranteed_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(an_identity_change_discards_every_message, setup, teardown),
    cmocka_unit_test_setup_teardown(a_completion_may_unbind_the_tracker, setup, teardown),
    cmocka_unit_test_setup_teardown(a_completion_may_rebind_the_tracker, setup, teardown),
    cmocka_unit_test_setup_teardown(cancel_and_encode_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(unbinding_forgets_shared_pool_publishes, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_later_observer_submit_does_not_resend_a_requeued_message, setup, teardown),
    cmocka_unit_test_setup_teardown(an_unsendable_message_is_discarded, setup, teardown),
    cmocka_unit_test_setup_teardown(a_send_error_is_retried_by_the_next_do_work, setup, teardown),
    cmocka_unit_test_setup_teardown(the_wait_ends_by_the_earliest_time_limit, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_requeued_message_does_not_wait_out_the_adapter, setup, teardown),
    cmocka_unit_test_setup_teardown(freed_capacity_sends_the_queued_message, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_identity_change_keeps_messages_submitted_during_it, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_slot_freed_elsewhere_is_used_without_waiting, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_busy_adapter_does_not_hold_back_a_requeued_message, setup, teardown),
    cmocka_unit_test_setup_teardown(arguments_capacity_and_binding, setup, teardown),
  };
  return cmocka_run_group_tests_name("delivery_tracker", tests, NULL, NULL);
}
