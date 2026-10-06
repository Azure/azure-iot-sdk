// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @brief MQTT 3.1.1 packet encoding and decoding.
 *
 * All operations use az_span for buffer management. No dynamic allocation.
 * The encoder writes into a caller-provided buffer and advances the span pointer.
 * The decoder reads from a span and produces typed structures.
 */

#include "az_mqtt_codec_internal.h"

#include <az_mqtt3/az_mqtt3_codec.h>

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <string.h>

// ============================================================================
// CONNECT encoding
// ============================================================================

// Calculate the variable header + payload length of a CONNECT packet.
static int32_t _connect_remaining_length(az_mqtt3_connect_options const* opts)
{
  // Protocol Name (2+4) + Protocol Level (1) + Connect Flags (1) + Keep Alive (2)
  int32_t len = 10;

  // Payload: Client ID
  len += 2 + az_span_size(opts->client_id);

  // Will
  if (opts->will != NULL)
  {
    len += 2 + az_span_size(opts->will->topic); // Will Topic
    len += 2 + az_span_size(opts->will->payload); // Will Payload
  }

  // Username
  if (az_span_size(opts->username) > 0)
  {
    len += 2 + az_span_size(opts->username);
  }

  // Password
  if (az_span_size(opts->password) > 0)
  {
    len += 2 + az_span_size(opts->password);
  }

  return len;
}

AZ_NODISCARD az_result
az_mqtt3_codec_encode_connect(az_span* dest, az_mqtt3_connect_options const* opts)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(opts);

  int32_t remaining = _connect_remaining_length(opts);

  // Fixed header
  az_result rc = _az_mqtt_write_byte(dest, (uint8_t)(AZ_MQTT_PACKET_TYPE_CONNECT << 4));
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  // Protocol Name "MQTT"
  rc = _az_mqtt_write_utf8_string(dest, AZ_SPAN_FROM_STR("MQTT"));
  if (az_result_failed(rc))
    return rc;

  // Protocol Version (4 = MQTT 3.1.1)
  rc = _az_mqtt_write_byte(dest, AZ_MQTT3_PROTOCOL_VERSION);
  if (az_result_failed(rc))
    return rc;

  // Connect Flags
  uint8_t flags = 0;
  if (opts->clean_session)
  {
    flags |= 0x02;
  }
  if (opts->will != NULL)
  {
    flags |= 0x04;
    flags |= (uint8_t)((uint8_t)opts->will->qos << 3);
    if (opts->will->retain)
    {
      flags |= 0x20;
    }
  }
  if (az_span_size(opts->password) > 0)
  {
    flags |= 0x40;
  }
  if (az_span_size(opts->username) > 0)
  {
    flags |= 0x80;
  }
  rc = _az_mqtt_write_byte(dest, flags);
  if (az_result_failed(rc))
    return rc;

  // Keep Alive
  rc = _az_mqtt_write_uint16(dest, opts->keep_alive_seconds);
  if (az_result_failed(rc))
    return rc;

  // Payload: Client ID
  rc = _az_mqtt_write_utf8_string(dest, opts->client_id);
  if (az_result_failed(rc))
    return rc;

  // Will
  if (opts->will != NULL)
  {
    rc = _az_mqtt_write_utf8_string(dest, opts->will->topic);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_binary_data(dest, opts->will->payload);
    if (az_result_failed(rc))
      return rc;
  }

  // Username
  if (az_span_size(opts->username) > 0)
  {
    rc = _az_mqtt_write_utf8_string(dest, opts->username);
    if (az_result_failed(rc))
      return rc;
  }

  // Password
  if (az_span_size(opts->password) > 0)
  {
    rc = _az_mqtt_write_binary_data(dest, opts->password);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// PUBLISH encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_publish(
    az_span* dest,
    az_mqtt3_publish_options const* opts,
    uint16_t packet_id)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(opts);

  // Calculate remaining length
  int32_t remaining = 2 + az_span_size(opts->topic); // Topic Name
  if (opts->qos != AZ_MQTT_QOS_AT_MOST_ONCE)
  {
    remaining += 2; // Packet Identifier
  }
  remaining += az_span_size(opts->payload); // Payload (no length prefix)

  // Fixed header
  uint8_t first_byte = (uint8_t)(AZ_MQTT_PACKET_TYPE_PUBLISH << 4);
  first_byte |= (uint8_t)((uint8_t)opts->qos << 1);
  if (opts->retain)
  {
    first_byte |= 0x01;
  }

  az_result rc = _az_mqtt_write_byte(dest, first_byte);
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  // Topic Name
  rc = _az_mqtt_write_utf8_string(dest, opts->topic);
  if (az_result_failed(rc))
    return rc;

  // Packet Identifier (for QoS > 0)
  if (opts->qos != AZ_MQTT_QOS_AT_MOST_ONCE)
  {
    rc = _az_mqtt_write_uint16(dest, packet_id);
    if (az_result_failed(rc))
      return rc;
  }

  // Payload (raw, no length prefix)
  if (az_span_size(opts->payload) > 0)
  {
    if (az_span_size(*dest) < az_span_size(opts->payload))
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }
    memcpy(az_span_ptr(*dest), az_span_ptr(opts->payload), (size_t)az_span_size(opts->payload));
    *dest = az_span_slice_to_end(*dest, az_span_size(opts->payload));
  }

  return AZ_OK;
}

// ============================================================================
// Simple ACK encoding (PUBACK, PUBREC, PUBREL, PUBCOMP)
// ============================================================================

static az_result
_encode_simple_ack(az_span* dest, az_mqtt_packet_type type, uint8_t fixed_flags, uint16_t packet_id)
{
  az_result rc = _az_mqtt_write_byte(dest, (uint8_t)((uint8_t)(type << 4) | fixed_flags));
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_byte(dest, 2); // Remaining Length: packet identifier only.
  if (az_result_failed(rc))
    return rc;
  return _az_mqtt_write_uint16(dest, packet_id);
}

AZ_NODISCARD az_result az_mqtt3_codec_encode_puback(az_span* dest, uint16_t packet_id)
{
  return _encode_simple_ack(dest, AZ_MQTT_PACKET_TYPE_PUBACK, 0, packet_id);
}

AZ_NODISCARD az_result az_mqtt3_codec_encode_pubrec(az_span* dest, uint16_t packet_id)
{
  return _encode_simple_ack(dest, AZ_MQTT_PACKET_TYPE_PUBREC, 0, packet_id);
}

AZ_NODISCARD az_result az_mqtt3_codec_encode_pubrel(az_span* dest, uint16_t packet_id)
{
  // PUBREL has fixed flags = 0x02
  return _encode_simple_ack(dest, AZ_MQTT_PACKET_TYPE_PUBREL, 0x02, packet_id);
}

AZ_NODISCARD az_result az_mqtt3_codec_encode_pubcomp(az_span* dest, uint16_t packet_id)
{
  return _encode_simple_ack(dest, AZ_MQTT_PACKET_TYPE_PUBCOMP, 0, packet_id);
}

// ============================================================================
// SUBSCRIBE encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_subscribe(
    az_span* dest,
    az_mqtt3_subscription const* subs,
    int32_t sub_count,
    uint16_t packet_id)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(subs);
  _az_PRECONDITION(sub_count > 0);

  // Calculate remaining length
  int32_t remaining = 2; // Packet ID

  for (int32_t i = 0; i < sub_count; i++)
  {
    remaining += 2 + az_span_size(subs[i].topic_filter) + 1; // string + options byte
  }

  // Fixed header: SUBSCRIBE type with reserved flags = 0x02
  az_result rc = _az_mqtt_write_byte(dest, (uint8_t)((AZ_MQTT_PACKET_TYPE_SUBSCRIBE << 4) | 0x02));
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  // Packet Identifier
  rc = _az_mqtt_write_uint16(dest, packet_id);
  if (az_result_failed(rc))
    return rc;

  // Payload
  for (int32_t i = 0; i < sub_count; i++)
  {
    rc = _az_mqtt_write_utf8_string(dest, subs[i].topic_filter);
    if (az_result_failed(rc))
      return rc;

    uint8_t options = (uint8_t)subs[i].qos;
    // MQTT 3.1.1 subscribe options only carry requested QoS (bits 0-1).

    rc = _az_mqtt_write_byte(dest, options);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// UNSUBSCRIBE encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_unsubscribe(
    az_span* dest,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t packet_id)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(topic_filters);
  _az_PRECONDITION(filter_count > 0);

  int32_t remaining = 2; // Packet ID

  for (int32_t i = 0; i < filter_count; i++)
  {
    remaining += 2 + az_span_size(topic_filters[i]);
  }

  // Fixed header: UNSUBSCRIBE type with reserved flags = 0x02
  az_result rc = _az_mqtt_write_byte(dest, (uint8_t)((AZ_MQTT_PACKET_TYPE_UNSUBSCRIBE << 4) | 0x02));
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  rc = _az_mqtt_write_uint16(dest, packet_id);
  if (az_result_failed(rc))
    return rc;

  for (int32_t i = 0; i < filter_count; i++)
  {
    rc = _az_mqtt_write_utf8_string(dest, topic_filters[i]);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// DISCONNECT encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_encode_disconnect(az_span* dest)
{
  _az_PRECONDITION_NOT_NULL(dest);

  az_result rc = _az_mqtt_write_byte(dest, (uint8_t)(AZ_MQTT_PACKET_TYPE_DISCONNECT << 4));
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_vbi(dest, 0);
  if (az_result_failed(rc))
    return rc;
  return AZ_OK;
}

// ============================================================================
// Decoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt3_codec_decode_connack(az_span body, az_mqtt3_connack_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  uint8_t ack_flags;
  az_result rc = _az_mqtt_read_byte(&body, &ack_flags);
  if (az_result_failed(rc))
    return rc;
  out->session_present = (ack_flags & 0x01) != 0;

  uint8_t return_code;
  rc = _az_mqtt_read_byte(&body, &return_code);
  if (az_result_failed(rc))
    return rc;
  out->return_code = (az_mqtt3_connack_return_code)return_code;

  // MQTT 3.1.1 3.2.2: reserved flag bits are 0; return codes 6-255 are reserved; a refusal does
  // not report a session.
  if ((ack_flags & 0xFE) != 0 || return_code > 5 || (out->session_present && return_code != 0))
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }
  return az_span_size(body) == 0 ? AZ_OK : AZ_MQTT_ERROR_MALFORMED_PACKET;
}

AZ_NODISCARD az_result
az_mqtt3_codec_decode_publish(az_span body, uint8_t flags, az_mqtt3_publish_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  out->dup = (flags & 0x08) != 0;
  out->qos = (az_mqtt_qos)((flags >> 1) & 0x03);
  if (out->qos > AZ_MQTT_QOS_EXACTLY_ONCE)
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET; // QoS 3 is reserved.
  }
  out->retain = (flags & 0x01) != 0;
  out->packet_id = 0;

  az_result rc = _az_mqtt_read_utf8_string(&body, &out->topic);
  if (az_result_failed(rc))
    return rc;
  // 4.7.3: at least one character; no wildcards in a Topic Name.
  if (az_span_size(out->topic) == 0 || _az_mqtt_topic_has_wildcard(out->topic))
    return AZ_MQTT_ERROR_MALFORMED_PACKET;

  if (out->qos != AZ_MQTT_QOS_AT_MOST_ONCE)
  {
    rc = _az_mqtt_read_uint16(&body, &out->packet_id);
    if (az_result_failed(rc))
      return rc;
    if (out->packet_id == 0)
      return AZ_MQTT_ERROR_MALFORMED_PACKET; // MQTT 3.1.1 2.3.1: non-zero.
  }

  out->payload = body;
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt3_codec_decode_ack(az_span body, az_mqtt3_ack_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  out->status = AZ_OK;
  out->incoming = false;
  az_result rc = _az_mqtt_read_uint16(&body, &out->packet_id);
  if (az_result_failed(rc))
    return rc;
  if (out->packet_id == 0)
    return AZ_MQTT_ERROR_MALFORMED_PACKET; // Packet identifiers are non-zero.
  return az_span_size(body) == 0 ? AZ_OK : AZ_MQTT_ERROR_MALFORMED_PACKET;
}

AZ_NODISCARD az_result az_mqtt3_codec_decode_suback(az_span body, az_mqtt3_suback_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  az_result rc = _az_mqtt_read_uint16(&body, &out->packet_id);
  if (az_result_failed(rc))
    return rc;
  if (out->packet_id == 0)
    return AZ_MQTT_ERROR_MALFORMED_PACKET; // Packet identifiers are non-zero.
  out->return_codes = body;
  // One return code per topic filter, and a SUBSCRIBE has at least one; 0, 1, 2 or 0x80 (3.9.3).
  for (int32_t i = 0; i < az_span_size(body); i++)
  {
    uint8_t const code = az_span_ptr(body)[i];
    if (code > 2 && code != 0x80)
      return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }
  return az_span_size(body) > 0 ? AZ_OK : AZ_MQTT_ERROR_MALFORMED_PACKET;
}
