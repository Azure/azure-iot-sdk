// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_DISPATCH_H_PUBLIC
#define AZ_IOT_DISPATCH_H_PUBLIC

#include <stdbool.h>
#include <stddef.h>

#include "az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AZ_IOT_MAX_INBOUND_HANDLERS 8
#define AZ_IOT_DISPATCH_PREFIX_MAX  128

typedef void (*az_iot_inbound_handler_cb)(
    void* user_ctx, const az_iot_mqtt_message_t* msg);

typedef struct az_iot_dispatch_entry_tag
{
    char prefix[AZ_IOT_DISPATCH_PREFIX_MAX];
    size_t prefix_len;
    az_iot_inbound_handler_cb cb;
    void* user_ctx;
    bool in_use;
} az_iot_dispatch_entry_t;

typedef struct az_iot_dispatch_table_tag
{
    az_iot_dispatch_entry_t entries[AZ_IOT_MAX_INBOUND_HANDLERS];
    size_t count;
} az_iot_dispatch_table_t;

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DISPATCH_H_PUBLIC */
