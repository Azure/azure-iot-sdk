// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MESSAGE_H
#define AZ_IOT_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"
/* For az_iot_connection_profile, which tags a method request with the
 * generation that built it. */
#include "az_iot_connection_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Well-known IoT Hub system property keys.
 *
 * They apply in both directions: set one on an az_iot_telemetry_message going
 * out, read one off an az_iot_c2d_message coming in (MQTTv3 IoT Hub only). The
 * same spelling works either way because the SDK owns the encoding -- these
 * are the plain, human-readable names, never the pre-encoded form.
 *
 * On the MQTTv3 path the SDK percent-encodes both halves into
 * the topic's property bag, so "$.ct" travels as "%24.ct" and a value of
 * "application/json" as "application%2Fjson", and decodes them again on the way
 * in. azure-sdk-for-c spells the same names pre-encoded
 * (AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE is "%24.ct"); the bytes on the wire
 * are identical. On the MQTTv5 path they travel as User Properties
 * and need no encoding at all, except "$.ct" and "$.cid", which travel as the
 * native Content Type and Correlation Data.
 *
 * See
 * https://learn.microsoft.com/azure/iot-hub/iot-hub-devguide-messages-construct
 */
#define AZ_IOT_MSG_PROP_CONTENT_TYPE "$.ct"
#define AZ_IOT_MSG_PROP_CONTENT_ENCODING "$.ce"
#define AZ_IOT_MSG_PROP_MESSAGE_ID "$.mid"
#define AZ_IOT_MSG_PROP_CORRELATION_ID "$.cid"
#define AZ_IOT_MSG_PROP_USER_ID "$.uid"
#define AZ_IOT_MSG_PROP_CREATION_TIME "$.ctime"
#define AZ_IOT_MSG_PROP_COMPONENT_NAME "$.sub"
/* Absolute expiry time (ISO 8601 UTC) of a C2D message. Set by the service;
 * receive only. */
#define AZ_IOT_MSG_PROP_EXPIRY_TIME "$.exp"

  /* Shared by the mqttv3 and mqttv5 telemetry clients. Callers always pass plain
   * text: mqttv3 percent-encodes properties into the topic, while mqttv5 carries
   * them as MQTT v5 User Properties, except "$.ct" and "$.cid" (native Content
   * Type and Correlation Data). */
  typedef struct az_iot_telemetry_property
  {
    const char* key;
    const char* value;
  } az_iot_telemetry_property;

  typedef struct az_iot_telemetry_message
  {
    const uint8_t* payload;
    size_t payload_len;
    const az_iot_telemetry_property* properties;
    size_t properties_count;
  } az_iot_telemetry_message;

  typedef void (*az_iot_telemetry_send_callback)(az_iot_result status, void* user_ctx);

  /**
   * @brief Read-only view of the property bag of a received cloud-to-device
   *        message.
   *
   * Points into the received topic: nothing is copied or decoded until the
   * application asks, so there is no SDK buffer and no limit on the number or
   * size of properties. Read it with az_iot_c2d_properties_next(),
   * az_iot_c2d_properties_find() or the az_iot_c2d_message_get_*() accessors,
   * which percent-decode into buffers the application provides. Valid only for
   * the duration of the handler callback. Opaque -- do NOT read _internal.
   */
  typedef struct az_iot_c2d_properties
  {
    struct
    {
      const char* bag;
      size_t bag_len;
    } _internal;
  } az_iot_c2d_properties;

  /**
   * @brief A received cloud-to-device message.
   *
   * MQTTv3 IoT Hub only: C2D is not carried on the MQTT v5 hub.
   *
   * Every pointer is owned by the SDK and valid only for the duration of the
   * handler callback. Copy anything the application needs to keep.
   */
  typedef struct az_iot_c2d_message
  {
    const uint8_t* payload; /**< May be NULL when @p payload_len is 0. */
    size_t payload_len;
    az_iot_c2d_properties properties;
  } az_iot_c2d_message;

  typedef void (*az_iot_c2d_handler_callback)(const az_iot_c2d_message* msg, void* user_ctx);

  /**
   * @brief Decode the next property.
   *
   * IoT Hub sends a null value as a bare key and an empty value as `key=`;
   * both yield an empty @p value, told apart by @p out_has_value.
   *
   * @param[in] props         Properties of the message.
   * @param[in,out] cursor    Start at 0. Advanced past the property even when
   *                          this call fails, so a loop always ends.
   * @param[out] key          Decoded, NUL-terminated name.
   * @param[in] key_size      Size of @p key, terminator included.
   * @param[out] value        Decoded, NUL-terminated value.
   * @param[in] value_size    Size of @p value, terminator included.
   * @param[out] out_has_value Optional. false for a bare key.
   *
   * @retval AZ_IOT_OK                   @p key and @p value hold the property.
   * @retval AZ_IOT_ERR_NOT_FOUND        No more properties.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE @p key or @p value is too small; both
   *                                     are left empty.
   * @retval AZ_IOT_ERR_PROTOCOL         Malformed or NUL percent-escape; both
   *                                     are left empty.
   * @retval AZ_IOT_ERR_INVALID_ARG      A NULL pointer or a zero size.
   */
  AZ_NODISCARD az_iot_result az_iot_c2d_properties_next(
      const az_iot_c2d_properties* props,
      size_t* cursor,
      char* key,
      size_t key_size,
      char* value,
      size_t value_size,
      bool* out_has_value);

  /**
   * @brief Decode the value of the first property named @p key.
   *
   * @param[in] props          Properties of the message.
   * @param[in] key            Plain-text name, e.g. #AZ_IOT_MSG_PROP_MESSAGE_ID.
   * @param[out] value         Decoded, NUL-terminated value; empty for a bare
   *                           key or `key=`.
   * @param[in] value_size     Size of @p value, terminator included.
   * @param[out] out_value_len Optional. Decoded length, terminator excluded;
   *                           also set on AZ_IOT_ERR_NOT_ENOUGH_SPACE so the
   *                           caller can size a retry.
   *
   * @retval AZ_IOT_OK                   @p value holds the value.
   * @retval AZ_IOT_ERR_NOT_FOUND        No such property.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE @p value is too small; left empty.
   * @retval AZ_IOT_ERR_PROTOCOL         Malformed or NUL percent-escape in the
   *                                     value; left empty.
   * @retval AZ_IOT_ERR_INVALID_ARG      A NULL pointer or a zero size.
   */
  AZ_NODISCARD az_iot_result az_iot_c2d_properties_find(
      const az_iot_c2d_properties* props,
      const char* key,
      char* value,
      size_t value_size,
      size_t* out_value_len);

  /**
   * @name Typed system property accessors
   * az_iot_c2d_properties_find() on @p msg->properties for one system property:
   * same parameters after @p msg, same results.
   * @{
   */
  /** @brief `$.mid`. */
  AZ_NODISCARD az_iot_result az_iot_c2d_message_get_message_id(
      const az_iot_c2d_message* msg,
      char* value,
      size_t value_size,
      size_t* out_value_len);
  /** @brief `$.cid`. */
  AZ_NODISCARD az_iot_result az_iot_c2d_message_get_correlation_id(
      const az_iot_c2d_message* msg,
      char* value,
      size_t value_size,
      size_t* out_value_len);
  /** @brief `$.exp`: absolute expiry, ISO 8601 UTC, as sent by the service. */
  AZ_NODISCARD az_iot_result az_iot_c2d_message_get_expiry_time(
      const az_iot_c2d_message* msg,
      char* value,
      size_t value_size,
      size_t* out_value_len);
  /** @brief `$.ct`. */
  AZ_NODISCARD az_iot_result az_iot_c2d_message_get_content_type(
      const az_iot_c2d_message* msg,
      char* value,
      size_t value_size,
      size_t* out_value_len);
  /** @brief `$.ce`. */
  AZ_NODISCARD az_iot_result az_iot_c2d_message_get_content_encoding(
      const az_iot_c2d_message* msg,
      char* value,
      size_t value_size,
      size_t* out_value_len);
  /** @brief `$.uid`. */
  AZ_NODISCARD az_iot_result az_iot_c2d_message_get_user_id(
      const az_iot_c2d_message* msg,
      char* value,
      size_t value_size,
      size_t* out_value_len);
  /** @} */

/* Storage bounds for the request handle below. Compile-time footprint knobs:
 * #define before including to tune. */
#ifndef AZ_IOT_DM_METHOD_NAME_MAX
#define AZ_IOT_DM_METHOD_NAME_MAX 96
#endif
/* Longest mqttv3 request id ($rid) carried on an invocation. */
#ifndef AZ_IOT_DM_RID_MAX
#define AZ_IOT_DM_RID_MAX 32
#endif
/* Longest mqttv5 correlation data echoed back on a response. */
#ifndef AZ_IOT_DM_CORR_DATA_MAX
#define AZ_IOT_DM_CORR_DATA_MAX 64
#endif
/* Max concurrent in-flight invocations one client can hold. Requests may
 * outlive the handler (the application can respond asynchronously), so they
 * live in a bounded pool inside the caller-allocated client struct rather than
 * on the heap. */
#ifndef AZ_IOT_DM_MAX_INFLIGHT
#define AZ_IOT_DM_MAX_INFLIGHT 4
#endif

  /**
   * @brief Storage for one in-flight invocation.
   *
   * Lives in a bounded pool inside the caller-allocated client. Opaque to
   * applications -- they never hold one; they hold an
   * az_iot_direct_method_request naming it.
   *
   * The correlation fields are generation-specific: mqttv3 answers on a `$rid`,
   * mqttv5 echoes MQTT v5 correlation data. Both live here because the pool is
   * the one piece of this feature the two generations share.
   */
  typedef struct az_iot_direct_method_slot
  {
    struct
    {
      char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
      char rid[AZ_IOT_DM_RID_MAX];
      uint8_t correlation_data[AZ_IOT_DM_CORR_DATA_MAX];
      size_t correlation_data_len;
      /* Bumped every time this slot is handed out. A request naming an older
       * value is stale, which is what makes a reused slot detectable. Never 0
       * once issued, so a zeroed request cannot match a live slot. */
      uint32_t seq;
      bool in_use; /* acquired -> responded, or reclaimed on timeout */
    } _internal;
  } az_iot_direct_method_slot;

  /**
   * @brief Identifies one method invocation awaiting a response.
   *
   * Passed to the handler and back to the owning generation's respond call
   * **by value**, so it names a slot rather than pointing at one. A slot that
   * has since been reclaimed and handed to another invocation no longer
   * matches, and the stale response is refused instead of answering the wrong
   * call. Opaque to callers -- do NOT read _internal.
   */
  typedef struct az_iot_direct_method_request
  {
    struct
    {
      /* Which client issued it. Compared, never dereferenced -- it says which
       * pool the slot belongs to, so a request cannot resolve against a
       * different client that happens to have the same slot live. This is not
       * the pointer-as-storage the value handle replaced: that one *was* the
       * invocation, this one only names the pool it came from. */
      const void* owner;
      uint32_t slot;
      uint32_t seq;
      /* Which generation issued it, so a respond call from the other one is
       * refused rather than matching a slot index by coincidence. */
      az_iot_connection_profile profile;
    } _internal;
  } az_iot_direct_method_request;

  typedef void (*az_iot_direct_method_handler_callback)(
      az_iot_direct_method_request request,
      const char* method_name,
      const uint8_t* payload,
      size_t payload_len,
      void* user_ctx);

/* Max concurrent in-flight twin requests (GET + reported patch) one client can
 * hold. Each awaits a service response, so the slots live in a bounded pool
 * inside the caller-allocated client struct rather than on the heap. */
#ifndef AZ_IOT_TWIN_MAX_PENDING
#define AZ_IOT_TWIN_MAX_PENDING 8
#endif

  /**
   * @brief Delivers the result of a twin GET.
   *
   * @param status           AZ_IOT_OK when the service returned the document.
   * @param twin_payload     The twin document. Owned by the SDK and valid only
   *                         for the duration of this call; copy what you keep.
   *                         NULL on failure.
   * @param twin_payload_len Length of @p twin_payload.
   * @param user_ctx         Context passed to the get() call.
   */
  typedef void (*az_iot_twin_get_callback)(
      az_iot_result status,
      const uint8_t* twin_payload,
      size_t twin_payload_len,
      void* user_ctx);

  /**
   * @brief Delivers the outcome of a reported-properties patch.
   *
   * Named for completion rather than acknowledgement because it reports
   * failures too, and because on MQTT v5 an "ack" would be ambiguous with the
   * QoS 1 PUBACK -- this fires on the service's answer, not on the transport's.
   *
   * @param status   AZ_IOT_OK when the service accepted the patch.
   * @param version  The new version of the reported-properties section. An
   *                 application that tracks this can tell a lost update from an
   *                 applied one. 0 when the service did not send a version,
   *                 which includes every failure.
   * @param user_ctx Context passed to the patch call.
   */
  typedef void (
      *az_iot_twin_patch_complete_callback)(az_iot_result status, uint64_t version, void* user_ctx);

  /**
   * @brief Delivers a desired-properties patch pushed by the service.
   *
   * @param desired_patch     The patch. Owned by the SDK and valid only for the
   *                          duration of this call; copy what you keep.
   * @param desired_patch_len Length of @p desired_patch.
   * @param version           Version of the desired section this patch produced,
   *                          or 0 when the service did not send one.
   * @param user_ctx          Context passed to the set-handler call.
   */
  typedef void (*az_iot_twin_desired_callback)(
      const uint8_t* desired_patch,
      size_t desired_patch_len,
      uint64_t version,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MESSAGE_H */
