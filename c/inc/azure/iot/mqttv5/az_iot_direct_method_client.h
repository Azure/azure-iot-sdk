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
#include "azure/iot/az_iot_delivery_tracker.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(AZ_IOT_MQTTV5_DM_TOPIC_MAX)
#error "AZ_IOT_MQTTV5_DM_TOPIC_MAX is fixed: see c/docs/client-configuration.md."
#endif

/** @brief Longest topic this client builds: "ih/" + device id + "/dev/methods". Fixed. */
#define AZ_IOT_MQTTV5_DM_TOPIC_MAX 192

#if defined(AZ_IOT_MQTTV5_DM_MAX_CONCURRENT) || defined(AZ_IOT_MQTTV5_DM_NOTICE_QUEUE_MAX) \
    || defined(AZ_IOT_MQTTV5_DM_MAX_METHODS) || defined(AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX)
#error \
    "AZ_IOT_MQTTV5_DM_MAX_CONCURRENT, _NOTICE_QUEUE_MAX, _MAX_METHODS and _RESULT_BODY_MAX are removed: set az_iot_mqttv5_direct_method_client_limits"
#endif

/* Bytes the protobuf framing adds around a result body: a tag and a worst-case
 * ten-byte status varint, then a tag and a five-byte length varint. */
#define AZ_IOT_MQTTV5_DM_RESULT_FRAME_OVERHEAD 17u

/** @brief Default az_iot_mqttv5_direct_method_client_limits::max_concurrent_invocations. */
#ifndef AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_CONCURRENT_INVOCATIONS
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_CONCURRENT_INVOCATIONS 4
#endif

/** @brief Default az_iot_mqttv5_direct_method_client_limits::max_methods. */
#ifndef AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_METHODS
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_METHODS 8
#endif

/** @brief Default az_iot_mqttv5_direct_method_client_limits::max_result_body_size. */
#ifndef AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_RESULT_BODY_SIZE
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_RESULT_BODY_SIZE 512
#endif

/** @brief Largest az_iot_mqttv5_direct_method_client_limits::max_concurrent_invocations. */
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_CONCURRENT_INVOCATIONS_LIMIT 255
/** @brief Largest az_iot_mqttv5_direct_method_client_limits::max_methods. */
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_METHODS_LIMIT 1024
/** @brief Largest az_iot_mqttv5_direct_method_client_limits::max_queued_notices. */
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_QUEUED_NOTICES_LIMIT 1024
/** @brief Largest az_iot_mqttv5_direct_method_client_limits::max_result_body_size: the IoT Hub
 * direct method payload limit, 128 KiB. */
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_RESULT_BODY_SIZE_LIMIT 131072

#if AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_CONCURRENT_INVOCATIONS < 1 \
    || AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_CONCURRENT_INVOCATIONS  \
        > AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_CONCURRENT_INVOCATIONS_LIMIT \
    || AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_METHODS < 1             \
    || AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_METHODS                 \
        > AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_METHODS_LIMIT                \
    || AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_RESULT_BODY_SIZE < 1    \
    || AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_RESULT_BODY_SIZE        \
        > AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_RESULT_BODY_SIZE_LIMIT
#error "AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_*: each must be from 1 to its _LIMIT"
#endif

  /**
   * @brief How much the client holds. 0 fields select the defaults.
   *
   * Run-time values: the client struct does not depend on them. Everything they count lives in
   * az_iot_mqttv5_direct_method_client_options::workspace.
   */
  typedef struct az_iot_mqttv5_direct_method_client_limits
  {
    /** Direct method state machines held at once: ready tokens awaiting their execute
     * authorization, plus invocations not answered yet. A probe past it is refused with
     * DEVICE_BUSY, creating no token and reserving nothing. The request ids of this many ended
     * tokens are also kept, so a redelivered probe is answered by its PUBACK alone, as one whose
     * token is live or whose invocation is in flight is. The client reserves twice this many of
     * the connection's limits.max_pending_publishes (AZ_IOT_MQTTV5_DM_RESERVED_PUBACKS()). Default
     * AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_CONCURRENT_INVOCATIONS (4), at most
     * AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_CONCURRENT_INVOCATIONS_LIMIT (255). */
    size_t max_concurrent_invocations;
    /** Method names one client can declare. Default
     * AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_METHODS (8), at most
     * AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_METHODS_LIMIT (1024). */
    size_t max_methods;
    /** Probe rejections and abandons kept until their PUBACK, to publish again after a
     * reconnect. Accepted probe-acks and results are kept with their token or invocation instead.
     * When all are in use, a new one is published untracked while CONNECTED and dropped while
     * not; both are advisory, and losing one leaves the caller to time out. Default
     * max_concurrent_invocations, at most
     * AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_QUEUED_NOTICES_LIMIT (1024). */
    size_t max_queued_notices;
    /** Bytes of a result body az_iot_mqttv5_direct_method_respond() sends; a larger one is refused
     * with AZ_IOT_ERR_NOT_ENOUGH_SPACE. The client keeps one result buffer per concurrent
     * invocation, so a result can wait for CONNECTED and be published again if its PUBACK is lost
     * with the connection. Default AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_RESULT_BODY_SIZE
     * (512), at most AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_MAX_RESULT_BODY_SIZE_LIMIT (131072). */
    size_t max_result_body_size;
  } az_iot_mqttv5_direct_method_client_limits;

  /** @brief Options for az_iot_mqttv5_direct_method_client_init(). */
  typedef struct az_iot_mqttv5_direct_method_client_options
  {
    /** Counts and sizes; 0 fields select the defaults. */
    az_iot_mqttv5_direct_method_client_limits limits;
    /** Required. The client's tokens, invocations, methods, notices and result buffers, owned by
     * the client until deinit(), which wipes it. At least
     * az_iot_mqttv5_direct_method_client_get_workspace_size(&limits) bytes, any alignment. */
    uint8_t* workspace;
    size_t workspace_size; /**< Bytes in workspace. */
  } az_iot_mqttv5_direct_method_client_options;

  /**
   * @brief Returns options with the default limits (0 fields) and no workspace.
   *
   * Set workspace and workspace_size before az_iot_mqttv5_direct_method_client_init().
   *
   * @return The options.
   */
  AZ_NODISCARD az_iot_mqttv5_direct_method_client_options
  az_iot_mqttv5_direct_method_client_options_default(void);

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
   *                          with a log naming the fixed limit, rather than
   *                          binding a truncated topic; use a shorter device id.
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

/* Bytes of the largest ProbeAck or Abandon frame. */
#define AZ_IOT_MQTTV5_DM_NOTICE_FRAME_MAX 20u

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
      /** Encoded ProbeAck{ready}. */
      uint8_t probe_ack_frame[AZ_IOT_MQTTV5_DM_NOTICE_FRAME_MAX];
      size_t probe_ack_frame_len;
      /** Delivery tracker entry of the probe-ack, plus one; 0: none. */
      size_t probe_ack_tracker_entry;
      /** The probe-ack was published at least once, so the service may know this token. */
      bool probe_ack_published;
      /** Connect budget of the probe, in seconds; 0 means unbounded. */
      uint32_t connect_budget_seconds;
      /** Monotonic milliseconds when the probe arrived. */
      uint64_t probe_received_at_ms;
    } _internal;
  } az_iot_mqttv5_direct_method_ready_slot;

  /* A ProbeAck{rejected} or Abandon kept until its PUBACK. Opaque -- do NOT
   * read _internal. */
  typedef struct az_iot_mqttv5_direct_method_notice
  {
    struct
    {
      /** Delivery tracker entry, plus one; 0: the notice is free. */
      size_t tracker_entry;
      /** Its `type` property ("probe-ack:1" or "abandon:1"); static. */
      const az_iot_mqtt_user_property* type;
      uint8_t request_id[AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN];
      uint8_t frame[AZ_IOT_MQTTV5_DM_NOTICE_FRAME_MAX];
      size_t frame_len;
      /** Message Expiry counted from budget_start_ms, in seconds; 0 means none, and the notice
       *  is kept at most the connection's max_unconfirmed_delivery_timeout_ms. */
      uint32_t budget_seconds;
      /** Monotonic milliseconds the budget is counted from. */
      uint64_t budget_start_ms;
    } _internal;
  } az_iot_mqttv5_direct_method_notice;

  /** @brief Internal: one invocation, in the workspace. */
  typedef struct az_iot_mqttv5_direct_method_invocation
  {
    az_iot_direct_method_slot slot;
    /* The execution budget it was admitted with, and when it was admitted. Held here rather than
     * on az_iot_direct_method_slot because the two generations time out on different inputs --
     * mqttv5 on the budget the service declared, mqttv3 on a locally configured one. */
    uint32_t exec_budget_seconds;
    uint64_t exec_received_at_ms;
    /* Monotonic milliseconds at and after which no result is published -- receipt time plus the
     * execution budget, less the safety margin; receipt time plus the connection's
     * max_unconfirmed_delivery_timeout_ms when the exec carried no expiry. */
    uint64_t exec_deadline_ms;
    /* seq of the last unanswered invocation reclaimed at its result deadline, so respond() on it
     * still returns AZ_IOT_ERR_TIMEOUT. */
    uint32_t expired_request_seq;
    /* Its encoded result, in the workspace (max_result_body_size plus
     * AZ_IOT_MQTTV5_DM_RESULT_FRAME_OVERHEAD bytes). The invocation stays taken until the result
     * is acknowledged by the broker or its result deadline passes, so a result whose PUBACK is
     * lost with the connection is published again on the next CONNECTED. */
    uint8_t* result_frame;
    size_t result_frame_len;
    /** Delivery tracker entry of the result, plus one; 0: not answered yet. */
    size_t result_tracker_entry;
  } az_iot_mqttv5_direct_method_invocation;

/** @brief limits.max_pending_publishes slots one client reserves: a probe-ack and a result or
 * abandon per invocation. */
#define AZ_IOT_MQTTV5_DM_RESERVED_PUBACKS(max_concurrent_invocations) \
  (2u * (size_t)(max_concurrent_invocations))

/**
 * @brief Workspace bytes for these limits, as a constant expression for static arrays. Pass the
 * values az_iot_mqttv5_direct_method_client_limits would hold, with the defaults written out in
 * place of 0. Equal to az_iot_mqttv5_direct_method_client_get_workspace_size() for the same values.
 */
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_WORKSPACE_SIZE(                                    \
    max_concurrent_invocations, max_methods, max_queued_notices, max_result_body_size)        \
  (15u                                                                                        \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                                                \
       (size_t)(max_concurrent_invocations) * sizeof(az_iot_mqttv5_direct_method_invocation)) \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                                                \
       (size_t)(max_concurrent_invocations) * sizeof(az_iot_mqttv5_direct_method_ready_slot)) \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                                                \
       (size_t)(max_concurrent_invocations) * AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN)                \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                                                \
       (size_t)(max_methods) * sizeof(az_iot_mqttv5_direct_method_registration))              \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                                                \
       (size_t)(max_queued_notices) * sizeof(az_iot_mqttv5_direct_method_notice))             \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                                                \
       ((size_t)(max_concurrent_invocations) + (size_t)(max_queued_notices))                  \
       * sizeof(az_iot_delivery_tracker_entry))                                               \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                                                \
       (size_t)(max_concurrent_invocations)                                                   \
       * ((size_t)(max_result_body_size) + AZ_IOT_MQTTV5_DM_RESULT_FRAME_OVERHEAD)))

/** @brief Workspace bytes for the default limits, as a constant expression for static arrays. */
#define AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_WORKSPACE_SIZE            \
  AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_WORKSPACE_SIZE(                         \
      AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_CONCURRENT_INVOCATIONS, \
      AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_METHODS,                \
      AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_CONCURRENT_INVOCATIONS, \
      AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_DEFAULT_MAX_RESULT_BODY_SIZE)

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
      /* limits.max_concurrent_invocations entries, in the workspace. */
      az_iot_mqttv5_direct_method_invocation* invocations;
      /* Bumped on every acquire and copied into the request handed out, so a
       * request naming a slot that has since been reused no longer matches. */
      uint32_t next_seq;
      /* Next invocations index to try. Allocation cycles rather than always taking
       * the lowest free slot, so a slot just reclaimed from an application that
       * never answered is the last one reused, not the first. */
      size_t next_slot;
      /* Ready tokens and in-flight invocations draw on the one
       * limits.max_concurrent_invocations budget, so a probe is only accepted when
       * an invocation will be free to run it. In the workspace. */
      az_iot_mqttv5_direct_method_ready_slot* ready_pool;
      /* Request ids of the last limits.max_concurrent_invocations ended tokens, oldest overwritten
       * first, in the workspace: a probe redelivered after its token ended is not answered. */
      uint8_t (*ended_request_ids)[AZ_IOT_MQTTV5_DM_REQUEST_ID_LEN];
      /* ended_request_ids entries filled, and the next one to write. */
      size_t ended_request_count;
      size_t next_ended_request;
      /* limits.max_methods entries, in the workspace. */
      az_iot_mqttv5_direct_method_registration* methods;
      /* Built by the connect-time bind callback, once the assigned device id is
       * authoritative. Empty before the first connect. */
      char inbound_topic[AZ_IOT_MQTTV5_DM_TOPIC_MAX];
      char outbound_topic[AZ_IOT_MQTTV5_DM_TOPIC_MAX];
      /* Ready-id generator state. Uniqueness, not unpredictability: the token
       * is single-use and the service only knows it because we sent it. */
      uint64_t rng_state;
      /* Probe rejections and abandons kept until their PUBACK: limits.max_queued_notices
       * entries, in the workspace. */
      az_iot_mqttv5_direct_method_notice* notices;
      /* Resolved: no field is 0. */
      az_iot_mqttv5_direct_method_client_limits limits;
      /* PUBACK slots of the reservation kept for probe-acks and results: one per invocation. */
      uint8_t slots_kept_for_replies;
      uint8_t* workspace;
      size_t workspace_size;
      /* Sends, resends and expires the probe-acks, results and notices. */
      az_iot_delivery_tracker tracker;
    } _internal;
  } az_iot_mqttv5_direct_method_client;

  /**
   * @brief Workspace bytes az_iot_mqttv5_direct_method_client_init() needs for @p limits (0
   * fields: defaults).
   *
   * @return The size, or 0 when @p limits is NULL or a value is above its maximum.
   */
  AZ_NODISCARD size_t az_iot_mqttv5_direct_method_client_get_workspace_size(
      const az_iot_mqttv5_direct_method_client_limits* limits);

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
   *
   * Reserves AZ_IOT_MQTTV5_DM_RESERVED_PUBACKS(limits.max_concurrent_invocations) (twice that
   * limit) of the connection's limits.max_pending_publishes slots until deinit. Returns
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE if they do not fit, or AZ_IOT_ERR_BUSY if
   * tracked publishes in flight still occupy them (retry once acknowledged).
   * Also observes the connection state, to publish held messages on CONNECTED:
   * returns AZ_IOT_ERR_BUSY when called from a connection state callback.
   *
   * Probe-acks, results and abandons are QoS 1 and kept until their PUBACK:
   * one that ends without a PUBACK (the connection closed, or the adapter lost
   * it) is published again on the next CONNECTED until its deadline: a probe-ack
   * until its connect budget (the probe's Message Expiry) or its token's ready wait
   * ends, whichever is first; a result until its result deadline (with no exec
   * expiry, max_unconfirmed_delivery_timeout_ms after the exec); an abandon or
   * rejection until its budget ends (with none, max_unconfirmed_delivery_timeout_ms);
   * one the broker refuses (AZ_IOT_ERR_PUBLISH_REFUSED) is not. A send the
   * adapter refuses (BUSY, NOT_CONNECTED, a transport error) leaves it queued for
   * a later CONNECTED, hub PUBACK (of any publish) or do_work(); one it can never
   * send (AZ_IOT_ERR_INVALID_ARG) is dropped. A probe-ack or result is never sent
   * untracked: with every pending-PUBACK slot in use it waits for one. They are
   * kept in memory only, so a device restart loses them; they are dropped when
   * the hub or device id changes.
   *
   * @p options is read during the call only; its workspace is used until deinit(). Returns
   * AZ_IOT_ERR_INVALID_ARG when @p options is NULL, a limit is above its maximum or the workspace
   * is NULL, and AZ_IOT_ERR_NOT_ENOUGH_SPACE when the workspace is too small; both workspace
   * errors log the size needed.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_direct_method_client_init(
      az_iot_mqttv5_direct_method_client* client,
      az_iot_connection_client* conn,
      const az_iot_mqttv5_direct_method_client_options* options);

  /** @brief Detaches the client and wipes its workspace. */
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
   *         limits.max_methods names are already declared.
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
   * @brief Answer an invocation, releasing its pool slot once the broker has the result.
   *
   * Not AZ_NODISCARD: best-effort response send, commonly fire-and-forget.
   *
   * The request is consumed on every outcome except the ones that reject the
   * call before it takes effect: invalid arguments, a handle this client did not
   * hand out, and a body that does not fit. That last one is worth answering
   * again with a shorter body; the others are programming errors.
   *
   * **Result deadline.** When the exec arrives, the client fixes a deadline:
   * its receipt time plus its execution budget (the exec's MQTT message
   * expiry), less a safety margin for network transit -- a tenth of the
   * probe's response timeout, between 1 and 10 seconds. No result is published
   * at or after that deadline: a call at or past it publishes nothing,
   * releases the slot and returns AZ_IOT_ERR_TIMEOUT. For an exec that carries
   * no expiry, the deadline is its receipt time plus the connection's
   * max_unconfirmed_delivery_timeout_ms, and its result is sent with no expiry;
   * it is abandoned (insufficient time) unless that timeout exceeds the
   * method's declared minimum.
   *
   * Before the connection is CONNECTED (for example between CONNACK and the
   * birth-ack, or while reconnecting) the result is held, with its slot, and
   * published on the next CONNECTED; it is dropped if the deadline passes
   * first. A send the adapter refuses (BUSY, a transport error) is retried the
   * same way. AZ_IOT_OK covers all these cases.
   *
   * The slot stays taken until the broker acknowledges the result. If the
   * publish ends without a PUBACK, for example because the connection closed,
   * the result is published again on the next CONNECTED, with the remaining
   * response budget as its Message Expiry, until it is acknowledged or its
   * result deadline passes. The service may therefore receive a result twice; the
   * request id identifies it.
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
   *         @p payload_len exceeds limits.max_result_body_size,
   *         AZ_IOT_ERR_TIMEOUT if the result deadline has been reached -- nothing
   *         is sent in that case, because the service has given up or would
   *         before the result arrived; also when its slot was already
   *         reclaimed at that deadline.
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
