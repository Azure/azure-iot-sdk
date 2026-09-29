// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_H
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Longest topic this client builds: "ih/" + device id + "/dev/methods". */
#ifndef AZ_IOT_MQTTV5_DM_TOPIC_MAX
#define AZ_IOT_MQTTV5_DM_TOPIC_MAX 192
#endif

/* Bytes of a result body this client can frame into a protobuf Result. The
 * frame is built in a buffer inside the client rather than on the heap, so this
 * is the hard limit on what az_iot_mqttv5_direct_method_respond() will send; a
 * larger body is refused with AZ_IOT_ERR_NOT_ENOUGH_SPACE. */
#ifndef AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX
#define AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX 512
#endif

/* Bytes the protobuf framing adds around a result body: a tag and a worst-case
 * ten-byte status varint, then a tag and a five-byte length varint. */
#define AZ_IOT_MQTTV5_DM_RESULT_FRAME_OVERHEAD 17u

/* The cap on concurrent direct method state machines this device will hold:
 * ready tokens awaiting their execute authorization, plus invocations the
 * application has not answered yet. A probe arriving at the cap is refused with
 * DEVICE_BUSY, creating no token and reserving nothing.
 *
 * Finite by construction -- it bounds arrays inside the client -- and meant to
 * be tuned to the device class. Override it at compile time (for example
 * -DAZ_IOT_MQTTV5_DM_MAX_CONCURRENT=2) before this header is included. It
 * defaults to the shared AZ_IOT_DM_MAX_INFLIGHT so a device that tunes only
 * that knob still gets a matching mqttv5 limit, but the two can be set apart:
 * mqttv3 has no probe phase and so nothing to hold in a ready state.
 *
 * It sizes az_iot_mqttv5_direct_method_client, so the SDK and the application
 * MUST be compiled with the same value. Defining it for the application alone
 * leaves the SDK writing past a struct the application sized smaller. Set it on
 * the whole build, not on one target.
 *
 * Each pool is sized to this value because either can hold the whole budget on
 * its own -- all tokens waiting, or all invocations running -- while their sum
 * is what the limit caps. */
#ifndef AZ_IOT_MQTTV5_DM_MAX_CONCURRENT
#define AZ_IOT_MQTTV5_DM_MAX_CONCURRENT AZ_IOT_DM_MAX_INFLIGHT
#endif

/* Distinct method names one client can declare. The protocol answers a probe
 * by name, so this is how many methods the device can advertise. */
#ifndef AZ_IOT_MQTTV5_DM_MAX_METHODS
#define AZ_IOT_MQTTV5_DM_MAX_METHODS 8
#endif

/* The protocol fixes both ids at 16 bytes: the request id is a binary UUID in
 * MQTT v5 Correlation Data, and the ready id is the device's half of the
 * single-use execution token. */
#define AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN 16u
#define AZ_IOT_MQTTV5_DM_READY_ID_LEN 16u

  /* Nothing received is ever truncated into a buffer. Each bound below either
   * cannot be exceeded or is answered before anything is stored:
   *
   *   request id / ready id  fixed by the protocol at 16 bytes. Correlation
   *                          data of any other length is dropped and logged; an
   *                          exec whose ready id is the wrong length or does
   *                          not match the live token is ignored and logged.
   *   method name            a name longer than AZ_IOT_DM_METHOD_NAME_MAX, or
   *                          one containing an embedded NUL, is refused at the
   *                          probe with ProbeAck{rejected: METHOD_NOT_FOUND},
   *                          so the service is told and the caller gets a
   *                          reason rather than a timeout.
   *   method parameters      never copied. The handler receives a view into the
   *                          received message, so there is no bound to exceed.
   *   topics                 built once at connect. A device id too long for
   *                          AZ_IOT_MQTTV5_DM_TOPIC_MAX fails the connect attempt
   *                          with a log naming the limit, rather than binding a
   *                          truncated topic.
   *   result body            outbound only. See
   *                          az_iot_mqttv5_direct_method_respond().
   */

  /**
   * @brief A delivery probe: the service asking whether this device can run a
   *        method, before it sends the arguments.
   *
   * Valid only for the duration of the probe callback.
   */
  typedef struct az_iot_mqttv5_direct_method_probe
  {
    /** NUL-terminated method name, from the probe payload. */
    const char* method_name;
    /** How long the caller will wait for a result once execution starts. */
    uint32_t response_timeout_seconds;
  } az_iot_mqttv5_direct_method_probe;

  /**
   * @brief What to answer a delivery probe with.
   *
   * A rejection is final for that invocation: the service reports the reason to
   * the caller and never sends the arguments, so nothing is retried and neither
   * side keeps any state for it.
   */
  typedef enum
  {
    /** Ready to run it. The service may follow with the arguments. */
    AZ_IOT_MQTTV5_DM_PROBE_ACCEPT = 0,
    /** This device has no handler for that name. */
    AZ_IOT_MQTTV5_DM_PROBE_REJECT_METHOD_NOT_FOUND = 1,
    /** The caller's response timeout is shorter than the handler needs. */
    AZ_IOT_MQTTV5_DM_PROBE_REJECT_INSUFFICIENT_TIME = 2,
    /** Already holding as many invocations as this device will take. */
    AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY = 3
  } az_iot_mqttv5_direct_method_probe_result;

  /**
   * @brief Answer a delivery probe.
   *
   * Called from do_work(), before any arguments exist. Answer promptly: the
   * service is holding the caller's connect timeout open, and this client will
   * not send an acceptance that is already too late to be acted on.
   */
  typedef az_iot_mqttv5_direct_method_probe_result (*az_iot_mqttv5_direct_method_probe_callback)(
      const az_iot_mqttv5_direct_method_probe* probe,
      void* user_ctx);

  /* One method this device has declared it implements, with the handler that
   * runs it. Opaque -- do NOT read _internal. */
  typedef struct az_iot_mqttv5_direct_method_registration
  {
    struct
    {
      bool in_use;
      char name[AZ_IOT_DM_METHOD_NAME_MAX];
      uint32_t minimum_execution_seconds;
      az_iot_direct_method_handler_callback handler;
      void* handler_ctx;
    } _internal;
  } az_iot_mqttv5_direct_method_registration;

  /* One accepted probe waiting for its execute authorization. Opaque -- do NOT
   * read _internal. Lives inside the client so the ready state costs no
   * allocation. */
  typedef struct az_iot_mqttv5_direct_method_ready_slot
  {
    struct
    {
      bool in_use;
      uint8_t request_id[AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN];
      uint8_t ready_id[AZ_IOT_MQTTV5_DM_READY_ID_LEN];
      char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
      uint32_t response_timeout_seconds;
      /* Copied from the registration when the probe was admitted, so the exec
       * is judged against the terms the token was issued under even if the
       * method is re-declared in between. */
      uint32_t minimum_execution_seconds;
      /* Snapshotted with the minimum above, for the same reason: the exec runs
       * the handler that was declared when the device said yes. */
      az_iot_direct_method_handler_callback handler;
      void* handler_ctx;
      /** Monotonic milliseconds after which the token is abandoned. */
      uint64_t expires_at_ms;
    } _internal;
  } az_iot_mqttv5_direct_method_ready_slot;

  /* MQTTv5 direct methods (MQTT v5).
   *
   * Two flat topics, neither carrying a method name:
   *
   *   Inbound   "ih/{device_id}/dev/methods"   probe:1, exec:1
   *   Outbound  "ih/{device_id}/srv/methods"   probe-ack:1, abandon:1, result:1
   *
   * The phase is a `type` user property, the request id is 16 bytes of MQTT v5
   * correlation data, and every payload is protobuf
   * (common/Protos/directmethods.proto). The method name arrives in the probe
   * payload, so this client holds it until the matching exec authorizes the
   * call.
   *
   * No subscription of its own: delivery is covered by the presence
   * handshake's "ih/{device_id}/dev/#".
   */
  typedef struct az_iot_mqttv5_direct_method_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_mqttv5_direct_method_probe_callback probe_handler;
      void* probe_handler_ctx;
      az_iot_direct_method_slot req_pool[AZ_IOT_MQTTV5_DM_MAX_CONCURRENT];
      /* Parallel to req_pool: the execution budget each in-flight invocation
       * was admitted with, and when it was admitted. Held here rather than on
       * az_iot_direct_method_slot because the two generations time out on
       * different inputs -- mqttv5 on the budget the service declared, mqttv3 on a
       * locally configured one -- so neither belongs on the shared type. */
      uint32_t exec_budget_seconds[AZ_IOT_MQTTV5_DM_MAX_CONCURRENT];
      uint64_t exec_received_at_ms[AZ_IOT_MQTTV5_DM_MAX_CONCURRENT];
      /* Bumped on every acquire and copied into the request handed out, so a
       * request naming a slot that has since been reused no longer matches. */
      uint32_t next_seq;
      /* Next req_pool index to try. Allocation cycles rather than always taking
       * the lowest free slot, so a slot just reclaimed from an application that
       * never answered is the last one reused, not the first. */
      size_t next_slot;
      /* Ready tokens and in-flight invocations draw on the one
       * AZ_IOT_MQTTV5_DM_MAX_CONCURRENT budget, so a probe is only accepted when
       * a request slot will be free to run it. */
      az_iot_mqttv5_direct_method_ready_slot ready_pool[AZ_IOT_MQTTV5_DM_MAX_CONCURRENT];
      az_iot_mqttv5_direct_method_registration methods[AZ_IOT_MQTTV5_DM_MAX_METHODS];
      /* Built by the connect-time bind callback, once the assigned device id is
       * authoritative. Empty before the first connect. */
      char inbound_topic[AZ_IOT_MQTTV5_DM_TOPIC_MAX];
      char outbound_topic[AZ_IOT_MQTTV5_DM_TOPIC_MAX];
      /* Ready-id generator state. Uniqueness, not unpredictability: the token
       * is single-use and the service only knows it because we sent it. */
      uint64_t rng_state;
      uint8_t
          result_frame[AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX + AZ_IOT_MQTTV5_DM_RESULT_FRAME_OVERHEAD];
    } _internal;
  } az_iot_mqttv5_direct_method_client;

  /**
   * @brief Initialize the MQTT v5 direct method client.
   *
   * The connection need not be open: this records that it must resolve to the
   * MQTT v5 profile, and the topics are built when it connects and the
   * assigned device id is known. A connection already known to be MQTTv3 -- a
   * direct connection, or a DPS one past assignment -- is rejected here with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict surfaces when
   * the connection resolves, which fails it before it reports CONNECTED. The
   * same error is returned when an mqttv3 client is already attached.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_direct_method_client_init(
      az_iot_mqttv5_direct_method_client* client,
      az_iot_connection_client* conn);

  void az_iot_mqttv5_direct_method_client_deinit(az_iot_mqttv5_direct_method_client* client);

  /**
   * @brief Declare a method this device implements, and the handler that runs
   *        it.
   *
   * A probe names a method, and the protocol expects the device to answer
   * whether it has that method before any arguments are sent. Nothing is
   * declared by default, so **a probe for an undeclared method is rejected with
   * METHOD_NOT_FOUND** and never reaches the application.
   *
   * That is what makes the probe phase worth its round trip. An unknown
   * invocation costs two small messages and no state: the arguments are never
   * transferred, no handler runs, and no capacity is held -- so a caller
   * spraying names it made up cannot starve the invocations this device does
   * serve.
   *
   * The name and its handler are declared together because neither is any use
   * alone: a declared name with nothing behind it is a promise the device
   * cannot keep, and a handler for a name the device never declared can never
   * be reached.
   *
   * **Name matching is an exact byte comparison** -- case-sensitive, with no
   * normalization. The protocol carries `method_name` as a UTF-8 protobuf
   * string and says nothing about how to match it, and the alternatives are
   * worse: case folding UTF-8 correctly is a Unicode problem, not a `tolower()`
   * loop, and a loose match risks running a method the caller did not ask for.
   * Declare the name exactly as it is registered service-side; ASCII names
   * avoid the question of which Unicode normal form each side used.
   *
   * @param minimum_execution_seconds How long this method needs to produce a
   *        result. Pass 0 when it does not need a floor, which admits any
   *        positive remaining budget. A probe whose response timeout cannot
   *        cover this is rejected with INSUFFICIENT_TIME rather than accepted
   *        and abandoned later, and an execute authorization that arrives with
   *        too little left is abandoned instead of started.
   *
   * Declaring the same name twice replaces the previous declaration rather than
   * consuming a second entry. Safe to call before or after the connection is
   * open; the registry is local.
   *
   * @return AZ_IOT_ERR_INVALID_ARG for a NULL @p handler, or a NULL, empty or
   *         over-long name; AZ_IOT_ERR_NOT_ENOUGH_SPACE when
   *         AZ_IOT_MQTTV5_DM_MAX_METHODS names are already declared.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_direct_method_client_register_method(
      az_iot_mqttv5_direct_method_client* client,
      const char* method_name,
      uint32_t minimum_execution_seconds,
      az_iot_direct_method_handler_callback handler,
      void* user_ctx);

  /**
   * @brief Withdraw a method, so probes for it are answered METHOD_NOT_FOUND
   *        again.
   *
   * Tokens already issued for it are left alone: the device agreed to run those
   * before the method was withdrawn.
   *
   * Not AZ_NODISCARD: withdrawing something that was never declared is not an
   * error worth forcing every caller to check.
   *
   * @return AZ_IOT_ERR_NOT_FOUND when the name was not declared.
   */
  az_iot_result az_iot_mqttv5_direct_method_client_unregister_method(
      az_iot_mqttv5_direct_method_client* client,
      const char* method_name);

  /**
   * @brief Set the handler that answers delivery probes.
   *
   * Optional, and consulted only for a method that is already declared: the
   * registry answers whether the device has the method, and this answers
   * whether it will take the work right now. Use it to decline while busy, on
   * low battery, or in a state where the method would fail anyway -- the caller
   * gets a reason instead of a timeout.
   *
   * With no probe handler, a declared method whose timing is feasible and for
   * which there is capacity is accepted.
   *
   * Not AZ_NODISCARD: a configuration setter (fails only on invalid arguments).
   */
  az_iot_result az_iot_mqttv5_direct_method_client_set_probe_handler(
      az_iot_mqttv5_direct_method_client* client,
      az_iot_mqttv5_direct_method_probe_callback cb,
      void* user_ctx);

  /**
   * @brief Answer an invocation, releasing its pool slot.
   *
   * Not AZ_NODISCARD: best-effort response send, commonly fire-and-forget.
   *
   * The slot is released on every outcome except the ones that reject the call
   * before it takes effect: invalid arguments, a handle this client did not
   * hand out, and a body that does not fit. That last one is worth answering
   * again with a shorter body; the others are programming errors.
   *
   * Nothing is sent to the service on any failure. It reports the invocation as
   * timed out, which is the truthful outcome -- the handler may already have
   * run, so this SDK must not claim otherwise.
   *
   * @p request names a slot rather than pointing at one, so a slot that has
   * since been reclaimed and handed to another invocation no longer matches and
   * the late answer is refused instead of being published under that other
   * call's correlation id.
   *
   * @return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH if @p request came from a
   *         mqttv3 client, AZ_IOT_ERR_INVALID_ARG if it was already answered, if
   *         its slot was reclaimed and reused, or if it never came from this
   *         client, AZ_IOT_ERR_NOT_ENOUGH_SPACE if
   *         @p payload_len exceeds AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX,
   *         AZ_IOT_ERR_TIMEOUT if the caller's response timeout ran out while
   *         the handler worked -- nothing is sent in that case, because the
   *         service has already given up.
   */
  az_iot_result az_iot_mqttv5_direct_method_respond(
      az_iot_mqttv5_direct_method_client* client,
      az_iot_direct_method_request request,
      int status_code,
      const uint8_t* payload,
      size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_H */
