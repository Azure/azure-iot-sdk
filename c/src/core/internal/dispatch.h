// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Internal inbound-message dispatch table — function declarations.
 * Type definitions live in the public header (az_iot_dispatch.h) so the
 * connection_client struct can embed the table directly. */
#ifndef AZ_IOT_DISPATCH_H
#define AZ_IOT_DISPATCH_H

#include "azure/iot/az_iot_dispatch.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  void az_iot_dispatch_init(az_iot_dispatch_table* tbl);

  az_iot_result az_iot_dispatch_register_prefix(
      az_iot_dispatch_table* tbl,
      const char* topic_prefix,
      az_iot_inbound_handler_callback cb,
      void* user_ctx);

  size_t az_iot_dispatch_unregister_by_ctx(az_iot_dispatch_table* tbl, void* user_ctx);

  bool az_iot_dispatch_route(const az_iot_dispatch_table* tbl, const az_iot_mqtt_message* msg);

  size_t az_iot_dispatch_count(const az_iot_dispatch_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_DISPATCH_H */
