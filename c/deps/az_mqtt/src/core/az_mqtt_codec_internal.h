// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_codec_internal.h
 * @brief Internal: wire primitives shared by the core and the MQTT 3.1.1 and 5.0 codecs.
 *
 * Defined once, in az_mqtt_core (az_mqtt_codec_common.c).
 *
 * Writers fail with AZ_ERROR_NOT_ENOUGH_SPACE (AZ_ERROR_ARG for a value the
 * format cannot hold); readers with AZ_ERROR_UNEXPECTED_END
 * (AZ_MQTT_ERROR_MALFORMED_PACKET for a Variable Byte Integer over 4 bytes).
 * A failed call may have consumed part of the span: abandon the packet.
 */

#ifndef AZ_MQTT_CODEC_INTERNAL_H
#define AZ_MQTT_CODEC_INTERNAL_H

#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Write one byte; advance @p dest. */
AZ_NODISCARD az_result _az_mqtt_write_byte(az_span* dest, uint8_t b);

/** @brief Write a big-endian 16-bit integer; advance @p dest. */
AZ_NODISCARD az_result _az_mqtt_write_uint16(az_span* dest, uint16_t val);

/**
 * @brief Write a 2-byte length followed by @p str; advance @p dest.
 * @retval AZ_ERROR_ARG Longer than 65,535 bytes, not well-formed UTF-8, or contains U+0000.
 */
AZ_NODISCARD az_result _az_mqtt_write_utf8_string(az_span* dest, az_span str);

/** @brief Whether @p topic contains a wildcard ('+' or '#'), not allowed in a Topic Name. */
AZ_NODISCARD AZ_INLINE bool _az_mqtt_topic_has_wildcard(az_span topic)
{
  return az_span_find(topic, AZ_SPAN_FROM_STR("+")) >= 0
      || az_span_find(topic, AZ_SPAN_FROM_STR("#")) >= 0;
}

/**
 * @brief Write Topic Name @p topic (Will Topic, Response Topic); advance @p dest.
 * @retval AZ_ERROR_ARG Empty, with a wildcard ('+', '#'), or as _az_mqtt_write_utf8_string().
 */
AZ_NODISCARD AZ_INLINE az_result _az_mqtt_write_topic_name(az_span* dest, az_span topic)
{
  return az_span_size(topic) == 0 || _az_mqtt_topic_has_wildcard(topic)
      ? AZ_ERROR_ARG
      : _az_mqtt_write_utf8_string(dest, topic);
}

/**
 * @brief Whether @p filter is a valid Topic Filter (4.7.1): non-empty; '+' fills a whole level;
 * '#' fills the last level.
 */
AZ_NODISCARD AZ_INLINE bool _az_mqtt_topic_filter_valid(az_span filter)
{
  int32_t const size = az_span_size(filter);
  uint8_t const* const p = az_span_ptr(filter);
  for (int32_t i = 0; i < size; i++)
  {
    bool const level_start = i == 0 || p[i - 1] == '/';
    if ((p[i] == '+' && !(level_start && (i + 1 == size || p[i + 1] == '/')))
        || (p[i] == '#' && !(level_start && i + 1 == size)))
    {
      return false;
    }
  }
  return size > 0;
}

/**
 * @brief Write Topic Filter @p filter; advance @p dest.
 * @retval AZ_ERROR_ARG Not valid (_az_mqtt_topic_filter_valid()), or as
 * _az_mqtt_write_utf8_string().
 */
AZ_NODISCARD AZ_INLINE az_result _az_mqtt_write_topic_filter(az_span* dest, az_span filter)
{
  return _az_mqtt_topic_filter_valid(filter) ? _az_mqtt_write_utf8_string(dest, filter)
                                             : AZ_ERROR_ARG;
}

/**
 * @brief Write a 2-byte length followed by @p data; advance @p dest.
 * @retval AZ_ERROR_ARG Longer than 65,535 bytes.
 */
AZ_NODISCARD az_result _az_mqtt_write_binary_data(az_span* dest, az_span data);

/** @brief Write a Variable Byte Integer; advance @p dest. */
AZ_NODISCARD az_result _az_mqtt_write_vbi(az_span* dest, int32_t value);

/** @brief Bytes a Variable Byte Integer of @p value takes (1-4). */
AZ_NODISCARD int32_t _az_mqtt_vbi_size(int32_t value);

/** @brief Read one byte; advance @p src. */
AZ_NODISCARD az_result _az_mqtt_read_byte(az_span* src, uint8_t* out);

/** @brief Read a big-endian 16-bit integer; advance @p src. */
AZ_NODISCARD az_result _az_mqtt_read_uint16(az_span* src, uint16_t* out);

/** @brief Read a Variable Byte Integer; advance @p src. */
AZ_NODISCARD az_result _az_mqtt_read_vbi(az_span* src, int32_t* out);

/**
 * @brief Whether @p text is well-formed UTF-8 without U+0000 (MQTT 3.1.1 1.5.3, 5.0 1.5.4).
 */
bool _az_mqtt_utf8_valid(az_span text);

/**
 * @brief Read a length-prefixed UTF-8 string (a view into @p src); advance @p src.
 * @retval AZ_MQTT_ERROR_MALFORMED_PACKET Not well-formed UTF-8, or contains U+0000.
 */
AZ_NODISCARD az_result _az_mqtt_read_utf8_string(az_span* src, az_span* out);

/** @brief Read length-prefixed binary data (a view into @p src); advance @p src. */
AZ_NODISCARD az_result _az_mqtt_read_binary_data(az_span* src, az_span* out);

/**
 * @brief Whether the fixed-header flags (low nibble of @p first_byte) are valid for its packet
 * type: 0b0010 for PUBREL, SUBSCRIBE, UNSUBSCRIBE; 0 for others; any for PUBLISH.
 */
bool _az_mqtt_fixed_header_flags_valid(uint8_t first_byte);

/**
 * @brief Read a fixed header; advance @p src.
 * @retval AZ_MQTT_ERROR_MALFORMED_PACKET Reserved flags not as MQTT requires for the type.
 */
AZ_NODISCARD az_result _az_mqtt_decode_fixed_header(
    az_span* src,
    az_mqtt_packet_type* out_packet_type,
    uint8_t* out_flags,
    int32_t* out_remaining);

/** @brief Write a PINGREQ (2 bytes); advance @p dest. */
AZ_NODISCARD az_result _az_mqtt_encode_pingreq(az_span* dest);

#endif // AZ_MQTT_CODEC_INTERNAL_H
