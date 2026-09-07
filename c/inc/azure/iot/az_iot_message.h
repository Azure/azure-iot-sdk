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

  /* Shared by the gen1 and gen2 telemetry clients. Callers always pass plain
   * text: gen1 percent-encodes properties into the topic, while gen2 carries
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
 * message. Only gen1 decodes -- gen2 user properties arrive already decoded --
 * and decoding never grows the text, so this only has to cover the
 * property-bag portion of a gen1 topic. */
#ifndef AZ_IOT_C2D_PROPERTY_BUFFER
#define AZ_IOT_C2D_PROPERTY_BUFFER 256
#endif

  /**
   * @brief One property carried by a cloud-to-device message.
   *
   * Plain text, already decoded: gen1 reverses the percent-encoding IoT Hub
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
    /** Convenience view of the `$.ct` property (gen1) or the MQTT v5
     * content-type (gen2). NULL when the message carries none. */
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

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MESSAGE_H */
