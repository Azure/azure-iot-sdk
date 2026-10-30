// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv5 direct method replies on the delivery tracker.
 *
 * Probe-acks and results (priority 0) and notices -- probe rejections and abandons (priority 1)
 * -- are tracker entries. Their frames are encoded once and kept in the client; encode() builds
 * the envelope with the Message Expiry left at each send.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv5/az_iot_direct_method_client.h"

#include "internal/connection_client_internal.h"
#include "internal/delivery_tracker.h"
#include "internal/direct_method_budget.h"
#include "internal/direct_method_codec.h"
#include "internal/direct_method_delivery.h"
#include "internal/log_internal.h"
#include "internal/mono_time.h"

#define DI(d) ((d)->_internal)
#define RI(r) ((r)->_internal)

#define DM_CONTENT_TYPE "application/protobuf"
#define DM_TYPE_PROBE_ACK "probe-ack:" AZ_IOT_MQTTV5_DM_TYPE_VERSION
#define DM_TYPE_ABANDON "abandon:" AZ_IOT_MQTTV5_DM_TYPE_VERSION
#define DM_TYPE_RESULT "result:" AZ_IOT_MQTTV5_DM_TYPE_VERSION

#define DM_NO_TRACKER_ENTRY AZ_IOT_MQTTV5_DM_NO_TRACKER_ENTRY
/* Being submitted: claims the entry the tracker first asks about (see
 * element_holds_tracker_entry()). */
#define DM_TRACKER_ENTRY_PENDING SIZE_MAX

/* k_slots_kept_for_replies holds the limit in a byte. */
typedef char
    az_iot_dm_concurrency_fits_a_byte[(AZ_IOT_MQTTV5_DM_MAX_CONCURRENT <= UINT8_MAX) ? 1 : -1];

/* A held notice stores the encoded frame; the public header sizes it without seeing the codec. */
typedef char az_iot_dm_notice_frame_fits
    [(AZ_IOT_MQTTV5_DM_NOTICE_FRAME_MAX >= AZ_IOT_DM_PROTO_PROBE_ACK_MAX
      && AZ_IOT_MQTTV5_DM_NOTICE_FRAME_MAX >= AZ_IOT_DM_PROTO_ABANDON_MAX)
         ? 1
         : -1];

/* The `type` user property of each device-to-service phase. Static: the tracker publishes the
 * message encode() fills after encode() returns. */
static const az_iot_mqtt_user_property k_probe_ack_type = { "type", DM_TYPE_PROBE_ACK };
static const az_iot_mqtt_user_property k_abandon_type = { "type", DM_TYPE_ABANDON };
static const az_iot_mqtt_user_property k_result_type = { "type", DM_TYPE_RESULT };

/** @brief Fill @p out with a device-to-service message; every phase shares this envelope. */
static az_iot_result build_reply_message(
    const az_iot_mqttv5_direct_method_client* dm,
    const az_iot_mqtt_user_property* type,
    const uint8_t* payload,
    size_t payload_len,
    const uint8_t* request_id,
    uint32_t expiry_seconds,
    az_iot_mqtt_message* out)
{
  if (DI(dm).outbound_topic[0] == '\0')
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  memset(out, 0, sizeof(*out));
  out->topic = DI(dm).outbound_topic;
  out->payload = payload;
  out->payload_len = payload_len;
  out->qos = AZ_IOT_MQTT_QOS_1;
  out->user_properties = az_iot_mqtt_user_properties_from_array(type, 1);
  out->correlation_data = request_id;
  out->correlation_data_len = AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN;
  out->content_type = DM_CONTENT_TYPE;
  out->message_expiry_seconds = expiry_seconds;
  return AZ_IOT_OK;
}

/**
 * @brief Whether pool element entry @p held names tracker entry @p entry_index.
 * @param claim_pending Also claim a DM_TRACKER_ENTRY_PENDING element: the one being submitted,
 * asked about only after every element with a known entry.
 */
static bool element_holds_tracker_entry(size_t* held, size_t entry_index, bool claim_pending)
{
  if (claim_pending && *held == DM_TRACKER_ENTRY_PENDING)
  {
    *held = entry_index + 1u;
  }
  return *held == entry_index + 1u;
}

/** @brief The ready slot whose probe-ack is tracker entry @p entry_index, or NULL. */
static az_iot_mqttv5_direct_method_ready_slot* find_ready_slot_by_tracker_entry(
    az_iot_mqttv5_direct_method_client* dm,
    size_t entry_index)
{
  for (int pass = 0; pass < 2; ++pass)
  {
    for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
    {
      az_iot_mqttv5_direct_method_ready_slot* slot = &DI(dm).ready_pool[i];
      if (RI(slot).in_use
          && element_holds_tracker_entry(&RI(slot).probe_ack_tracker_entry, entry_index, pass == 1))
      {
        return slot;
      }
    }
  }
  return NULL;
}

/** @brief The req_pool index whose result is tracker entry @p entry_index, or MAX_CONCURRENT. */
static size_t find_request_by_tracker_entry(
    az_iot_mqttv5_direct_method_client* dm,
    size_t entry_index)
{
  for (int pass = 0; pass < 2; ++pass)
  {
    for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
    {
      if (DI(dm).req_pool[i]._internal.in_use
          && element_holds_tracker_entry(&DI(dm).result_tracker_entry[i], entry_index, pass == 1))
      {
        return i;
      }
    }
  }
  return AZ_IOT_MQTTV5_DM_MAX_CONCURRENT;
}

/** @brief The notice that is tracker entry @p entry_index, or NULL. */
static az_iot_mqttv5_direct_method_notice* find_notice_by_tracker_entry(
    az_iot_mqttv5_direct_method_client* dm,
    size_t entry_index)
{
  for (int pass = 0; pass < 2; ++pass)
  {
    for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX; ++i)
    {
      if (element_holds_tracker_entry(
              &DI(dm).notices[i]._internal.tracker_entry, entry_index, pass == 1))
      {
        return &DI(dm).notices[i];
      }
    }
  }
  return NULL;
}

/** @brief Stop tracking @p *entry, if any; its late PUBACK is ignored. */
static void cancel_tracked_reply(az_iot_mqttv5_direct_method_client* dm, size_t* entry)
{
  size_t const held = *entry;
  *entry = DM_NO_TRACKER_ENTRY;
  if (held != DM_NO_TRACKER_ENTRY && held != DM_TRACKER_ENTRY_PENDING)
  {
    az_iot_delivery_tracker__cancel(&DI(dm).tracker, held - 1u);
  }
}

static void log_broker_refusal(const char* type_value, const az_iot_publish_result* detail)
{
  AZ_IOT_LOG_ERRORF(
      AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
      "the broker rejected a '%s' message (%s); the service will not see it",
      type_value,
      az_iot_result_to_string(detail != NULL ? detail->status : AZ_IOT_ERR_PUBLISH_REFUSED));
}

/**
 * @brief Track a message for a pool element whose entry field is @p held.
 * @param deadline_ms Monotonic time it may be sent until; 0 for the connection's
 *        max_unconfirmed_delivery_timeout_ms.
 * @return As az_iot_delivery_tracker__submit(); its first send may already have ended it.
 */
static az_iot_result submit_reply_for_delivery(
    az_iot_mqttv5_direct_method_client* dm,
    const az_iot_delivery_tracker_profile* profile,
    size_t* held,
    const uint8_t* request_id,
    uint64_t deadline_ms)
{
  uint32_t timeout_ms = 0;
  if (deadline_ms != 0u)
  {
    uint64_t const now = az_iot_time_mono_ms();
    uint64_t const left = (deadline_ms > now) ? deadline_ms - now : 1u;
    timeout_ms = (left > UINT32_MAX) ? UINT32_MAX : (uint32_t)left;
  }
  *held = DM_TRACKER_ENTRY_PENDING;
  size_t entry = 0;
  az_iot_result const result = az_iot_delivery_tracker__submit(
      &DI(dm).tracker, profile, request_id, AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN, timeout_ms, &entry);
  if (result != AZ_IOT_OK)
  {
    *held = DM_NO_TRACKER_ENTRY;
  }
  else if (*held == DM_TRACKER_ENTRY_PENDING)
  {
    *held = entry + 1u; /* Not sent yet. */
  }
  return result;
}

static az_iot_result encode_probe_ack_message(
    void* owner,
    size_t entry_index,
    az_iot_mqtt_message* out)
{
  az_iot_mqttv5_direct_method_client* dm = (az_iot_mqttv5_direct_method_client*)owner;
  az_iot_mqttv5_direct_method_ready_slot* slot = find_ready_slot_by_tracker_entry(dm, entry_index);
  if (slot == NULL)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  /* The tracker counts its time limit from submit(), so check the absolute ready wait too: a ready
   * id offered past it names a token that can no longer run. */
  if (az_iot_time_mono_ms() >= RI(slot).expires_at_ms)
  {
    return AZ_IOT_ERR_TIMEOUT;
  }
  /* Read once: the value checked is the value sent, so a finite budget never goes out as 0. */
  uint32_t const expiry = az_iot_mqttv5_direct_method_client__remaining_budget_seconds(
      RI(slot).connect_budget_seconds, RI(slot).probe_received_at_ms);
  if (RI(slot).connect_budget_seconds > 0u && expiry < AZ_IOT_MQTTV5_DM_MIN_USEFUL_BUDGET_SECONDS)
  {
    return AZ_IOT_ERR_TIMEOUT;
  }
  return build_reply_message(
      dm,
      &k_probe_ack_type,
      RI(slot).probe_ack_frame,
      RI(slot).probe_ack_frame_len,
      RI(slot).request_id,
      expiry,
      out);
}

static void on_probe_ack_sent(void* owner, size_t entry_index)
{
  az_iot_mqttv5_direct_method_ready_slot* slot
      = find_ready_slot_by_tracker_entry((az_iot_mqttv5_direct_method_client*)owner, entry_index);
  if (slot != NULL)
  {
    RI(slot).probe_ack_published = true;
  }
}

/**
 * @brief A probe-ack ended. Its token stays for an exec unless the service never saw it: a
 *        probe-ack that ran out of budget, or could not be sent, before its first publish.
 */
static void on_probe_ack_complete(
    void* owner,
    size_t entry_index,
    az_iot_delivery_tracker_outcome outcome,
    const az_iot_publish_result* detail)
{
  az_iot_mqttv5_direct_method_ready_slot* slot
      = find_ready_slot_by_tracker_entry((az_iot_mqttv5_direct_method_client*)owner, entry_index);
  if (slot == NULL)
  {
    return;
  }
  RI(slot).probe_ack_tracker_entry = DM_NO_TRACKER_ENTRY;
  if (outcome == AZ_IOT_DELIVERY_TRACKER_OUTCOME_REFUSED)
  {
    log_broker_refusal(DM_TYPE_PROBE_ACK, detail);
  }
  else if (outcome != AZ_IOT_DELIVERY_TRACKER_OUTCOME_DELIVERED && !RI(slot).probe_ack_published)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "the probe-ack for '%s' could not be sent within its budget; dropping the token",
        RI(slot).method_name);
    RI(slot).in_use = false;
  }
}

static az_iot_result encode_result_message(
    void* owner,
    size_t entry_index,
    az_iot_mqtt_message* out)
{
  az_iot_mqttv5_direct_method_client* dm = (az_iot_mqttv5_direct_method_client*)owner;
  size_t const index = find_request_by_tracker_entry(dm, entry_index);
  if (index >= AZ_IOT_MQTTV5_DM_MAX_CONCURRENT)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  /* The tracker counts its time limit from submit(), so check the absolute deadline too. */
  if (az_iot_mqttv5_direct_method_client__result_deadline_has_passed(dm, index))
  {
    return AZ_IOT_ERR_TIMEOUT;
  }
  /* The result deadline reserves the safety margin (at least 1 s) before the budget ends, so the
   * expiry is never 0 for a finite budget while the entry is live. */
  return build_reply_message(
      dm,
      &k_result_type,
      DI(dm).result_frame[index],
      DI(dm).result_frame_len[index],
      DI(dm).req_pool[index]._internal.correlation_data,
      az_iot_mqttv5_direct_method_client__remaining_budget_seconds(
          DI(dm).exec_budget_seconds[index], DI(dm).exec_received_at_ms[index]),
      out);
}

/** @brief A result ended: acknowledged, refused, past its deadline or dropped. Frees its slot. */
static void on_result_complete(
    void* owner,
    size_t entry_index,
    az_iot_delivery_tracker_outcome outcome,
    const az_iot_publish_result* detail)
{
  az_iot_mqttv5_direct_method_client* dm = (az_iot_mqttv5_direct_method_client*)owner;
  size_t const index = find_request_by_tracker_entry(dm, entry_index);
  if (index >= AZ_IOT_MQTTV5_DM_MAX_CONCURRENT)
  {
    return;
  }
  az_iot_direct_method_slot* s = &DI(dm).req_pool[index];
  if (outcome == AZ_IOT_DELIVERY_TRACKER_OUTCOME_REFUSED)
  {
    log_broker_refusal(DM_TYPE_RESULT, detail);
  }
  else if (outcome == AZ_IOT_DELIVERY_TRACKER_OUTCOME_EXPIRED)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "the result deadline for '%s' passed before its result was sent or acknowledged; "
        "reclaiming its slot",
        RI(s).method_name);
  }
  DI(dm).result_tracker_entry[index] = DM_NO_TRACKER_ENTRY;
  RI(s).in_use = false;
}

static az_iot_result encode_notice_message(
    void* owner,
    size_t entry_index,
    az_iot_mqtt_message* out)
{
  az_iot_mqttv5_direct_method_client* dm = (az_iot_mqttv5_direct_method_client*)owner;
  az_iot_mqttv5_direct_method_notice* n = find_notice_by_tracker_entry(dm, entry_index);
  if (n == NULL)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  uint32_t expiry = az_iot_mqttv5_direct_method_client__remaining_budget_seconds(
      RI(n).budget_seconds, RI(n).budget_start_ms);
  /* A spent budget ends the notice: the caller has stopped waiting. 0 on the wire means no
   * expiry at all, so it is never sent for a finite budget. */
  if (RI(n).budget_seconds > 0u && expiry < AZ_IOT_MQTTV5_DM_MIN_USEFUL_BUDGET_SECONDS)
  {
    return AZ_IOT_ERR_TIMEOUT;
  }
  return build_reply_message(
      dm, RI(n).type, RI(n).frame, RI(n).frame_len, RI(n).request_id, expiry, out);
}

static void on_notice_complete(
    void* owner,
    size_t entry_index,
    az_iot_delivery_tracker_outcome outcome,
    const az_iot_publish_result* detail)
{
  az_iot_mqttv5_direct_method_notice* n
      = find_notice_by_tracker_entry((az_iot_mqttv5_direct_method_client*)owner, entry_index);
  if (n == NULL)
  {
    return;
  }
  if (outcome == AZ_IOT_DELIVERY_TRACKER_OUTCOME_REFUSED)
  {
    log_broker_refusal(RI(n).type->value, detail);
  }
  RI(n).tracker_entry = DM_NO_TRACKER_ENTRY;
}

/* Probe-acks and results first; notices are advisory, so they are sent untracked rather than take
 * a PUBACK slot kept for a probe-ack or result. */
static const az_iot_delivery_tracker_profile k_probe_ack_profile = {
  DM_TYPE_PROBE_ACK,
  AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_PUBACK,
  AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_RESEND,
  0u,
  false,
  encode_probe_ack_message,
  on_probe_ack_complete,
  on_probe_ack_sent,
};

static const az_iot_delivery_tracker_profile k_result_profile = {
  DM_TYPE_RESULT,
  AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_PUBACK,
  AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_RESEND,
  0u,
  false,
  encode_result_message,
  on_result_complete,
  NULL,
};

static const az_iot_delivery_tracker_profile k_notice_profile = {
  "probe-ack or abandon notice",
  AZ_IOT_DELIVERY_TRACKER_CONFIRM_BY_PUBACK,
  AZ_IOT_DELIVERY_TRACKER_ON_SESSION_LOSS_RESEND,
  1u,
  true,
  encode_notice_message,
  on_notice_complete,
  NULL,
};

/* PUBACK slots kept, of the reservation, for probe-acks and results (priority 0). */
static const uint8_t k_slots_kept_for_replies[] = { (uint8_t)AZ_IOT_MQTTV5_DM_MAX_CONCURRENT };

/**
 * @brief Send a ProbeAck{rejected} or Abandon, keeping it until its PUBACK.
 *
 * Best effort when every notice is in use: published untracked while CONNECTED, otherwise
 * dropped. Both are logged.
 *
 * @param budget_seconds Message Expiry counted from @p start_ms; 0 sends none, and the notice is
 *        then kept at most the max unconfirmed delivery timeout.
 */
static void send_notice(
    az_iot_mqttv5_direct_method_client* dm,
    const az_iot_mqtt_user_property* type,
    const uint8_t* frame,
    size_t frame_len,
    const uint8_t* request_id,
    uint32_t budget_seconds,
    uint64_t start_ms)
{
  az_iot_mqttv5_direct_method_notice* n = NULL;
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX && n == NULL; ++i)
  {
    n = (DI(dm).notices[i]._internal.tracker_entry == DM_NO_TRACKER_ENTRY) ? &DI(dm).notices[i]
                                                                           : NULL;
  }
  if (n != NULL)
  {
    RI(n).type = type;
    memcpy(RI(n).request_id, request_id, AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN);
    memcpy(RI(n).frame, frame, frame_len);
    RI(n).frame_len = frame_len;
    RI(n).budget_seconds = budget_seconds;
    RI(n).budget_start_ms = start_ms;
    uint64_t const deadline_ms = (budget_seconds > 0u)
        ? start_ms + (uint64_t)budget_seconds * AZ_IOT_MQTTV5_DM_MS_PER_SECOND
        : 0u;
    if (submit_reply_for_delivery(
            dm, &k_notice_profile, &RI(n).tracker_entry, request_id, deadline_ms)
            == AZ_IOT_OK
        && !az_iot_connection_client__is_connected(DI(dm).conn))
    {
      AZ_IOT_LOG_INFOF(
          AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
          "holding a '%s' message until the connection is CONNECTED",
          type->value);
    }
    return;
  }

  uint32_t const expiry
      = az_iot_mqttv5_direct_method_client__remaining_budget_seconds(budget_seconds, start_ms);
  if (budget_seconds > 0u && expiry < AZ_IOT_MQTTV5_DM_MIN_USEFUL_BUDGET_SECONDS)
  {
    return; /* The caller has stopped waiting. */
  }
  az_iot_mqtt_message message;
  az_iot_result result
      = build_reply_message(dm, type, frame, frame_len, request_id, expiry, &message);
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__publish_with_result(
        DI(dm).conn, &DI(dm).tracker, &message, NULL, NULL, NULL);
  }
  if (result == AZ_IOT_ERR_NOT_CONNECTED)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "dropping a '%s' message: not connected and all %d notice-queue entries are in use",
        type->value,
        (int)AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX);
  }
  else if (result == AZ_IOT_OK)
  {
    AZ_IOT_LOG_INFOF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "sent a '%s' message untracked: all %d notice-queue entries are in use",
        type->value,
        (int)AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX);
  }
  else
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "could not publish a '%s' message (%s)",
        type->value,
        az_iot_result_to_string(result));
  }
}

/**
 * @brief Best-effort advisory that a ready token ended before execution began.
 *
 * The service uses it to fail the caller early instead of waiting out the response timeout, so
 * losing one costs latency, not correctness.
 *
 * @param budget_seconds Budget the caller still waits within, from @p start_ms; 0 uses the response
 *        timeout from now.
 */
void az_iot_mqttv5_direct_method_client__send_abandon(
    az_iot_mqttv5_direct_method_client* dm,
    const az_iot_mqttv5_direct_method_ready_slot* slot,
    az_iot_dm_proto_abandon_reason reason,
    uint32_t budget_seconds,
    uint64_t start_ms)
{
  uint8_t frame[AZ_IOT_DM_PROTO_ABANDON_MAX];
  size_t frame_len = 0;
  az_iot_result encoded = az_iot_dm_proto_encode_abandon(
      frame, sizeof(frame), RI(slot).ready_id, AZ_IOT_MQTTV5_DM_READY_ID_LEN, reason, &frame_len);
  if (encoded != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "could not encode the abandon for '%s' (%s); the service will wait "
        "out "
        "its response timeout instead",
        RI(slot).method_name,
        az_iot_result_to_string(encoded));
    return;
  }
  if (budget_seconds == 0u)
  {
    budget_seconds = RI(slot).response_timeout_seconds;
    start_ms = az_iot_time_mono_ms();
  }
  send_notice(dm, &k_abandon_type, frame, frame_len, RI(slot).request_id, budget_seconds, start_ms);
}

az_iot_result az_iot_mqttv5_direct_method_client__start_reply_delivery(
    az_iot_mqttv5_direct_method_client* client,
    az_iot_connection_client* conn)
{
  az_iot_result result = az_iot_delivery_tracker__init(
      &DI(client).tracker,
      DI(client).tracker_entries,
      AZ_IOT_MQTTV5_DM_TRACKED_MESSAGES,
      AZ_IOT_MQTTV5_DM_RESERVED_PUBACKS,
      k_slots_kept_for_replies,
      sizeof(k_slots_kept_for_replies),
      client);
  if (result == AZ_IOT_OK)
  {
    result = az_iot_delivery_tracker__bind(&DI(client).tracker, conn);
  }
  return result;
}

void az_iot_mqttv5_direct_method_client__stop_reply_delivery(
    az_iot_mqttv5_direct_method_client* client)
{
  az_iot_delivery_tracker__unbind(&DI(client).tracker);
}

az_iot_result az_iot_mqttv5_direct_method_client__deliver_probe_ack(
    az_iot_mqttv5_direct_method_client* client,
    az_iot_mqttv5_direct_method_ready_slot* slot,
    uint64_t deadline_ms)
{
  az_iot_result result = az_iot_dm_proto_encode_probe_ack_ready(
      RI(slot).probe_ack_frame,
      sizeof(RI(slot).probe_ack_frame),
      RI(slot).ready_id,
      AZ_IOT_MQTTV5_DM_READY_ID_LEN,
      &RI(slot).probe_ack_frame_len);
  if (result == AZ_IOT_OK)
  {
    result = submit_reply_for_delivery(
        client,
        &k_probe_ack_profile,
        &RI(slot).probe_ack_tracker_entry,
        RI(slot).request_id,
        deadline_ms);
  }
  return result;
}

az_iot_result az_iot_mqttv5_direct_method_client__deliver_result(
    az_iot_mqttv5_direct_method_client* client,
    size_t index)
{
  return submit_reply_for_delivery(
      client,
      &k_result_profile,
      &DI(client).result_tracker_entry[index],
      DI(client).req_pool[index]._internal.correlation_data,
      DI(client).exec_deadline_ms[index]);
}

void az_iot_mqttv5_direct_method_client__send_probe_rejection(
    az_iot_mqttv5_direct_method_client* client,
    const uint8_t* frame,
    size_t frame_len,
    const uint8_t* request_id,
    uint32_t budget_seconds,
    uint64_t start_ms)
{
  send_notice(client, &k_probe_ack_type, frame, frame_len, request_id, budget_seconds, start_ms);
}

void az_iot_mqttv5_direct_method_client__cancel_reply(
    az_iot_mqttv5_direct_method_client* client,
    size_t* tracker_entry)
{
  cancel_tracked_reply(client, tracker_entry);
}

void az_iot_mqttv5_direct_method_client__expire_overdue_replies(
    az_iot_mqttv5_direct_method_client* client)
{
  az_iot_delivery_tracker__expire_overdue(&DI(client).tracker);
}
