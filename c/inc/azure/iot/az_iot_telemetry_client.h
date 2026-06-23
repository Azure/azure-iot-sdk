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

typedef struct az_iot_telemetry_client_tag
{
    struct
    {
        az_iot_connection_client_t* conn;
    } _internal;
} az_iot_telemetry_client_t;

typedef struct az_iot_telemetry_property_tag
{
    const char* key;
    const char* value;
} az_iot_telemetry_property_t;

/* Well-known system property keys. Use these as the `key` in
 * az_iot_telemetry_property_t to set IoT Hub system properties. */
#define AZ_IOT_MSG_PROP_CONTENT_TYPE     "$.ct"
#define AZ_IOT_MSG_PROP_CONTENT_ENCODING "$.ce"
#define AZ_IOT_MSG_PROP_MESSAGE_ID       "$.mid"
#define AZ_IOT_MSG_PROP_CORRELATION_ID   "$.cid"
#define AZ_IOT_MSG_PROP_USER_ID          "$.uid"
#define AZ_IOT_MSG_PROP_CREATION_TIME    "$.ctime"
#define AZ_IOT_MSG_PROP_COMPONENT_NAME   "$.sub"

typedef struct az_iot_telemetry_message_tag
{
    const uint8_t* payload;
    size_t payload_len;
    const az_iot_telemetry_property_t* properties;
    size_t properties_count;
} az_iot_telemetry_message_t;

typedef void (*az_iot_telemetry_send_cb)(az_iot_result_t status, void* user_ctx);

az_iot_result_t az_iot_telemetry_client_init(
    az_iot_telemetry_client_t* client,
    az_iot_connection_client_t* conn);

void az_iot_telemetry_client_deinit(az_iot_telemetry_client_t* client);

az_iot_result_t az_iot_telemetry_client_send(
    az_iot_telemetry_client_t* client,
    const az_iot_telemetry_message_t* msg,
    az_iot_telemetry_send_cb cb,
    void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_TELEMETRY_CLIENT_H */
