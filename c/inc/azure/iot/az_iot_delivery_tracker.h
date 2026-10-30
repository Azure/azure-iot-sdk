// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file az_iot_delivery_tracker.h
 * @brief Storage of the SDK's outbound delivery tracker, embedded in feature clients.
 *
 * Fields are internal and not for application use.
 */
#ifndef AZ_IOT_DELIVERY_TRACKER_H
#define AZ_IOT_DELIVERY_TRACKER_H

#include <stddef.h>
#include <stdint.h>

#include "az_iot_deadline.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Longest correlation id a tracked message can carry, in bytes. */
#define AZ_IOT_DELIVERY_TRACKER_CORRELATION_ID_MAX 16u

  struct az_iot_delivery_tracker;
  struct az_iot_delivery_tracker_profile;
  struct az_iot_connection_client;

  /** @brief One message tracked until it is confirmed, refused, expired or dropped. */
  typedef struct az_iot_delivery_tracker_entry
  {
    struct
    {
      struct az_iot_delivery_tracker* tracker;
      const struct az_iot_delivery_tracker_profile* profile; /**< NULL: entry free. */
      az_iot_deadline deadline;
      uint64_t sequence; /**< Submit order. */
      uint64_t publish_id; /**< Tracked publish awaiting its PUBACK; 0: none. */
      size_t publish_slot;
      uint8_t correlation_id[AZ_IOT_DELIVERY_TRACKER_CORRELATION_ID_MAX];
      uint8_t correlation_id_length;
      uint8_t state;
      uint32_t identity; /**< The connection's identity_changes at submit. */
      uint8_t held_for_resend; /**< Requeued by a PUBACK error or session loss. */
      uint32_t held_at_tick; /**< tick_count when held; paces the resend. */
    } _internal;
  } az_iot_delivery_tracker_entry;

  /** @brief A feature client's tracker: its entries and its PUBACK reservation. */
  typedef struct az_iot_delivery_tracker
  {
    struct
    {
      struct az_iot_connection_client* connection; /**< NULL: not bound. */
      az_iot_delivery_tracker_entry* entries;
      size_t capacity;
      size_t reserved_pubacks;
      const uint8_t* guaranteed_slots_by_priority;
      size_t priority_levels;
      void* owner_context;
      uint64_t next_sequence;
      uint32_t tick_count; /**< TICK events seen; paces resends. */
      uint32_t binding_id; /**< The connection's observer registration count at bind(). */
      uint8_t send_refused; /**< The last flush stopped on NOT_CONNECTED or BUSY. */
    } _internal;
  } az_iot_delivery_tracker;

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DELIVERY_TRACKER_H */
