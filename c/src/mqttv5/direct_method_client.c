// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv5 direct methods (MQTT v5).
 *
 * A three-phase handshake over two flat topics, per the MQTTv5 direct-methods
 * design and common/Protos/directmethods.proto:
 *
 *   probe:1      service -> device   ih/{device_id}/dev/methods
 *   probe-ack:1  device -> service   ih/{device_id}/srv/methods
 *   exec:1       service -> device   ih/{device_id}/dev/methods
 *   abandon:1    device -> service   ih/{device_id}/srv/methods
 *   result:1     device -> service   ih/{device_id}/srv/methods
 *
 * The service asks first and sends the arguments only to a device that said
 * yes. The acceptance carries a ready id and the service quotes it back on the
 * exec, so (request id, ready id) is a single-use execution token -- which is
 * what lets every message be QoS 1 without a redelivery running the method
 * twice.
 *
 * Neither topic carries a method name, so the name from the probe payload is
 * held in a ready slot until the matching exec arrives.
 */
#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv5/az_iot_direct_method_client.h"

#include "internal/connection_client_internal.h"
#include "internal/direct_method_codec.h"
#include "internal/log_internal.h"
#include "internal/mono_time.h"
#include "internal/span_writer.h"

#define DI(d) ((d)->_internal)
#define RI(r) ((r)->_internal)

#define DM_CONTENT_TYPE "application/protobuf"

/* Only version 1 of each phase exists. The version is part of the dispatch key:
 * a later "exec:2" may carry a different payload, so treating it as an exec
 * would decode the wrong shape. */
#define DM_TYPE_VERSION "1"

/* The service starts a timer and waits; a probe-ack or abandon with under a
 * second of budget left has already missed that window, so it is dropped rather
 * than published into a request the service has given up on. A result is held
 * to its own deadline instead (see exec_deadline_ms()). */
#define DM_MIN_USEFUL_BUDGET_SECONDS 1u

/* Pending-PUBACK slots reserved at init, one per ticket. */
#define DM_PUBACK_RESERVATION AZ_IOT_MQTTV5_DM_PUBACK_TICKETS

/** @brief What a PUBACK ticket's index names. */
typedef enum
{
  DM_TICKET_PROBE_ACK = 1, /**< ready_pool[index] */
  DM_TICKET_RESULT = 2, /**< req_pool[index] */
  DM_TICKET_NOTICE = 3 /**< notice_queue[index] */
} dm_ticket_kind;

#define DM_TYPE_PROBE_ACK "probe-ack:" DM_TYPE_VERSION
#define DM_TYPE_ABANDON "abandon:" DM_TYPE_VERSION
#define DM_TYPE_RESULT "result:" DM_TYPE_VERSION

/* Budgets travel in whole seconds; the monotonic clock counts milliseconds. */
#define MS_PER_SECOND 1000u

/* Safety margin covering the network transit a broker-adjusted message expiry
 * cannot measure: a tenth of the response timeout, held within these bounds so
 * a very short timeout still reserves something and a very long one does not
 * reserve minutes. */
#define DM_SAFETY_MARGIN_DIVISOR 10u
#define DM_SAFETY_MARGIN_MIN_SECONDS 1u
#define DM_SAFETY_MARGIN_MAX_SECONDS 10u

/* Multiplier and increment of the 64-bit linear congruential generator that
 * advances the ready-id state. */
#define DM_READY_ID_LCG_MULTIPLIER 6364136223846793005ull
#define DM_READY_ID_LCG_INCREMENT 1442695040888963407ull
/* One draw from that generator supplies this many bytes of the ready id. */
#define DM_READY_ID_BYTES_PER_DRAW 8u
/* Bits to shift a draw by to reach its Nth byte. */
#define DM_BITS_PER_BYTE 8u

/* Both method topics are built from three parts: the "ih/" root, the device id
 * and the direction-specific tail. */
#define DM_TOPIC_PART_COUNT 3

/* 64-bit FNV-1a parameters for the identity fingerprint. */
#define DM_FNV_OFFSET_BASIS 14695981039346656037ull
#define DM_FNV_PRIME 1099511628211ull

/* The public header has to size the result buffer without being able to see the
 * codec's internal header, so it carries its own copy of the framing overhead.
 * This is the only place both are in scope; a mismatch would silently undersize
 * the buffer, so fail the build instead. */
typedef char az_iot_dm_result_overhead_agrees
    [(AZ_IOT_MQTTV5_DM_RESULT_FRAME_OVERHEAD == AZ_IOT_DM_PROTO_RESULT_OVERHEAD) ? 1 : -1];

/* The concurrency limit is meant to be tuned down for small devices, but a
 * limit of zero refuses every probe and makes the client inert. */
typedef char az_iot_dm_concurrency_is_usable[(AZ_IOT_MQTTV5_DM_MAX_CONCURRENT >= 1) ? 1 : -1];

/* A held notice stores the encoded frame; the public header sizes it without
 * seeing the codec. */
typedef char az_iot_dm_notice_frame_fits
    [(AZ_IOT_MQTTV5_DM_NOTICE_FRAME_MAX >= AZ_IOT_DM_PROTO_PROBE_ACK_MAX
      && AZ_IOT_MQTTV5_DM_NOTICE_FRAME_MAX >= AZ_IOT_DM_PROTO_ABANDON_MAX)
         ? 1
         : -1];

/* ------------------------------------------------------------------------- */
/* budgets                                                                   */
/* ------------------------------------------------------------------------- */

/* Compensates for the network transit that a broker-adjusted message expiry
 * cannot account for. */
static uint32_t safety_margin_seconds(uint32_t response_timeout_seconds)
{
  uint32_t margin = response_timeout_seconds / DM_SAFETY_MARGIN_DIVISOR;
  if (margin < DM_SAFETY_MARGIN_MIN_SECONDS)
  {
    margin = DM_SAFETY_MARGIN_MIN_SECONDS;
  }
  if (margin > DM_SAFETY_MARGIN_MAX_SECONDS)
  {
    margin = DM_SAFETY_MARGIN_MAX_SECONDS;
  }
  return margin;
}

/* What is left of a budget of `budget_seconds` that started at `start_ms`.
 *
 * A budget of 0 means the message carried no expiry at all, which is not the
 * same as an exhausted one; it is reported back unchanged so callers can tell
 * "unbounded" from "out of time". */
static uint32_t remaining_budget_seconds(uint32_t budget_seconds, uint64_t start_ms)
{
  if (budget_seconds == 0u)
  {
    return 0u;
  }
  uint64_t now = az_iot_time_mono_ms();
  uint64_t elapsed_ms = (now > start_ms) ? (now - start_ms) : 0u;
  uint64_t elapsed = elapsed_ms / MS_PER_SECOND;
  return (elapsed >= budget_seconds) ? 0u : (budget_seconds - (uint32_t)elapsed);
}

/* ------------------------------------------------------------------------- */
/* declared methods                                                          */
/* ------------------------------------------------------------------------- */

static az_iot_mqttv5_direct_method_registration* method_find(
    az_iot_mqttv5_direct_method_client* dm,
    const char* method_name)
{
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_METHODS; ++i)
  {
    az_iot_mqttv5_direct_method_registration* entry = &DI(dm).methods[i];
    if (RI(entry).in_use && strcmp(RI(entry).name, method_name) == 0)
    {
      return entry;
    }
  }
  return NULL;
}

/* ------------------------------------------------------------------------- */
/* ready slots                                                               */
/* ------------------------------------------------------------------------- */

/* Fill `out` with a ready id. Uniqueness is the whole requirement: the token is
 * single-use, good for one exec, and the service only knows it because this
 * device just published it. Same generator the presence nonce uses. */
static void generate_ready_id(
    az_iot_mqttv5_direct_method_client* dm,
    uint8_t out[AZ_IOT_MQTTV5_DM_READY_ID_LEN])
{
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_READY_ID_LEN; i += DM_READY_ID_BYTES_PER_DRAW)
  {
    uint64_t x = az_iot_time_mono_ms()
        ^ (DI(dm).rng_state * DM_READY_ID_LCG_MULTIPLIER + DM_READY_ID_LCG_INCREMENT);
    DI(dm).rng_state = x;
    for (size_t b = 0; b < DM_READY_ID_BYTES_PER_DRAW; ++b)
    {
      out[i + b] = (uint8_t)(x >> (b * DM_BITS_PER_BYTE));
    }
  }
}

static az_iot_mqttv5_direct_method_ready_slot* ready_find(
    az_iot_mqttv5_direct_method_client* dm,
    const uint8_t* request_id)
{
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    az_iot_mqttv5_direct_method_ready_slot* slot = &DI(dm).ready_pool[i];
    if (RI(slot).in_use
        && memcmp(RI(slot).request_id, request_id, AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN) == 0)
    {
      return slot;
    }
  }
  return NULL;
}

/** @brief Next value of the shared sequence counter; never 0, so a zeroed slot never matches. */
static uint32_t next_sequence(az_iot_mqttv5_direct_method_client* dm)
{
  DI(dm).next_seq++;
  if (DI(dm).next_seq == 0u)
  {
    DI(dm).next_seq = 1u;
  }
  return DI(dm).next_seq;
}

static az_iot_mqttv5_direct_method_ready_slot* ready_acquire(az_iot_mqttv5_direct_method_client* dm)
{
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    az_iot_mqttv5_direct_method_ready_slot* slot = &DI(dm).ready_pool[i];
    if (!RI(slot).in_use)
    {
      memset(slot, 0, sizeof(*slot));
      RI(slot).in_use = true;
      RI(slot).seq = next_sequence(dm);
      return slot;
    }
  }
  return NULL;
}

/* Acquire a free slot from the bounded pool. A slot is returned by
 * az_iot_mqttv5_direct_method_respond(), or reclaimed by requests_expire_stale()
 * once its response window has closed. Running out means this many invocations
 * are genuinely still in flight.
 *
 * Returns false when the pool is full; otherwise fills @p out_request with the
 * value the application hands back to respond(). */
static bool request_acquire(
    az_iot_mqttv5_direct_method_client* dm,
    az_iot_direct_method_request* out_request,
    az_iot_direct_method_slot** out_slot,
    size_t* out_index)
{
  for (size_t n = 0; n < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++n)
  {
    size_t i = (DI(dm).next_slot + n) % AZ_IOT_MQTTV5_DM_MAX_CONCURRENT;
    az_iot_direct_method_slot* s = &DI(dm).req_pool[i];
    if (!s->_internal.in_use)
    {
      memset(s, 0, sizeof(*s));
      s->_internal.seq = next_sequence(dm);
      s->_internal.in_use = true;
      DI(dm).result_state[i] = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
      *out_index = i;
      DI(dm).next_slot = (i + 1u) % AZ_IOT_MQTTV5_DM_MAX_CONCURRENT;

      memset(out_request, 0, sizeof(*out_request));
      out_request->_internal.owner = dm;
      out_request->_internal.slot = (uint32_t)i;
      out_request->_internal.seq = s->_internal.seq;
      out_request->_internal.profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
      *out_slot = s;
      return true;
    }
  }
  AZ_IOT_LOG_WARNF(
      AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
      "dropping an invocation, all %d concurrent slots are taken by requests "
      "still inside their response budget. Raise AZ_IOT_MQTTV5_DM_MAX_CONCURRENT to hold more at "
      "once.",
      (int)AZ_IOT_MQTTV5_DM_MAX_CONCURRENT);
  return false;
}

/* Resolve a request to the slot it names, or NULL when it no longer refers to a
 * live invocation of this client.
 *
 * The sequence check is what makes a reused slot detectable: the request is a
 * value the pool cannot reach, so a slot reclaimed and handed to another
 * invocation carries a newer seq and the stale request stops matching. */
static az_iot_direct_method_slot* request_resolve(
    az_iot_mqttv5_direct_method_client* dm,
    az_iot_direct_method_request request,
    size_t* out_index)
{
  if (request._internal.owner != dm)
  {
    return NULL;
  }
  size_t i = (size_t)request._internal.slot;
  if (i >= AZ_IOT_MQTTV5_DM_MAX_CONCURRENT || request._internal.seq == 0u)
  {
    return NULL;
  }
  az_iot_direct_method_slot* s = &DI(dm).req_pool[i];
  /* A result queued or awaiting its PUBACK has already answered the request. */
  if (!s->_internal.in_use || s->_internal.seq != request._internal.seq
      || DI(dm).result_state[i] != AZ_IOT_MQTTV5_DM_OUTBOUND_NONE)
  {
    return NULL;
  }
  *out_index = i;
  return s;
}

/* True when the device can take on one more invocation.
 *
 * Ready tokens and in-flight invocations draw on one budget, because a ready
 * token is a promise to execute and executing needs a req_pool slot that an
 * earlier invocation may still be holding -- those are released by respond(),
 * which the application may call long after its handler returned. Admitting on
 * free ready slots alone would let the device accept work it has no room to
 * run, and the caller would find out only when the exec was abandoned, after
 * its parameters had already been shipped to the device. Deciding it here is
 * what the probe phase is for. */
static bool has_execution_capacity(const az_iot_mqttv5_direct_method_client* dm)
{
  size_t committed = 0;
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    if (DI(dm).ready_pool[i]._internal.in_use)
    {
      committed++;
    }
    if (DI(dm).req_pool[i]._internal.in_use)
    {
      committed++;
    }
  }
  return committed < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT;
}

/* ------------------------------------------------------------------------- */
/* publishing                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief The delivery state a PUBACK ticket names, or NULL when that message was dropped, or its
 *        slot reused, since it was published.
 */
static az_iot_mqttv5_direct_method_outbound_state* ticket_target(
    az_iot_mqttv5_direct_method_client* dm,
    uint8_t kind,
    uint32_t index,
    uint32_t seq)
{
  az_iot_mqttv5_direct_method_outbound_state* state = NULL;
  if (kind == DM_TICKET_PROBE_ACK && index < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT)
  {
    az_iot_mqttv5_direct_method_ready_slot* slot = &DI(dm).ready_pool[index];
    if (RI(slot).in_use && RI(slot).seq == seq)
    {
      state = &RI(slot).probe_ack_state;
    }
  }
  else if (kind == DM_TICKET_RESULT && index < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT)
  {
    az_iot_direct_method_slot* s = &DI(dm).req_pool[index];
    if (RI(s).in_use && RI(s).seq == seq)
    {
      state = &DI(dm).result_state[index];
    }
  }
  else if (kind == DM_TICKET_NOTICE && index < AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX)
  {
    az_iot_mqttv5_direct_method_queued_notice* n = &DI(dm).notice_queue[index];
    if (RI(n).seq == seq)
    {
      state = &RI(n).state;
    }
  }
  return (state != NULL && *state == AZ_IOT_MQTTV5_DM_OUTBOUND_AWAITING_PUBACK) ? state : NULL;
}

/**
 * @brief Completes a tracked publish.
 *
 * A PUBACK, or a refusal (AZ_IOT_ERR_PUBLISH_REFUSED), ends the message; a result frees its slot.
 * Any other failure is treated as no PUBACK (the session ended, or the adapter dropped the
 * publish), so the message is queued and published again.
 *
 * @p user_ctx is a ticket inside the client; deinit() abandons the pending PUBACKs, so this never
 * runs after it.
 */
static void on_publish_result(const az_iot_publish_result* outcome, void* user_ctx)
{
  az_iot_mqttv5_direct_method_puback_ticket* ticket
      = (az_iot_mqttv5_direct_method_puback_ticket*)user_ctx;
  az_iot_mqttv5_direct_method_client* dm = RI(ticket).client;
  if (dm == NULL || outcome == NULL)
  {
    return;
  }
  uint8_t kind = RI(ticket).kind;
  uint32_t index = RI(ticket).index;
  uint32_t seq = RI(ticket).seq;
  const char* type_value = RI(ticket).type;
  memset(ticket, 0, sizeof(*ticket));

  bool const acknowledged = outcome->status == AZ_IOT_OK;
  /* reason_code is diagnostic only: an adapter also reports one for a publish it dropped. */
  bool const refused = outcome->status == AZ_IOT_ERR_PUBLISH_REFUSED;
  if (refused)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "the broker rejected a '%s' message (%s); the service will not see "
        "it",
        type_value,
        az_iot_result_to_string(outcome->status));
  }

  az_iot_mqttv5_direct_method_outbound_state* state = ticket_target(dm, kind, index, seq);
  if (state != NULL)
  {
    if (!acknowledged && !refused)
    {
      *state = AZ_IOT_MQTTV5_DM_OUTBOUND_QUEUED;
      /* Do not publish it again from this same PUBACK. A late failure for a message that already
       * ended frees a ticket without requeuing anything, so it does not skip the drain. */
      DI(dm).skip_next_drain = true;
      AZ_IOT_LOG_INFOF(
          AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
          "a '%s' message was not acknowledged (%s); queued to be published again",
          type_value,
          az_iot_result_to_string(outcome->status));
      return;
    }
    *state = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
    if (kind == DM_TICKET_RESULT)
    {
      DI(dm).req_pool[index]._internal.in_use = false;
    }
  }
}

/**
 * @brief A free PUBACK ticket for @p ticket_kind, or NULL.
 *
 * Notices are advisory, so they leave a ticket free for each of the AZ_IOT_MQTTV5_DM_MAX_CONCURRENT
 * probe-acks and results that could still be published and do not hold one yet.
 */
static az_iot_mqttv5_direct_method_puback_ticket* ticket_acquire(
    az_iot_mqttv5_direct_method_client* dm,
    uint8_t ticket_kind)
{
  if (!az_iot_connection_client__can_track_publish(DI(dm).conn, dm))
  {
    return NULL;
  }
  size_t free_count = 0;
  size_t held_for_executions = 0;
  az_iot_mqttv5_direct_method_puback_ticket* free_ticket = NULL;
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_PUBACK_TICKETS; ++i)
  {
    az_iot_mqttv5_direct_method_puback_ticket* ticket = &DI(dm).puback_tickets[i];
    if (RI(ticket).client == NULL)
    {
      free_count++;
      free_ticket = ticket;
    }
    else if (RI(ticket).kind != DM_TICKET_NOTICE)
    {
      held_for_executions++;
    }
  }
  size_t const reserved = (held_for_executions < (size_t)AZ_IOT_MQTTV5_DM_MAX_CONCURRENT)
      ? (size_t)AZ_IOT_MQTTV5_DM_MAX_CONCURRENT - held_for_executions
      : 0u;
  if (ticket_kind == DM_TICKET_NOTICE && free_count <= reserved)
  {
    return NULL;
  }
  return free_ticket;
}

/**
 * @brief Publish one device-to-service message. Every phase shares this envelope; only the `type`,
 *        the payload and the expiry differ.
 *
 * Probe-acks and results are only published tracked; notices are published untracked when no
 * ticket is free, and are then not published again after a reconnect.
 *
 * @param ticket_kind What @p index names, so the PUBACK can find the message again; 0 publishes
 *        without tracking.
 * @param[out] out_tracked true when the PUBACK is tracked.
 *
 * @retval AZ_IOT_ERR_NOT_CONNECTED Not CONNECTED, or no ticket is free for a probe-ack or result;
 *         nothing was sent, and the caller keeps it queued.
 */
static az_iot_result publish_typed(
    az_iot_mqttv5_direct_method_client* dm,
    const char* type_value,
    const uint8_t* payload,
    size_t payload_len,
    const uint8_t* request_id,
    uint32_t expiry_seconds,
    uint8_t ticket_kind,
    size_t index,
    uint32_t seq,
    bool* out_tracked)
{
  *out_tracked = false;
  if (DI(dm).outbound_topic[0] == '\0')
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  az_iot_mqtt_user_property type_prop;
  type_prop.key = "type";
  type_prop.value = type_value;

  az_iot_mqtt_message out = { 0 };
  out.topic = DI(dm).outbound_topic;
  out.payload = payload;
  out.payload_len = payload_len;
  out.qos = AZ_IOT_MQTT_QOS_1;
  out.retain = false;
  out.user_properties = az_iot_mqtt_user_properties_from_array(&type_prop, 1);
  out.correlation_data = request_id;
  out.correlation_data_len = AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN;
  out.content_type = DM_CONTENT_TYPE;
  out.message_expiry_seconds = expiry_seconds;

  az_iot_mqttv5_direct_method_puback_ticket* ticket = NULL;
  if (ticket_kind != 0u)
  {
    ticket = ticket_acquire(dm, ticket_kind);
    if (ticket == NULL && ticket_kind != DM_TICKET_NOTICE)
    {
      /* Sent untracked, it would be lost with the connection. A PUBACK frees a ticket. */
      return AZ_IOT_ERR_NOT_CONNECTED;
    }
    if (ticket != NULL)
    {
      RI(ticket).client = dm;
      RI(ticket).kind = ticket_kind;
      RI(ticket).index = (uint32_t)index;
      RI(ticket).seq = seq;
      RI(ticket).type = type_value;
    }
  }

  az_iot_result result = az_iot_connection_client__publish_with_result(
      DI(dm).conn, dm, &out, (ticket != NULL) ? on_publish_result : NULL, ticket, NULL);
  if (result != AZ_IOT_OK)
  {
    if (ticket != NULL)
    {
      memset(ticket, 0, sizeof(*ticket));
    }
    if (result != AZ_IOT_ERR_NOT_CONNECTED)
    {
      AZ_IOT_LOG_WARNF(
          AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
          "could not publish a '%s' message (%s)",
          type_value,
          az_iot_result_to_string(result));
    }
    return result;
  }
  if (ticket != NULL)
  {
    *out_tracked = true;
  }
  else if (ticket_kind != 0u)
  {
    AZ_IOT_LOG_INFOF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "sent '%s' without ack tracking, its pending-ack slots are all in use; it is not sent "
        "again if the connection drops before its PUBACK",
        type_value);
  }
  return AZ_IOT_OK;
}

/** @brief True when a budget (0: unbounded) started at @p start_ms is spent. */
static bool outbound_budget_is_spent(uint32_t budget_seconds, uint64_t start_ms)
{
  return budget_seconds > 0u
      && remaining_budget_seconds(budget_seconds, start_ms) < DM_MIN_USEFUL_BUDGET_SECONDS;
}

/** @brief True when a queued probe-ack can no longer be sent: its ready wait or connect budget ran
 *         out. */
static bool queued_probe_ack_is_stale(
    const az_iot_mqttv5_direct_method_ready_slot* slot,
    uint64_t now)
{
  return now >= RI(slot).expires_at_ms
      || outbound_budget_is_spent(RI(slot).connect_budget_seconds, RI(slot).probe_received_at_ms);
}

/**
 * @brief Publish the ProbeAck{ready} of ready_pool[@p index] and record its delivery state.
 *
 * @param is_replay true when publishing a queued probe-ack: any failure leaves it queued.
 *
 * @retval AZ_IOT_ERR_TIMEOUT The connect budget is spent: nothing sent, retransmission stops, and a
 *         never-published token is released.
 * @return Otherwise as publish_typed(). On AZ_IOT_ERR_NOT_CONNECTED, or any failure of a replay, it
 *         stays queued; on any other failure the caller decides what happens to the token.
 */
static az_iot_result publish_probe_ack(
    az_iot_mqttv5_direct_method_client* dm,
    size_t index,
    bool is_replay)
{
  az_iot_mqttv5_direct_method_ready_slot* slot = &DI(dm).ready_pool[index];
  /* Read once: the value checked is the value sent, so a finite budget never goes out as 0. */
  uint32_t const expiry
      = remaining_budget_seconds(RI(slot).connect_budget_seconds, RI(slot).probe_received_at_ms);
  if (RI(slot).connect_budget_seconds > 0u && expiry < DM_MIN_USEFUL_BUDGET_SECONDS)
  {
    RI(slot).probe_ack_state = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
    /* A token the service may know lives on until its ready wait ends. */
    if (!RI(slot).probe_ack_published)
    {
      RI(slot).in_use = false;
    }
    return AZ_IOT_ERR_TIMEOUT;
  }
  uint8_t frame[AZ_IOT_DM_PROTO_PROBE_ACK_MAX];
  size_t frame_len = 0;
  az_iot_result result = az_iot_dm_proto_encode_probe_ack_ready(
      frame, sizeof(frame), RI(slot).ready_id, AZ_IOT_MQTTV5_DM_READY_ID_LEN, &frame_len);
  if (result != AZ_IOT_OK)
  {
    return result;
  }
  bool tracked = false;
  result = publish_typed(
      dm,
      DM_TYPE_PROBE_ACK,
      frame,
      frame_len,
      RI(slot).request_id,
      expiry,
      DM_TICKET_PROBE_ACK,
      index,
      RI(slot).seq,
      &tracked);
  if (result == AZ_IOT_ERR_NOT_CONNECTED || (is_replay && result != AZ_IOT_OK))
  {
    RI(slot).probe_ack_state = AZ_IOT_MQTTV5_DM_OUTBOUND_QUEUED;
  }
  else
  {
    if (result == AZ_IOT_OK)
    {
      RI(slot).probe_ack_published = true;
    }
    RI(slot).probe_ack_state = (result == AZ_IOT_OK && tracked)
        ? AZ_IOT_MQTTV5_DM_OUTBOUND_AWAITING_PUBACK
        : AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
  }
  return result;
}

static bool exec_budget_is_spent(const az_iot_mqttv5_direct_method_client* dm, size_t index);

/**
 * @brief Publish the encoded result of req_pool[@p index] and record its delivery state.
 *
 * The slot is released here unless the result is queued or awaiting its PUBACK.
 *
 * @param is_replay true when publishing a queued result: any failure leaves it queued, since
 *        respond() already returned AZ_IOT_OK for it.
 *
 * @retval AZ_IOT_ERR_TIMEOUT The result deadline has passed: nothing sent, and the slot is
 * released.
 * @return Otherwise as publish_typed().
 */
static az_iot_result publish_result(
    az_iot_mqttv5_direct_method_client* dm,
    size_t index,
    bool is_replay)
{
  az_iot_direct_method_slot* s = &DI(dm).req_pool[index];
  /* The result deadline reserves the safety margin (at least 1 s) before the budget ends, so the
   * expiry read after it is never 0 for a finite budget. */
  if (exec_budget_is_spent(dm, index))
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "the result deadline for '%s' passed before its result was sent; sending no result",
        RI(s).method_name);
    DI(dm).result_state[index] = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
    RI(s).in_use = false;
    return AZ_IOT_ERR_TIMEOUT;
  }
  uint32_t const expiry = remaining_budget_seconds(
      DI(dm).exec_budget_seconds[index], DI(dm).exec_received_at_ms[index]);
  bool tracked = false;
  az_iot_result result = publish_typed(
      dm,
      DM_TYPE_RESULT,
      DI(dm).result_frame[index],
      DI(dm).result_frame_len[index],
      RI(s).correlation_data,
      expiry,
      DM_TICKET_RESULT,
      index,
      RI(s).seq,
      &tracked);
  if (result == AZ_IOT_ERR_NOT_CONNECTED || (is_replay && result != AZ_IOT_OK))
  {
    DI(dm).result_state[index] = AZ_IOT_MQTTV5_DM_OUTBOUND_QUEUED;
  }
  else if (result == AZ_IOT_OK && tracked)
  {
    DI(dm).result_state[index] = AZ_IOT_MQTTV5_DM_OUTBOUND_AWAITING_PUBACK;
  }
  else
  {
    DI(dm).result_state[index] = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
    RI(s).in_use = false;
  }
  return result;
}

/**
 * @brief Publish notice_queue[@p index] and record its delivery state.
 *
 * @param is_replay true when publishing a queued notice: any failure leaves it queued.
 *
 * @return As publish_typed().
 */
static az_iot_result publish_notice(
    az_iot_mqttv5_direct_method_client* dm,
    size_t index,
    bool is_replay)
{
  az_iot_mqttv5_direct_method_queued_notice* n = &DI(dm).notice_queue[index];
  uint32_t expiry = remaining_budget_seconds(RI(n).budget_seconds, RI(n).budget_start_ms);
  /* 0 on the wire means no expiry at all, not an exhausted one. */
  if (RI(n).budget_seconds > 0u && expiry < DM_MIN_USEFUL_BUDGET_SECONDS)
  {
    expiry = DM_MIN_USEFUL_BUDGET_SECONDS;
  }
  bool tracked = false;
  az_iot_result result = publish_typed(
      dm,
      RI(n).type,
      RI(n).frame,
      RI(n).frame_len,
      RI(n).request_id,
      expiry,
      DM_TICKET_NOTICE,
      index,
      RI(n).seq,
      &tracked);
  if (result == AZ_IOT_ERR_NOT_CONNECTED || (is_replay && result != AZ_IOT_OK))
  {
    RI(n).state = AZ_IOT_MQTTV5_DM_OUTBOUND_QUEUED;
  }
  else
  {
    RI(n).state = (result == AZ_IOT_OK && tracked) ? AZ_IOT_MQTTV5_DM_OUTBOUND_AWAITING_PUBACK
                                                   : AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
  }
  return result;
}

/**
 * @brief A notice_queue entry for a new notice: a free one, or one whose budget is spent.
 *
 * @return Its index, or AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX when every entry holds a live notice.
 */
static size_t notice_acquire(const az_iot_mqttv5_direct_method_client* dm)
{
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX; ++i)
  {
    const az_iot_mqttv5_direct_method_queued_notice* n = &DI(dm).notice_queue[i];
    if (RI(n).state == AZ_IOT_MQTTV5_DM_OUTBOUND_NONE
        || outbound_budget_is_spent(RI(n).budget_seconds, RI(n).budget_start_ms))
    {
      return i;
    }
  }
  return AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX;
}

/**
 * @brief Send a ProbeAck{rejected} or Abandon, keeping it until its PUBACK.
 *
 * Best effort when the notice queue is full: published untracked while CONNECTED, otherwise
 * dropped. Both are logged.
 *
 * @param budget_seconds Message Expiry counted from @p start_ms; 0 is unbounded.
 */
static void send_notice(
    az_iot_mqttv5_direct_method_client* dm,
    const char* type_value,
    const uint8_t* frame,
    size_t frame_len,
    const uint8_t* request_id,
    uint32_t budget_seconds,
    uint64_t start_ms)
{
  size_t index = notice_acquire(dm);
  if (index < AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX)
  {
    az_iot_mqttv5_direct_method_queued_notice* n = &DI(dm).notice_queue[index];
    RI(n).state = AZ_IOT_MQTTV5_DM_OUTBOUND_QUEUED;
    RI(n).seq = next_sequence(dm);
    RI(n).type = type_value;
    memcpy(RI(n).request_id, request_id, AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN);
    memcpy(RI(n).frame, frame, frame_len);
    RI(n).frame_len = frame_len;
    RI(n).budget_seconds = budget_seconds;
    RI(n).budget_start_ms = start_ms;
    if (publish_notice(dm, index, false) == AZ_IOT_ERR_NOT_CONNECTED)
    {
      AZ_IOT_LOG_INFOF(
          AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
          "holding a '%s' message until the connection is CONNECTED",
          type_value);
    }
    return;
  }

  uint32_t expiry = remaining_budget_seconds(budget_seconds, start_ms);
  if (budget_seconds > 0u && expiry < DM_MIN_USEFUL_BUDGET_SECONDS)
  {
    expiry = DM_MIN_USEFUL_BUDGET_SECONDS;
  }
  bool tracked = false;
  az_iot_result result
      = publish_typed(dm, type_value, frame, frame_len, request_id, expiry, 0u, 0u, 0u, &tracked);
  if (result == AZ_IOT_ERR_NOT_CONNECTED)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "dropping a '%s' message: not connected and all %d notice-queue entries are in use",
        type_value,
        (int)AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX);
  }
  else if (result == AZ_IOT_OK)
  {
    AZ_IOT_LOG_INFOF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "sent a '%s' message untracked: all %d notice-queue entries are in use",
        type_value,
        (int)AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX);
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
static void publish_abandon(
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
  send_notice(dm, DM_TYPE_ABANDON, frame, frame_len, RI(slot).request_id, budget_seconds, start_ms);
}

/* Release every ready token whose wait has run out.
 *
 * Feature clients get no periodic tick, so this runs whenever a method message
 * arrives -- which is exactly when the capacity it frees is about to be needed.
 * A device that is never probed again keeps its stale slots, at the cost of
 * memory it had already reserved.
 */
static void ready_expire_stale(az_iot_mqttv5_direct_method_client* dm)
{
  uint64_t now = az_iot_time_mono_ms();
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    az_iot_mqttv5_direct_method_ready_slot* slot = &DI(dm).ready_pool[i];
    if (!RI(slot).in_use)
    {
      continue;
    }
    if (RI(slot).probe_ack_state == AZ_IOT_MQTTV5_DM_OUTBOUND_QUEUED)
    {
      if (!RI(slot).probe_ack_published)
      {
        /* The service never saw this ready id, so nothing is abandoned. */
        if (queued_probe_ack_is_stale(slot, now))
        {
          AZ_IOT_LOG_WARNF(
              AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
              "the budget for '%s' ran out before its probe-ack could be sent; dropping the token",
              RI(slot).method_name);
          RI(slot).in_use = false;
          RI(slot).probe_ack_state = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
        }
        continue;
      }
      /* Possibly delivered: stop sending it once the caller's connect timeout has passed, but
       * keep the token for an exec until its ready wait ends. */
      if (outbound_budget_is_spent(RI(slot).connect_budget_seconds, RI(slot).probe_received_at_ms))
      {
        RI(slot).probe_ack_state = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
      }
    }
    if (now < RI(slot).expires_at_ms)
    {
      continue;
    }
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "no exec arrived for '%s' within its ready wait; abandoning the "
        "token",
        RI(slot).method_name);
    /* Committed out of the ready state before the advisory goes out, so the
     * message can never claim a token that might still run. */
    az_iot_mqttv5_direct_method_ready_slot expired = *slot;
    RI(slot).in_use = false;
    publish_abandon(dm, &expired, AZ_IOT_DM_PROTO_ABANDON_READY_WAIT_TIMEOUT, 0u, 0u);
  }
}

/**
 * @brief Monotonic milliseconds at and after which no result may be published.
 *
 * @param received_at_ms When the exec arrived.
 * @param exec_budget_seconds The exec's message expiry; 0 means it carried none.
 * @param margin_seconds Safety margin for network transit.
 * @return UINT64_MAX when @p exec_budget_seconds is 0; @p received_at_ms when
 *         the budget does not exceed the margin.
 */
static uint64_t exec_deadline_ms(
    uint64_t received_at_ms,
    uint32_t exec_budget_seconds,
    uint32_t margin_seconds)
{
  if (exec_budget_seconds == 0u)
  {
    return UINT64_MAX;
  }
  if (exec_budget_seconds <= margin_seconds)
  {
    return received_at_ms;
  }
  return received_at_ms + (uint64_t)(exec_budget_seconds - margin_seconds) * MS_PER_SECOND;
}

/* True once the result deadline of this slot has been reached.
 *
 * Shared with respond() so the sweep below and the send path cannot disagree
 * about when an invocation stopped being answerable. */
static bool exec_budget_is_spent(const az_iot_mqttv5_direct_method_client* dm, size_t index)
{
  return az_iot_time_mono_ms() >= DI(dm).exec_deadline_ms[index];
}

/* Release every in-flight slot whose response window has closed.
 *
 * respond() already frees the slot when an answer arrives too late; this covers
 * the invocation that is never answered at all, which would otherwise hold its
 * slot for the life of the client and eventually leave no capacity to accept a
 * probe. Swept on message arrival for the same reason as ready_expire_stale().
 *
 * Nothing goes on the wire. Abandon is defined only for a ready token that is
 * terminated before execution begins, and it is keyed on that token's ready id;
 * an exec slot has moved past it. The service's own response timeout has
 * already fired by here, so it has stopped waiting. */
static void requests_expire_stale(az_iot_mqttv5_direct_method_client* dm)
{
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    az_iot_direct_method_slot* s = &DI(dm).req_pool[i];
    if (!RI(s).in_use || !exec_budget_is_spent(dm, i))
    {
      continue;
    }
    if (DI(dm).result_state[i] != AZ_IOT_MQTTV5_DM_OUTBOUND_NONE)
    {
      AZ_IOT_LOG_WARNF(
          AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
          "the result of '%s' was not acknowledged before its response budget ran out; "
          "reclaiming its slot",
          RI(s).method_name);
    }
    else
    {
      AZ_IOT_LOG_WARNF(
          AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
          "'%s' was never answered and its response budget has run out; "
          "reclaiming its slot",
          RI(s).method_name);
    }
    /* A PUBACK still to come no longer matches: the ticket checks in_use and seq. */
    RI(s).in_use = false;
    DI(dm).result_state[i] = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
  }
}

/* ------------------------------------------------------------------------- */
/* inbound dispatch                                                          */
/* ------------------------------------------------------------------------- */

/* The `type` user property; false when the message carries none. */
static bool message_type(const az_iot_mqtt_message* msg, az_iot_mqtt_user_property_view* out)
{
  return az_iot_mqtt_user_properties_find(&msg->user_properties, "type", out) == AZ_IOT_OK;
}

/* True when `type_value` is exactly "{step}:1". */
static bool dm_msg_type_is(const az_iot_mqtt_user_property_view* type, const char* step)
{
  size_t const step_len = strlen(step);
  size_t const version_len = strlen(DM_TYPE_VERSION);
  return type->value_len == step_len + 1 + version_len && memcmp(type->value, step, step_len) == 0
      && type->value[step_len] == ':'
      && memcmp(type->value + step_len + 1, DM_TYPE_VERSION, version_len) == 0;
}

static void handle_probe(
    az_iot_mqttv5_direct_method_client* dm,
    const az_iot_mqtt_message* msg,
    const uint8_t* request_id)
{
  /* A probe redelivered while its token is still live needs no second answer:
   * the original probe-ack has its own QoS 1 delivery lifecycle. */
  if (ready_find(dm, request_id) != NULL)
  {
    return;
  }

  az_iot_dm_proto_probe probe;
  if (az_iot_dm_proto_decode_probe(msg->payload, msg->payload_len, &probe) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "dropping a probe whose payload is not a Probe message");
    return;
  }

  /* The budget the service is holding open for the caller's connect timeout,
   * already reduced by the broker for the time the probe sat queued. */
  uint32_t connect_budget = msg->message_expiry_seconds;
  uint64_t received_at_ms = az_iot_time_mono_ms();

  char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
  az_iot_mqttv5_direct_method_probe_result answer = AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
  uint32_t minimum_execution_seconds = 0;
  az_iot_direct_method_handler_callback handler = NULL;
  void* handler_ctx = NULL;

  if (probe.method_name_len == 0u || probe.method_name_len >= sizeof(method_name)
      || memchr(probe.method_name, '\0', probe.method_name_len) != NULL)
  {
    /* A name this client cannot store, or cannot hand over as a C string
     * without losing part of it, is one it certainly cannot dispatch. An
     * embedded NUL matters beyond tidiness: the application would compare a
     * prefix and could run a different method from the one authorized. Saying
     * so beats leaving the caller to time out. */
    answer = AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND;
    method_name[0] = '\0';
  }
  else
  {
    memcpy(method_name, probe.method_name, probe.method_name_len);
    method_name[probe.method_name_len] = '\0';

    const az_iot_mqttv5_direct_method_registration* entry = method_find(dm, method_name);
    if (entry == NULL)
    {
      /* Undeclared. Answering now costs the caller one round trip and this
       * device nothing: no arguments cross the wire and no capacity is
       * reserved. */
      answer = AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND;
    }
    else
    {
      minimum_execution_seconds = RI(entry).minimum_execution_seconds;
      handler = RI(entry).handler;
      handler_ctx = RI(entry).handler_ctx;
      /* The best case at exec time is the whole response timeout less the
       * margin. If that cannot cover the method's declared floor, the caller
       * asked for something this device was never going to finish. */
      if (probe.response_timeout_seconds > 0u
          && (uint64_t)probe.response_timeout_seconds <= (uint64_t)minimum_execution_seconds
                  + safety_margin_seconds(probe.response_timeout_seconds))
      {
        answer = AZ_IOT_MQTTV5_DM_PROBE_REJECT_INSUFFICIENT_TIME;
      }
      else if (DI(dm).probe_handler != NULL)
      {
        az_iot_mqttv5_direct_method_probe view;
        view.method_name = method_name;
        view.response_timeout_seconds = probe.response_timeout_seconds;
        answer = DI(dm).probe_handler(&view, DI(dm).probe_handler_ctx);
      }
    }
  }

  az_iot_mqttv5_direct_method_ready_slot* slot = NULL;
  if (answer == AZ_IOT_MQTTV5_DM_PROBE_ACCEPT)
  {
    if (has_execution_capacity(dm))
    {
      slot = ready_acquire(dm);
    }
    if (slot == NULL)
    {
      answer = AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY;
    }
  }

  /* Whatever the application spent deciding comes out of the same budget the
   * service is counting down. */
  uint32_t remaining = remaining_budget_seconds(connect_budget, received_at_ms);
  if (connect_budget > 0u && remaining < DM_MIN_USEFUL_BUDGET_SECONDS)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "the connect budget for '%s' ran out while the probe was being "
        "answered; sending no probe-ack",
        method_name);
    if (slot != NULL)
    {
      RI(slot).in_use = false;
    }
    return;
  }

  if (slot != NULL)
  {
    RI(slot).connect_budget_seconds = connect_budget;
    RI(slot).probe_received_at_ms = received_at_ms;
    generate_ready_id(dm, RI(slot).ready_id);
    memcpy(RI(slot).request_id, request_id, AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN);
    memcpy(RI(slot).method_name, method_name, probe.method_name_len + 1u);
    RI(slot).response_timeout_seconds = probe.response_timeout_seconds;
    RI(slot).minimum_execution_seconds = minimum_execution_seconds;
    RI(slot).handler = handler;
    RI(slot).handler_ctx = handler_ctx;
    /* Hold the token long enough to survive the two delays that follow: the
     * probe-ack still reaching the service, and the exec coming back. */
    RI(slot).expires_at_ms = az_iot_time_mono_ms()
        + ((uint64_t)remaining + probe.response_timeout_seconds
           + safety_margin_seconds(probe.response_timeout_seconds))
            * MS_PER_SECOND;

    az_iot_result sent = publish_probe_ack(dm, (size_t)(slot - DI(dm).ready_pool), false);
    if (sent == AZ_IOT_ERR_NOT_CONNECTED)
    {
      /* Typically between CONNACK and the birth-ack, or every ticket is awaiting
       * a PUBACK. The probe was already acknowledged to the broker, so dropping
       * the answer would lose the invocation; it goes out on the next CONNECTED
       * or PUBACK instead. */
      AZ_IOT_LOG_INFOF(
          AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
          "holding the probe-ack for '%s' until it can be sent",
          method_name);
    }
    else if (sent != AZ_IOT_OK)
    {
      /* Nothing reached the service, so no token was ever promised, and holding
       * one would only consume capacity until its wait ran out. */
      RI(slot).in_use = false;
    }
    return;
  }

  uint8_t frame[AZ_IOT_DM_PROTO_PROBE_ACK_MAX];
  size_t frame_len = 0;
  az_iot_result encoded = az_iot_dm_proto_encode_probe_ack_rejected(
      frame, sizeof(frame), (az_iot_dm_proto_rejected_reason)answer, &frame_len);
  if (encoded != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "could not encode the probe-ack for '%s' (%s); the caller will wait "
        "out its connect timeout instead",
        method_name,
        az_iot_result_to_string(encoded));
    return;
  }
  send_notice(dm, DM_TYPE_PROBE_ACK, frame, frame_len, request_id, connect_budget, received_at_ms);
}

static void handle_exec(
    az_iot_mqttv5_direct_method_client* dm,
    const az_iot_mqtt_message* msg,
    const uint8_t* request_id)
{
  az_iot_mqttv5_direct_method_ready_slot* slot = ready_find(dm, request_id);
  if (slot == NULL)
  {
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "ignoring an exec with no matching ready token -- it was never "
        "accepted, it already ran, or its ready wait ran out");
    return;
  }

  az_iot_dm_proto_exec exec;
  if (az_iot_dm_proto_decode_exec(msg->payload, msg->payload_len, &exec) != AZ_IOT_OK)
  {
    /* The token stays live: a redelivery of the same exec may still decode. */
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "dropping an exec whose payload is not an Exec message");
    return;
  }

  if (exec.ready_id_len != AZ_IOT_MQTTV5_DM_READY_ID_LEN
      || memcmp(exec.ready_id, RI(slot).ready_id, AZ_IOT_MQTTV5_DM_READY_ID_LEN) != 0)
  {
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "ignoring an exec whose ready id does not match the current token");
    return;
  }

  uint64_t received_at_ms = az_iot_time_mono_ms();
  uint32_t exec_budget = msg->message_expiry_seconds;
  uint32_t margin = safety_margin_seconds(RI(slot).response_timeout_seconds);
  /* The admission floor: the margin covers transit the broker cannot measure,
   * and the declared minimum is what the method itself needs once started. */
  uint64_t admission_floor = (uint64_t)margin + RI(slot).minimum_execution_seconds;

  /* Starting work that cannot finish in time burns the device's cycles and
   * still leaves the caller waiting; abandoning says so now. */
  if (exec_budget > 0u && (uint64_t)exec_budget <= admission_floor)
  {
    az_iot_mqttv5_direct_method_ready_slot abandoned = *slot;
    RI(slot).in_use = false;
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "only %u second(s) of budget left for '%s', at or under the %u "
        "second "
        "admission floor; not starting it",
        (unsigned)exec_budget,
        RI(&abandoned).method_name,
        (unsigned)admission_floor);
    publish_abandon(
        dm, &abandoned, AZ_IOT_DM_PROTO_ABANDON_INSUFFICIENT_TIME, exec_budget, received_at_ms);
    return;
  }

  size_t index = 0;
  az_iot_direct_method_request req;
  az_iot_direct_method_slot* req_slot = NULL;
  if (!request_acquire(dm, &req, &req_slot, &index))
  {
    /* Unreachable while probe admission reserves capacity for every token it
     * issues. Kept because dropping the exec silently would strand the caller
     * for its whole response timeout; no reason code fits, and the service only
     * needs to learn that execution did not begin. */
    az_iot_mqttv5_direct_method_ready_slot abandoned = *slot;
    RI(slot).in_use = false;
    publish_abandon(
        dm, &abandoned, AZ_IOT_DM_PROTO_ABANDON_UNSPECIFIED, exec_budget, received_at_ms);
    return;
  }

  memcpy(RI(req_slot).method_name, RI(slot).method_name, strlen(RI(slot).method_name) + 1u);
  memcpy(RI(req_slot).correlation_data, request_id, AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN);
  RI(req_slot).correlation_data_len = AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN;
  DI(dm).exec_budget_seconds[index] = exec_budget;
  DI(dm).exec_received_at_ms[index] = received_at_ms;
  DI(dm).exec_deadline_ms[index] = exec_deadline_ms(received_at_ms, exec_budget, margin);

  /* The token is single-use: consumed here, so a redelivered exec cannot run
   * the method a second time. Its handler is read out first -- the slot is
   * free for the next probe the moment it is released. */
  az_iot_direct_method_handler_callback handler = RI(slot).handler;
  void* handler_ctx = RI(slot).handler_ctx;
  RI(slot).in_use = false;

  handler(req, RI(req_slot).method_name, exec.params, exec.params_len, handler_ctx);
}

static void flush_outbound_queue(az_iot_mqttv5_direct_method_client* dm);
static void on_publish_completed(void* user_ctx);

/** @brief Validate and dispatch one message on the inbound methods topic. */
static void dispatch_method_message(
    az_iot_mqttv5_direct_method_client* dm,
    const az_iot_mqtt_message* msg)
{
  az_iot_mqtt_user_property_view type_value;
  if (!message_type(msg, &type_value))
  {
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "dropping a method message with no `type` property -- the phase is "
        "what says whether it is a probe or an authorization to run, and neither is a safe guess");
    return;
  }

  bool is_probe = dm_msg_type_is(&type_value, "probe");
  if (!is_probe && !dm_msg_type_is(&type_value, "exec"))
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "ignoring a method message of type '%.*s', which this client does not "
        "implement",
        type_value.value_len > INT_MAX ? INT_MAX : (int)type_value.value_len,
        type_value.value);
    return;
  }

  if (msg->correlation_data == NULL || msg->correlation_data_len != AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "dropping a '%s' message whose correlation data is not a %u-byte "
        "request id",
        is_probe ? "probe" : "exec",
        (unsigned)AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN);
    return;
  }

  if (is_probe)
  {
    handle_probe(dm, msg, msg->correlation_data);
  }
  else
  {
    handle_exec(dm, msg, msg->correlation_data);
  }
}

static void on_method_message(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_mqttv5_direct_method_client* dm = (az_iot_mqttv5_direct_method_client*)user_ctx;
  if (!dm || !msg || !msg->topic)
  {
    return;
  }

  /* Handlers are registered by prefix, so a message on a longer topic under
   * the same root reaches this one too. The protocol has exactly one inbound
   * topic; anything below it is not part of it. */
  if (strcmp(msg->topic, DI(dm).inbound_topic) != 0)
  {
    return;
  }

  ready_expire_stale(dm);
  requests_expire_stale(dm);
  /* Read before dispatch: a handler may deinitialize the client and reuse its storage. */
  az_iot_connection_client* conn = DI(dm).conn;
  dispatch_method_message(dm, msg);
  /* Retries what a failed replay left queued, in case no PUBACK of this client is due. After
   * dispatch, so an exec has already ended its token and its probe-ack is not sent again. Only
   * while the client is still initialized: deinit withdraws its observer from the connection. */
  if (az_iot_connection_client__has_publish_completion_observer(conn, on_publish_completed, dm))
  {
    flush_outbound_queue(dm);
  }
}

/* ------------------------------------------------------------------------- */
/* outbound queue                                                            */
/* ------------------------------------------------------------------------- */

/** @brief True when a replay result means nothing more can be published now. */
static bool flush_should_stop(az_iot_result result)
{
  return result == AZ_IOT_ERR_NOT_CONNECTED || result == AZ_IOT_ERR_BUSY;
}

/**
 * @brief Publish every queued probe-ack, result and notice whose budget is not spent.
 *
 * Queued are those answered while not CONNECTED or without a free ticket, and those whose publish
 * ended without a PUBACK. Runs on CONNECTED, on every PUBACK and on every method message. A failed
 * publish leaves the entry queued for the next run; AZ_IOT_ERR_NOT_CONNECTED or AZ_IOT_ERR_BUSY
 * also ends this run.
 */
static void flush_outbound_queue(az_iot_mqttv5_direct_method_client* dm)
{
  /* Any drain answers a pending skip, so one set by a completion with no PUBACK event after it
   * (session teardown) cannot hold back a later PUBACK. */
  DI(dm).skip_next_drain = false;
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    az_iot_mqttv5_direct_method_ready_slot* slot = &DI(dm).ready_pool[i];
    if (!RI(slot).in_use || RI(slot).probe_ack_state != AZ_IOT_MQTTV5_DM_OUTBOUND_QUEUED)
    {
      continue;
    }
    /* The connect budget is checked where the expiry is computed, in publish_probe_ack(). */
    if (az_iot_time_mono_ms() >= RI(slot).expires_at_ms)
    {
      /* Possibly delivered: an exec could still come, but not past its ready wait. */
      if (!RI(slot).probe_ack_published)
      {
        AZ_IOT_LOG_WARNF(
            AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
            "the ready wait for '%s' ran out while its probe-ack was held; dropping the token",
            RI(slot).method_name);
        RI(slot).in_use = false;
      }
      RI(slot).probe_ack_state = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
      continue;
    }
    if (flush_should_stop(publish_probe_ack(dm, i, true)))
    {
      return;
    }
  }

  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    az_iot_direct_method_slot* s = &DI(dm).req_pool[i];
    if (!RI(s).in_use || DI(dm).result_state[i] != AZ_IOT_MQTTV5_DM_OUTBOUND_QUEUED)
    {
      continue;
    }
    if (flush_should_stop(publish_result(dm, i, true)))
    {
      return;
    }
  }

  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX; ++i)
  {
    az_iot_mqttv5_direct_method_queued_notice* n = &DI(dm).notice_queue[i];
    if (RI(n).state != AZ_IOT_MQTTV5_DM_OUTBOUND_QUEUED)
    {
      continue;
    }
    if (outbound_budget_is_spent(RI(n).budget_seconds, RI(n).budget_start_ms))
    {
      RI(n).state = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
      continue;
    }
    if (flush_should_stop(publish_notice(dm, i, true)))
    {
      return;
    }
  }
}

/* Queued messages go out once the hub connection is CONNECTED (on mqttv5, after
 * the birth-ack). */
static void on_connection_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_mqttv5_direct_method_client* dm = (az_iot_mqttv5_direct_method_client*)user_ctx;
  if (dm != NULL && event != NULL && event->scope == AZ_IOT_CONN_SCOPE_HUB
      && event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    flush_outbound_queue(dm);
  }
}

/** @brief After any hub PUBACK: capacity, and possibly a ticket, is free for queued messages. */
static void on_publish_completed(void* user_ctx)
{
  az_iot_mqttv5_direct_method_client* dm = (az_iot_mqttv5_direct_method_client*)user_ctx;
  if (DI(dm).skip_next_drain)
  {
    /* This PUBACK failed one of ours: wait for a later event. */
    DI(dm).skip_next_drain = false;
    return;
  }
  flush_outbound_queue(dm);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Drop every token, invocation and reply of the identity the client was bound to.
 *
 * They belong to the previous hub and device id: published under new ones they would answer
 * nothing, and an exec for an old token can no longer arrive. A request the application still holds
 * is refused by respond() afterwards.
 */
static void discard_previous_identity_state(az_iot_mqttv5_direct_method_client* dm)
{
  size_t dropped = 0;
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    if (DI(dm).ready_pool[i]._internal.in_use || DI(dm).req_pool[i]._internal.in_use)
    {
      dropped++;
    }
    DI(dm).ready_pool[i]._internal.in_use = false;
    DI(dm).ready_pool[i]._internal.probe_ack_state = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
    DI(dm).req_pool[i]._internal.in_use = false;
    DI(dm).result_state[i] = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
  }
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX; ++i)
  {
    if (DI(dm).notice_queue[i]._internal.state != AZ_IOT_MQTTV5_DM_OUTBOUND_NONE)
    {
      dropped++;
    }
    DI(dm).notice_queue[i]._internal.state = AZ_IOT_MQTTV5_DM_OUTBOUND_NONE;
  }
  if (dropped > 0u)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "the hub or device id changed; dropping %u token(s), invocation(s) and reply(ies) of "
        "the previous identity",
        (unsigned)dropped);
  }
}

/**
 * @brief FNV-1a hash of the hub host name and device id, separated by a NUL.
 *
 * Identifies the identity retained replies belong to; NULL parts hash as empty.
 */
static uint64_t identity_fingerprint(const char* hub_hostname, const char* device_id)
{
  uint64_t hash = DM_FNV_OFFSET_BASIS;
  const char* parts[] = { hub_hostname, device_id };
  for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); ++p)
  {
    for (const char* c = parts[p]; c != NULL && *c != '\0'; ++c)
    {
      hash = (hash ^ (uint8_t)*c) * DM_FNV_PRIME;
    }
    /* The separator: hashes a NUL byte, so ("ab","c") and ("a","bc") differ. */
    hash *= DM_FNV_PRIME;
  }
  return hash;
}

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_mqttv5_direct_method_client* client = (az_iot_mqttv5_direct_method_client*)owner;

  const char* device_id = az_iot_connection_client__device_id(conn);
  if (!device_id)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "the connection has no device id to build the method topics from");
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  const char* inbound_parts[] = { "ih/", device_id, "/dev/methods" };
  const char* outbound_parts[] = { "ih/", device_id, "/srv/methods" };
  if (az_iot_span_writer_build_str(
          AZ_SPAN_FROM_BUFFER(DI(client).inbound_topic), NULL, inbound_parts, DM_TOPIC_PART_COUNT)
          != AZ_IOT_OK
      || az_iot_span_writer_build_str(
             AZ_SPAN_FROM_BUFFER(DI(client).outbound_topic),
             NULL,
             outbound_parts,
             DM_TOPIC_PART_COUNT)
          != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "device id '%s' does not fit the %u byte method topic buffers; raise "
        "AZ_IOT_MQTTV5_DM_TOPIC_MAX",
        device_id,
        (unsigned)AZ_IOT_MQTTV5_DM_TOPIC_MAX);
    DI(client).inbound_topic[0] = '\0';
    DI(client).outbound_topic[0] = '\0';
    discard_previous_identity_state(client);
    return AZ_IOT_ERR_INTERNAL;
  }

  /* Replies are tied to the hub and device id they were received under: DPS can change either. */
  uint64_t const identity
      = identity_fingerprint(az_iot_connection_client_get_iothub_address(conn), device_id);
  if (DI(client).identity_bound && DI(client).bound_identity != identity)
  {
    discard_previous_identity_state(client);
  }
  DI(client).bound_identity = identity;
  DI(client).identity_bound = true;

  /* No SUBSCRIBE: ih/{device_id}/dev/# from the presence handshake already
   * covers this. */
  return az_iot_connection_client__register_inbound_handler(
      conn, DI(client).inbound_topic, on_method_message, client);
}

AZ_NODISCARD az_iot_result az_iot_mqttv5_direct_method_client_init(
    az_iot_mqttv5_direct_method_client* client,
    az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_result result
      = az_iot_connection_client__require_profile(conn, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  if (result != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return result;
  }

  memset(client, 0, sizeof(*client));
  DI(client).conn = conn;
  DI(client).rng_state = az_iot_time_mono_ms();
  /* Seeded rather than started from zero: deinit() zeroes the client, so a
   * fresh init would otherwise reissue the same {slot, seq} a request from the
   * previous lifetime still names. */
  DI(client).next_seq = (uint32_t)az_iot_time_mono_ms();

  result = az_iot_connection_client__register_feature_client_bind(conn, client, bind_topics);
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__reserve_pubacks(conn, client, DM_PUBACK_RESERVATION);
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__add_state_observer(conn, on_connection_state, client);
  }
  if (result == AZ_IOT_OK)
  {
    result = az_iot_connection_client__add_publish_completion_observer(
        conn, on_publish_completed, client);
  }
  if (result != AZ_IOT_OK)
  {
    az_iot_connection_client__remove_publish_completion_observer(
        conn, on_publish_completed, client);
    (void)az_iot_connection_client__remove_state_observer(conn, on_connection_state, client);
    az_iot_connection_client__release_pubacks(conn, client);
    az_iot_connection_client__release_profile(conn);
    (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
    az_iot_connection_client__unregister_feature_client_bind(conn, client);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_mqttv5_direct_method_client_deinit(az_iot_mqttv5_direct_method_client* client)
{
  if (!client)
  {
    return;
  }
  (void)az_iot_connection_client__remove_state_observer(
      DI(client).conn, on_connection_state, client);
  az_iot_connection_client__remove_publish_completion_observer(
      DI(client).conn, on_publish_completed, client);
  az_iot_connection_client__unregister_feature_client_bind(DI(client).conn, client);
  /* Pending PUBACK callbacks point into this client. */
  az_iot_connection_client__abandon_pubacks(DI(client).conn, client);
  az_iot_connection_client__release_pubacks(DI(client).conn, client);
  az_iot_connection_client__release_profile(DI(client).conn);
  (void)az_iot_connection_client__remove_subscriptions_for(DI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(DI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_mqttv5_direct_method_client_set_probe_handler(
    az_iot_mqttv5_direct_method_client* client,
    az_iot_mqttv5_direct_method_probe_callback cb,
    void* user_ctx)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  DI(client).probe_handler = cb;
  DI(client).probe_handler_ctx = user_ctx;
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_mqttv5_direct_method_client_register_method(
    az_iot_mqttv5_direct_method_client* client,
    const char* method_name,
    uint32_t minimum_execution_seconds,
    az_iot_direct_method_handler_callback handler,
    void* user_ctx)
{
  if (client == NULL || method_name == NULL || method_name[0] == '\0' || handler == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  size_t name_len = strlen(method_name);
  if (name_len >= AZ_IOT_DM_METHOD_NAME_MAX)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "method name '%s' does not fit %u bytes; a probe could never match "
        "it "
        "anyway",
        method_name,
        (unsigned)AZ_IOT_DM_METHOD_NAME_MAX);
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_mqttv5_direct_method_registration* entry = method_find(client, method_name);
  if (entry == NULL)
  {
    for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_METHODS; ++i)
    {
      if (!DI(client).methods[i]._internal.in_use)
      {
        entry = &DI(client).methods[i];
        break;
      }
    }
  }
  if (entry == NULL)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "cannot declare '%s', all %d method slots are taken; raise "
        "AZ_IOT_MQTTV5_DM_MAX_METHODS",
        method_name,
        (int)AZ_IOT_MQTTV5_DM_MAX_METHODS);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  RI(entry).in_use = true;
  memcpy(RI(entry).name, method_name, name_len + 1u);
  RI(entry).minimum_execution_seconds = minimum_execution_seconds;
  RI(entry).handler = handler;
  RI(entry).handler_ctx = user_ctx;
  return AZ_IOT_OK;
}

az_iot_result az_iot_mqttv5_direct_method_client_unregister_method(
    az_iot_mqttv5_direct_method_client* client,
    const char* method_name)
{
  if (client == NULL || method_name == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_mqttv5_direct_method_registration* entry = method_find(client, method_name);
  if (entry == NULL)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  memset(entry, 0, sizeof(*entry));
  return AZ_IOT_OK;
}

az_iot_result az_iot_mqttv5_direct_method_respond(
    az_iot_mqttv5_direct_method_client* client,
    az_iot_direct_method_request request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len)
{
  if (client == NULL || (payload_len > 0 && payload == NULL))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "respond() needs the client that delivered the invocation, and a "
        "payload whenever payload_len is not zero");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (RI(&request).profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "this request was delivered by the mqttv3 client");
    return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH;
  }

  az_iot_mqttv5_direct_method_client* dm = client;
  /* Covers three cases at once, none of which may reach the wire: already
   * answered, reclaimed when the budget ran out, and reclaimed then handed to
   * another invocation -- the last would otherwise publish under that call's
   * correlation id. */
  size_t index = 0;
  az_iot_direct_method_slot* slot = request_resolve(dm, request, &index);
  if (slot == NULL)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "respond() called on a request that is no longer live -- "
        "it was already "
        "answered, or its slot was reclaimed when the response budget ran out");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  if (payload_len > AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "a %u byte result body does not fit the framing buffer; send a "
        "shorter "
        "one, or raise AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX past %u",
        (unsigned)payload_len,
        (unsigned)AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX);
    /* The slot stays held. Nothing was sent and nothing consumed the request,
     * so the obvious recovery -- answer again with a body that fits -- has to
     * still be open. */
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  /* The caller's response timer has been running since the service published
   * the exec, and a result that lands after it fires is discarded -- so send
   * only before the deadline, which reserves the safety margin for transit. */
  if (exec_budget_is_spent(dm, index))
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "'%s' finished at or after its result deadline; sending no "
        "result",
        RI(slot).method_name);
    RI(slot).in_use = false;
    return AZ_IOT_ERR_TIMEOUT;
  }

  size_t frame_len = 0;
  az_iot_result result = az_iot_dm_proto_encode_result(
      DI(dm).result_frame[index],
      sizeof(DI(dm).result_frame[index]),
      (int32_t)status_code,
      payload,
      payload_len,
      &frame_len);
  if (result != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "could not encode the result for '%s' (%s)",
        RI(slot).method_name,
        az_iot_result_to_string(result));
    RI(slot).in_use = false;
    return result;
  }

  DI(dm).result_frame_len[index] = frame_len;
  result = publish_result(dm, index, false);
  if (result == AZ_IOT_ERR_NOT_CONNECTED)
  {
    /* The handler may have run between CONNACK and the birth-ack, or every
     * ticket is awaiting a PUBACK: keep the slot and send the result on the
     * next CONNECTED or PUBACK. */
    AZ_IOT_LOG_INFOF(
        AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD,
        "holding the result of '%s' until it can be sent",
        RI(slot).method_name);
    return AZ_IOT_OK;
  }
  return result;
}
