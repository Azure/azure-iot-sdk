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

#ifndef AZ_IOT_MAX_INBOUND_HANDLERS
#define AZ_IOT_MAX_INBOUND_HANDLERS 8
#endif
#ifndef AZ_IOT_DISPATCH_PREFIX_MAX
#define AZ_IOT_DISPATCH_PREFIX_MAX  128
#endif

typedef void (*az_iot_inbound_handler_callback)(
    void* user_ctx, const az_iot_mqtt_message* msg);

typedef struct az_iot_dispatch_entry
{
    char prefix[AZ_IOT_DISPATCH_PREFIX_MAX];
    size_t prefix_len;
    az_iot_inbound_handler_callback cb;
    void* user_ctx;
    bool in_use;
} az_iot_dispatch_entry;

typedef struct az_iot_dispatch_table
{
    az_iot_dispatch_entry entries[AZ_IOT_MAX_INBOUND_HANDLERS];
    size_t count;
} az_iot_dispatch_table;

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DISPATCH_H_PUBLIC */
