// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_GEN2_TWIN_CLIENT_H
#define AZ_IOT_GEN2_TWIN_CLIENT_H

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

  /* One section of the twin, with the authoritative version the service holds
   * for it.
   *
   * `version` is the service's version of this section, and 0 doubles as the
   * protocol's "no authoritative version" sentinel. `payload` is opaque to the
   * SDK (IoT Hub's JSON merge-patch convention is a layer above) and is NULL
   * when the service sent no payload for this section -- either because it was
   * not requested, or because the device's if-not-match filter already matched.
   * It is valid only for the duration of the callback: copy what you keep. */
  typedef struct az_iot_gen2_twin_section
  {
    uint64_t version;
    const uint8_t* payload;
    size_t payload_len;
  } az_iot_gen2_twin_section;

  /* A view of the device twin delivered by the service. The two sections are
   * returned separately, each carrying its own authoritative version. */
  typedef struct az_iot_gen2_twin_state
  {
    az_iot_gen2_twin_section desired;
    az_iot_gen2_twin_section reported;
  } az_iot_gen2_twin_state;

  /* The service's verdict on a reported-properties patch.
   *
   * Reported writes use optimistic concurrency: the SDK sends the version it
   * believes is authoritative as `if_match`, and the service rejects the write
   * if it has moved on. VERSION_MISMATCH means another writer got there first --
   * re-read the twin, re-apply your changes and retry. The SDK adopts the
   * version the service returns, so a retry issued from the callback carries a
   * correct if_match without the application tracking it. */
  typedef enum az_iot_gen2_twin_patch_status
  {
    AZ_IOT_GEN2_TWIN_PATCH_UNSPECIFIED = 0,
    AZ_IOT_GEN2_TWIN_PATCH_OK = 1,
    AZ_IOT_GEN2_TWIN_PATCH_VERSION_MISMATCH = 2,
    AZ_IOT_GEN2_TWIN_PATCH_PAYLOAD_INVALID = 3,
    AZ_IOT_GEN2_TWIN_PATCH_PAYLOAD_TOO_LARGE = 4,
    AZ_IOT_GEN2_TWIN_PATCH_INTERNAL_ERROR = 5
  } az_iot_gen2_twin_patch_status;

  typedef struct az_iot_gen2_twin_patch_result
  {
    az_iot_gen2_twin_patch_status status;
    /* The new authoritative reported version on OK, and the current one on
     * VERSION_MISMATCH. */
    uint64_t version;
  } az_iot_gen2_twin_patch_result;

  /**
   * @brief Delivers the result of a twin GET.
   *
   * @param status   Whether the exchange completed at all: AZ_IOT_OK, or an
   *                 AZ_IOT_ERR_* when it could not be delivered or answered.
   * @param twin     The returned sections. NULL unless @p status is AZ_IOT_OK,
   *                 and valid only for the duration of this call.
   * @param user_ctx Context passed to the get call.
   */
  typedef void (*az_iot_gen2_twin_get_callback)(
      az_iot_result status,
      const az_iot_gen2_twin_state* twin,
      void* user_ctx);

  /**
   * @brief Delivers the outcome of a reported-properties patch.
   *
   * @param status   Whether the exchange completed at all. A service verdict of
   *                 VERSION_MISMATCH still arrives as AZ_IOT_OK: the exchange
   *                 succeeded, the write did not.
   * @param result   The service's verdict. NULL unless @p status is AZ_IOT_OK.
   * @param user_ctx Context passed to the patch call.
   */
  typedef void (*az_iot_gen2_twin_patch_ack_callback)(
      az_iot_result status,
      const az_iot_gen2_twin_patch_result* result,
      void* user_ctx);

  /**
   * @brief Delivers a backend-initiated full-state push.
   *
   * The service sends this when the device's twin versions are behind at birth
   * admission and the matching `options.twin_push` bit was advertised on the
   * birth, and may also send it mid-connection as a recovery mechanism. A
   * section the service chose not to push has a NULL payload.
   */
  typedef void (
      *az_iot_gen2_twin_push_callback)(const az_iot_gen2_twin_state* twin, void* user_ctx);

/* Bytes of protobuf framing the SDK wraps around a reported-properties patch:
 * the if_match field plus the payload's tag and length prefix. Size an encode
 * buffer to your largest patch plus this. */
#define AZ_IOT_GEN2_TWIN_ENCODE_OVERHEAD 24

  typedef struct az_iot_gen2_twin_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_twin_desired_callback desired_handler;
      void* desired_handler_ctx;
      az_iot_gen2_twin_push_callback push_cb;
      void* push_user_ctx;

      /* Length of "ih/{device_id}/dev/twin", resolved when the connection
       * binds. Dispatch matches by prefix, so the handler compares the
       * delivered topic's length against this to confirm it is that exact
       * topic and not something below it. 0 until bind_topics() runs. */
      size_t inbound_topic_len;

      /* The service's authoritative versions as the client currently
       * understands them: seeded from the birth-ack, then advanced by every
       * response and push. `reported_version` is what rides the next patch as
       * if_match. */
      uint64_t desired_version;
      uint64_t reported_version;

      /* The birth nonce those versions were seeded from, so a reconnect is
       * noticed and the versions re-seeded from the new birth-ack. */
      uint8_t nonce[16];
      bool nonce_valid;

      /* Caller-owned scratch for framing a reported patch. The SDK does not
       * allocate; without it a patch cannot be sent. */
      uint8_t* encode_buffer;
      size_t encode_buffer_len;

      struct
      {
        bool in_use;
        int kind; /* internal enum */
        /* Per-attempt correlation id: a 16-byte UUID the service echoes. */
        uint8_t corr[16];
        union
        {
          az_iot_gen2_twin_get_callback get_cb;
          az_iot_gen2_twin_patch_ack_callback patch_cb;
        } cb;
        void* user_ctx;
      } pending[AZ_IOT_TWIN_MAX_PENDING];
    } _internal;
  } az_iot_gen2_twin_client;

  /**
   * @brief Initialize the MQTT v5 twin client.
   *
   * Registers delivery for `ih/{device_id}/dev/twin`, the single topic every
   * service-to-device twin message arrives on; the message kind rides a `type`
   * user property. It issues no subscription of its own: the presence handshake
   * already holds `ih/{device_id}/dev/#`, which covers it.
   *
   * The connection need not be open: this records that it must resolve to the
   * MQTT v5 profile, and the topics are built when it connects and the assigned
   * device id is known. A connection already known to be Classic -- a direct
   * connection, or a DPS one past assignment -- is rejected here with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict surfaces when
   * the connection resolves, which fails it before it reports CONNECTED. The
   * same error is returned when a gen1 client is already attached.
   */
  AZ_NODISCARD az_iot_result
  az_iot_gen2_twin_client_init(az_iot_gen2_twin_client* client, az_iot_connection_client* conn);

  void az_iot_gen2_twin_client_deinit(az_iot_gen2_twin_client* client);

  /**
   * @brief Request both twin sections.
   *
   * @return AZ_IOT_ERR_NOT_SUPPORTED when AZ_IOT_TWIN_MAX_PENDING requests are
   *         already in flight.
   */
  AZ_NODISCARD az_iot_result az_iot_gen2_twin_client_get(
      az_iot_gen2_twin_client* twin,
      az_iot_gen2_twin_get_callback cb,
      void* user_ctx);

  /**
   * @brief Merge @p patch into the reported-properties section.
   *
   * The patch is framed with the client's current view of the authoritative
   * reported version as `if_match`, so a write that raced another writer is
   * rejected rather than silently overwriting it. Requires an encode buffer.
   *
   * @return AZ_IOT_ERR_NOT_ENOUGH_SPACE when no encode buffer is set or the
   *         framed patch does not fit in it.
   */
  AZ_NODISCARD az_iot_result az_iot_gen2_twin_client_patch_reported(
      az_iot_gen2_twin_client* twin,
      const uint8_t* patch,
      size_t patch_len,
      az_iot_gen2_twin_patch_ack_callback cb,
      void* user_ctx);

  /**
   * @brief Set the handler for service-pushed desired-property patches.
   *
   * The handler receives every desired update and decides whether it carries
   * keys it cares about. NULL @p cb pauses delivery without unregistering the
   * handler.
   */
  az_iot_result az_iot_gen2_twin_client_set_desired_handler(
      az_iot_gen2_twin_client* twin,
      az_iot_twin_desired_callback cb,
      void* user_ctx);

  /**
   * @brief Set the handler for backend-initiated full-state pushes.
   *
   * Without one, a twin-push is decoded to advance the client's tracked
   * versions and then dropped. Set this to receive the state itself.
   */
  az_iot_result az_iot_gen2_twin_client_set_push_callback(
      az_iot_gen2_twin_client* twin,
      az_iot_gen2_twin_push_callback cb,
      void* user_ctx);

  /**
   * @brief Provide the scratch buffer used to frame reported patches.
   *
   * The SDK does not allocate, so a reported patch needs somewhere to be
   * wrapped in its protobuf envelope (the same pattern as
   * `options.csr_payload_buffer`). Size it to your largest patch plus
   * AZ_IOT_GEN2_TWIN_ENCODE_OVERHEAD. The buffer is borrowed, not copied: it
   * must outlive the client, and its contents are only meaningful for the
   * duration of a patch call.
   *
   * @return AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p buffer_len is smaller than the
   *         framing alone requires.
   */
  az_iot_result az_iot_gen2_twin_client_set_encode_buffer(
      az_iot_gen2_twin_client* twin,
      uint8_t* buffer,
      size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_GEN2_TWIN_CLIENT_H */
