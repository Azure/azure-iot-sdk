// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "internal/delivery_tracker.h"

#include <stdbool.h>
#include <string.h>

#include "internal/connection_client_internal.h"
#include "internal/deadline.h"
#include "internal/log_internal.h"
#include "internal/mono_time.h"

#define TI(t) ((t)->_internal)
#define EI(e) ((e)->_internal)

enum
{
  ENTRY_FREE = 0,
  ENTRY_QUEUED, /**< Waiting to be sent. */
  ENTRY_IN_FLIGHT, /**< Sent tracked; waiting for its PUBACK. */
  ENTRY_AWAITING_RESPONSE, /**< Sent; waiting for az_iot_delivery_tracker__complete(). */
};

/** @brief Result of try_send() for an entry with no PUBACK slot it may take now. */
#define NO_SLOT_FOR_PRIORITY AZ_IOT_ERR_NOT_ENOUGH_SPACE

static void on_connection_event(const az_iot_connection_feature_event* event, void* user_ctx);

static size_t index_of(const az_iot_delivery_tracker* t, const az_iot_delivery_tracker_entry* e)
{
  return (size_t)(e - TI(t).entries);
}

/** @brief A tracker binding, as seen before a walk that calls on_complete. */
typedef struct binding
{
  az_iot_connection_client* connection;
  uint32_t id; /**< Tells a rebind to the same connection apart, even after init(). */
} binding;

static binding binding_of(const az_iot_delivery_tracker* t)
{
  binding const b = { TI(t).connection, TI(t).binding_id };
  return b;
}

/**
 * @brief Whether @p t still has binding @p b; an on_complete may have unbound (and freed) or
 *        rebound it. The observer check comes first: it does not read @p t.
 */
static bool still_bound(const az_iot_delivery_tracker* t, binding b)
{
  return az_iot_connection_client__has_feature_observer(b.connection, on_connection_event, t)
      && TI(t).binding_id == b.id;
}

/** @brief Forget the entry's tracked publish, so its PUBACK reaches nobody. */
static void forget_publish(az_iot_delivery_tracker_entry* e)
{
  az_iot_delivery_tracker* t = EI(e).tracker;
  if (EI(e).publish_id != 0u && TI(t).connection != NULL)
  {
    az_iot_publish_handle const handle = { EI(e).publish_slot, EI(e).publish_id };
    az_iot_connection_client__forget_publish(TI(t).connection, handle);
  }
  EI(e).publish_id = 0u;
}

/** @brief Free the entry, then report @p outcome to its owner. */
static void finish(
    az_iot_delivery_tracker_entry* e,
    az_iot_delivery_tracker_outcome outcome,
    const az_iot_publish_result* detail)
{
  az_iot_delivery_tracker* t = EI(e).tracker;
  const az_iot_delivery_tracker_profile* profile = EI(e).profile;
  forget_publish(e);
  memset(e, 0, sizeof(*e));
  AZ_IOT_LOG_DEBUGF(
      AZ_IOT_LOG_COMPONENT_CONNECTION, "'%s' message done (%d)", profile->type, (int)outcome);
  profile->on_complete(TI(t).owner_context, index_of(t, e), outcome, detail);
}

/** @brief Keep @p e queued but unsent until the TICK after next (see held_back()). */
static void hold_for_resend(az_iot_delivery_tracker_entry* e)
{
  EI(e).held_for_resend = 1u;
  EI(e).held_at_tick = TI(EI(e).tracker).tick_count;
}

/** @brief A message not confirmed on the session that ended: requeue or complete per policy. */
static void lose(az_iot_delivery_tracker_entry* e, const az_iot_publish_result* detail)
{
  EI(e).publish_id = 0u;
  if (EI(e).profile->on_session_loss == AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_RESEND)
  {
    EI(e).state = ENTRY_QUEUED;
    hold_for_resend(e);
    return;
  }
  finish(e, AZ_IOT_DELIVERY_TRACKER_OUTCOME_SESSION_LOST, detail);
}

static void on_publish_result(const az_iot_publish_result* result, void* user_ctx)
{
  az_iot_delivery_tracker_entry* e = (az_iot_delivery_tracker_entry*)user_ctx;
  EI(e).publish_id = 0u;
  if (result->status == AZ_IOT_OK)
  {
    if (EI(e).profile->confirmation == AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_RESPONSE)
    {
      EI(e).state = ENTRY_AWAITING_RESPONSE;
    }
    else
    {
      finish(e, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED, result);
    }
  }
  else if (result->status == AZ_IOT_ERR_PUBLISH_REFUSED)
  {
    finish(e, AZ_IOT_DELIVERY_TRACKER_OUTCOME_REFUSED, result);
  }
  else
  {
    lose(e, result);
  }
}

/**
 * @brief Whether a priority @p priority message may take a PUBACK slot now: one must be free, and
 *        taking it must leave every higher priority (lower number) its unused guarantee.
 */
static bool may_take_slot(const az_iot_delivery_tracker* t, uint8_t priority)
{
  if (!az_iot_connection_client__can_track_publish(TI(t).connection, t))
  {
    return false;
  }
  size_t held_total = 0;
  size_t owed = 0;
  for (size_t level = 0; level < TI(t).priority_levels; ++level)
  {
    size_t held = 0;
    for (size_t i = 0; i < TI(t).capacity; ++i)
    {
      const az_iot_delivery_tracker_entry* e = &TI(t).entries[i];
      held += (EI(e).publish_id != 0u && EI(e).profile->priority == level) ? 1u : 0u;
    }
    if (level < priority && held < TI(t).guaranteed_slots_by_priority[level])
    {
      owed += TI(t).guaranteed_slots_by_priority[level] - held;
    }
  }
  for (size_t i = 0; i < TI(t).capacity; ++i)
  {
    held_total += (EI(&TI(t).entries[i]).publish_id != 0u) ? 1u : 0u;
  }
  return owed == 0u || held_total + 1u + owed <= TI(t).reserved_pubacks;
}

/**
 * @brief Encode and publish @p e.
 * @retval NO_SLOT_FOR_PRIORITY No PUBACK slot it may take; stays queued.
 * @return Otherwise the publish result; any failure leaves it queued.
 */
static az_iot_result try_send(az_iot_delivery_tracker* t, az_iot_delivery_tracker_entry* e)
{
  const az_iot_delivery_tracker_profile* profile = EI(e).profile;
  az_iot_mqtt_message message = { 0 };
  az_iot_result result = profile->encode(TI(t).owner_context, index_of(t, e), &message);
  if (result != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "could not encode a '%s' message (%s); dropping it",
        profile->type,
        az_iot_result_to_string(result));
    finish(e, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DISCARDED, NULL);
    return AZ_IOT_OK;
  }
  bool track = message.qos != AZ_IOT_MQTT_QOS_0;
  if (track && !may_take_slot(t, profile->priority))
  {
    if (!profile->send_untracked_without_slot)
    {
      return NO_SLOT_FOR_PRIORITY;
    }
    track = false; /* Best effort: sent once, untracked. */
  }
  az_iot_publish_handle handle = { 0 };
  result = az_iot_connection_client__publish_with_result(
      TI(t).connection, t, &message, track ? on_publish_result : NULL, track ? e : NULL, &handle);
  /* INVALID_ARG, or NOT_ENOUGH_SPACE (az_mqtt: too large to send), is final for this message. */
  if (result == AZ_IOT_ERR_INVALID_ARG || result == AZ_IOT_ERR_NOT_ENOUGH_SPACE)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "the adapter cannot send a '%s' message (%s); dropping it",
        profile->type,
        az_iot_result_to_string(result));
    finish(e, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DISCARDED, NULL);
    return AZ_IOT_OK;
  }
  if (result != AZ_IOT_OK)
  {
    return result;
  }
  if (profile->on_sent != NULL)
  {
    profile->on_sent(TI(t).owner_context, index_of(t, e));
  }
  if (track)
  {
    EI(e).state = ENTRY_IN_FLIGHT;
    EI(e).held_for_resend = 0u;
    EI(e).publish_slot = handle.slot;
    EI(e).publish_id = handle.id;
  }
  else if (profile->confirmation == AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_RESPONSE)
  {
    EI(e).state = ENTRY_AWAITING_RESPONSE;
  }
  else
  {
    finish(e, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED, NULL);
  }
  return AZ_IOT_OK;
}

static bool has_expired(az_iot_delivery_tracker_entry* e)
{
  return az_iot_deadline__has_passed(&EI(e).deadline, az_iot_time_mono_ms());
}

static void expire(az_iot_delivery_tracker_entry* e)
{
  finish(e, AZ_IOT_DELIVERY_TRACKER_OUTCOME_EXPIRED, NULL);
}

/** @brief Whether @p e comes after the (@p priority, @p sequence) cursor in send order. */
static bool sends_after(const az_iot_delivery_tracker_entry* e, uint8_t priority, uint64_t sequence)
{
  return EI(e).profile->priority > priority
      || (EI(e).profile->priority == priority && EI(e).sequence > sequence);
}

/**
 * @brief Whether a message a PUBACK error requeued still waits: it goes out from the TICK of the
 *        do_work() after the one that requeued it, never earlier, whatever observer runs later.
 */
static bool held_back(const az_iot_delivery_tracker* t, const az_iot_delivery_tracker_entry* e)
{
  return EI(e).held_for_resend != 0u && (uint32_t)(TI(t).tick_count - EI(e).held_at_tick) < 2u;
}

/**
 * @brief Send queued messages by priority, then submit order, until the connection refuses
 *        one (any send error but INVALID_ARG or NOT_ENOUGH_SPACE, which discard that message).
 * @param skip_requeued Leave out the messages held_back() holds.
 */
static void flush(az_iot_delivery_tracker* t, bool skip_requeued)
{
  az_iot_connection_client* const connection = TI(t).connection;
  binding const bound = binding_of(t);
  TI(t).send_refused = 0u;
  uint8_t priority = 0;
  uint64_t sequence = 0; /* Sequences start at 1, so (0, 0) precedes every entry. */
  while (az_iot_connection_client__is_connected(connection))
  {
    az_iot_delivery_tracker_entry* candidate = NULL;
    for (size_t i = 0; i < TI(t).capacity; ++i)
    {
      az_iot_delivery_tracker_entry* e = &TI(t).entries[i];
      if (EI(e).state == ENTRY_QUEUED && sends_after(e, priority, sequence)
          && (candidate == NULL
              || !sends_after(e, EI(candidate).profile->priority, EI(candidate).sequence)))
      {
        candidate = e;
      }
    }
    if (candidate == NULL)
    {
      break;
    }
    priority = EI(candidate).profile->priority;
    sequence = EI(candidate).sequence;
    if (skip_requeued && held_back(t, candidate))
    {
      continue;
    }
    az_iot_result result = AZ_IOT_OK;
    if (has_expired(candidate))
    {
      expire(candidate);
    }
    else
    {
      result = try_send(t, candidate);
    }
    if (!still_bound(t, bound))
    {
      return;
    }
    if (result != AZ_IOT_OK && result != NO_SLOT_FOR_PRIORITY)
    {
      TI(t).send_refused = 1u;
      break; /* The connection refused; the rest waits for a later event. */
    }
    if (TI(t).send_refused != 0u)
    {
      break; /* A flush nested in an on_complete was refused. */
    }
  }
}

/** @brief Apply @p apply to each entry in @p state (ENTRY_FREE: any used one) that @p selected
 * accepts (NULL: all); stops if a completion unbinds the tracker. */
static void for_each_entry(
    az_iot_delivery_tracker* t,
    uint8_t state,
    bool (*selected)(az_iot_delivery_tracker_entry* e),
    void (*apply)(az_iot_delivery_tracker_entry* e))
{
  binding const bound = binding_of(t);
  uint64_t const submitted_before = TI(t).next_sequence; /* Not what callbacks submit now. */
  for (size_t i = 0; i < TI(t).capacity; ++i)
  {
    az_iot_delivery_tracker_entry* e = &TI(t).entries[i];
    if (EI(e).state != ENTRY_FREE && EI(e).sequence < submitted_before
        && (state == ENTRY_FREE || EI(e).state == state) && (selected == NULL || selected(e)))
    {
      apply(e);
      if (!still_bound(t, bound))
      {
        return;
      }
    }
  }
}

/** @brief Whether @p e was submitted before the latest identity change. */
static bool is_for_a_previous_identity(az_iot_delivery_tracker_entry* e)
{
  return EI(e).identity != TI(EI(e).tracker).connection->identity_changes;
}

static void lose_without_detail(az_iot_delivery_tracker_entry* e) { lose(e, NULL); }

static void discard(az_iot_delivery_tracker_entry* e)
{
  finish(e, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DISCARDED, NULL);
}

/**
 * @brief Lower @p remaining_ms to the earliest message time limit, or to 0 when this
 *        do_work()'s TICK can send a queued message: a held back one becomes sendable, or a
 *        PUBACK slot freed since the last flush. Not after a refused flush: events resume it.
 */
static void lower_remaining_to_next_action(const az_iot_delivery_tracker* t, uint64_t* remaining_ms)
{
  uint64_t const now = az_iot_time_mono_ms();
  bool const can_send_now
      = TI(t).send_refused == 0u && az_iot_connection_client__is_connected(TI(t).connection);
  for (size_t i = 0; i < TI(t).capacity; ++i)
  {
    const az_iot_delivery_tracker_entry* e = &TI(t).entries[i];
    uint64_t left = EI(e).state == ENTRY_FREE ? UINT64_MAX
                                              : az_iot_deadline__remaining_ms(&EI(e).deadline, now);
    /* A held back message is sendable from the TICK where held_back() turns false. */
    if (EI(e).state == ENTRY_QUEUED && can_send_now
        && (EI(e).held_for_resend == 0u || TI(t).tick_count != EI(e).held_at_tick)
        && may_take_slot(t, EI(e).profile->priority))
    {
      left = 0u;
    }
    *remaining_ms = left < *remaining_ms ? left : *remaining_ms;
  }
}

static void on_connection_event(const az_iot_connection_feature_event* event, void* user_ctx)
{
  az_iot_delivery_tracker* t = (az_iot_delivery_tracker*)user_ctx;
  binding const bound = binding_of(t);
  switch (event->kind)
  {
    case AZ_IOT_CONNECTION_FEATURE_EVENT_STATE:
      if (event->state->scope == AZ_IOT_CONN_SCOPE_HUB
          && event->state->state == AZ_IOT_CONN_STATE_CONNECTED)
      {
        flush(t, false);
      }
      break;
    case AZ_IOT_CONNECTION_FEATURE_EVENT_PUBLISH_COMPLETED:
      flush(t, true);
      break;
    case AZ_IOT_CONNECTION_FEATURE_EVENT_TICK:
      for_each_entry(t, ENTRY_FREE, has_expired, expire);
      if (still_bound(t, bound))
      {
        ++TI(t).tick_count;
        flush(t, true);
      }
      break;
    case AZ_IOT_CONNECTION_FEATURE_EVENT_SESSION_END:
      /* Tracked publishes were already completed with NOT_CONNECTED by the teardown. */
      for_each_entry(t, ENTRY_AWAITING_RESPONSE, NULL, lose_without_detail);
      break;
    case AZ_IOT_CONNECTION_FEATURE_EVENT_IDENTITY_CHANGED:
      for_each_entry(t, ENTRY_FREE, is_for_a_previous_identity, discard);
      break;
    case AZ_IOT_CONNECTION_FEATURE_EVENT_NEXT_DEADLINE:
      lower_remaining_to_next_action(t, event->remaining_ms);
      break;
  }
}

az_iot_result az_iot_delivery_tracker__init(
    az_iot_delivery_tracker* tracker,
    az_iot_delivery_tracker_entry* entries,
    size_t capacity,
    size_t reserved_pubacks,
    const uint8_t* guaranteed_slots_by_priority,
    size_t priority_levels,
    void* owner_context)
{
  if (tracker == NULL || entries == NULL || capacity == 0u
      || (priority_levels != 0u && guaranteed_slots_by_priority == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t guaranteed = 0;
  for (size_t level = 0; level < priority_levels; ++level)
  {
    guaranteed += guaranteed_slots_by_priority[level];
  }
  if (guaranteed > reserved_pubacks)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(tracker, 0, sizeof(*tracker));
  memset(entries, 0, capacity * sizeof(*entries));
  TI(tracker).entries = entries;
  TI(tracker).capacity = capacity;
  TI(tracker).reserved_pubacks = reserved_pubacks;
  TI(tracker).guaranteed_slots_by_priority = guaranteed_slots_by_priority;
  TI(tracker).priority_levels = priority_levels;
  TI(tracker).owner_context = owner_context;
  TI(tracker).next_sequence = 1u;
  return AZ_IOT_OK;
}

az_iot_result az_iot_delivery_tracker__bind(
    az_iot_delivery_tracker* tracker,
    az_iot_connection_client* connection)
{
  if (tracker == NULL || connection == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (TI(tracker).connection != NULL)
  {
    return TI(tracker).connection == connection ? AZ_IOT_OK : AZ_IOT_ERR_ALREADY_INITIALIZED;
  }
  az_iot_result result = AZ_IOT_OK;
  if (TI(tracker).reserved_pubacks != 0u)
  {
    result = az_iot_connection_client__reserve_pubacks(
        connection, tracker, TI(tracker).reserved_pubacks);
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__add_feature_observer(
        connection,
        AZ_IOT_CONNECTION_FEATURE_EVENT_STATE | AZ_IOT_CONNECTION_FEATURE_EVENT_SESSION_END
            | AZ_IOT_CONNECTION_FEATURE_EVENT_PUBLISH_COMPLETED
            | AZ_IOT_CONNECTION_FEATURE_EVENT_TICK
            | AZ_IOT_CONNECTION_FEATURE_EVENT_IDENTITY_CHANGED
            | AZ_IOT_CONNECTION_FEATURE_EVENT_NEXT_DEADLINE,
        on_connection_event,
        tracker);
    if (result != AZ_IOT_OK)
    {
      az_iot_connection_client__release_pubacks(connection, tracker);
    }
  }
  if (result == AZ_IOT_OK)
  {
    TI(tracker).connection = connection;
    TI(tracker).binding_id = connection->feature_observer_registrations;
  }
  return result;
}

void az_iot_delivery_tracker__unbind(az_iot_delivery_tracker* tracker)
{
  if (tracker == NULL || TI(tracker).connection == NULL)
  {
    return;
  }
  az_iot_connection_client* const connection = TI(tracker).connection;
  az_iot_connection_client__remove_feature_observer(connection, on_connection_event, tracker);
  /* Pending PUBACK callbacks point into the entries, reserved and shared-pool ones alike. */
  for (size_t i = 0; i < TI(tracker).capacity; ++i)
  {
    forget_publish(&TI(tracker).entries[i]);
  }
  az_iot_connection_client__release_pubacks(connection, tracker);
  memset(TI(tracker).entries, 0, TI(tracker).capacity * sizeof(*TI(tracker).entries));
  TI(tracker).connection = NULL;
}

az_iot_result az_iot_delivery_tracker__submit(
    az_iot_delivery_tracker* tracker,
    const az_iot_delivery_tracker_profile* profile,
    const uint8_t* correlation_id,
    size_t correlation_id_length,
    uint32_t timeout_ms,
    size_t* out_entry_index)
{
  if (tracker == NULL || profile == NULL || profile->encode == NULL || profile->on_complete == NULL
      || out_entry_index == NULL
      || correlation_id_length > AZ_IOT_DELIVERY_TRACKER_CORRELATION_ID_MAX
      || (correlation_id == NULL && correlation_id_length != 0u)
      || (profile->confirmation == AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_RESPONSE
          && correlation_id_length == 0u))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (TI(tracker).connection == NULL)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  az_iot_delivery_tracker_entry* e = NULL;
  for (size_t i = 0; i < TI(tracker).capacity && e == NULL; ++i)
  {
    e = (EI(&TI(tracker).entries[i]).state == ENTRY_FREE) ? &TI(tracker).entries[i] : NULL;
  }
  if (e == NULL)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  EI(e).tracker = tracker;
  EI(e).profile = profile;
  EI(e).sequence = TI(tracker).next_sequence++;
  EI(e).identity = TI(tracker).connection->identity_changes;
  if (correlation_id_length != 0u)
  {
    memcpy(EI(e).correlation_id, correlation_id, correlation_id_length);
  }
  EI(e).correlation_id_length = (uint8_t)correlation_id_length;
  az_iot_deadline__arm_after_ms(
      &EI(e).deadline,
      az_iot_time_mono_ms(),
      timeout_ms != 0u ? timeout_ms
                       : TI(tracker).connection->opts.max_unconfirmed_delivery_timeout_ms);
  EI(e).state = ENTRY_QUEUED;
  *out_entry_index = index_of(tracker, e);
  flush(tracker, true);
  return AZ_IOT_OK;
}

/** @brief Send what a freed entry or PUBACK slot now lets go, unless @p t lost binding @p b. */
static void flush_if_still_bound(az_iot_delivery_tracker* t, binding b)
{
  if (still_bound(t, b))
  {
    flush(t, true);
  }
}

az_iot_result az_iot_delivery_tracker__complete(
    az_iot_delivery_tracker* tracker,
    const uint8_t* correlation_id,
    size_t correlation_id_length)
{
  if (tracker == NULL || correlation_id == NULL || correlation_id_length == 0u)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  binding const bound = binding_of(tracker);
  for (size_t i = 0; i < TI(tracker).capacity; ++i)
  {
    az_iot_delivery_tracker_entry* e = &TI(tracker).entries[i];
    if (EI(e).state != ENTRY_FREE
        && EI(e).profile->confirmation == AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_RESPONSE
        && EI(e).correlation_id_length == correlation_id_length
        && memcmp(EI(e).correlation_id, correlation_id, correlation_id_length) == 0)
    {
      finish(e, AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED, NULL);
      flush_if_still_bound(tracker, bound);
      return AZ_IOT_OK;
    }
  }
  return AZ_IOT_ERR_NOT_FOUND;
}

void az_iot_delivery_tracker__cancel(az_iot_delivery_tracker* tracker, size_t entry_index)
{
  if (tracker == NULL || entry_index >= TI(tracker).capacity)
  {
    return;
  }
  az_iot_delivery_tracker_entry* e = &TI(tracker).entries[entry_index];
  if (EI(e).state != ENTRY_FREE)
  {
    forget_publish(e);
    memset(e, 0, sizeof(*e));
    flush_if_still_bound(tracker, binding_of(tracker));
  }
}

void az_iot_delivery_tracker__expire_due(az_iot_delivery_tracker* tracker)
{
  if (tracker != NULL && TI(tracker).connection != NULL)
  {
    for_each_entry(tracker, ENTRY_FREE, has_expired, expire);
  }
}
