// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file delivery_tracker.h
 * @brief Tracks SDK messages until confirmed: resends, expires or fails them.
 *
 * A feature client submits a message with a profile (how it is confirmed, what a lost session
 * does to it, its priority) and an optional time limit. The tracker publishes it through the
 * bound connection when it can, keeps it until a PUBACK or a matching response confirms it, and
 * reports the outcome once through the profile's on_complete. It flushes queued messages on
 * CONNECTED, after any hub PUBACK and on each do_work() tick, and expires them on the tick.
 *
 * The payload is not copied: the profile's encode() rebuilds the message from the owner's state
 * at each send. Storage is the caller's; nothing is allocated. RAM only.
 */
#ifndef AZ_IOT_DELIVERY_TRACKER_INTERNAL_H
#define AZ_IOT_DELIVERY_TRACKER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_delivery_tracker.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief After a refused send (BUSY, a transport error), do_work() waits at most this long, or
 *        half a queued message's remaining time limit if that is shorter.
 */
#define AZ_IOT_DELIVERY_TRACKER_REFUSED_SEND_RETRY_MS 1000u

  /** @brief How a message is confirmed. */
  typedef enum az_iot_delivery_tracker_confirmation
  {
    AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_PUBACK = 1, /**< At PUBACK; QoS 0 at send. */
    /** By az_iot_delivery_tracker__complete() with its correlation id, before or after PUBACK. */
    AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_RESPONSE = 2,
  } az_iot_delivery_tracker_confirmation;

  /** @brief What an ended session does to a message sent on it and not yet confirmed. */
  typedef enum az_iot_delivery_tracker_session_loss_policy
  {
    AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_FAIL = 1, /**< Completes as SESSION_LOST. */
    AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_RESEND = 2, /**< Sent again on a later session. */
  } az_iot_delivery_tracker_session_loss_policy;

  /** @brief Final outcome of a tracked message. */
  typedef enum az_iot_delivery_tracker_outcome
  {
    AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED, /**< PUBACK success, or its response. */
    AZ_IOT_DELIVERY_TRACKER_OUTCOME_REFUSED, /**< The hub refused the PUBLISH. */
    AZ_IOT_DELIVERY_TRACKER_OUTCOME_EXPIRED, /**< Its time limit passed. */
    AZ_IOT_DELIVERY_TRACKER_OUTCOME_SESSION_LOST, /**< Policy FAIL or DROP. */
    /** Not sendable (encode() failed, or the adapter returned AZ_IOT_ERR_INVALID_ARG or
     * AZ_IOT_ERR_NOT_ENOUGH_SPACE), or the device identity changed. */
    AZ_IOT_DELIVERY_TRACKER_OUTCOME_DISCARDED,
  } az_iot_delivery_tracker_outcome;

  /** @brief How one kind of message is sent and confirmed; const, shared by its messages. */
  typedef struct az_iot_delivery_tracker_profile
  {
    const char* type; /**< For logs. */
    az_iot_delivery_tracker_confirmation confirmation;
    az_iot_delivery_tracker_session_loss_policy on_session_loss;
    uint8_t priority; /**< 0 first: orders sends and PUBACK slots. */
    /** With no PUBACK slot for it, sent once untracked (completes as DELIVERED) instead of held. */
    bool send_untracked_without_slot;
    /**
     * @brief Fill @p out for entry @p entry_index; called at each send.
     * @return AZ_IOT_OK; AZ_IOT_ERR_TIMEOUT when the owner's own deadline has passed (the message
     *         completes as EXPIRED); any other error discards the message.
     */
    az_iot_result (*encode)(void* owner_context, size_t entry_index, az_iot_mqtt_message* out);
    /**
     * @brief The message ended with @p outcome; the entry is free again.
     * @param detail The PUBACK result for DELIVERED/REFUSED/SESSION_LOST at PUBACK; else NULL.
     */
    void (*on_complete)(
        void* owner_context,
        size_t entry_index,
        az_iot_delivery_tracker_outcome outcome,
        const az_iot_publish_result* detail);
    /**
     * @brief Optional: entry @p entry_index was just published (it may be sent again). Only record
     *        it: no tracker call.
     */
    void (*on_sent)(void* owner_context, size_t entry_index);
  } az_iot_delivery_tracker_profile;

  /**
   * @brief Set up @p tracker over caller-owned @p entries.
   *
   * @param reserved_pubacks PUBACK slots reserved for this tracker at bind; 0 for none.
   * @param guaranteed_slots_by_priority Slots kept for each priority level (index = priority),
   *        summing to at most @p reserved_pubacks; NULL when @p priority_levels is 0.
   * @retval AZ_IOT_ERR_INVALID_ARG A NULL pointer, 0 capacity or guarantees over the reservation.
   */
  az_iot_result az_iot_delivery_tracker__init(
      az_iot_delivery_tracker* tracker,
      az_iot_delivery_tracker_entry* entries,
      size_t capacity,
      size_t reserved_pubacks,
      const uint8_t* guaranteed_slots_by_priority,
      size_t priority_levels,
      void* owner_context);

  /**
   * @brief Reserve the PUBACK slots and observe @p connection.
   * @return AZ_IOT_OK (also when already bound to @p connection), AZ_IOT_ERR_ALREADY_INITIALIZED
   *         when bound to another connection, or the error of the reservation or observer
   *         registration (nothing kept).
   */
  az_iot_result az_iot_delivery_tracker__bind(
      az_iot_delivery_tracker* tracker,
      az_iot_connection_client* connection);

  /** @brief Withdraw from the connection and drop every entry without completing it. */
  void az_iot_delivery_tracker__unbind(az_iot_delivery_tracker* tracker);

  /**
   * @brief Track a message and send it as soon as it can be.
   *
   * on_complete may run before this returns (a QoS 0 message confirmed by PUBACK, or one
   * encode() refuses).
   *
   * @param timeout_ms Time limit from now; 0 uses the connection's
   *        max_unconfirmed_delivery_timeout_ms.
   * @param correlation_id Required (1 to AZ_IOT_DELIVERY_TRACKER_CORRELATION_ID_MAX bytes) when
   *        the profile confirms by response.
   * @param[out] out_entry_index The entry the message holds.
   * @retval AZ_IOT_ERR_NOT_INITIALIZED Not bound.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE Every entry is in use.
   */
  az_iot_result az_iot_delivery_tracker__submit(
      az_iot_delivery_tracker* tracker,
      const az_iot_delivery_tracker_profile* profile,
      const uint8_t* correlation_id,
      size_t correlation_id_length,
      uint64_t timeout_ms,
      size_t* out_entry_index);

  /**
   * @brief A response matched @p correlation_id: complete its message as DELIVERED. Queued
   *        messages the freed capacity lets go are sent.
   * @retval AZ_IOT_ERR_NOT_FOUND No tracked message has it.
   */
  az_iot_result az_iot_delivery_tracker__complete(
      az_iot_delivery_tracker* tracker,
      const uint8_t* correlation_id,
      size_t correlation_id_length);

  /**
   * @brief Drop entry @p entry_index without completing it; its late PUBACK is ignored. Queued
   *        messages the freed capacity lets go are sent.
   */
  void az_iot_delivery_tracker__cancel(az_iot_delivery_tracker* tracker, size_t entry_index);

  /**
   * @brief Complete as EXPIRED every message whose time limit has passed, as the next TICK would.
   *        For an owner about to need the entries back.
   */
  void az_iot_delivery_tracker__expire_overdue(az_iot_delivery_tracker* tracker);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DELIVERY_TRACKER_INTERNAL_H */
