// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv3 hub C2D receiver.
 *
 *   Subscribe  "devices/{device_id}/messages/devicebound/#"
 *   Inbound    "devices/{device_id}/messages/devicebound/{property-bag}"
 *
 * Per the MQTTv3 topic spec the bag is percent-encoded, so every key and
 * value is decoded back into the plain text the sender used -- a property then
 * survives a round trip through az_iot_telemetry_property unchanged.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/mqttv3/az_iot_c2d_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#define AZ_IOT_C2D_TOPIC_MAX 192

#define CI(c) ((c)->_internal)

/** @brief System properties surfaced as typed fields of az_iot_c2d_message. */
typedef enum c2d_system_property
{
  C2D_SYS_CONTENT_TYPE,
  C2D_SYS_MESSAGE_ID,
  C2D_SYS_CORRELATION_ID,
  C2D_SYS_EXPIRY_TIME,
  C2D_SYS_CONTENT_ENCODING,
  C2D_SYS_USER_ID,
  C2D_SYS_COUNT
} c2d_system_property;

/* Indexed by c2d_system_property. */
static const char* const k_system_keys[C2D_SYS_COUNT] = {
  AZ_IOT_MSG_PROP_CONTENT_TYPE, AZ_IOT_MSG_PROP_MESSAGE_ID,       AZ_IOT_MSG_PROP_CORRELATION_ID,
  AZ_IOT_MSG_PROP_EXPIRY_TIME,  AZ_IOT_MSG_PROP_CONTENT_ENCODING, AZ_IOT_MSG_PROP_USER_ID,
};

/* Longest system key ("$.mid") with headroom; a longer key is not a system key. */
#define C2D_SYSTEM_KEY_MAX 8

/**
 * @brief Decoded properties of one delivery.
 *
 * Lives on the dispatch stack, so it costs nothing per client and is valid for
 * the duration of the handler callback. @p text is shared: typed system
 * property values are stored from its end and the generic list from its start,
 * so the typed values survive a generic list that does not fit.
 */
typedef struct c2d_property_parse_result
{
  char text[AZ_IOT_C2D_PROPERTY_BUFFER];
  size_t system_property_values_size; /**< Bytes used at the end of @p text. */
  const char* system_property_values[C2D_SYS_COUNT];
  /** Pair each typed value was taken from; NULL when the key was not found. */
  const char* system_property_found_at[C2D_SYS_COUNT];
  az_iot_c2d_property properties[AZ_IOT_C2D_MAX_PROPERTIES];
  size_t properties_count;
} c2d_property_parse_result;

/**
 * @brief Length @p value percent-decodes to.
 *
 * @return The decoded length, or SIZE_MAX when an escape is truncated. Bad hex
 *         digits are left to the decoder to reject.
 */
static size_t c2d_url_decoded_length(const char* value, size_t length)
{
  size_t decoded = 0;
  size_t i = 0;
  while (i < length)
  {
    if (value[i] == '%')
    {
      if (length - i < 3)
      {
        return SIZE_MAX;
      }
      i += 3;
    }
    else
    {
      i++;
    }
    decoded++;
  }
  return decoded;
}

/**
 * @brief Store @p pair's value at the end of @p r->text when its key is a known
 *        system property.
 *
 * Only the first occurrence of a key counts, matching
 * az_iot_c2d_message_property(). A value that does not fit, does not decode or
 * decodes to an embedded NUL leaves the field NULL and is logged.
 */
static void c2d_capture_system_property(
    c2d_property_parse_result* r,
    const char* pair,
    size_t pair_len)
{
  const char* eq = (const char*)memchr(pair, '=', pair_len);
  size_t key_len = eq ? (size_t)(eq - pair) : pair_len;

  uint8_t key_buffer[C2D_SYSTEM_KEY_MAX];
  az_iot_span_writer key_writer;
  az_iot_span_writer_init(&key_writer, AZ_SPAN_FROM_BUFFER(key_buffer));
  az_iot_span_writer_append_url_decoded(&key_writer, pair, key_len);
  az_span key;
  if (az_iot_span_writer_end(&key_writer, &key) != AZ_IOT_OK)
  {
    return; /* Too long or malformed: not a system key. */
  }

  size_t id = 0;
  while (id < C2D_SYS_COUNT
         && !az_span_is_content_equal(
             key, az_span_create_from_str((char*)(uintptr_t)k_system_keys[id])))
  {
    ++id;
  }
  if (id == C2D_SYS_COUNT || r->system_property_found_at[id] != NULL)
  {
    return;
  }
  r->system_property_found_at[id] = pair;
  if (eq == NULL)
  {
    return; /* Sent with no value. */
  }

  const char* encoded = eq + 1;
  size_t encoded_len = pair_len - key_len - 1;
  size_t available = sizeof(r->text) - r->system_property_values_size;
  size_t decoded_len = c2d_url_decoded_length(encoded, encoded_len);
  bool stored = false;
  if (decoded_len < available)
  {
    char* dst = r->text + available - decoded_len - 1;
    az_iot_span_writer value_writer;
    az_iot_span_writer_init(
        &value_writer, az_span_create((uint8_t*)dst, (int32_t)(decoded_len + 1)));
    az_iot_span_writer_append_url_decoded(&value_writer, encoded, encoded_len);
    if (az_iot_span_writer_end(&value_writer, NULL) == AZ_IOT_OK
        && memchr(dst, '\0', decoded_len) == NULL)
    {
      dst[decoded_len] = '\0';
      r->system_property_values[id] = dst;
      r->system_property_values_size += decoded_len + 1;
      stored = true;
    }
  }
  if (!stored)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_C2D,
        "omitting %s from a message -- it did not decode into the remaining "
        "AZ_IOT_C2D_PROPERTY_BUFFER bytes, or it carried a malformed or NUL percent-escape",
        k_system_keys[id]);
  }
}

/* Decode one "key" / "key=" / "key=value" pair into the generic list. A pair a
 * typed value was taken from reuses that value instead of decoding it again.
 * Returns false when the list is full, which is the caller's signal to stop
 * and warn rather than to fail the delivery. */
static bool c2d_add_property(
    c2d_property_parse_result* r,
    az_iot_span_writer* writer,
    const char* pair,
    size_t pair_len)
{
  if (pair_len == 0 || r->properties_count >= AZ_IOT_C2D_MAX_PROPERTIES)
  {
    return false;
  }

  az_iot_c2d_property* p = &r->properties[r->properties_count];
  for (size_t id = 0; id < C2D_SYS_COUNT; ++id)
  {
    if (r->system_property_found_at[id] == pair && r->system_property_values[id] != NULL)
    {
      p->key = k_system_keys[id];
      p->value = r->system_property_values[id];
      r->properties_count++;
      return true;
    }
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

  p->key = r->text + az_iot_span_writer_length(writer);
  az_iot_span_writer_append_url_decoded(writer, pair, key_len);
  az_iot_span_writer_append_u8(writer, 0);

  if (eq == NULL)
  {
    p->value = NULL;
  }
  else
  {
    p->value = r->text + az_iot_span_writer_length(writer);
    az_iot_span_writer_append_url_decoded(writer, eq + 1, pair_len - key_len - 1);
    az_iot_span_writer_append_u8(writer, 0);
  }

  r->properties_count++;
  return true;
}

/** @brief Calls @p fn on each non-empty '&'-separated pair of @p bag. */
static void c2d_for_each_pair(
    const char* bag,
    size_t bag_len,
    void (*fn)(void* ctx, const char* pair, size_t pair_len),
    void* ctx)
{
  size_t start = 0;
  for (size_t i = 0; i <= bag_len; ++i)
  {
    if (i != bag_len && bag[i] != '&')
    {
      continue;
    }
    if (i > start)
    {
      fn(ctx, bag + start, i - start);
    }
    start = i + 1;
  }
}

static void c2d_capture_pair(void* ctx, const char* pair, size_t pair_len)
{
  c2d_capture_system_property((c2d_property_parse_result*)ctx, pair, pair_len);
}

typedef struct c2d_generic_pass
{
  c2d_property_parse_result* result;
  az_iot_span_writer writer;
  bool truncated;
} c2d_generic_pass;

static void c2d_add_pair(void* ctx, const char* pair, size_t pair_len)
{
  c2d_generic_pass* pass = (c2d_generic_pass*)ctx;
  if (!c2d_add_property(pass->result, &pass->writer, pair, pair_len))
  {
    pass->truncated = (pass->result->properties_count >= AZ_IOT_C2D_MAX_PROPERTIES);
  }
}

/* Split "a=1&b&c=" into pairs and decode them: typed system properties first,
 * into the end of the buffer, then the generic list into what is left.
 *
 * A malformed escape latches on the writer and is reported once at the end:
 * the message itself is still delivered, because losing a payload over an
 * unreadable property would be a worse outcome than delivering it with fewer
 * properties. */
static void c2d_parse_property_bag(c2d_property_parse_result* r, const char* bag, size_t bag_len)
{
  if (bag == NULL || bag_len == 0)
  {
    return;
  }

  c2d_for_each_pair(bag, bag_len, c2d_capture_pair, r);

  c2d_generic_pass pass;
  pass.result = r;
  pass.truncated = false;
  az_iot_span_writer_init(
      &pass.writer,
      az_span_create(
          (uint8_t*)r->text, (int32_t)(sizeof(r->text) - r->system_property_values_size)));
  c2d_for_each_pair(bag, bag_len, c2d_add_pair, &pass);

  if (az_iot_span_writer_end(&pass.writer, NULL) != AZ_IOT_OK)
  {
    /* Either the text did not fit or an escape was malformed. Whatever was
     * decoded before the failure may be incomplete, so surface none of it
     * rather than a truncated key or value. */
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_C2D,
        "dropping the properties of a message -- the bag did not decode into "
        "AZ_IOT_C2D_PROPERTY_BUFFER bytes, or it carried a malformed percent-escape");
    r->properties_count = 0;
  }
  else if (pass.truncated)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_C2D,
        "delivering only the first %d properties of a message; raise "
        "AZ_IOT_C2D_MAX_PROPERTIES to see the rest",
        (int)AZ_IOT_C2D_MAX_PROPERTIES);
  }
}

static void on_c2d(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_mqttv3_c2d_client* c2d = (az_iot_mqttv3_c2d_client*)user_ctx;
  if (!c2d || !msg || !CI(c2d).handler)
  {
    return;
  }

  c2d_property_parse_result parsed;
  parsed.properties_count = 0;
  parsed.system_property_values_size = 0;
  for (size_t i = 0; i < C2D_SYS_COUNT; ++i)
  {
    parsed.system_property_values[i] = NULL;
    parsed.system_property_found_at[i] = NULL;
  }
  if (msg->topic != NULL)
  {
    static const char k_marker[] = "/messages/devicebound/";
    const char* bag = strstr(msg->topic, k_marker);
    if (bag != NULL)
    {
      bag += sizeof(k_marker) - 1;
      c2d_parse_property_bag(&parsed, bag, strlen(bag));
    }
  }

  az_iot_c2d_message out;
  out.payload = msg->payload;
  out.payload_len = msg->payload_len;
  out.properties = parsed.properties;
  out.properties_count = parsed.properties_count;
  out.content_type = parsed.system_property_values[C2D_SYS_CONTENT_TYPE];
  out.message_id = parsed.system_property_values[C2D_SYS_MESSAGE_ID];
  out.correlation_id = parsed.system_property_values[C2D_SYS_CORRELATION_ID];
  out.expiry_time = parsed.system_property_values[C2D_SYS_EXPIRY_TIME];
  out.content_encoding = parsed.system_property_values[C2D_SYS_CONTENT_ENCODING];
  out.user_id = parsed.system_property_values[C2D_SYS_USER_ID];
  CI(c2d).handler(&out, CI(c2d).handler_ctx);
}

static az_iot_result bind_topics(void* owner, az_iot_connection_client* conn)
{
  az_iot_mqttv3_c2d_client* client = (az_iot_mqttv3_c2d_client*)owner;

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

AZ_NODISCARD az_iot_result
az_iot_mqttv3_c2d_client_init(az_iot_mqttv3_c2d_client* client, az_iot_connection_client* conn)
{
  if (!client || !conn)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_result result
      = az_iot_connection_client__require_profile(conn, AZ_IOT_CONNECTION_PROFILE_MQTT_V3);
  if (result != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return result;
  }

  memset(client, 0, sizeof(*client));
  CI(client).conn = conn;

  result = az_iot_connection_client__register_feature_client_bind(conn, client, bind_topics);
  if (result != AZ_IOT_OK)
  {
    az_iot_connection_client__release_profile(conn);
    (void)az_iot_connection_client__remove_subscriptions_for(conn, client);
    (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
    az_iot_connection_client__unregister_feature_client_bind(conn, client);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_mqttv3_c2d_client_deinit(az_iot_mqttv3_c2d_client* client)
{
  if (!client)
  {
    return;
  }
  az_iot_connection_client__unregister_feature_client_bind(CI(client).conn, client);
  az_iot_connection_client__release_profile(CI(client).conn);
  (void)az_iot_connection_client__remove_subscriptions_for(CI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(CI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_mqttv3_c2d_client_set_handler(
    az_iot_mqttv3_c2d_client* client,
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
