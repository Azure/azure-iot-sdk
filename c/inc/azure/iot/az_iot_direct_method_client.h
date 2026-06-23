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
typedef struct az_iot_direct_method_request_tag az_iot_direct_method_request_t;

typedef void (*az_iot_direct_method_handler_cb)(
    az_iot_direct_method_request_t* request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx);

typedef struct az_iot_direct_method_client_tag
{
    struct
    {
        az_iot_connection_client_t* conn;
        az_iot_direct_method_handler_cb handler;
        void* handler_ctx;
    } _internal;
} az_iot_direct_method_client_t;

az_iot_result_t az_iot_direct_method_client_init(
    az_iot_direct_method_client_t* client,
    az_iot_connection_client_t* conn);

void az_iot_direct_method_client_deinit(az_iot_direct_method_client_t* client);

az_iot_result_t az_iot_direct_method_client_set_handler(
    az_iot_direct_method_client_t* dm,
    az_iot_direct_method_handler_cb cb,
    void* user_ctx);

az_iot_result_t az_iot_direct_method_respond(
    az_iot_direct_method_request_t* request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DIRECT_METHOD_CLIENT_H */
