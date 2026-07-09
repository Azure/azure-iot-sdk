// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_TWIN_CLIENT_H
#define AZ_IOT_TWIN_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"
#include "az_iot_connection_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*az_iot_twin_get_callback)(
    az_iot_result status,
    const uint8_t* twin_payload,
    size_t twin_payload_len,
    void* user_ctx);

typedef void (*az_iot_twin_patch_ack_callback)(
    az_iot_result status,
    void* user_ctx);

typedef void (*az_iot_twin_desired_callback)(
    const uint8_t* desired_patch,
    size_t desired_patch_len,
    uint64_t version,
    void* user_ctx);

#define AZ_IOT_TWIN_MAX_PENDING 8

/* Desired-property subscriber registry capacity (compile-time configurable).
 * Two pools: feature-client slots (e.g. ADU) are notified before application
 * slots. See az_iot_twin_client_subscribe_desired(). */
#ifndef AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS
#define AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS 2
#endif
#ifndef AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS
#define AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS 2
#endif

typedef struct az_iot_twin_desired_sub
{
    az_iot_twin_desired_callback cb;
    void*                  user_ctx;
    bool                   in_use;
} az_iot_twin_desired_sub;

typedef struct az_iot_twin_client
{
    struct
    {
        az_iot_connection_client* conn;
        /* Desired-property subscriber registry. Feature-client slots are
         * dispatched before application slots (two-pass). */
        az_iot_twin_desired_sub   desired_feature_subs[AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS];
        az_iot_twin_desired_sub   desired_app_subs[AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS];
        bool                        dispatching;
        uint32_t                    next_rid;
        struct
        {
            bool     in_use;
            uint32_t rid;
            int      kind;  /* internal enum */
            union {
                az_iot_twin_get_callback       get_cb;
                az_iot_twin_patch_ack_callback patch_cb;
            } cb;
            void* user_ctx;
        } pending[AZ_IOT_TWIN_MAX_PENDING];
    } _internal;
} az_iot_twin_client;

az_iot_result az_iot_twin_client_init(
    az_iot_twin_client* client,
    az_iot_connection_client* conn);

void az_iot_twin_client_destroy(az_iot_twin_client* client);

az_iot_result az_iot_twin_client_get(az_iot_twin_client* twin, az_iot_twin_get_callback cb, void* user_ctx);

az_iot_result az_iot_twin_client_patch_reported(
    az_iot_twin_client* twin,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_twin_patch_ack_callback cb,
    void* user_ctx);

/* Register/unregister an application subscriber for desired-property patches.
 * Each subscriber receives the full patch on every desired update and decides
 * whether it carries keys it cares about. Returns AZ_IOT_ERR_NOT_SUPPORTED when
 * the application pool is full, AZ_IOT_ERR_BUSY if called from within a dispatch.
 */
az_iot_result az_iot_twin_client_subscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx);

az_iot_result az_iot_twin_client_unsubscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_TWIN_CLIENT_H */
