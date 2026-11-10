// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MQTTV3_TWIN_CLIENT_H
#define AZ_IOT_MQTTV3_TWIN_CLIENT_H

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

/** @brief Default for az_iot_mqttv3_twin_client_set_request_timeout(). */
#define AZ_IOT_MQTTV3_TWIN_REQUEST_TIMEOUT_MS_DEFAULT 60000u

/** @brief Longest twin request topic, with its terminator. */
#define AZ_IOT_MQTTV3_TWIN_TOPIC_MAX 64u

  /** @brief A twin request held for its tracker entry. Internal; treat as opaque. */
  typedef struct az_iot_mqttv3_twin_request
  {
    int kind; /* internal enum; 0: none */
    uint32_t rid;
    const uint8_t* patch; /* Reported patch: the caller's, during the call only. */
    size_t patch_len;
    bool sent;
    union
    {
      az_iot_twin_get_callback get_cb;
      az_iot_twin_patch_complete_callback patch_cb;
    } cb;
    void* user_ctx;
  } az_iot_mqttv3_twin_request;

/**
 * @brief Workspace bytes for @p max_pending_requests, as a constant expression for static arrays.
 * Pass the default written out in place of 0. Equal to
 * az_iot_mqttv3_twin_client_get_workspace_size() for the same value.
 */
#define AZ_IOT_MQTTV3_TWIN_CLIENT_WORKSPACE_SIZE(max_pending_requests)      \
  (15u                                                                      \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                              \
       (size_t)(max_pending_requests) * sizeof(az_iot_mqttv3_twin_request)) \
   + AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(                              \
       (size_t)(max_pending_requests) * sizeof(az_iot_delivery_tracker_entry)))

/** @brief Workspace bytes for the default limits, as a constant expression for static arrays. */
#define AZ_IOT_MQTTV3_TWIN_CLIENT_DEFAULT_WORKSPACE_SIZE \
  AZ_IOT_MQTTV3_TWIN_CLIENT_WORKSPACE_SIZE(AZ_IOT_TWIN_CLIENT_DEFAULT_MAX_PENDING_REQUESTS)

  /** @brief Options for az_iot_mqttv3_twin_client_init(). */
  typedef struct az_iot_mqttv3_twin_client_options
  {
    /** Request counts; 0 fields select the defaults. */
    az_iot_twin_client_limits limits;
    /** Required. The client's requests, owned by the client until deinit(), which wipes it. At
     * least az_iot_mqttv3_twin_client_get_workspace_size(&limits) bytes, any alignment. */
    uint8_t* workspace;
    size_t workspace_size; /**< Bytes in workspace. */
  } az_iot_mqttv3_twin_client_options;

  /**
   * @brief Returns options with the default limits (0 fields) and no workspace.
   *
   * Set workspace and workspace_size before az_iot_mqttv3_twin_client_init().
   *
   * @return The options.
   */
  AZ_NODISCARD az_iot_mqttv3_twin_client_options az_iot_mqttv3_twin_client_options_default(void);

  typedef struct az_iot_mqttv3_twin_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_twin_desired_callback desired_handler;
      void* desired_handler_ctx;
      uint32_t next_rid;
      uint32_t request_timeout_ms;
      /* The request each tracker entry holds (index = entry), in the workspace. Kind 0: none. */
      az_iot_mqttv3_twin_request* requests;
      size_t max_pending_requests;
      uint8_t* workspace;
      size_t workspace_size;
      size_t submitting_entry; /* The entry being submitted, while submit_in_progress. */
      bool submit_in_progress;
      az_iot_result
          submit_failure; /* How the submitted request ended during submit; OK: it did not. */
      /* Where encode() builds the topic the tracker sends next. */
      char outbound_topic[AZ_IOT_MQTTV3_TWIN_TOPIC_MAX];
      az_iot_delivery_tracker tracker;
    } _internal;
  } az_iot_mqttv3_twin_client;

  /**
   * @brief Workspace bytes az_iot_mqttv3_twin_client_init() needs for @p limits (0 fields:
   * defaults).
   *
   * @return The size, or 0 when @p limits is NULL or a value is above its maximum.
   */
  AZ_NODISCARD size_t
  az_iot_mqttv3_twin_client_get_workspace_size(const az_iot_twin_client_limits* limits);

  /**
   * @brief Initialize the MQTTv3 hub twin client.
   *
   * Subscribes `$iothub/twin/res/#` for GET and patch acknowledgements, and
   * `$iothub/twin/PATCH/properties/desired/#` for service-pushed desired
   * updates. Requests correlate on the `$rid` carried in the topic.
   *
   * The connection need not be open: this records that it must resolve to the
   * MQTTv3 profile. Unlike C2D, these topics carry no device id, so they are
   * known before the connection resolves and no connect-time bind is needed.
   * A connection already resolved to another profile is rejected here with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict surfaces when
   * the connection resolves, which fails it before it reports CONNECTED.
   *
   * @p options is read during the call only; its workspace is used until deinit().
   *
   * @return AZ_IOT_ERR_INVALID_ARG when @p options is NULL, a limit is above its maximum or the
   *         workspace is NULL; AZ_IOT_ERR_NOT_ENOUGH_SPACE when the workspace is too small. Both
   *         workspace errors log the size needed.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv3_twin_client_init(
      az_iot_mqttv3_twin_client* client,
      az_iot_connection_client* conn,
      const az_iot_mqttv3_twin_client_options* options);

  /** @brief Detaches the client and wipes its workspace. */
  void az_iot_mqttv3_twin_client_deinit(az_iot_mqttv3_twin_client* client);

  /**
   * @brief Request the full twin document.
   *
   * Sent now, or once connected; sent again on the next session if its session ends
   * unanswered. @p cb gets the answer, or AZ_IOT_ERR_TIMEOUT once the request timeout
   * from this call passes.
   *
   * @return AZ_IOT_ERR_NOT_SUPPORTED when limits.max_pending_requests requests are
   *         already in flight. The service reporting a throttle returns
   *         AZ_IOT_ERR_BUSY to @p cb instead, so the two are distinguishable.
   *         AZ_IOT_ERR_BUSY from a twin callback run by another request call.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv3_twin_client_get(
      az_iot_mqttv3_twin_client* twin,
      az_iot_twin_get_callback cb,
      void* user_ctx);

  /**
   * @brief Merge @p patch into the reported-properties section.
   *
   * Sent now or refused: @p patch is read during the call only. @p cb gets the answer,
   * AZ_IOT_ERR_TIMEOUT once the request timeout from this call passes, or
   * AZ_IOT_ERR_NOT_CONNECTED if its session ends first (it may have been applied).
   *
   * @return AZ_IOT_ERR_NOT_SUPPORTED when limits.max_pending_requests requests are in
   *         flight; AZ_IOT_ERR_NOT_CONNECTED when not connected; AZ_IOT_ERR_BUSY when the
   *         connection could not send it now, or from a twin callback run by another
   *         request call; AZ_IOT_ERR_NOT_ENOUGH_SPACE when the adapter cannot send it.
   *         Nothing is sent on an error.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv3_twin_client_patch_reported(
      az_iot_mqttv3_twin_client* twin,
      const uint8_t* patch,
      size_t patch_len,
      az_iot_twin_patch_complete_callback cb,
      void* user_ctx);

  /**
   * @brief Set the handler for service-pushed desired-property patches.
   *
   * The handler receives every desired update delivered while connected, and
   * decides whether it carries keys it cares about. NULL @p cb pauses delivery
   * without tearing the subscription down.
   *
   * Patches sent while the device is disconnected are not redelivered. To
   * catch up after a reconnect, call az_iot_mqttv3_twin_client_get() and ignore
   * patches whose version is not newer than the document's.
   */
  az_iot_result az_iot_mqttv3_twin_client_set_desired_handler(
      az_iot_mqttv3_twin_client* twin,
      az_iot_twin_desired_callback cb,
      void* user_ctx);

  /**
   * @brief How long a request made after this call waits for its response, from the call,
   *        before completing with AZ_IOT_ERR_TIMEOUT. 0 selects
   *        AZ_IOT_MQTTV3_TWIN_REQUEST_TIMEOUT_MS_DEFAULT. az_iot_connection_client_do_work()
   *        times requests out.
   */
  az_iot_result az_iot_mqttv3_twin_client_set_request_timeout(
      az_iot_mqttv3_twin_client* twin,
      uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV3_TWIN_CLIENT_H */
