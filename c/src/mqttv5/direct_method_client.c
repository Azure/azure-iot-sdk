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
#include <stddef.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv5/az_iot_direct_method_client.h"

#include "internal/connection_client_internal.h"
#include "internal/direct_method_codec.h"
#include "internal/log_internal.h"
#include "internal/reconnect.h"
#include "internal/span_writer.h"

#define DI(d) ((d)->_internal)
#define RI(r) ((r)->_internal)

#define DM_CONTENT_TYPE "application/protobuf"

/* Only version 1 of each phase exists. The version is part of the dispatch key:
 * a later "exec:2" may carry a different payload, so treating it as an exec
 * would decode the wrong shape. */
#define DM_TYPE_VERSION "1"

/* The service starts a timer and waits; a probe-ack or result with under a
 * second of budget left has already missed that window, so it is dropped rather
 * than published into a request the service has given up on. */
#define DM_MIN_USEFUL_BUDGET_SECONDS 1u

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

/* The public header has to size the result buffer without being able to see the
 * codec's internal header, so it carries its own copy of the framing overhead.
 * This is the only place both are in scope; a mismatch would silently undersize
 * the buffer, so fail the build instead. */
typedef char az_iot_dm_result_overhead_agrees
    [(AZ_IOT_MQTTV5_DM_RESULT_FRAME_OVERHEAD == AZ_IOT_DM_PROTO_RESULT_OVERHEAD) ? 1 : -1];

/* The concurrency limit is meant to be tuned down for small devices, but a
 * limit of zero refuses every probe and makes the client inert. */
typedef char az_iot_dm_concurrency_is_usable[(AZ_IOT_MQTTV5_DM_MAX_CONCURRENT >= 1) ? 1 : -1];

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

static az_iot_mqttv5_direct_method_ready_slot* ready_acquire(az_iot_mqttv5_direct_method_client* dm)
{
  for (size_t i = 0; i < AZ_IOT_MQTTV5_DM_MAX_CONCURRENT; ++i)
  {
    az_iot_mqttv5_direct_method_ready_slot* slot = &DI(dm).ready_pool[i];
    if (!RI(slot).in_use)
    {
      memset(slot, 0, sizeof(*slot));
      RI(slot).in_use = true;
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
      /* Skips 0 on wrap, so a zeroed request can never match a live slot. */
      DI(dm).next_seq++;
      if (DI(dm).next_seq == 0u)
      {
        DI(dm).next_seq = 1u;
      }
      s->_internal.seq = DI(dm).next_seq;
      s->_internal.in_use = true;
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
      "mqttv5_direct_method: dropping an invocation, all %d concurrent slots are taken by requests "
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
  if (!s->_internal.in_use || s->_internal.seq != request._internal.seq)
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

/* A QoS 1 PUBACK arrives later, from do_work(), so a publish call reporting
 * success only means the adapter accepted the message. Without this, a broker
 * that refuses one is invisible here and the caller simply times out.
 *
 * user_ctx is the phase's `type` string literal and never the client: this can
 * fire after the feature client has been deinitialized, and a literal outlives
 * everything. */
static void on_publish_ack(az_iot_result status, void* user_ctx)
{
  if (status != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        "mqttv5_direct_method: the broker rejected a '%s' message (%s); the service will not see "
        "it",
        (const char*)user_ctx,
        az_iot_result_to_string(status));
  }
}

/* Publish one device-to-service message. Every phase shares this envelope; only
 * the `type`, the payload and the expiry differ. */
static az_iot_result publish_typed(
    az_iot_mqttv5_direct_method_client* dm,
    const char* type_value,
    const uint8_t* payload,
    size_t payload_len,
    const uint8_t* request_id,
    uint32_t expiry_seconds)
{
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
  out.user_properties = &type_prop;
  out.user_properties_count = 1;
  out.correlation_data = request_id;
  out.correlation_data_len = AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN;
  out.content_type = DM_CONTENT_TYPE;
  out.message_expiry_seconds = expiry_seconds;

  az_iot_result result
      = az_iot_connection_client__publish(DI(dm).conn, &out, on_publish_ack, (void*)type_value);
  if (result == AZ_IOT_ERR_NOT_SUPPORTED)
  {
    /* The message went out; only the pending-ack table was full, so the PUBACK
     * will be absorbed silently. Losing that observability is not a send
     * failure, and reporting one would have the caller treat a delivered
     * result as lost. */
    AZ_IOT_LOG_WARNF(
        "mqttv5_direct_method: sent '%s' without ack tracking, the pending-ack table is full",
        type_value);
    return AZ_IOT_OK;
  }
  if (result != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "mqttv5_direct_method: could not publish a '%s' message (%s)",
        type_value,
        az_iot_result_to_string(result));
  }
  return result;
}

/* Best-effort advisory that a ready token ended before execution began. The
 * service uses it to fail the caller early instead of waiting out the response
 * timeout, so losing one costs latency, not correctness. */
static void publish_abandon(
    az_iot_mqttv5_direct_method_client* dm,
    const az_iot_mqttv5_direct_method_ready_slot* slot,
    az_iot_dm_proto_abandon_reason reason)
{
  uint8_t frame[AZ_IOT_DM_PROTO_ABANDON_MAX];
  size_t frame_len = 0;
  az_iot_result encoded = az_iot_dm_proto_encode_abandon(
      frame, sizeof(frame), RI(slot).ready_id, AZ_IOT_MQTTV5_DM_READY_ID_LEN, reason, &frame_len);
  if (encoded != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        "mqttv5_direct_method: could not encode the abandon for '%s' (%s); the service will wait "
        "out "
        "its response timeout instead",
        RI(slot).method_name,
        az_iot_result_to_string(encoded));
    return;
  }
  (void)publish_typed(
      dm,
      "abandon:" DM_TYPE_VERSION,
      frame,
      frame_len,
      RI(slot).request_id,
      RI(slot).response_timeout_seconds);
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
    if (!RI(slot).in_use || now < RI(slot).expires_at_ms)
    {
      continue;
    }
    AZ_IOT_LOG_WARNF(
        "mqttv5_direct_method: no exec arrived for '%s' within its ready wait; abandoning the "
        "token",
        RI(slot).method_name);
    /* Committed out of the ready state before the advisory goes out, so the
     * message can never claim a token that might still run. */
    az_iot_mqttv5_direct_method_ready_slot expired = *slot;
    RI(slot).in_use = false;
    publish_abandon(dm, &expired, AZ_IOT_DM_PROTO_ABANDON_READY_WAIT_TIMEOUT);
  }
}

/* True once a result for this slot could no longer reach the caller in time.
 *
 * Shared with respond() so the sweep below and the send path cannot disagree
 * about when an invocation stopped being answerable. A budget of 0 means the
 * exec carried no expiry at all, which is not an exhausted one. */
static bool exec_budget_is_spent(const az_iot_mqttv5_direct_method_client* dm, size_t index)
{
  return DI(dm).exec_budget_seconds[index] > 0u
      && remaining_budget_seconds(
             DI(dm).exec_budget_seconds[index], DI(dm).exec_received_at_ms[index])
      < DM_MIN_USEFUL_BUDGET_SECONDS;
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
    AZ_IOT_LOG_WARNF(
        "mqttv5_direct_method: '%s' was never answered and its response budget has run out; "
        "reclaiming its slot",
        RI(s).method_name);
    RI(s).in_use = false;
  }
}

/* ------------------------------------------------------------------------- */
/* inbound dispatch                                                          */
/* ------------------------------------------------------------------------- */

/* Value of the `type` user property, or NULL when the message carries none. */
static const char* message_type(const az_iot_mqtt_message* msg)
{
  for (size_t i = 0; i < msg->user_properties_count; ++i)
  {
    if (msg->user_properties[i].key && strcmp(msg->user_properties[i].key, "type") == 0)
    {
      return msg->user_properties[i].value;
    }
  }
  return NULL;
}

/* True when `type_value` is exactly "{step}:1". */
static bool dm_msg_type_is(const char* type_value, const char* step)
{
  size_t step_len = strlen(step);
  return strncmp(type_value, step, step_len) == 0 && type_value[step_len] == ':'
      && strcmp(type_value + step_len + 1, DM_TYPE_VERSION) == 0;
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
    AZ_IOT_LOG_WARN("mqttv5_direct_method: dropping a probe whose payload is not a Probe message");
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
        "mqttv5_direct_method: the connect budget for '%s' ran out while the probe was being "
        "answered; sending no probe-ack",
        method_name);
    if (slot != NULL)
    {
      RI(slot).in_use = false;
    }
    return;
  }

  uint8_t frame[AZ_IOT_DM_PROTO_PROBE_ACK_MAX];
  size_t frame_len = 0;
  az_iot_result encoded;

  if (slot != NULL)
  {
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

    encoded = az_iot_dm_proto_encode_probe_ack_ready(
        frame, sizeof(frame), RI(slot).ready_id, AZ_IOT_MQTTV5_DM_READY_ID_LEN, &frame_len);
  }
  else
  {
    encoded = az_iot_dm_proto_encode_probe_ack_rejected(
        frame, sizeof(frame), (az_iot_dm_proto_rejected_reason)answer, &frame_len);
  }

  if (encoded != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        "mqttv5_direct_method: could not encode the probe-ack for '%s' (%s); the caller will wait "
        "out its connect timeout instead",
        method_name,
        az_iot_result_to_string(encoded));
    if (slot != NULL)
    {
      RI(slot).in_use = false;
    }
    return;
  }

  az_iot_result sent
      = publish_typed(dm, "probe-ack:" DM_TYPE_VERSION, frame, frame_len, request_id, remaining);
  if (sent != AZ_IOT_OK && slot != NULL)
  {
    /* Nothing reached the service, so no token was ever promised, and holding
     * one would only consume capacity until its wait ran out. */
    RI(slot).in_use = false;
  }
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
        "mqttv5_direct_method: ignoring an exec with no matching ready token -- it was never "
        "accepted, it already ran, or its ready wait ran out");
    return;
  }

  az_iot_dm_proto_exec exec;
  if (az_iot_dm_proto_decode_exec(msg->payload, msg->payload_len, &exec) != AZ_IOT_OK)
  {
    /* The token stays live: a redelivery of the same exec may still decode. */
    AZ_IOT_LOG_WARN("mqttv5_direct_method: dropping an exec whose payload is not an Exec message");
    return;
  }

  if (exec.ready_id_len != AZ_IOT_MQTTV5_DM_READY_ID_LEN
      || memcmp(exec.ready_id, RI(slot).ready_id, AZ_IOT_MQTTV5_DM_READY_ID_LEN) != 0)
  {
    AZ_IOT_LOG_WARN(
        "mqttv5_direct_method: ignoring an exec whose ready id does not match the current token");
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
        "mqttv5_direct_method: only %u second(s) of budget left for '%s', at or under the %u "
        "second "
        "admission floor; not starting it",
        (unsigned)exec_budget,
        RI(&abandoned).method_name,
        (unsigned)admission_floor);
    publish_abandon(dm, &abandoned, AZ_IOT_DM_PROTO_ABANDON_INSUFFICIENT_TIME);
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
    publish_abandon(dm, &abandoned, AZ_IOT_DM_PROTO_ABANDON_UNSPECIFIED);
    return;
  }

  memcpy(RI(req_slot).method_name, RI(slot).method_name, strlen(RI(slot).method_name) + 1u);
  memcpy(RI(req_slot).correlation_data, request_id, AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN);
  RI(req_slot).correlation_data_len = AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN;
  DI(dm).exec_budget_seconds[index] = exec_budget;
  DI(dm).exec_received_at_ms[index] = received_at_ms;

  /* The token is single-use: consumed here, so a redelivered exec cannot run
   * the method a second time. Its handler is read out first -- the slot is
   * free for the next probe the moment it is released. */
  az_iot_direct_method_handler_callback handler = RI(slot).handler;
  void* handler_ctx = RI(slot).handler_ctx;
  RI(slot).in_use = false;

  handler(req, RI(req_slot).method_name, exec.params, exec.params_len, handler_ctx);
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

  const char* type_value = message_type(msg);
  if (type_value == NULL)
  {
    AZ_IOT_LOG_WARN(
        "mqttv5_direct_method: dropping a method message with no `type` property -- the phase is "
        "what says whether it is a probe or an authorization to run, and neither is a safe guess");
    return;
  }

  bool is_probe = dm_msg_type_is(type_value, "probe");
  if (!is_probe && !dm_msg_type_is(type_value, "exec"))
  {
    AZ_IOT_LOG_WARNF(
        "mqttv5_direct_method: ignoring a method message of type '%s', which this client does not "
        "implement",
        type_value);
    return;
  }

  if (msg->correlation_data == NULL || msg->correlation_data_len != AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN)
  {
    AZ_IOT_LOG_WARNF(
        "mqttv5_direct_method: dropping a '%s' message whose correlation data is not a %u-byte "
        "request id",
        type_value,
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

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_mqttv5_direct_method_client* client = (az_iot_mqttv5_direct_method_client*)owner;

  const char* device_id = az_iot_connection_client__device_id(conn);
  if (!device_id)
  {
    AZ_IOT_LOG_ERROR(
        "mqttv5_direct_method: the connection has no device id to build the method topics from");
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
        "mqttv5_direct_method: device id '%s' does not fit the %u byte method topic buffers; raise "
        "AZ_IOT_MQTTV5_DM_TOPIC_MAX",
        device_id,
        (unsigned)AZ_IOT_MQTTV5_DM_TOPIC_MAX);
    DI(client).inbound_topic[0] = '\0';
    DI(client).outbound_topic[0] = '\0';
    return AZ_IOT_ERR_INTERNAL;
  }

  /* No SUBSCRIBE: ih/{device_id}/dev/# from the presence handshake already
   * covers this. */
  return az_iot_connection_client__register_inbound_handler(
      conn, DI(client).inbound_topic, on_method_message, client);
}

az_iot_result az_iot_mqttv5_direct_method_client_init(
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
  if (result != AZ_IOT_OK)
  {
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
  az_iot_connection_client__unregister_feature_client_bind(DI(client).conn, client);
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
        "mqttv5_direct_method: method name '%s' does not fit %u bytes; a probe could never match "
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
        "mqttv5_direct_method: cannot declare '%s', all %d method slots are taken; raise "
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
        "mqttv5_direct_method: respond() needs the client that delivered the invocation, and a "
        "payload whenever payload_len is not zero");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (RI(&request).profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    AZ_IOT_LOG_ERROR("mqttv5_direct_method: this request was delivered by the mqttv3 client");
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
        "mqttv5_direct_method: respond() called on a request that is no longer live -- "
        "it was already "
        "answered, or its slot was reclaimed when the response budget ran out");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  if (payload_len > AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX)
  {
    AZ_IOT_LOG_ERRORF(
        "mqttv5_direct_method: a %u byte result body does not fit the framing buffer; send a "
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
   * only while a window to arrive in is left. */
  uint32_t remaining = remaining_budget_seconds(
      DI(dm).exec_budget_seconds[index], DI(dm).exec_received_at_ms[index]);
  if (exec_budget_is_spent(dm, index))
  {
    AZ_IOT_LOG_WARNF(
        "mqttv5_direct_method: '%s' finished after its response timeout had elapsed; sending no "
        "result",
        RI(slot).method_name);
    RI(slot).in_use = false;
    return AZ_IOT_ERR_TIMEOUT;
  }

  size_t frame_len = 0;
  az_iot_result result = az_iot_dm_proto_encode_result(
      DI(dm).result_frame,
      sizeof(DI(dm).result_frame),
      (int32_t)status_code,
      payload,
      payload_len,
      &frame_len);
  if (result != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        "mqttv5_direct_method: could not encode the result for '%s' (%s)",
        RI(slot).method_name,
        az_iot_result_to_string(result));
    RI(slot).in_use = false;
    return result;
  }

  result = publish_typed(
      dm,
      "result:" DM_TYPE_VERSION,
      DI(dm).result_frame,
      frame_len,
      RI(slot).correlation_data,
      remaining);
  RI(slot).in_use = false;
  return result;
}
