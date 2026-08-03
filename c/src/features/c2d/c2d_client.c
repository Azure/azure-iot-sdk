// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* C2D Client (Cloud-to-Device message receiver).
 *
 * Supports both IoT Hub Classic (MQTT v3.1.1) and Hub-Next (MQTT v5).
 *
 * Classic:
 *   Subscribe  "devices/{device_id}/messages/devicebound/#"
 *   Inbound    "devices/{device_id}/messages/devicebound/{properties}"
 *
 * Next:
 *   Subscribe  "ih/{device_id}/dev/c2d"
 *   Inbound    "ih/{device_id}/dev/c2d"
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_c2d_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/protocol_profile.h"
#include "internal/span_writer.h"

#define AZ_IOT_C2D_TOPIC_MAX 192

/* Internal shorthand */
#define CI(c) ((c)->_internal)

/* ─────────────────────────────────────────────────────────────────────────── */
/* Inbound dispatch handlers                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

static void on_c2d_classic(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_c2d_client* c2d = (az_iot_c2d_client*)user_ctx;
  if (!c2d || !msg || !CI(c2d).handler)
    return;

  /* In Classic, content-type is encoded in the topic properties segment.
   * For simplicity, pass NULL for content_type; callers that need it can
   * parse the topic properties (same as azure-sdk-for-c does). */
  CI(c2d).handler(msg->payload, msg->payload_len, NULL, CI(c2d).handler_ctx);
}

static void on_c2d_next(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_c2d_client* c2d = (az_iot_c2d_client*)user_ctx;
  if (!c2d || !msg || !CI(c2d).handler)
    return;

  /* In Hub-Next, content-type comes as an MQTT v5 property. */
  CI(c2d).handler(msg->payload, msg->payload_len, msg->content_type, CI(c2d).handler_ctx);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Public API                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

az_iot_result az_iot_c2d_client_init(az_iot_c2d_client* client, az_iot_connection_client* conn)
{
  if (!client || !conn)
    return AZ_IOT_ERR_INVALID_ARG;

  const az_iot_protocol_profile* profile = az_iot_connection_client__profile(conn);
  if (!profile)
    return AZ_IOT_ERR_NOT_SUPPORTED;

  memset(client, 0, sizeof(*client));
  CI(client).conn = conn;

  if (profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
  {
    /* Hub-Next: subscribe to "ih/{device_id}/dev/c2d" */
    const char* device_id = az_iot_connection_client__device_id(conn);
    if (!device_id)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_NOT_INITIALIZED;
    }

    /* Dispatch prefix = exact topic (no wildcard in Next) */
    char prefix[AZ_IOT_C2D_TOPIC_MAX];
    const char* prefix_parts[] = { "ih/", device_id, "/dev/c2d" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), NULL, prefix_parts, 3)
        != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }

    az_iot_result r
        = az_iot_connection_client__register_inbound_handler(conn, prefix, on_c2d_next, client);
    if (r != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return r;
    }

    /* Subscribe to the same topic (no wildcard needed for Next) */
    r = az_iot_connection_client__add_subscription_on_connect(conn, prefix, AZ_IOT_MQTT_QOS_1);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }
  }
  else
  {
    /* Classic: subscribe to "devices/{device_id}/messages/devicebound/#"
     * The device_id for topic construction comes from the connection
     * client's registration_id (which equals the assigned device_id in the
     * DPS flow). The connection_client resolves this via its DPS config. */
    const char* device_id = conn->opts.dps.registration_id;
    if (!device_id)
      device_id = conn->opts.client_id;
    if (!device_id)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_NOT_INITIALIZED;
    }

    /* Dispatch prefix (no wildcard — prefix match handles sub-topics) */
    char prefix[AZ_IOT_C2D_TOPIC_MAX];
    const char* prefix_parts[] = { "devices/", device_id, "/messages/devicebound/" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), NULL, prefix_parts, 3)
        != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }

    az_iot_result r
        = az_iot_connection_client__register_inbound_handler(conn, prefix, on_c2d_classic, client);
    if (r != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return r;
    }

    /* MQTT subscription filter with '#' wildcard */
    char filter[AZ_IOT_C2D_TOPIC_MAX];
    const char* filter_parts[] = { "devices/", device_id, "/messages/devicebound/#" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(filter), NULL, filter_parts, 3)
        != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }

    r = az_iot_connection_client__add_subscription_on_connect(conn, filter, AZ_IOT_MQTT_QOS_1);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }
  }

  return AZ_IOT_OK;
}

void az_iot_c2d_client_destroy(az_iot_c2d_client* client)
{
  if (!client)
    return;
  (void)az_iot_connection_client__unregister_inbound_handlers(CI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_c2d_client_set_handler(
    az_iot_c2d_client* client,
    az_iot_c2d_handler_callback cb,
    void* user_ctx)
{
  if (!client)
    return AZ_IOT_ERR_INVALID_ARG;
  CI(client).handler = cb;
  CI(client).handler_ctx = user_ctx;
  return AZ_IOT_OK;
}
