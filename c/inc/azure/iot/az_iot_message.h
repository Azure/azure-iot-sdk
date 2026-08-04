// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MESSAGE_H
#define AZ_IOT_MESSAGE_H

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

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MESSAGE_H */
