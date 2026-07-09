// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_DIRECT_METHOD_CLIENT_H
#define AZ_IOT_DIRECT_METHOD_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"
#include "az_iot_connection_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque request handle delivered to the user; required to respond. */
typedef struct az_iot_direct_method_request az_iot_direct_method_request;

typedef void (*az_iot_direct_method_handler_callback)(
    az_iot_direct_method_request* request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx);

typedef struct az_iot_direct_method_client
{
    struct
    {
        az_iot_connection_client* conn;
        az_iot_direct_method_handler_callback handler;
        void* handler_ctx;
    } _internal;
} az_iot_direct_method_client;

az_iot_result az_iot_direct_method_client_init(
    az_iot_direct_method_client* client,
    az_iot_connection_client* conn);

void az_iot_direct_method_client_destroy(az_iot_direct_method_client* client);

az_iot_result az_iot_direct_method_client_set_handler(
    az_iot_direct_method_client* dm,
    az_iot_direct_method_handler_callback cb,
    void* user_ctx);

az_iot_result az_iot_direct_method_respond(
    az_iot_direct_method_request* request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DIRECT_METHOD_CLIENT_H */
