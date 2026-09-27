// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file
 * @brief MQTT v5 (IoT Hub Next) twin client.
 *
 * Desired properties are kept in sync with the service: the handler receives a
 * @ref AZ_IOT_MQTTV5_TWIN_DESIRED_SNAPSHOT (full document) whenever the device is
 * behind, then each in-order @ref AZ_IOT_MQTTV5_TWIN_DESIRED_PATCH. The client
 * fetches the snapshot itself: on connect when the birth-ack shows the device
 * behind (unless `options.twin_push.push_desired` has the service push it), and
 * whenever a patch or probe shows it behind. It holds no twin content, only
 * versions.
 *
 * GET and reported patches are requests: each completes exactly once, with the
 * service's answer, AZ_IOT_ERR_TIMEOUT, or AZ_IOT_ERR_NOT_CONNECTED. The SDK
 * never retries them.
 */
#ifndef AZ_IOT_MQTTV5_TWIN_CLIENT_H
#define AZ_IOT_MQTTV5_TWIN_CLIENT_H

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

  /**
   * @brief One twin section and the service's version of it.
   *
   * `payload` is NULL when the service sent none: not requested, or the GET's
   * if-not-match matched. Valid only during the callback.
   */
  typedef struct az_iot_mqttv5_twin_section
  {
    /** Service version; 0 means none. */
    uint64_t version;
    /** Opaque section content, or NULL. */
    const uint8_t* payload;
    size_t payload_len;
  } az_iot_mqttv5_twin_section;

  /** @brief Both twin sections, as returned by a GET. */
  typedef struct az_iot_mqttv5_twin_state
  {
    az_iot_mqttv5_twin_section desired;
    az_iot_mqttv5_twin_section reported;
  } az_iot_mqttv5_twin_state;

  /** @brief The service's verdict on a reported patch. */
  typedef enum az_iot_mqttv5_twin_patch_status
  {
    AZ_IOT_MQTTV5_TWIN_PATCH_UNSPECIFIED = 0,
    AZ_IOT_MQTTV5_TWIN_PATCH_OK = 1,
    /** `if_match` was not the current version; another writer was first. */
    AZ_IOT_MQTTV5_TWIN_PATCH_VERSION_MISMATCH = 2,
    AZ_IOT_MQTTV5_TWIN_PATCH_PAYLOAD_INVALID = 3,
    AZ_IOT_MQTTV5_TWIN_PATCH_PAYLOAD_TOO_LARGE = 4,
    AZ_IOT_MQTTV5_TWIN_PATCH_INTERNAL_ERROR = 5
  } az_iot_mqttv5_twin_patch_status;

  /** @brief Outcome of a reported patch. */
  typedef struct az_iot_mqttv5_twin_patch_result
  {
    az_iot_mqttv5_twin_patch_status status;
    /** New reported version on OK; the unchanged current one otherwise. */
    uint64_t version;
  } az_iot_mqttv5_twin_patch_result;

  /** @brief What a desired-handler delivery contains. */
  typedef enum az_iot_mqttv5_twin_desired_kind
  {
    /** Incremental change; merge it onto the previous delivery. */
    AZ_IOT_MQTTV5_TWIN_DESIRED_PATCH = 0,
    /** Full desired document; replace local desired state with it. */
    AZ_IOT_MQTTV5_TWIN_DESIRED_SNAPSHOT = 1
  } az_iot_mqttv5_twin_desired_kind;

  /** @brief Selects the sections a GET returns. */
  typedef enum az_iot_mqttv5_twin_sections
  {
    AZ_IOT_MQTTV5_TWIN_SECTIONS_DESIRED = 1,
    AZ_IOT_MQTTV5_TWIN_SECTIONS_REPORTED = 2,
    AZ_IOT_MQTTV5_TWIN_SECTIONS_BOTH = 3
  } az_iot_mqttv5_twin_sections;

  /** @brief GET options. Initialize with az_iot_mqttv5_twin_get_options_default(). */
  typedef struct az_iot_mqttv5_twin_get_options
  {
    az_iot_mqttv5_twin_sections sections;
    /** Omit the desired payload if the service version equals this; 0 = no filter. */
    uint64_t if_not_match_desired;
    /** Omit the reported payload if the service version equals this; 0 = no filter. */
    uint64_t if_not_match_reported;
  } az_iot_mqttv5_twin_get_options;

  /**
   * @brief Delivers a GET result.
   *
   * @param status   AZ_IOT_OK, AZ_IOT_ERR_TIMEOUT, AZ_IOT_ERR_NOT_CONNECTED, or
   *                 AZ_IOT_ERR_PROTOCOL for an unreadable response.
   * @param twin     The sections; NULL unless @p status is AZ_IOT_OK.
   * @param user_ctx Context given to the GET call.
   */
  typedef void (*az_iot_mqttv5_twin_get_callback)(
      az_iot_result status,
      const az_iot_mqttv5_twin_state* twin,
      void* user_ctx);

  /**
   * @brief Delivers a reported-patch outcome.
   *
   * @param status   As for az_iot_mqttv5_twin_get_callback. AZ_IOT_OK means the
   *                 service answered; @p result says whether it applied the patch.
   * @param result   The verdict; NULL unless @p status is AZ_IOT_OK.
   * @param user_ctx Context given to the patch call.
   */
  typedef void (*az_iot_mqttv5_twin_patch_ack_callback)(
      az_iot_result status,
      const az_iot_mqttv5_twin_patch_result* result,
      void* user_ctx);

  /**
   * @brief Delivers desired state.
   *
   * @param kind        Whether @p payload is a patch or the full document.
   * @param version     Desired version after applying this delivery.
   * @param payload     The content; valid only during the call.
   * @param payload_len Length of @p payload.
   * @param user_ctx    Context given to az_iot_mqttv5_twin_client_set_desired_handler().
   */
  typedef void (*az_iot_mqttv5_twin_desired_callback)(
      az_iot_mqttv5_twin_desired_kind kind,
      uint64_t version,
      const uint8_t* payload,
      size_t payload_len,
      void* user_ctx);

  /**
   * @brief Delivers the reported section the service pushed on connect.
   *
   * Sent only when `options.twin_push.push_reported` is set and the device is
   * behind. @p section is valid only during the call.
   */
  typedef void (*az_iot_mqttv5_twin_reported_callback)(
      const az_iot_mqttv5_twin_section* section,
      void* user_ctx);

/** @brief Protobuf framing added around a reported patch. Size the encode buffer to the
 * largest patch plus this. */
#define AZ_IOT_MQTTV5_TWIN_ENCODE_OVERHEAD 24

/** @brief Size of a twin MQTT v5 Correlation Data value: a binary UUID. */
#define AZ_IOT_MQTTV5_TWIN_CORRELATION_ID_LEN 16u

/** @brief Default for az_iot_mqttv5_twin_client_set_request_timeout(). */
#define AZ_IOT_MQTTV5_TWIN_REQUEST_TIMEOUT_MS_DEFAULT 60000u

  /** @brief MQTT v5 twin client. Caller-allocated; treat as opaque. */
  typedef struct az_iot_mqttv5_twin_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_mqttv5_twin_desired_callback desired_handler;
      void* desired_handler_ctx;
      az_iot_mqttv5_twin_reported_callback reported_handler;
      void* reported_handler_ctx;
      /* Length of "ih/{device_id}/dev/twin"; 0 until bound. */
      size_t inbound_topic_len;
      /* Highest desired version the service has reported. */
      uint64_t desired_properties_service_version;
      /* Version of the last desired delivery to the handler; 0 before the first. */
      uint64_t desired_properties_device_version;
      /* The handler's state predates the service's current lineage (the
       * birth-ack reported a lower version); only a snapshot may follow. */
      bool desired_snapshot_required;
      /* A patch or probe showed the device behind before hub CONNECTED. */
      bool desired_catch_up_pending;
      /* Birth nonce of the session whose birth-ack baseline was adopted. */
      uint8_t birth_nonce[AZ_IOT_MQTTV5_TWIN_CORRELATION_ID_LEN];
      bool birth_nonce_valid;
      /* Service's reported version; the default `if_match`. */
      uint64_t reported_properties_service_version;
      az_span encode_buffer;
      uint32_t request_timeout_ms;
      /* The SDK's own desired-snapshot GET, kept out of `pending`. */
      struct
      {
        bool in_flight;
        uint8_t correlation_id[AZ_IOT_MQTTV5_TWIN_CORRELATION_ID_LEN];
        uint64_t deadline_ms;
      } snapshot;
      struct
      {
        bool in_use;
        int kind; /* internal enum */
        uint8_t correlation_id[AZ_IOT_MQTTV5_TWIN_CORRELATION_ID_LEN];
        uint64_t deadline_ms;
        union
        {
          az_iot_mqttv5_twin_get_callback get_cb;
          az_iot_mqttv5_twin_patch_ack_callback patch_cb;
        } cb;
        void* user_ctx;
      } pending[AZ_IOT_TWIN_MAX_PENDING];
    } _internal;
  } az_iot_mqttv5_twin_client;

  /**
   * @brief Initialize the client on @p conn.
   *
   * Receives on `ih/{device_id}/dev/twin`, which the presence subscription
   * already covers. @p conn need not be open.
   *
   * @return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH if @p conn is known to be
   *         Classic, or an mqttv3 twin client is attached.
   */
  AZ_NODISCARD az_iot_result
  az_iot_mqttv5_twin_client_init(az_iot_mqttv5_twin_client* client, az_iot_connection_client* conn);

  /** @brief Detach from the connection and zero @p client. */
  void az_iot_mqttv5_twin_client_deinit(az_iot_mqttv5_twin_client* client);

  /** @brief Returns GET options for both sections with no filters. */
  AZ_NODISCARD az_iot_mqttv5_twin_get_options az_iot_mqttv5_twin_get_options_default(void);

  /**
   * @brief Request both twin sections.
   *
   * @return AZ_IOT_ERR_NOT_SUPPORTED when AZ_IOT_TWIN_MAX_PENDING requests are in flight.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_twin_client_get(
      az_iot_mqttv5_twin_client* twin,
      az_iot_mqttv5_twin_get_callback cb,
      void* user_ctx);

  /**
   * @brief Request selected sections, optionally skipping ones already held.
   *
   * @return AZ_IOT_ERR_INVALID_ARG for an unknown section selector.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_twin_client_get_with_options(
      az_iot_mqttv5_twin_client* twin,
      const az_iot_mqttv5_twin_get_options* opts,
      az_iot_mqttv5_twin_get_callback cb,
      void* user_ctx);

  /**
   * @brief Merge @p patch into the reported section.
   *
   * Sends the service's last known reported version as `if_match`, so a write
   * that raced another writer is refused with VERSION_MISMATCH. That version
   * is learned from the birth-ack, GET and patch responses, and pushes.
   *
   * @return AZ_IOT_ERR_NOT_ENOUGH_SPACE when no encode buffer is set or the
   *         framed patch does not fit.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_twin_client_patch_reported(
      az_iot_mqttv5_twin_client* twin,
      const uint8_t* patch,
      size_t patch_len,
      az_iot_mqttv5_twin_patch_ack_callback cb,
      void* user_ctx);

  /**
   * @brief As az_iot_mqttv5_twin_client_patch_reported(), with an explicit @p if_match.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_twin_client_patch_reported_if_match(
      az_iot_mqttv5_twin_client* twin,
      uint64_t if_match,
      const uint8_t* patch,
      size_t patch_len,
      az_iot_mqttv5_twin_patch_ack_callback cb,
      void* user_ctx);

  /**
   * @brief Set the desired-state handler.
   *
   * A handler set while connected is fetched a snapshot when the service's
   * desired version is above 0, so one set late or resumed after a pause starts
   * from current state; at version 0 the document is empty and nothing is sent.
   * NULL pauses delivery; nothing received while paused counts as delivered.
   */
  az_iot_result az_iot_mqttv5_twin_client_set_desired_handler(
      az_iot_mqttv5_twin_client* twin,
      az_iot_mqttv5_twin_desired_callback cb,
      void* user_ctx);

  /** @brief Set the handler for a pushed reported section. NULL drops it. */
  az_iot_result az_iot_mqttv5_twin_client_set_reported_handler(
      az_iot_mqttv5_twin_client* twin,
      az_iot_mqttv5_twin_reported_callback cb,
      void* user_ctx);

  /**
   * @brief Provide the buffer reported patches are framed in.
   *
   * Borrowed; must outlive the client. Size it to the largest patch plus
   * AZ_IOT_MQTTV5_TWIN_ENCODE_OVERHEAD. Free for reuse once the patch call returns.
   * AZ_SPAN_EMPTY removes it.
   *
   * @return AZ_IOT_ERR_INVALID_ARG for a negative size, or a non-empty span with a NULL pointer.
   * @return AZ_IOT_ERR_NOT_ENOUGH_SPACE when smaller than the framing alone.
   */
  az_iot_result az_iot_mqttv5_twin_client_set_encode_buffer(
      az_iot_mqttv5_twin_client* twin,
      az_span buffer);

  /**
   * @brief How long a request waits for its response before completing with
   *        AZ_IOT_ERR_TIMEOUT. 0 selects AZ_IOT_MQTTV5_TWIN_REQUEST_TIMEOUT_MS_DEFAULT.
   */
  az_iot_result az_iot_mqttv5_twin_client_set_request_timeout(
      az_iot_mqttv5_twin_client* twin,
      uint32_t timeout_ms);

  /**
   * @brief Expire requests past their timeout. Call alongside
   *        az_iot_connection_client_do_work().
   *
   * Without it, an unanswered request completes only when the session ends.
   */
  az_iot_result az_iot_mqttv5_twin_client_do_work(az_iot_mqttv5_twin_client* twin);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV5_TWIN_CLIENT_H */
