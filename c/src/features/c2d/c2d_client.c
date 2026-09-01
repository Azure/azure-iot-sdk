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
 *   Covered by "ih/{device_id}/dev/#" from the presence handshake
 *   Inbound    "ih/{device_id}/dev/c2d"
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_c2d_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/protocol_profile.h"
#include "internal/span_writer.h"

#define AZ_IOT_C2D_TOPIC_MAX 192

/* Internal shorthand */
#define CI(c) ((c)->_internal)

/* ─────────────────────────────────────────────────────────────────────────── */
/* Property bag                                                               */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Scratch for one delivery: the decoded property text plus the key/value
 * pointers into it. Lives on the dispatch stack, so it costs nothing per
 * client and its lifetime matches exactly what the public header promises --
 * valid for the duration of the handler callback. */
typedef struct c2d_property_scratch
{
  char text[AZ_IOT_C2D_PROPERTY_BUFFER];
  az_iot_c2d_property properties[AZ_IOT_C2D_MAX_PROPERTIES];
  size_t count;
} c2d_property_scratch;

/* Decode one "key" / "key=" / "key=value" pair into the scratch and record it.
 * Returns false when the scratch is full, which is the caller's signal to stop
 * and warn rather than to fail the delivery.
 *
 * Keys and values are appended to one buffer as back-to-back NUL-terminated
 * strings, and each property points at where its own text began. */
static bool c2d_add_property(
    c2d_property_scratch* s,
    az_iot_span_writer* writer,
    const char* pair,
    size_t pair_len)
{
  if (pair_len == 0 || s->count >= AZ_IOT_C2D_MAX_PROPERTIES)
  {
    return false;
  }

  /* IoT Hub represents a null value as a bare key and an empty value as a
   * trailing '=', so the separator's presence carries meaning and must be
   * looked for before decoding (an encoded '=' inside a value is "%3D"). */
  const char* eq = (const char*)memchr(pair, '=', pair_len);
  size_t key_len = eq ? (size_t)(eq - pair) : pair_len;
  if (key_len == 0)
  {
    return false; /* "=value" has no name; not something the service emits. */
  }

  az_iot_c2d_property* p = &s->properties[s->count];

  p->key = s->text + az_iot_span_writer_length(writer);
  az_iot_span_writer_append_url_decoded(writer, pair, key_len);
  az_iot_span_writer_append_u8(writer, 0);

  if (eq == NULL)
  {
    p->value = NULL;
  }
  else
  {
    p->value = s->text + az_iot_span_writer_length(writer);
    az_iot_span_writer_append_url_decoded(writer, eq + 1, pair_len - key_len - 1);
    az_iot_span_writer_append_u8(writer, 0);
  }

  s->count++;
  return true;
}

/* Split "a=1&b&c=" into pairs and decode each one.
 *
 * The bag is everything after the topic prefix. A malformed escape latches on
 * the writer and is reported once at the end: the message itself is still
 * delivered, because losing a payload over an unreadable property would be a
 * worse outcome than delivering it with fewer properties. */
static void c2d_parse_property_bag(c2d_property_scratch* s, const char* bag, size_t bag_len)
{
  s->count = 0;
  if (bag == NULL || bag_len == 0)
  {
    return;
  }

  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(s->text));

  size_t start = 0;
  bool truncated = false;
  for (size_t i = 0; i <= bag_len; ++i)
  {
    if (i != bag_len && bag[i] != '&')
    {
      continue;
    }
    if (i > start && !c2d_add_property(s, &writer, bag + start, i - start))
    {
      truncated = (s->count >= AZ_IOT_C2D_MAX_PROPERTIES);
    }
    start = i + 1;
  }

  if (az_iot_span_writer_end(&writer, NULL) != AZ_IOT_OK)
  {
    /* Either the text did not fit or an escape was malformed. Whatever was
     * decoded before the failure may be incomplete, so surface none of it
     * rather than a truncated key or value. */
    AZ_IOT_LOG_WARN("c2d: dropping the properties of a message -- the bag did not decode into "
                    "AZ_IOT_C2D_PROPERTY_BUFFER bytes, or it carried a malformed percent-escape");
    s->count = 0;
  }
  else if (truncated)
  {
    AZ_IOT_LOG_WARNF(
        "c2d: delivering only the first %d properties of a message; raise "
        "AZ_IOT_C2D_MAX_PROPERTIES to see the rest",
        (int)AZ_IOT_C2D_MAX_PROPERTIES);
  }
}

const char* az_iot_c2d_message_property(const az_iot_c2d_message* msg, const char* key)
{
  /* properties may be NULL with a non-zero count only if a caller built the
   * struct by hand, but this is public API and a crash is a poor answer to
   * that. */
  if (!msg || !key || !msg->properties)
  {
    return NULL;
  }
  for (size_t i = 0; i < msg->properties_count; ++i)
  {
    if (msg->properties[i].key && strcmp(msg->properties[i].key, key) == 0)
    {
      return msg->properties[i].value;
    }
  }
  return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Inbound dispatch handlers                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */

static void on_c2d_classic(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_c2d_client* c2d = (az_iot_c2d_client*)user_ctx;
  if (!c2d || !msg || !CI(c2d).handler)
  {
    return;
  }

  /* Classic encodes every property into the topic after the devicebound
   * prefix: "devices/{id}/messages/devicebound/{property-bag}". Decode it back
   * into the plain text the sender used, so a property survives a round trip
   * through az_iot_telemetry_property unchanged. */
  c2d_property_scratch scratch;
  scratch.count = 0;
  if (msg->topic != NULL)
  {
    static const char k_marker[] = "/messages/devicebound/";
    const char* bag = strstr(msg->topic, k_marker);
    if (bag != NULL)
    {
      bag += sizeof(k_marker) - 1;
      c2d_parse_property_bag(&scratch, bag, strlen(bag));
    }
  }

  az_iot_c2d_message out;
  out.payload = msg->payload;
  out.payload_len = msg->payload_len;
  out.properties = scratch.properties;
  out.properties_count = scratch.count;
  out.content_type = az_iot_c2d_message_property(&out, AZ_IOT_MSG_PROP_CONTENT_TYPE);
  CI(c2d).handler(&out, CI(c2d).handler_ctx);
}

static void on_c2d_next(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_c2d_client* c2d = (az_iot_c2d_client*)user_ctx;
  if (!c2d || !msg || !CI(c2d).handler)
  {
    return;
  }

  /* Hub-Next carries properties as MQTT v5 User Properties, already decoded by
   * the adapter, and the content type in its own field. */
  az_iot_c2d_property properties[AZ_IOT_C2D_MAX_PROPERTIES];
  size_t count = 0;
  size_t seen = 0;
  for (size_t i = 0; i < msg->user_properties_count; ++i)
  {
    if (msg->user_properties[i].key == NULL)
    {
      continue;
    }
    seen++;
    if (count >= AZ_IOT_C2D_MAX_PROPERTIES)
    {
      continue;
    }
    properties[count].key = msg->user_properties[i].key;
    properties[count].value = msg->user_properties[i].value;
    count++;
  }
  if (seen > count)
  {
    /* Same contract the Classic path honours, and the same one the public
     * header states: the message still arrives, but say what was lost. */
    AZ_IOT_LOG_WARNF(
        "c2d: delivering only the first %d of %zu properties; raise "
        "AZ_IOT_C2D_MAX_PROPERTIES to see the rest",
        (int)AZ_IOT_C2D_MAX_PROPERTIES,
        seen);
  }

  az_iot_c2d_message out;
  out.payload = msg->payload;
  out.payload_len = msg->payload_len;
  out.properties = properties;
  out.properties_count = count;
  out.content_type = msg->content_type;
  CI(c2d).handler(&out, CI(c2d).handler_ctx);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Public API                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */

az_iot_result az_iot_c2d_client_init(az_iot_c2d_client* client, az_iot_connection_client* conn)
{
  if (!client || !conn)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  const az_iot_protocol_profile* profile = az_iot_connection_client__profile(conn);
  if (!profile)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  memset(client, 0, sizeof(*client));
  CI(client).conn = conn;

  if (profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
  {
    /* Hub-Next delivery comes through the presence handshake's device-wide wildcard. */
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
    /* No SUBSCRIBE here. The presence handshake already holds
     * ih/{device_id}/dev/#, which covers this topic, so registering it would
     * spend a registry slot and re-issue a redundant filter every reconnect. */
  }
  else
  {
    /* Classic: subscribe to "devices/{device_id}/messages/devicebound/#"
     * The device_id for topic construction comes from the connection
     * client's registration_id (which equals the assigned device_id in the
     * DPS flow). The connection_client resolves this via its DPS config. */
    const char* device_id = conn->opts.dps.registration_id;
    if (!device_id)
    {
      device_id = conn->opts.client_id;
    }
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
      (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }

    r = az_iot_connection_client__add_subscription_on_connect(
        conn, filter, AZ_IOT_MQTT_QOS_1, client, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
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
  {
    return;
  }
  (void)az_iot_connection_client__remove_subscriptions_for(CI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(CI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_c2d_client_set_handler(
    az_iot_c2d_client* client,
    az_iot_c2d_handler_callback cb,
    void* user_ctx)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  CI(client).handler = cb;
  CI(client).handler_ctx = user_ctx;
  return AZ_IOT_OK;
}
