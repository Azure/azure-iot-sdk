// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_H
#define AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_H

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

  /* Default for az_iot_mqttv3_direct_method_client_set_response_timeout().
   *
   * IoT Hub never tells the device the caller's responseTimeoutInSeconds, so the
   * only safe default is the service maximum. Past it the invocation has
   * certainly been abandoned service-side -- a result sent later is discarded --
   * so reclaiming cannot race an answer anyone is still waiting for. */
#ifndef AZ_IOT_MQTTV3_DM_RESPONSE_TIMEOUT_SECONDS
#define AZ_IOT_MQTTV3_DM_RESPONSE_TIMEOUT_SECONDS 300u
#endif

/** @brief Default az_iot_mqttv3_direct_method_client_limits::max_in_flight_invocations. */
#ifndef AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_MAX_IN_FLIGHT_INVOCATIONS
#define AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_MAX_IN_FLIGHT_INVOCATIONS 4
#endif

/** @brief Largest az_iot_mqttv3_direct_method_client_limits::max_in_flight_invocations. */
#define AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_MAX_IN_FLIGHT_INVOCATIONS_LIMIT 1024

#if AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_MAX_IN_FLIGHT_INVOCATIONS < 1 \
    || AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_MAX_IN_FLIGHT_INVOCATIONS  \
        > AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_MAX_IN_FLIGHT_INVOCATIONS_LIMIT
#error "AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_MAX_IN_FLIGHT_INVOCATIONS must be 1-1024"
#endif

  /**
   * @brief How many invocations the client holds. 0 selects the default.
   *
   * Run-time values: the client struct does not depend on them. The invocations live in
   * az_iot_mqttv3_direct_method_client_options::workspace.
   */
  typedef struct az_iot_mqttv3_direct_method_client_limits
  {
    /** Invocations delivered and not answered yet. When all are held, a new invocation is
     * dropped with a WARN log and gets no response. Default
     * AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_MAX_IN_FLIGHT_INVOCATIONS (4), at most
     * AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_MAX_IN_FLIGHT_INVOCATIONS_LIMIT (1024). */
    size_t max_in_flight_invocations;
  } az_iot_mqttv3_direct_method_client_limits;

  /** @brief Options for az_iot_mqttv3_direct_method_client_init(). */
  typedef struct az_iot_mqttv3_direct_method_client_options
  {
    /** Entry counts; 0 fields select the defaults. */
    az_iot_mqttv3_direct_method_client_limits limits;
    /** Required. The client's invocations, owned by the client until deinit(), which wipes it.
     * At least az_iot_mqttv3_direct_method_client_get_workspace_size(&limits) bytes, any
     * alignment. */
    uint8_t* workspace;
    size_t workspace_size; /**< Bytes in workspace. */
  } az_iot_mqttv3_direct_method_client_options;

  /** @brief Internal: one in-flight invocation, in the workspace. */
  typedef struct az_iot_mqttv3_direct_method_invocation
  {
    az_iot_direct_method_slot slot;
    /* When it stops being worth answering. Held here rather than on az_iot_direct_method_slot
     * so mqttv5, which shares that type, does not carry storage for a timeout it derives from
     * the exec budget instead. */
    uint64_t expires_at_ms;
    /* seq of the last invocation of this entry that a session end (or a respond() refused
     * NOT_CONNECTED) took, so respond() on it says NOT_CONNECTED rather than INVALID_ARG. A later
     * one replaces it. */
    uint32_t ended_by_session_seq;
  } az_iot_mqttv3_direct_method_invocation;

/**
 * @brief Workspace bytes for @p max_in_flight_invocations, as a constant expression for static
 * arrays. Pass the default written out in place of 0. Equal to
 * az_iot_mqttv3_direct_method_client_get_workspace_size() for the same value.
 */
#define AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_WORKSPACE_SIZE(max_in_flight_invocations) \
  (15u                                                                               \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                                       \
       (size_t)(max_in_flight_invocations) * sizeof(az_iot_mqttv3_direct_method_invocation)))

/** @brief Workspace bytes for the default limits, as a constant expression for static arrays. */
#define AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_WORKSPACE_SIZE \
  AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_WORKSPACE_SIZE(              \
      AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_MAX_IN_FLIGHT_INVOCATIONS)

  /* MQTTv3 hub direct methods (MQTT v3.1.1).
   *
   *   Subscribe  "$iothub/methods/POST/#"
   *   Inbound    "$iothub/methods/POST/{methodName}/?$rid={rid}"
   *   Respond    "$iothub/methods/res/{status}/?$rid={rid}"
   */
  typedef struct az_iot_mqttv3_direct_method_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_direct_method_handler_callback handler;
      void* handler_ctx;
      /* limits.max_in_flight_invocations entries, in the workspace. */
      az_iot_mqttv3_direct_method_invocation* invocations;
      size_t max_in_flight_invocations;
      uint8_t* workspace;
      size_t workspace_size;
      uint32_t response_timeout_seconds;
      /* Bumped on every acquire and copied into the request handed out, so a
       * request naming a slot that has since been reused no longer matches. */
      uint32_t next_seq;
      /* Next invocation index to try. Allocation cycles rather than always taking the
       * lowest free entry, so an entry just reclaimed from an application that
       * never answered is the last one reused, not the first. */
      size_t next_slot;
    } _internal;
  } az_iot_mqttv3_direct_method_client;

  /**
   * @brief Workspace bytes az_iot_mqttv3_direct_method_client_init() needs for @p limits (0
   * fields: defaults).
   *
   * @return The size, or 0 when @p limits is NULL or a value is above its maximum.
   */
  AZ_NODISCARD size_t az_iot_mqttv3_direct_method_client_get_workspace_size(
      const az_iot_mqttv3_direct_method_client_limits* limits);

  /**
   * @brief Initialize the MQTTv3 hub direct method client.
   *
   * The connection need not be open: this records that it must resolve to the
   * MQTTv3 profile. A connection already known to be MQTT_V5 -- a direct
   * connection, or a DPS one past assignment -- is rejected here with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict surfaces when
   * the connection resolves, which fails it before it reports CONNECTED. The
   * same error is returned when an mqttv5 client is already attached.
   *
   * Observes the connection: when a session ends, every invocation not answered yet ends with
   * it (IoT Hub fails a pending call when the device disconnects), and its slot is free again.
   * Returns AZ_IOT_ERR_NOT_ENOUGH_SPACE when no feature observer is free.
   *
   * @p options is read during the call only; its workspace is used until deinit().
   *
   * @return AZ_IOT_ERR_INVALID_ARG when @p options is NULL, a limit is above its maximum or the
   *         workspace is NULL; AZ_IOT_ERR_NOT_ENOUGH_SPACE when the workspace is too small. Both
   *         workspace errors log the size needed.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv3_direct_method_client_init(
      az_iot_mqttv3_direct_method_client* client,
      az_iot_connection_client* conn,
      const az_iot_mqttv3_direct_method_client_options* options);

  /** @brief Detaches the client and wipes its workspace. */

  void az_iot_mqttv3_direct_method_client_deinit(az_iot_mqttv3_direct_method_client* client);

  /* Not AZ_NODISCARD: a configuration setter (fails only on invalid arguments). */
  az_iot_result az_iot_mqttv3_direct_method_client_set_handler(
      az_iot_mqttv3_direct_method_client* client,
      az_iot_direct_method_handler_callback cb,
      void* user_ctx);

  /**
   * @brief How long an invocation may hold its pool slot before the client
   *        takes it back.
   *
   * An application that never answers an invocation would otherwise hold that
   * slot for the life of the client, and once limits.max_in_flight_invocations are held
   * every further invocation is dropped. Set this above the longest method this
   * device services; @p seconds of 0 restores
   * AZ_IOT_MQTTV3_DM_RESPONSE_TIMEOUT_SECONDS.
   *
   * Values past the IoT Hub maximum responseTimeoutInSeconds (300) only delay
   * the reclaim -- the service has stopped waiting by then either way.
   *
   * Not AZ_NODISCARD: a configuration setter (fails only on invalid arguments).
   */
  az_iot_result az_iot_mqttv3_direct_method_client_set_response_timeout(
      az_iot_mqttv3_direct_method_client* client,
      uint32_t seconds);

  /**
   * @brief Answer an invocation, releasing its pool slot.
   *
   * Not AZ_NODISCARD: best-effort response send, commonly fire-and-forget.
   * A send refused for a reason that can pass keeps the slot until the response
   * timeout, so a caller that ignores the result costs that slot a delay.
   *
   * An invocation that is never answered has its slot reclaimed once its
   * response timeout passes, so dropping one costs a delay rather than the
   * client's capacity to receive methods. Answering after that is refused: the
   * service stopped waiting long before.
   *
   * @p request names a slot rather than pointing at one, so a slot that has
   * since been reclaimed and handed to another invocation no longer matches and
   * the late answer is refused instead of being published against that other
   * call. Holding a request indefinitely is therefore safe; it simply stops
   * being answerable. A request is also scoped to the client that issued it, so
   * handing one to a different client is refused rather than resolved against
   * whatever occupies the same slot there.
   *
   * One case is not detectable: a request held across deinit() *and* a fresh
   * init() of the same client. Teardown zeroes the pool, so the sequence starts
   * over and the new lifetime can reissue the pair the old request names.
   * Detecting it would need identity that outlives the caller's storage, which
   * this SDK does not keep. Do not hold a request across the teardown of the
   * client that issued it.
   *
   * @return AZ_IOT_ERR_TIMEOUT if @p request has passed its response timeout while
   *         it still holds its slot -- nothing is sent and the slot is released;
   *         AZ_IOT_ERR_NOT_CONNECTED if the session it arrived on has ended, even past
   *         its response timeout once the session end reclaimed the slot -- nothing
   *         is sent, as the call already failed service-side. Kept even after its
   *         slot is reused, until the slot ends another invocation the same way;
   *         after that, AZ_IOT_ERR_INVALID_ARG;
   *         AZ_IOT_ERR_BUSY if the connection is still setting up (a method can
   *         arrive before every subscription is acknowledged) -- nothing is sent
   *         and the request stays answerable: answer it again once connected;
   *         any other send error but AZ_IOT_ERR_INVALID_ARG (e.g. AZ_IOT_ERR_BUSY,
   *         AZ_IOT_ERR_MQTT, or AZ_IOT_ERR_NOT_ENOUGH_SPACE for a payload too large
   *         for the adapter; an adapter AZ_IOT_ERR_TIMEOUT is returned as
   *         AZ_IOT_ERR_BUSY) -- nothing is sent and the request stays answerable until
   *         its response timeout, or AZ_IOT_ERR_NOT_CONNECTED once its session ends;
   *         AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH if @p request came from a
   *         mqttv5 client; AZ_IOT_ERR_INVALID_ARG if it was already answered, if
   *         its slot was reclaimed on timeout and reused, or if it never came from
   *         this client.
   */
  az_iot_result az_iot_mqttv3_direct_method_respond(
      az_iot_mqttv3_direct_method_client* client,
      az_iot_direct_method_request request,
      int status_code,
      const uint8_t* payload,
      size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_H */
