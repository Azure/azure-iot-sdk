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

/* Max number of inbound-topic handlers a dispatch table can hold. */
#ifndef AZ_IOT_MAX_INBOUND_HANDLERS
#define AZ_IOT_MAX_INBOUND_HANDLERS 8
#endif
/* Max length (bytes) of a handler's topic-prefix match string. */
#ifndef AZ_IOT_DISPATCH_PREFIX_MAX
#define AZ_IOT_DISPATCH_PREFIX_MAX  128
#endif

/* Invoked when an inbound MQTT message's topic matches a registered prefix. */
typedef void (*az_iot_inbound_handler_callback)(
    void* user_ctx, const az_iot_mqtt_message* msg);

/* One routing rule: deliver messages whose topic starts with `prefix` to `cb`. */
typedef struct az_iot_dispatch_entry
{
    char prefix[AZ_IOT_DISPATCH_PREFIX_MAX];  /* topic-prefix to match          */
    size_t prefix_len;                        /* cached strlen(prefix)          */
    az_iot_inbound_handler_callback cb;       /* handler invoked on a match     */
    void* user_ctx;                           /* opaque arg passed to cb        */
    bool in_use;                              /* slot occupied?                 */
} az_iot_dispatch_entry;

/* Fixed-capacity table of prefix->handler routes, scanned on each inbound msg. */
typedef struct az_iot_dispatch_table
{
    az_iot_dispatch_entry entries[AZ_IOT_MAX_INBOUND_HANDLERS];
    size_t count;                             /* number of populated entries    */
} az_iot_dispatch_table;

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DISPATCH_H_PUBLIC */
