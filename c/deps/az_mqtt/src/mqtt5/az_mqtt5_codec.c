// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @brief MQTT 5.0 packet encoding and decoding.
 *
 * All operations use az_span for buffer management. No dynamic allocation.
 * The encoder writes into a caller-provided buffer and advances the span pointer.
 * The decoder reads from a span and produces typed structures.
 */

#include "az_mqtt_codec_internal.h"

#include <az_mqtt5/az_mqtt5_codec.h>

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <string.h>

// ============================================================================
// Internal helpers
// ============================================================================

// Write a 32-bit big-endian value.
static az_result _write_uint32(az_span* dest, uint32_t val)
{
  if (az_span_size(*dest) < 4)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  az_span_ptr(*dest)[0] = (uint8_t)(val >> 24);
  az_span_ptr(*dest)[1] = (uint8_t)(val >> 16);
  az_span_ptr(*dest)[2] = (uint8_t)(val >> 8);
  az_span_ptr(*dest)[3] = (uint8_t)(val & 0xFF);
  *dest = az_span_slice_to_end(*dest, 4);
  return AZ_OK;
}

// Read a 32-bit big-endian value.
static az_result _read_uint32(az_span* src, uint32_t* out)
{
  if (az_span_size(*src) < 4)
  {
    return AZ_ERROR_UNEXPECTED_END;
  }
  *out = ((uint32_t)az_span_ptr(*src)[0] << 24) | ((uint32_t)az_span_ptr(*src)[1] << 16)
      | ((uint32_t)az_span_ptr(*src)[2] << 8) | ((uint32_t)az_span_ptr(*src)[3]);
  *src = az_span_slice_to_end(*src, 4);
  return AZ_OK;
}

// ============================================================================
// Property encoding helpers
// ============================================================================

// Calculate property section size for CONNECT.
static int32_t _connect_props_size(az_mqtt5_connect_options const* opts)
{
  int32_t size = 0;

  if (opts->session_expiry_interval != 0)
  {
    size += 1 + 4; // prop id + uint32
  }
  if (opts->receive_maximum != 65535)
  {
    size += 1 + 2; // prop id + uint16
  }
  if (opts->maximum_packet_size != 0)
  {
    size += 1 + 4;
  }
  if (opts->topic_alias_maximum != 0)
  {
    size += 1 + 2;
  }
  if (opts->request_response_information)
  {
    size += 1 + 1;
  }
  if (!opts->request_problem_information)
  {
    size += 1 + 1; // only send if false (default is true per MQTT5 spec)
  }
  if (az_span_size(opts->authentication_method) > 0)
  {
    size += 1 + 2 + az_span_size(opts->authentication_method);
  }
  if (az_span_size(opts->authentication_data) > 0)
  {
    size += 1 + 2 + az_span_size(opts->authentication_data);
  }
  for (int32_t i = 0; i < opts->user_property_count; i++)
  {
    size += 1 + 2 + az_span_size(opts->user_properties[i].key) + 2
        + az_span_size(opts->user_properties[i].value);
  }
  return size;
}

static az_result _write_connect_props(az_span* dest, az_mqtt5_connect_options const* opts)
{
  int32_t prop_len = _connect_props_size(opts);
  az_result rc = _az_mqtt_write_vbi(dest, prop_len);
  if (az_result_failed(rc))
  {
    return rc;
  }

  if (opts->session_expiry_interval != 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_SESSION_EXPIRY_INTERVAL);
    if (az_result_failed(rc))
      return rc;
    rc = _write_uint32(dest, opts->session_expiry_interval);
    if (az_result_failed(rc))
      return rc;
  }
  if (opts->receive_maximum != 65535)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_RECEIVE_MAXIMUM);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_uint16(dest, opts->receive_maximum);
    if (az_result_failed(rc))
      return rc;
  }
  if (opts->maximum_packet_size != 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_MAXIMUM_PACKET_SIZE);
    if (az_result_failed(rc))
      return rc;
    rc = _write_uint32(dest, opts->maximum_packet_size);
    if (az_result_failed(rc))
      return rc;
  }
  if (opts->topic_alias_maximum != 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_TOPIC_ALIAS_MAXIMUM);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_uint16(dest, opts->topic_alias_maximum);
    if (az_result_failed(rc))
      return rc;
  }
  if (opts->request_response_information)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_REQUEST_RESPONSE_INFORMATION);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_byte(dest, 1);
    if (az_result_failed(rc))
      return rc;
  }
  if (!opts->request_problem_information)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_REQUEST_PROBLEM_INFORMATION);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_byte(dest, 0);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(opts->authentication_method) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_AUTHENTICATION_METHOD);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, opts->authentication_method);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(opts->authentication_data) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_AUTHENTICATION_DATA);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_binary_data(dest, opts->authentication_data);
    if (az_result_failed(rc))
      return rc;
  }
  for (int32_t i = 0; i < opts->user_property_count; i++)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_USER_PROPERTY);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, opts->user_properties[i].key);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, opts->user_properties[i].value);
    if (az_result_failed(rc))
      return rc;
  }
  return AZ_OK;
}

// Calculate Will properties size.
static int32_t _will_props_size(az_mqtt5_will_options const* will)
{
  int32_t size = 0;
  if (will->will_delay_interval != 0)
  {
    size += 1 + 4;
  }
  if (will->payload_format_indicator != 0)
  {
    size += 1 + 1;
  }
  if (will->message_expiry_interval != 0)
  {
    size += 1 + 4;
  }
  if (az_span_size(will->content_type) > 0)
  {
    size += 1 + 2 + az_span_size(will->content_type);
  }
  if (az_span_size(will->response_topic) > 0)
  {
    size += 1 + 2 + az_span_size(will->response_topic);
  }
  if (az_span_size(will->correlation_data) > 0)
  {
    size += 1 + 2 + az_span_size(will->correlation_data);
  }
  for (int32_t i = 0; i < will->user_property_count; i++)
  {
    size += 1 + 2 + az_span_size(will->user_properties[i].key) + 2
        + az_span_size(will->user_properties[i].value);
  }
  return size;
}

static az_result _write_will_props(az_span* dest, az_mqtt5_will_options const* will)
{
  int32_t prop_len = _will_props_size(will);
  az_result rc = _az_mqtt_write_vbi(dest, prop_len);
  if (az_result_failed(rc))
    return rc;

  if (will->will_delay_interval != 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_WILL_DELAY_INTERVAL);
    if (az_result_failed(rc))
      return rc;
    rc = _write_uint32(dest, will->will_delay_interval);
    if (az_result_failed(rc))
      return rc;
  }
  if (will->payload_format_indicator != 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_byte(dest, will->payload_format_indicator);
    if (az_result_failed(rc))
      return rc;
  }
  if (will->message_expiry_interval != 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_MESSAGE_EXPIRY_INTERVAL);
    if (az_result_failed(rc))
      return rc;
    rc = _write_uint32(dest, will->message_expiry_interval);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(will->content_type) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_CONTENT_TYPE);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, will->content_type);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(will->response_topic) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_RESPONSE_TOPIC);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, will->response_topic);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(will->correlation_data) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_CORRELATION_DATA);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_binary_data(dest, will->correlation_data);
    if (az_result_failed(rc))
      return rc;
  }
  for (int32_t i = 0; i < will->user_property_count; i++)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_USER_PROPERTY);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, will->user_properties[i].key);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, will->user_properties[i].value);
    if (az_result_failed(rc))
      return rc;
  }
  return AZ_OK;
}

// Publish properties size.
static int32_t _publish_props_size(az_mqtt5_publish_options const* opts)
{
  int32_t size = 0;
  if (opts->payload_format_indicator != 0)
  {
    size += 1 + 1;
  }
  if (opts->message_expiry_interval != 0)
  {
    size += 1 + 4;
  }
  if (opts->topic_alias != 0)
  {
    size += 1 + 2;
  }
  if (az_span_size(opts->response_topic) > 0)
  {
    size += 1 + 2 + az_span_size(opts->response_topic);
  }
  if (az_span_size(opts->correlation_data) > 0)
  {
    size += 1 + 2 + az_span_size(opts->correlation_data);
  }
  if (az_span_size(opts->content_type) > 0)
  {
    size += 1 + 2 + az_span_size(opts->content_type);
  }
  for (int32_t i = 0; i < opts->user_property_count; i++)
  {
    size += 1 + 2 + az_span_size(opts->user_properties[i].key) + 2
        + az_span_size(opts->user_properties[i].value);
  }
  return size;
}

static az_result _write_publish_props(az_span* dest, az_mqtt5_publish_options const* opts)
{
  int32_t prop_len = _publish_props_size(opts);
  az_result rc = _az_mqtt_write_vbi(dest, prop_len);
  if (az_result_failed(rc))
    return rc;

  if (opts->payload_format_indicator != 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_byte(dest, opts->payload_format_indicator);
    if (az_result_failed(rc))
      return rc;
  }
  if (opts->message_expiry_interval != 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_MESSAGE_EXPIRY_INTERVAL);
    if (az_result_failed(rc))
      return rc;
    rc = _write_uint32(dest, opts->message_expiry_interval);
    if (az_result_failed(rc))
      return rc;
  }
  if (opts->topic_alias != 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_TOPIC_ALIAS);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_uint16(dest, opts->topic_alias);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(opts->response_topic) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_RESPONSE_TOPIC);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, opts->response_topic);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(opts->correlation_data) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_CORRELATION_DATA);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_binary_data(dest, opts->correlation_data);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(opts->content_type) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_CONTENT_TYPE);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, opts->content_type);
    if (az_result_failed(rc))
      return rc;
  }
  for (int32_t i = 0; i < opts->user_property_count; i++)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_USER_PROPERTY);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, opts->user_properties[i].key);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, opts->user_properties[i].value);
    if (az_result_failed(rc))
      return rc;
  }
  return AZ_OK;
}

// ============================================================================
// CONNECT encoding
// ============================================================================

// Calculate the variable header + payload length of a CONNECT packet.
static int32_t _connect_remaining_length(az_mqtt5_connect_options const* opts)
{
  // Protocol Name (2+4) + Protocol Level (1) + Connect Flags (1) + Keep Alive (2)
  int32_t len = 10;

  // Properties
  int32_t prop_len = _connect_props_size(opts);
  len += _az_mqtt_vbi_size(prop_len) + prop_len;

  // Payload: Client ID
  len += 2 + az_span_size(opts->client_id);

  // Will
  if (opts->will != NULL)
  {
    int32_t will_prop_len = _will_props_size(opts->will);
    len += _az_mqtt_vbi_size(will_prop_len) + will_prop_len;
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
az_mqtt5_codec_encode_connect(az_span* dest, az_mqtt5_connect_options const* opts)
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

  // Protocol Version (5)
  rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROTOCOL_VERSION);
  if (az_result_failed(rc))
    return rc;

  // Connect Flags
  uint8_t flags = 0;
  if (opts->clean_start)
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

  // CONNECT Properties
  rc = _write_connect_props(dest, opts);
  if (az_result_failed(rc))
    return rc;

  // Payload: Client ID
  rc = _az_mqtt_write_utf8_string(dest, opts->client_id);
  if (az_result_failed(rc))
    return rc;

  // Will
  if (opts->will != NULL)
  {
    rc = _write_will_props(dest, opts->will);
    if (az_result_failed(rc))
      return rc;
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

AZ_NODISCARD az_result az_mqtt5_codec_encode_publish(
    az_span* dest,
    az_mqtt5_publish_options const* opts,
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

  int32_t prop_len = _publish_props_size(opts);
  remaining += _az_mqtt_vbi_size(prop_len) + prop_len;
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

  // Properties
  rc = _write_publish_props(dest, opts);
  if (az_result_failed(rc))
    return rc;

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

static az_result _encode_simple_ack(
    az_span* dest,
    az_mqtt_packet_type type,
    uint8_t fixed_flags,
    uint16_t packet_id,
    az_mqtt5_reason_code reason_code)
{
  // If reason code is SUCCESS and no properties, we can use short form (2 bytes remaining)
  // But include reason code for clarity: 4 bytes remaining = packet_id(2) + reason(1) + props(1=0)
  int32_t remaining;
  bool short_form = (reason_code == AZ_MQTT5_REASON_SUCCESS);

  if (short_form)
  {
    remaining = 2; // Just packet ID
  }
  else
  {
    remaining = 4; // packet_id(2) + reason(1) + property_length(1, value=0)
  }

  az_result rc = _az_mqtt_write_byte(dest, (uint8_t)((uint8_t)(type << 4) | fixed_flags));
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_uint16(dest, packet_id);
  if (az_result_failed(rc))
    return rc;

  if (!short_form)
  {
    rc = _az_mqtt_write_byte(dest, (uint8_t)reason_code);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_byte(dest, 0); // No properties
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_codec_encode_puback(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt5_reason_code reason_code)
{
  return _encode_simple_ack(dest, AZ_MQTT_PACKET_TYPE_PUBACK, 0, packet_id, reason_code);
}

AZ_NODISCARD az_result az_mqtt5_codec_encode_pubrec(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt5_reason_code reason_code)
{
  return _encode_simple_ack(dest, AZ_MQTT_PACKET_TYPE_PUBREC, 0, packet_id, reason_code);
}

AZ_NODISCARD az_result az_mqtt5_codec_encode_pubrel(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt5_reason_code reason_code)
{
  // PUBREL has fixed flags = 0x02
  return _encode_simple_ack(dest, AZ_MQTT_PACKET_TYPE_PUBREL, 0x02, packet_id, reason_code);
}

AZ_NODISCARD az_result az_mqtt5_codec_encode_pubcomp(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt5_reason_code reason_code)
{
  return _encode_simple_ack(dest, AZ_MQTT_PACKET_TYPE_PUBCOMP, 0, packet_id, reason_code);
}

// ============================================================================
// SUBSCRIBE encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt5_codec_encode_subscribe(
    az_span* dest,
    az_mqtt5_subscription const* subs,
    int32_t sub_count,
    uint16_t packet_id)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(subs);
  _az_PRECONDITION(sub_count > 0);

  // Calculate remaining length
  int32_t remaining = 2; // Packet ID
  remaining += 1; // Property Length (0 = no properties)

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

  // Properties (none)
  rc = _az_mqtt_write_byte(dest, 0);
  if (az_result_failed(rc))
    return rc;

  // Payload
  for (int32_t i = 0; i < sub_count; i++)
  {
    rc = _az_mqtt_write_utf8_string(dest, subs[i].topic_filter);
    if (az_result_failed(rc))
      return rc;

    uint8_t options = (uint8_t)subs[i].qos;
    if (subs[i].no_local)
    {
      options |= 0x04;
    }
    if (subs[i].retain_as_published)
    {
      options |= 0x08;
    }
    options |= (uint8_t)((uint8_t)subs[i].retain_handling << 4);

    rc = _az_mqtt_write_byte(dest, options);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// UNSUBSCRIBE encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt5_codec_encode_unsubscribe(
    az_span* dest,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t packet_id)
{
  _az_PRECONDITION_NOT_NULL(dest);
  _az_PRECONDITION_NOT_NULL(topic_filters);
  _az_PRECONDITION(filter_count > 0);

  int32_t remaining = 2; // Packet ID
  remaining += 1; // Property Length (0)

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

  rc = _az_mqtt_write_byte(dest, 0); // No properties
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
// PINGREQ encoding
// ============================================================================

// ============================================================================
// DISCONNECT encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt5_codec_encode_disconnect(
    az_span* dest,
    az_mqtt5_reason_code reason_code,
    uint32_t session_expiry_interval)
{
  _az_PRECONDITION_NOT_NULL(dest);

  // Calculate properties size
  int32_t prop_len = 0;
  if (session_expiry_interval != 0)
  {
    prop_len += 1 + 4;
  }

  int32_t remaining;
  if (reason_code == AZ_MQTT5_REASON_NORMAL_DISCONNECTION && prop_len == 0)
  {
    remaining = 0; // Short form
  }
  else if (prop_len == 0)
  {
    remaining = 1; // Just reason code
  }
  else
  {
    remaining = 1 + _az_mqtt_vbi_size(prop_len) + prop_len;
  }

  az_result rc = _az_mqtt_write_byte(dest, (uint8_t)(AZ_MQTT_PACKET_TYPE_DISCONNECT << 4));
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  if (remaining == 0)
  {
    return AZ_OK;
  }

  rc = _az_mqtt_write_byte(dest, (uint8_t)reason_code);
  if (az_result_failed(rc))
    return rc;

  if (prop_len > 0)
  {
    rc = _az_mqtt_write_vbi(dest, prop_len);
    if (az_result_failed(rc))
      return rc;
    if (session_expiry_interval != 0)
    {
      rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_SESSION_EXPIRY_INTERVAL);
      if (az_result_failed(rc))
        return rc;
      rc = _write_uint32(dest, session_expiry_interval);
      if (az_result_failed(rc))
        return rc;
    }
  }

  return AZ_OK;
}

// ============================================================================
// AUTH encoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt5_codec_encode_auth(
    az_span* dest,
    az_mqtt5_reason_code reason_code,
    az_span authentication_method,
    az_span authentication_data)
{
  _az_PRECONDITION_NOT_NULL(dest);

  int32_t prop_len = 0;
  if (az_span_size(authentication_method) > 0)
  {
    prop_len += 1 + 2 + az_span_size(authentication_method);
  }
  if (az_span_size(authentication_data) > 0)
  {
    prop_len += 1 + 2 + az_span_size(authentication_data);
  }

  int32_t remaining = 1 + _az_mqtt_vbi_size(prop_len) + prop_len;

  az_result rc = _az_mqtt_write_byte(dest, (uint8_t)(AZ_MQTT_PACKET_TYPE_AUTH << 4));
  if (az_result_failed(rc))
    return rc;
  rc = _az_mqtt_write_vbi(dest, remaining);
  if (az_result_failed(rc))
    return rc;

  rc = _az_mqtt_write_byte(dest, (uint8_t)reason_code);
  if (az_result_failed(rc))
    return rc;

  rc = _az_mqtt_write_vbi(dest, prop_len);
  if (az_result_failed(rc))
    return rc;

  if (az_span_size(authentication_method) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_AUTHENTICATION_METHOD);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_utf8_string(dest, authentication_method);
    if (az_result_failed(rc))
      return rc;
  }
  if (az_span_size(authentication_data) > 0)
  {
    rc = _az_mqtt_write_byte(dest, AZ_MQTT5_PROPERTY_AUTHENTICATION_DATA);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_write_binary_data(dest, authentication_data);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// Fixed header decoding
// ============================================================================

// ============================================================================
// CONNACK decoding
// ============================================================================

static az_result _decode_connack_props(az_span* src, az_mqtt5_connack_data* out)
{
  int32_t prop_len;
  az_result rc = _az_mqtt_read_vbi(src, &prop_len);
  if (az_result_failed(rc))
    return rc;

  if (prop_len > az_span_size(*src))
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  az_span props = az_span_slice(*src, 0, prop_len);
  *src = az_span_slice_to_end(*src, prop_len);

  while (az_span_size(props) > 0)
  {
    uint8_t prop_id;
    rc = _az_mqtt_read_byte(&props, &prop_id);
    if (az_result_failed(rc))
      return rc;

    switch ((az_mqtt5_property_id)prop_id)
    {
      case AZ_MQTT5_PROPERTY_SESSION_EXPIRY_INTERVAL:
        rc = _read_uint32(&props, &out->session_expiry_interval);
        break;
      case AZ_MQTT5_PROPERTY_RECEIVE_MAXIMUM:
        rc = _az_mqtt_read_uint16(&props, &out->receive_maximum);
        break;
      case AZ_MQTT5_PROPERTY_MAXIMUM_QOS:
      {
        uint8_t v = 0;
        rc = _az_mqtt_read_byte(&props, &v);
        out->maximum_qos = v;
        break;
      }
      case AZ_MQTT5_PROPERTY_RETAIN_AVAILABLE:
      {
        uint8_t v = 0;
        rc = _az_mqtt_read_byte(&props, &v);
        out->retain_available = (v != 0);
        break;
      }
      case AZ_MQTT5_PROPERTY_MAXIMUM_PACKET_SIZE:
        rc = _read_uint32(&props, &out->maximum_packet_size);
        break;
      case AZ_MQTT5_PROPERTY_ASSIGNED_CLIENT_IDENTIFIER:
        rc = _az_mqtt_read_utf8_string(&props, &out->assigned_client_identifier);
        break;
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS_MAXIMUM:
        rc = _az_mqtt_read_uint16(&props, &out->topic_alias_maximum);
        break;
      case AZ_MQTT5_PROPERTY_REASON_STRING:
        rc = _az_mqtt_read_utf8_string(&props, &out->reason_string);
        break;
      case AZ_MQTT5_PROPERTY_WILDCARD_SUBSCRIPTION_AVAILABLE:
      {
        uint8_t v = 0;
        rc = _az_mqtt_read_byte(&props, &v);
        out->wildcard_subscription_available = (v != 0);
        break;
      }
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
      {
        uint8_t v = 0;
        rc = _az_mqtt_read_byte(&props, &v);
        out->subscription_identifier_available = (v != 0);
        break;
      }
      case AZ_MQTT5_PROPERTY_SHARED_SUBSCRIPTION_AVAILABLE:
      {
        uint8_t v = 0;
        rc = _az_mqtt_read_byte(&props, &v);
        out->shared_subscription_available = (v != 0);
        break;
      }
      case AZ_MQTT5_PROPERTY_SERVER_KEEP_ALIVE:
        rc = _az_mqtt_read_uint16(&props, &out->server_keep_alive);
        out->server_keep_alive_present = az_result_succeeded(rc);
        break;
      case AZ_MQTT5_PROPERTY_RESPONSE_INFORMATION:
        rc = _az_mqtt_read_utf8_string(&props, &out->response_information);
        break;
      case AZ_MQTT5_PROPERTY_SERVER_REFERENCE:
        rc = _az_mqtt_read_utf8_string(&props, &out->server_reference);
        break;
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_METHOD:
        rc = _az_mqtt_read_utf8_string(&props, &out->authentication_method);
        break;
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_DATA:
        rc = _az_mqtt_read_binary_data(&props, &out->authentication_data);
        break;
      case AZ_MQTT5_PROPERTY_USER_PROPERTY:
      {
        az_span key, value;
        rc = _az_mqtt_read_utf8_string(&props, &key);
        if (az_result_failed(rc))
          return rc;
        rc = _az_mqtt_read_utf8_string(&props, &value);
        if (az_result_failed(rc))
          return rc;
        if (out->user_properties != NULL
            && out->user_property_count < out->user_property_capacity)
        {
          out->user_properties[out->user_property_count].key = key;
          out->user_properties[out->user_property_count].value = value;
          out->user_property_count++;
        }
        continue; // Already handled rc
      }
      case AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR: // Not allowed in CONNACK.
      case AZ_MQTT5_PROPERTY_MESSAGE_EXPIRY_INTERVAL:
      case AZ_MQTT5_PROPERTY_CONTENT_TYPE:
      case AZ_MQTT5_PROPERTY_RESPONSE_TOPIC:
      case AZ_MQTT5_PROPERTY_CORRELATION_DATA:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_REQUEST_PROBLEM_INFORMATION:
      case AZ_MQTT5_PROPERTY_WILL_DELAY_INTERVAL:
      case AZ_MQTT5_PROPERTY_REQUEST_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS:
      default:
        return AZ_MQTT_ERROR_MALFORMED_PACKET;
    }
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_codec_decode_connack(az_span body, az_mqtt5_connack_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt5_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  // Initialize defaults
  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;
  out->receive_maximum = 65535;
  out->maximum_qos = 2;
  out->retain_available = true;
  out->maximum_packet_size = 0; // 0 = no limit specified
  out->wildcard_subscription_available = true;
  out->subscription_identifier_available = true;
  out->shared_subscription_available = true;

  // Acknowledge Flags
  uint8_t ack_flags;
  az_result rc = _az_mqtt_read_byte(&body, &ack_flags);
  if (az_result_failed(rc))
    return rc;
  out->session_present = (ack_flags & 0x01) != 0;

  // Reason Code
  uint8_t reason;
  rc = _az_mqtt_read_byte(&body, &reason);
  if (az_result_failed(rc))
    return rc;
  out->reason_code = (az_mqtt5_reason_code)reason;

  // MQTT 5.0 3.2.2.1: reserved flag bits are 0; a refusal (0x80 or above) does not report a
  // session.
  if ((ack_flags & 0xFE) != 0 || (out->session_present && reason >= 0x80))
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  // Properties
  if (az_span_size(body) > 0)
  {
    rc = _decode_connack_props(&body, out);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// PUBLISH decoding
// ============================================================================

static az_result _decode_publish_props(az_span* src, az_mqtt5_publish_data* out)
{
  int32_t prop_len;
  az_result rc = _az_mqtt_read_vbi(src, &prop_len);
  if (az_result_failed(rc))
    return rc;

  if (prop_len > az_span_size(*src))
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  az_span props = az_span_slice(*src, 0, prop_len);
  *src = az_span_slice_to_end(*src, prop_len);

  while (az_span_size(props) > 0)
  {
    uint8_t prop_id;
    rc = _az_mqtt_read_byte(&props, &prop_id);
    if (az_result_failed(rc))
      return rc;

    switch ((az_mqtt5_property_id)prop_id)
    {
      case AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR:
      {
        uint8_t v = 0;
        rc = _az_mqtt_read_byte(&props, &v);
        out->payload_format_indicator = v;
        break;
      }
      case AZ_MQTT5_PROPERTY_MESSAGE_EXPIRY_INTERVAL:
        rc = _read_uint32(&props, &out->message_expiry_interval);
        break;
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS:
        rc = _az_mqtt_read_uint16(&props, &out->topic_alias);
        break;
      case AZ_MQTT5_PROPERTY_RESPONSE_TOPIC:
        rc = _az_mqtt_read_utf8_string(&props, &out->response_topic);
        break;
      case AZ_MQTT5_PROPERTY_CORRELATION_DATA:
        rc = _az_mqtt_read_binary_data(&props, &out->correlation_data);
        break;
      case AZ_MQTT5_PROPERTY_CONTENT_TYPE:
        rc = _az_mqtt_read_utf8_string(&props, &out->content_type);
        break;
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER:
      {
        int32_t sub_id;
        rc = _az_mqtt_read_vbi(&props, &sub_id);
        if (az_result_failed(rc))
          return rc;
        if (out->subscription_identifiers != NULL
            && out->subscription_identifier_count < out->subscription_identifier_capacity)
        {
          out->subscription_identifiers[out->subscription_identifier_count] = sub_id;
          out->subscription_identifier_count++;
        }
        continue;
      }
      case AZ_MQTT5_PROPERTY_USER_PROPERTY:
      {
        az_span key, value;
        rc = _az_mqtt_read_utf8_string(&props, &key);
        if (az_result_failed(rc))
          return rc;
        rc = _az_mqtt_read_utf8_string(&props, &value);
        if (az_result_failed(rc))
          return rc;
        if (out->user_properties != NULL && out->user_property_count < out->user_property_capacity)
        {
          out->user_properties[out->user_property_count].key = key;
          out->user_properties[out->user_property_count].value = value;
          out->user_property_count++;
        }
        continue;
      }
      case AZ_MQTT5_PROPERTY_SESSION_EXPIRY_INTERVAL: // Not allowed in PUBLISH.
      case AZ_MQTT5_PROPERTY_ASSIGNED_CLIENT_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_SERVER_KEEP_ALIVE:
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_METHOD:
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_DATA:
      case AZ_MQTT5_PROPERTY_REQUEST_PROBLEM_INFORMATION:
      case AZ_MQTT5_PROPERTY_WILL_DELAY_INTERVAL:
      case AZ_MQTT5_PROPERTY_REQUEST_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_SERVER_REFERENCE:
      case AZ_MQTT5_PROPERTY_REASON_STRING:
      case AZ_MQTT5_PROPERTY_RECEIVE_MAXIMUM:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS_MAXIMUM:
      case AZ_MQTT5_PROPERTY_MAXIMUM_QOS:
      case AZ_MQTT5_PROPERTY_RETAIN_AVAILABLE:
      case AZ_MQTT5_PROPERTY_MAXIMUM_PACKET_SIZE:
      case AZ_MQTT5_PROPERTY_WILDCARD_SUBSCRIPTION_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SHARED_SUBSCRIPTION_AVAILABLE:
      default:
        return AZ_MQTT_ERROR_MALFORMED_PACKET;
    }
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result
az_mqtt5_codec_decode_publish(az_span body, uint8_t flags, az_mqtt5_publish_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt5_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;
  int32_t* subscription_identifiers = out->subscription_identifiers;
  int32_t subscription_identifier_capacity = out->subscription_identifier_capacity;

  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;
  out->subscription_identifiers = subscription_identifiers;
  out->subscription_identifier_capacity = subscription_identifier_capacity;

  out->dup = (flags & 0x08) != 0;
  out->qos = (az_mqtt_qos)((flags >> 1) & 0x03);
  if (out->qos > AZ_MQTT_QOS_EXACTLY_ONCE)
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET; // QoS 3 is reserved.
  }
  out->retain = (flags & 0x01) != 0;

  // Topic Name
  az_result rc = _az_mqtt_read_utf8_string(&body, &out->topic);
  if (az_result_failed(rc))
    return rc;

  // Packet Identifier (QoS > 0)
  if (out->qos != AZ_MQTT_QOS_AT_MOST_ONCE)
  {
    rc = _az_mqtt_read_uint16(&body, &out->packet_id);
    if (az_result_failed(rc))
      return rc;
    if (out->packet_id == 0)
      return AZ_MQTT_ERROR_MALFORMED_PACKET; // MQTT 5.0 2.2.1: non-zero.
  }

  // Properties
  rc = _decode_publish_props(&body, out);
  if (az_result_failed(rc))
    return rc;

  // The rest is the payload
  out->payload = body;

  return AZ_OK;
}

// ============================================================================
// ACK decoding (PUBACK, PUBREC, PUBREL, PUBCOMP)
// ============================================================================

static az_result _decode_ack_props(az_span* src, az_mqtt5_ack_data* out)
{
  int32_t prop_len;
  az_result rc = _az_mqtt_read_vbi(src, &prop_len);
  if (az_result_failed(rc))
    return rc;

  if (prop_len > az_span_size(*src))
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  az_span props = az_span_slice(*src, 0, prop_len);
  *src = az_span_slice_to_end(*src, prop_len);

  while (az_span_size(props) > 0)
  {
    uint8_t prop_id;
    rc = _az_mqtt_read_byte(&props, &prop_id);
    if (az_result_failed(rc))
      return rc;

    switch ((az_mqtt5_property_id)prop_id)
    {
      case AZ_MQTT5_PROPERTY_REASON_STRING:
        rc = _az_mqtt_read_utf8_string(&props, &out->reason_string);
        break;
      case AZ_MQTT5_PROPERTY_USER_PROPERTY:
      {
        az_span key, value;
        rc = _az_mqtt_read_utf8_string(&props, &key);
        if (az_result_failed(rc))
          return rc;
        rc = _az_mqtt_read_utf8_string(&props, &value);
        if (az_result_failed(rc))
          return rc;
        if (out->user_properties != NULL
            && out->user_property_count < out->user_property_capacity)
        {
          out->user_properties[out->user_property_count].key = key;
          out->user_properties[out->user_property_count].value = value;
          out->user_property_count++;
        }
        continue;
      }
      case AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR: // Not allowed in PUBACK..PUBCOMP.
      case AZ_MQTT5_PROPERTY_MESSAGE_EXPIRY_INTERVAL:
      case AZ_MQTT5_PROPERTY_CONTENT_TYPE:
      case AZ_MQTT5_PROPERTY_RESPONSE_TOPIC:
      case AZ_MQTT5_PROPERTY_CORRELATION_DATA:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_SESSION_EXPIRY_INTERVAL:
      case AZ_MQTT5_PROPERTY_ASSIGNED_CLIENT_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_SERVER_KEEP_ALIVE:
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_METHOD:
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_DATA:
      case AZ_MQTT5_PROPERTY_REQUEST_PROBLEM_INFORMATION:
      case AZ_MQTT5_PROPERTY_WILL_DELAY_INTERVAL:
      case AZ_MQTT5_PROPERTY_REQUEST_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_SERVER_REFERENCE:
      case AZ_MQTT5_PROPERTY_RECEIVE_MAXIMUM:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS_MAXIMUM:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS:
      case AZ_MQTT5_PROPERTY_MAXIMUM_QOS:
      case AZ_MQTT5_PROPERTY_RETAIN_AVAILABLE:
      case AZ_MQTT5_PROPERTY_MAXIMUM_PACKET_SIZE:
      case AZ_MQTT5_PROPERTY_WILDCARD_SUBSCRIPTION_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SHARED_SUBSCRIPTION_AVAILABLE:
      default:
        return AZ_MQTT_ERROR_MALFORMED_PACKET;
    }
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_codec_decode_ack(az_span body, az_mqtt5_ack_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt5_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;
  out->status = AZ_OK; // Not 0.

  az_result rc = _az_mqtt_read_uint16(&body, &out->packet_id);
  if (az_result_failed(rc))
    return rc;

  // If remaining length was 2, no reason code / properties
  if (az_span_size(body) == 0)
  {
    out->reason_code = AZ_MQTT5_REASON_SUCCESS;
    return AZ_OK;
  }

  uint8_t reason;
  rc = _az_mqtt_read_byte(&body, &reason);
  if (az_result_failed(rc))
    return rc;
  out->reason_code = (az_mqtt5_reason_code)reason;

  if (az_span_size(body) > 0)
  {
    rc = _decode_ack_props(&body, out);
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// SUBACK / UNSUBACK decoding
// ============================================================================

static az_result _decode_suback_props(az_span* src, az_mqtt5_suback_data* out)
{
  int32_t prop_len;
  az_result rc = _az_mqtt_read_vbi(src, &prop_len);
  if (az_result_failed(rc))
    return rc;

  if (prop_len > az_span_size(*src))
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  az_span props = az_span_slice(*src, 0, prop_len);
  *src = az_span_slice_to_end(*src, prop_len);

  while (az_span_size(props) > 0)
  {
    uint8_t prop_id;
    rc = _az_mqtt_read_byte(&props, &prop_id);
    if (az_result_failed(rc))
      return rc;

    switch ((az_mqtt5_property_id)prop_id)
    {
      case AZ_MQTT5_PROPERTY_REASON_STRING:
        rc = _az_mqtt_read_utf8_string(&props, &out->reason_string);
        break;
      case AZ_MQTT5_PROPERTY_USER_PROPERTY:
      {
        az_span key, value;
        rc = _az_mqtt_read_utf8_string(&props, &key);
        if (az_result_failed(rc))
          return rc;
        rc = _az_mqtt_read_utf8_string(&props, &value);
        if (az_result_failed(rc))
          return rc;
        if (out->user_properties != NULL
            && out->user_property_count < out->user_property_capacity)
        {
          out->user_properties[out->user_property_count].key = key;
          out->user_properties[out->user_property_count].value = value;
          out->user_property_count++;
        }
        continue;
      }
      case AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR: // Not allowed in SUBACK/UNSUBACK.
      case AZ_MQTT5_PROPERTY_MESSAGE_EXPIRY_INTERVAL:
      case AZ_MQTT5_PROPERTY_CONTENT_TYPE:
      case AZ_MQTT5_PROPERTY_RESPONSE_TOPIC:
      case AZ_MQTT5_PROPERTY_CORRELATION_DATA:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_SESSION_EXPIRY_INTERVAL:
      case AZ_MQTT5_PROPERTY_ASSIGNED_CLIENT_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_SERVER_KEEP_ALIVE:
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_METHOD:
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_DATA:
      case AZ_MQTT5_PROPERTY_REQUEST_PROBLEM_INFORMATION:
      case AZ_MQTT5_PROPERTY_WILL_DELAY_INTERVAL:
      case AZ_MQTT5_PROPERTY_REQUEST_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_SERVER_REFERENCE:
      case AZ_MQTT5_PROPERTY_RECEIVE_MAXIMUM:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS_MAXIMUM:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS:
      case AZ_MQTT5_PROPERTY_MAXIMUM_QOS:
      case AZ_MQTT5_PROPERTY_RETAIN_AVAILABLE:
      case AZ_MQTT5_PROPERTY_MAXIMUM_PACKET_SIZE:
      case AZ_MQTT5_PROPERTY_WILDCARD_SUBSCRIPTION_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SHARED_SUBSCRIPTION_AVAILABLE:
      default:
        return AZ_MQTT_ERROR_MALFORMED_PACKET;
    }
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

static az_result _decode_suback_common(az_span body, az_mqtt5_suback_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt5_reason_code* reason_codes = out->reason_codes;
  int32_t reason_code_capacity = out->reason_code_capacity;
  az_mqtt5_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  memset(out, 0, sizeof(*out));
  out->reason_codes = reason_codes;
  out->reason_code_capacity = reason_code_capacity;
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;

  az_result rc = _az_mqtt_read_uint16(&body, &out->packet_id);
  if (az_result_failed(rc))
    return rc;

  // Properties
  rc = _decode_suback_props(&body, out);
  if (az_result_failed(rc))
    return rc;

  // Remaining bytes are reason codes: one per topic filter, and there is at least one.
  out->reason_code_count = 0;
  if (az_span_size(body) == 0)
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }
  while (az_span_size(body) > 0)
  {
    uint8_t reason;
    rc = _az_mqtt_read_byte(&body, &reason);
    if (az_result_failed(rc))
      return rc;
    // Like user properties: keep what fits, so reason_code_count never exceeds capacity.
    if (out->reason_codes != NULL && out->reason_code_count < out->reason_code_capacity)
    {
      out->reason_codes[out->reason_code_count] = (az_mqtt5_reason_code)reason;
      out->reason_code_count++;
    }
  }

  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt5_codec_decode_suback(az_span body, az_mqtt5_suback_data* out)
{
  return _decode_suback_common(body, out);
}

AZ_NODISCARD az_result az_mqtt5_codec_decode_unsuback(az_span body, az_mqtt5_suback_data* out)
{
  return _decode_suback_common(body, out);
}

// ============================================================================
// DISCONNECT decoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt5_codec_decode_disconnect(az_span body, az_mqtt5_disconnect_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt5_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;

  // Short form: no reason code, no properties
  if (az_span_size(body) == 0)
  {
    out->reason_code = AZ_MQTT5_REASON_NORMAL_DISCONNECTION;
    return AZ_OK;
  }

  uint8_t reason;
  az_result rc = _az_mqtt_read_byte(&body, &reason);
  if (az_result_failed(rc))
    return rc;
  out->reason_code = (az_mqtt5_reason_code)reason;

  if (az_span_size(body) == 0)
  {
    return AZ_OK;
  }

  // Properties
  int32_t prop_len;
  rc = _az_mqtt_read_vbi(&body, &prop_len);
  if (az_result_failed(rc))
    return rc;

  if (prop_len > az_span_size(body))
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  az_span props = az_span_slice(body, 0, prop_len);

  while (az_span_size(props) > 0)
  {
    uint8_t prop_id;
    rc = _az_mqtt_read_byte(&props, &prop_id);
    if (az_result_failed(rc))
      return rc;

    switch ((az_mqtt5_property_id)prop_id)
    {
      case AZ_MQTT5_PROPERTY_SESSION_EXPIRY_INTERVAL:
        rc = _read_uint32(&props, &out->session_expiry_interval);
        break;
      case AZ_MQTT5_PROPERTY_REASON_STRING:
        rc = _az_mqtt_read_utf8_string(&props, &out->reason_string);
        break;
      case AZ_MQTT5_PROPERTY_SERVER_REFERENCE:
        rc = _az_mqtt_read_utf8_string(&props, &out->server_reference);
        break;
      case AZ_MQTT5_PROPERTY_USER_PROPERTY:
      {
        az_span key, value;
        rc = _az_mqtt_read_utf8_string(&props, &key);
        if (az_result_failed(rc))
          return rc;
        rc = _az_mqtt_read_utf8_string(&props, &value);
        if (az_result_failed(rc))
          return rc;
        if (out->user_properties != NULL
            && out->user_property_count < out->user_property_capacity)
        {
          out->user_properties[out->user_property_count].key = key;
          out->user_properties[out->user_property_count].value = value;
          out->user_property_count++;
        }
        continue;
      }
      case AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR: // Not allowed in DISCONNECT.
      case AZ_MQTT5_PROPERTY_MESSAGE_EXPIRY_INTERVAL:
      case AZ_MQTT5_PROPERTY_CONTENT_TYPE:
      case AZ_MQTT5_PROPERTY_RESPONSE_TOPIC:
      case AZ_MQTT5_PROPERTY_CORRELATION_DATA:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_ASSIGNED_CLIENT_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_SERVER_KEEP_ALIVE:
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_METHOD:
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_DATA:
      case AZ_MQTT5_PROPERTY_REQUEST_PROBLEM_INFORMATION:
      case AZ_MQTT5_PROPERTY_WILL_DELAY_INTERVAL:
      case AZ_MQTT5_PROPERTY_REQUEST_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_RECEIVE_MAXIMUM:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS_MAXIMUM:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS:
      case AZ_MQTT5_PROPERTY_MAXIMUM_QOS:
      case AZ_MQTT5_PROPERTY_RETAIN_AVAILABLE:
      case AZ_MQTT5_PROPERTY_MAXIMUM_PACKET_SIZE:
      case AZ_MQTT5_PROPERTY_WILDCARD_SUBSCRIPTION_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SHARED_SUBSCRIPTION_AVAILABLE:
      default:
        return AZ_MQTT_ERROR_MALFORMED_PACKET;
    }
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}

// ============================================================================
// AUTH decoding
// ============================================================================

AZ_NODISCARD az_result az_mqtt5_codec_decode_auth(az_span body, az_mqtt5_auth_data* out)
{
  _az_PRECONDITION_NOT_NULL(out);

  // Save caller-provided buffers before clearing
  az_mqtt5_user_property* user_properties = out->user_properties;
  int32_t user_property_capacity = out->user_property_capacity;

  memset(out, 0, sizeof(*out));
  out->user_properties = user_properties;
  out->user_property_capacity = user_property_capacity;

  if (az_span_size(body) == 0)
  {
    out->reason_code = AZ_MQTT5_REASON_SUCCESS;
    return AZ_OK;
  }

  uint8_t reason;
  az_result rc = _az_mqtt_read_byte(&body, &reason);
  if (az_result_failed(rc))
    return rc;
  out->reason_code = (az_mqtt5_reason_code)reason;

  if (az_span_size(body) == 0)
  {
    return AZ_OK;
  }

  int32_t prop_len;
  rc = _az_mqtt_read_vbi(&body, &prop_len);
  if (az_result_failed(rc))
    return rc;

  if (prop_len > az_span_size(body))
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  az_span props = az_span_slice(body, 0, prop_len);

  while (az_span_size(props) > 0)
  {
    uint8_t prop_id;
    rc = _az_mqtt_read_byte(&props, &prop_id);
    if (az_result_failed(rc))
      return rc;

    switch ((az_mqtt5_property_id)prop_id)
    {
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_METHOD:
        rc = _az_mqtt_read_utf8_string(&props, &out->authentication_method);
        break;
      case AZ_MQTT5_PROPERTY_AUTHENTICATION_DATA:
        rc = _az_mqtt_read_binary_data(&props, &out->authentication_data);
        break;
      case AZ_MQTT5_PROPERTY_REASON_STRING:
        rc = _az_mqtt_read_utf8_string(&props, &out->reason_string);
        break;
      case AZ_MQTT5_PROPERTY_USER_PROPERTY:
      {
        az_span key, value;
        rc = _az_mqtt_read_utf8_string(&props, &key);
        if (az_result_failed(rc))
          return rc;
        rc = _az_mqtt_read_utf8_string(&props, &value);
        if (az_result_failed(rc))
          return rc;
        if (out->user_properties != NULL
            && out->user_property_count < out->user_property_capacity)
        {
          out->user_properties[out->user_property_count].key = key;
          out->user_properties[out->user_property_count].value = value;
          out->user_property_count++;
        }
        continue;
      }
      case AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR: // Not allowed in AUTH.
      case AZ_MQTT5_PROPERTY_MESSAGE_EXPIRY_INTERVAL:
      case AZ_MQTT5_PROPERTY_CONTENT_TYPE:
      case AZ_MQTT5_PROPERTY_RESPONSE_TOPIC:
      case AZ_MQTT5_PROPERTY_CORRELATION_DATA:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_SESSION_EXPIRY_INTERVAL:
      case AZ_MQTT5_PROPERTY_ASSIGNED_CLIENT_IDENTIFIER:
      case AZ_MQTT5_PROPERTY_SERVER_KEEP_ALIVE:
      case AZ_MQTT5_PROPERTY_REQUEST_PROBLEM_INFORMATION:
      case AZ_MQTT5_PROPERTY_WILL_DELAY_INTERVAL:
      case AZ_MQTT5_PROPERTY_REQUEST_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_RESPONSE_INFORMATION:
      case AZ_MQTT5_PROPERTY_SERVER_REFERENCE:
      case AZ_MQTT5_PROPERTY_RECEIVE_MAXIMUM:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS_MAXIMUM:
      case AZ_MQTT5_PROPERTY_TOPIC_ALIAS:
      case AZ_MQTT5_PROPERTY_MAXIMUM_QOS:
      case AZ_MQTT5_PROPERTY_RETAIN_AVAILABLE:
      case AZ_MQTT5_PROPERTY_MAXIMUM_PACKET_SIZE:
      case AZ_MQTT5_PROPERTY_WILDCARD_SUBSCRIPTION_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SUBSCRIPTION_IDENTIFIER_AVAILABLE:
      case AZ_MQTT5_PROPERTY_SHARED_SUBSCRIPTION_AVAILABLE:
      default:
        return AZ_MQTT_ERROR_MALFORMED_PACKET;
    }
    if (az_result_failed(rc))
      return rc;
  }

  return AZ_OK;
}
