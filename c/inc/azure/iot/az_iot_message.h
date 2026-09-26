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
 * out, read one off an az_iot_c2d_message coming in. The same spelling works
 * either way because the SDK owns the encoding -- these are the plain,
 * human-readable names, never the pre-encoded form.
 *
 * On the Classic (MQTT v3.1.1) path the SDK percent-encodes both halves into
 * the topic's property bag, so "$.ct" travels as "%24.ct" and a value of
 * "application/json" as "application%2Fjson", and decodes them again on the way
 * in. azure-sdk-for-c spells the same names pre-encoded
 * (AZ_IOT_MESSAGE_PROPERTIES_CONTENT_TYPE is "%24.ct"); the bytes on the wire
 * are identical. On the Hub-Next (MQTT v5) path they travel as User Properties
 * and need no encoding at all.
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

  /* Shared by the mqttv3 and mqttv5 telemetry clients. Callers always pass plain
   * text: mqttv3 percent-encodes properties into the topic, while mqttv5 carries
   * them as MQTT v5 User Properties. */
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

/* Maximum number of properties surfaced on one received C2D message. A message
 * carrying more is still delivered; the properties past this bound are dropped
 * and a warning is logged. */
#ifndef AZ_IOT_C2D_MAX_PROPERTIES
#define AZ_IOT_C2D_MAX_PROPERTIES 8
#endif
/* Bytes available to hold the decoded property names and values of one
 * message. Only mqttv3 decodes -- mqttv5 user properties arrive already decoded --
 * and decoding never grows the text, so this only has to cover the
 * property-bag portion of an mqttv3 topic. */
#ifndef AZ_IOT_C2D_PROPERTY_BUFFER
#define AZ_IOT_C2D_PROPERTY_BUFFER 256
#endif

  /**
   * @brief One property carried by a cloud-to-device message.
   *
   * Plain text, already decoded: mqttv3 reverses the percent-encoding IoT Hub
   * applies to the topic property bag, so a value sent as `application%2Fjson`
   * arrives as `application/json` and a system property key arrives as `$.ct`
   * rather than `%24.ct`. Same shape az_iot_telemetry_property uses on the way
   * out, so a property survives a round trip unchanged.
   */
  typedef struct az_iot_c2d_property
  {
    const char* key;
    const char* value; /**< NULL when the property was sent with no value. */
  } az_iot_c2d_property;

  /**
   * @brief A received cloud-to-device message.
   *
   * Every pointer is owned by the SDK and valid only for the duration of the
   * handler callback. Copy anything the application needs to keep.
   */
  typedef struct az_iot_c2d_message
  {
    const uint8_t* payload; /**< May be NULL when @p payload_len is 0. */
    size_t payload_len;
    /** Convenience view of the `$.ct` property (mqttv3) or the MQTT v5
     * content-type (mqttv5). NULL when the message carries none. */
    const char* content_type;
    const az_iot_c2d_property* properties;
    size_t properties_count;
  } az_iot_c2d_message;

  typedef void (*az_iot_c2d_handler_callback)(const az_iot_c2d_message* msg, void* user_ctx);

  /**
   * @brief Look up one property by name.
   *
   * @return The property value, or NULL when the message carries no such
   *         property. A property sent with no value also yields NULL; walk
   *         @p msg->properties directly to tell the two apart.
   */
  const char* az_iot_c2d_message_property(const az_iot_c2d_message* msg, const char* key);

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
