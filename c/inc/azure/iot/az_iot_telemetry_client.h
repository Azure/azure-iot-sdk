// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_TELEMETRY_CLIENT_H
#define AZ_IOT_TELEMETRY_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"
#include "az_iot_mqtt_iface.h"
#include "az_iot_connection_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct az_iot_telemetry_client
{
    struct
    {
        az_iot_connection_client* conn;
    } _internal;
} az_iot_telemetry_client;

typedef struct az_iot_telemetry_property
{
    /* Plain text, never pre-encoded. On the Classic (MQTT v3.1.1) path the SDK
     * percent-encodes both halves into the topic's property bag, so "$.ct"
     * goes on the wire as "%24.ct" and a value of "application/json" as
     * "application%2Fjson". Pre-encoding here would be encoded again. On the
     * Hub-Next (MQTT v5) path they travel as User Properties and need no
     * encoding at all. */
    const char* key;
    const char* value;
} az_iot_telemetry_property;

/* Well-known system property keys. Use these as the `key` in
 * az_iot_telemetry_property to set IoT Hub system properties. They are spelled
 * in readable form; azure-sdk-for-c spells the same names pre-encoded
 * ("%24.ct"), and the bytes this SDK publishes are identical. */
#define AZ_IOT_MSG_PROP_CONTENT_TYPE     "$.ct"
#define AZ_IOT_MSG_PROP_CONTENT_ENCODING "$.ce"
#define AZ_IOT_MSG_PROP_MESSAGE_ID       "$.mid"
#define AZ_IOT_MSG_PROP_CORRELATION_ID   "$.cid"
#define AZ_IOT_MSG_PROP_USER_ID          "$.uid"
#define AZ_IOT_MSG_PROP_CREATION_TIME    "$.ctime"
#define AZ_IOT_MSG_PROP_COMPONENT_NAME   "$.sub"

typedef struct az_iot_telemetry_message
{
    const uint8_t* payload;
    size_t payload_len;
    const az_iot_telemetry_property* properties;
    size_t properties_count;
} az_iot_telemetry_message;

typedef void (*az_iot_telemetry_send_callback)(az_iot_result status, void* user_ctx);

AZ_NODISCARD az_iot_result az_iot_telemetry_client_init(
    az_iot_telemetry_client* client,
    az_iot_connection_client* conn);

void az_iot_telemetry_client_destroy(az_iot_telemetry_client* client);

AZ_NODISCARD az_iot_result az_iot_telemetry_client_send(
    az_iot_telemetry_client* client,
    const az_iot_telemetry_message* msg,
    az_iot_telemetry_send_callback cb,
    void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_TELEMETRY_CLIENT_H */
