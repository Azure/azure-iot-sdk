// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Readers over the property bag of a received C2D message. Only mqttv3
 * produces az_iot_c2d_message: C2D is not carried on the MQTT v5 hub.
 *
 * The bag is "k1=v1&k2&k3=" as it appears in the topic, percent-encoded. It is
 * never copied or modified; each call decodes one name or value into a buffer
 * the caller owns. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "azure/iot/az_iot_message.h"

#include "internal/span_writer.h"

/**
 * @brief Validate and measure @p in percent-decoded; a decoded NUL is
 *        malformed, as it would silently cut the C string.
 */
static az_iot_result c2d_measure(const char* in, size_t len, size_t* out_len)
{
  return az_iot_url_decoded_length(in, len, true, out_len);
}

/** @brief Decode measured @p in into @p out and NUL-terminate it. */
static void c2d_write(const char* in, size_t len, size_t decoded_len, char* out)
{
  az_iot_url_decode_into(in, len, (uint8_t*)out);
  out[decoded_len] = '\0';
}

/** @brief Whether @p in percent-decodes to exactly @p key. */
static bool c2d_key_equals(const char* in, size_t len, const char* key)
{
  size_t pos = 0;
  size_t k = 0;
  while (pos < len)
  {
    int b = az_iot_url_decode_byte(in, len, &pos);
    if (b <= 0 || key[k] == '\0' || (unsigned char)key[k] != (unsigned char)b)
    {
      return false;
    }
    ++k;
  }
  return key[k] == '\0';
}

/**
 * @brief Find the next non-empty pair at or after @p *cursor.
 *
 * Advances @p cursor past it. A pair with an empty name ("=v") is skipped: the
 * service does not emit one.
 *
 * @return false when the bag is exhausted.
 */
static bool c2d_next_pair(
    const az_iot_c2d_properties* props,
    size_t* cursor,
    const char** out_key,
    size_t* out_key_len,
    const char** out_value,
    size_t* out_value_len)
{
  const char* bag = props->_internal.bag;
  size_t bag_len = props->_internal.bag_len;
  while (bag != NULL && *cursor < bag_len)
  {
    const char* pair = bag + *cursor;
    const char* end = (const char*)memchr(pair, '&', bag_len - *cursor);
    size_t pair_len = end ? (size_t)(end - pair) : bag_len - *cursor;
    *cursor += pair_len + (end ? 1 : 0);

    /* The separator's presence carries meaning ("k" is null, "k=" is empty),
     * and an encoded '=' inside a value is "%3D", so the first raw '=' splits. */
    const char* eq = (const char*)memchr(pair, '=', pair_len);
    size_t key_len = eq ? (size_t)(eq - pair) : pair_len;
    if (key_len == 0)
    {
      continue;
    }
    *out_key = pair;
    *out_key_len = key_len;
    *out_value = eq ? eq + 1 : NULL;
    *out_value_len = eq ? pair_len - key_len - 1 : 0;
    return true;
  }
  return false;
}

AZ_NODISCARD az_iot_result az_iot_c2d_properties_next(
    const az_iot_c2d_properties* props,
    size_t* cursor,
    char* key,
    size_t key_size,
    size_t* out_key_len,
    char* value,
    size_t value_size,
    size_t* out_value_len,
    bool* out_has_value)
{
  if (!props || !cursor || !key || key_size == 0 || !value || value_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  key[0] = '\0';
  value[0] = '\0';
  if (out_key_len)
  {
    *out_key_len = 0;
  }
  if (out_value_len)
  {
    *out_value_len = 0;
  }
  if (out_has_value)
  {
    *out_has_value = false;
  }

  const char* k;
  const char* v;
  size_t k_enc;
  size_t v_enc;
  if (!c2d_next_pair(props, cursor, &k, &k_enc, &v, &v_enc))
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }

  size_t k_len = 0;
  size_t v_len = 0;
  if (c2d_measure(k, k_enc, &k_len) != AZ_IOT_OK
      || (v != NULL && c2d_measure(v, v_enc, &v_len) != AZ_IOT_OK))
  {
    return AZ_IOT_ERR_PROTOCOL;
  }
  if (out_key_len)
  {
    *out_key_len = k_len;
  }
  if (out_value_len)
  {
    *out_value_len = v_len;
  }
  if (k_len >= key_size || v_len >= value_size)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  c2d_write(k, k_enc, k_len, key);
  if (v != NULL)
  {
    c2d_write(v, v_enc, v_len, value);
  }
  if (out_has_value)
  {
    *out_has_value = (v != NULL);
  }
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_c2d_properties_find(
    const az_iot_c2d_properties* props,
    const char* key,
    char* value,
    size_t value_size,
    size_t* out_value_len)
{
  if (!props || !key || key[0] == '\0' || !value || value_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  value[0] = '\0';
  if (out_value_len)
  {
    *out_value_len = 0;
  }

  size_t cursor = 0;
  const char* k;
  const char* v;
  size_t k_len;
  size_t v_len;
  while (c2d_next_pair(props, &cursor, &k, &k_len, &v, &v_len))
  {
    if (c2d_key_equals(k, k_len, key))
    {
      if (v == NULL)
      {
        return AZ_IOT_OK;
      }
      size_t decoded = 0;
      if (c2d_measure(v, v_len, &decoded) != AZ_IOT_OK)
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
      if (out_value_len)
      {
        *out_value_len = decoded;
      }
      if (decoded >= value_size)
      {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
      }
      c2d_write(v, v_len, decoded, value);
      return AZ_IOT_OK;
    }
  }
  return AZ_IOT_ERR_NOT_FOUND;
}

/** @brief az_iot_c2d_properties_find() on @p msg for one system property. */
static az_iot_result c2d_get(
    const az_iot_c2d_message* msg,
    const char* key,
    char* value,
    size_t value_size,
    size_t* out_value_len)
{
  if (!msg)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return az_iot_c2d_properties_find(&msg->properties, key, value, value_size, out_value_len);
}

AZ_NODISCARD az_iot_result az_iot_c2d_message_get_message_id(
    const az_iot_c2d_message* msg,
    char* value,
    size_t value_size,
    size_t* out_value_len)
{
  return c2d_get(msg, AZ_IOT_MSG_PROP_MESSAGE_ID, value, value_size, out_value_len);
}

AZ_NODISCARD az_iot_result az_iot_c2d_message_get_correlation_id(
    const az_iot_c2d_message* msg,
    char* value,
    size_t value_size,
    size_t* out_value_len)
{
  return c2d_get(msg, AZ_IOT_MSG_PROP_CORRELATION_ID, value, value_size, out_value_len);
}

AZ_NODISCARD az_iot_result az_iot_c2d_message_get_expiry_time(
    const az_iot_c2d_message* msg,
    char* value,
    size_t value_size,
    size_t* out_value_len)
{
  return c2d_get(msg, AZ_IOT_MSG_PROP_EXPIRY_TIME, value, value_size, out_value_len);
}

AZ_NODISCARD az_iot_result az_iot_c2d_message_get_content_type(
    const az_iot_c2d_message* msg,
    char* value,
    size_t value_size,
    size_t* out_value_len)
{
  return c2d_get(msg, AZ_IOT_MSG_PROP_CONTENT_TYPE, value, value_size, out_value_len);
}

AZ_NODISCARD az_iot_result az_iot_c2d_message_get_content_encoding(
    const az_iot_c2d_message* msg,
    char* value,
    size_t value_size,
    size_t* out_value_len)
{
  return c2d_get(msg, AZ_IOT_MSG_PROP_CONTENT_ENCODING, value, value_size, out_value_len);
}

AZ_NODISCARD az_iot_result az_iot_c2d_message_get_user_id(
    const az_iot_c2d_message* msg,
    char* value,
    size_t value_size,
    size_t* out_value_len)
{
  return c2d_get(msg, AZ_IOT_MSG_PROP_USER_ID, value, value_size, out_value_len);
}
