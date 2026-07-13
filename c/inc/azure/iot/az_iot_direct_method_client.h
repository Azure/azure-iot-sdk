// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_DIRECT_METHOD_CLIENT_H
#define AZ_IOT_DIRECT_METHOD_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "az_iot_result.h"
#include "az_iot_connection_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Storage bounds for the request handle (see az_iot_direct_method_request).
 * Compile-time footprint knobs: #define before including to tune. */
#ifndef AZ_IOT_DM_METHOD_NAME_MAX
#define AZ_IOT_DM_METHOD_NAME_MAX 96
#endif
#ifndef AZ_IOT_DM_RID_MAX
#define AZ_IOT_DM_RID_MAX         32
#endif
#ifndef AZ_IOT_DM_CORR_DATA_MAX
#define AZ_IOT_DM_CORR_DATA_MAX   64
#endif
/* Max concurrent in-flight method requests one client can hold. Requests may
 * outlive the handler (the app can respond asynchronously), so they live in a
 * bounded pool inside this caller-allocated struct rather than on the heap. */
#ifndef AZ_IOT_DM_MAX_INFLIGHT
#define AZ_IOT_DM_MAX_INFLIGHT    4
#endif

typedef struct az_iot_direct_method_client az_iot_direct_method_client;

/* Request handle delivered to the user's handler and passed back to
 * az_iot_direct_method_respond(). Opaque to callers -- do NOT read _internal. */
typedef struct az_iot_direct_method_request
{
    struct
    {
        az_iot_direct_method_client* owner;
        char rid[AZ_IOT_DM_RID_MAX];
        char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
        uint8_t correlation_data[AZ_IOT_DM_CORR_DATA_MAX];
        size_t correlation_data_len;
        bool is_next;
        bool in_use; /* pool slot occupied: acquired -> responded */
    } _internal;
} az_iot_direct_method_request;

typedef void (*az_iot_direct_method_handler_callback)(
    az_iot_direct_method_request* request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx);

struct az_iot_direct_method_client
{
    struct
    {
        az_iot_connection_client* conn;
        az_iot_direct_method_handler_callback handler;
        void* handler_ctx;
        az_iot_direct_method_request req_pool[AZ_IOT_DM_MAX_INFLIGHT];
    } _internal;
};

AZ_NODISCARD az_iot_result az_iot_direct_method_client_init(
    az_iot_direct_method_client* client,
    az_iot_connection_client* conn);

void az_iot_direct_method_client_destroy(az_iot_direct_method_client* client);

AZ_NODISCARD az_iot_result az_iot_direct_method_client_set_handler(
    az_iot_direct_method_client* dm,
    az_iot_direct_method_handler_callback cb,
    void* user_ctx);

AZ_NODISCARD az_iot_result az_iot_direct_method_respond(
    az_iot_direct_method_request* request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DIRECT_METHOD_CLIENT_H */
