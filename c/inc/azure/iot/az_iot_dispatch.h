// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_DISPATCH_H_PUBLIC
#define AZ_IOT_DISPATCH_H_PUBLIC

#include <stdbool.h>
#include <stddef.h>

#include "az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(AZ_IOT_MAX_INBOUND_HANDLERS) || defined(AZ_IOT_DISPATCH_PREFIX_MAX)
#error "Dispatch sizes are no longer set by macros: use az_iot_connection_client_limits."
#endif

/* Max length (bytes) of a handler's topic-prefix match string, terminator included. */
#define AZ_IOT_DISPATCH_PREFIX_MAX 128

  /* Invoked when an inbound MQTT message's topic matches a registered prefix. */
  typedef void (*az_iot_inbound_handler_callback)(void* user_ctx, const az_iot_mqtt_message* msg);

  /* One routing rule: deliver messages whose topic starts with `prefix` to `cb`. */
  typedef struct az_iot_dispatch_entry
  {
    char prefix[AZ_IOT_DISPATCH_PREFIX_MAX]; /* topic-prefix to match          */
    size_t prefix_len; /* cached strlen(prefix)          */
    az_iot_inbound_handler_callback cb; /* handler invoked on a match     */
    void* user_ctx; /* opaque arg passed to cb        */
    bool in_use; /* slot occupied?                 */
  } az_iot_dispatch_entry;

  /* Table of prefix->handler routes over caller storage, scanned on each inbound msg. */
  typedef struct az_iot_dispatch_table
  {
    az_iot_dispatch_entry* entries; /* capacity entries               */
    size_t capacity; /* entries[] length               */
    size_t count; /* number of populated entries    */
  } az_iot_dispatch_table;

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DISPATCH_H_PUBLIC */
