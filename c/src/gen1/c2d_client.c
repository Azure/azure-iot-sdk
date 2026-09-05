// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* IoT Hub Classic C2D receiver.
 *
 *   Subscribe  "devices/{device_id}/messages/devicebound/#"
 *   Inbound    "devices/{device_id}/messages/devicebound/{property-bag}"
 *
 * Per the Classic MQTT topic spec the bag is percent-encoded, so every key and
 * value is decoded back into the plain text the sender used -- a property then
 * survives a round trip through az_iot_telemetry_property unchanged.
 */
#include <stdbool.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/gen1/az_iot_c2d_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_C2D_TOPIC_MAX 192

#define CI(c) ((c)->_internal)

/* Scratch for one delivery: the decoded property text plus the key/value
 * pointers into it. Lives on the dispatch stack, so it costs nothing per
 * client and its lifetime matches what the public header promises -- valid for
 * the duration of the handler callback. */
typedef struct c2d_property_scratch
{
  char text[AZ_IOT_C2D_PROPERTY_BUFFER];
  az_iot_c2d_property properties[AZ_IOT_C2D_MAX_PROPERTIES];
  size_t count;
} c2d_property_scratch;

/* Decode one "key" / "key=" / "key=value" pair into the scratch and record it.
 * Returns false when the scratch is full, which is the caller's signal to stop
 * and warn rather than to fail the delivery. */
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
 * A malformed escape latches on the writer and is reported once at the end:
 * the message itself is still delivered, because losing a payload over an
 * unreadable property would be a worse outcome than delivering it with fewer
 * properties. */
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

static void on_c2d(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_gen1_c2d_client* c2d = (az_iot_gen1_c2d_client*)user_ctx;
  if (!c2d || !msg || !CI(c2d).handler)
  {
    return;
  }

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

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_gen1_c2d_client* client = (az_iot_gen1_c2d_client*)owner;

  const char* device_id = az_iot_connection_client__device_id(conn);
  if (!device_id)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  /* Dispatch prefix carries no wildcard -- prefix match handles the bag. */
  char prefix[AZ_IOT_C2D_TOPIC_MAX];
  const char* prefix_parts[] = { "devices/", device_id, "/messages/devicebound/" };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), NULL, prefix_parts, 3) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_iot_result result
      = az_iot_connection_client__register_inbound_handler(conn, prefix, on_c2d, client);
  if (result != AZ_IOT_OK)
  {
    return result;
  }

  char filter[AZ_IOT_C2D_TOPIC_MAX];
  const char* filter_parts[] = { "devices/", device_id, "/messages/devicebound/#" };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(filter), NULL, filter_parts, 3) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  return az_iot_connection_client__add_subscription_on_connect(
      conn, filter, AZ_IOT_MQTT_QOS_1, client, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL);
}

az_iot_result az_iot_gen1_c2d_client_init(
    az_iot_gen1_c2d_client* client,
    az_iot_connection_client* conn)
{
  if (!client || !conn)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_result result
      = az_iot_connection_client__require_profile(conn, AZ_IOT_CONNECTION_PROFILE_CLASSIC);
  if (result != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return result;
  }

  memset(client, 0, sizeof(*client));
  CI(client).conn = conn;

  result = az_iot_connection_client__register_feature_bind(conn, client, bind_topics);
  if (result != AZ_IOT_OK)
  {
    az_iot_connection_client__release_profile(conn);
    (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
    (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
    az_iot_connection_client__unregister_feature_bind(conn, client);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_gen1_c2d_client_destroy(az_iot_gen1_c2d_client* client)
{
  if (!client)
  {
    return;
  }
  az_iot_connection_client__unregister_feature_bind(CI(client).conn, client);
  az_iot_connection_client__release_profile(CI(client).conn);
  (void)az_iot_connection_client__remove_subscriptions_for(CI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(CI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_gen1_c2d_client_set_handler(
    az_iot_gen1_c2d_client* client,
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
