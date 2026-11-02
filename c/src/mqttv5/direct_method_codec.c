// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Simplified proto3 codec for common/Protos/directmethods.proto. See the header
 * for why this is hand-rolled rather than generated. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "internal/direct_method_codec.h"

/* proto3 wire types. Types 3 and 4 are the deprecated group encoding, which
 * proto3 cannot produce; seeing one means the payload is not what it claims. */
#define WIRE_VARINT 0u
#define WIRE_64BIT 1u
#define WIRE_LEN 2u
#define WIRE_32BIT 5u

/* Payload widths of the two fixed-size wire types. */
#define WIRE_64BIT_BYTES 8u
#define WIRE_32BIT_BYTES 4u

/* A varint never needs more than ten bytes, which is what a 64-bit value takes
 * at seven bits per byte. */
#define VARINT_MAX_BYTES 10u
/* Base-128 varint: seven payload bits per byte, top bit set while more follow. */
#define VARINT_PAYLOAD_MASK 0x7Fu
#define VARINT_CONTINUATION_BIT 0x80u
#define VARINT_PAYLOAD_BITS 7u

/* A field key packs the field number above its three wire-type bits. */
#define FIELD_KEY_WIRE_TYPE_MASK 0x07u
#define FIELD_KEY_NUMBER_SHIFT 3u

/* Field numbers, straight from directmethods.proto. */
#define PROBE_FIELD_METHOD_NAME 1u
#define PROBE_FIELD_RESPONSE_TIMEOUT 2u
#define EXEC_FIELD_READY_ID 1u
#define EXEC_FIELD_PARAMS 2u
#define PROBE_ACK_FIELD_READY 1u
#define PROBE_ACK_FIELD_REJECTED 2u
#define READY_FIELD_READY_ID 1u
#define REJECTED_FIELD_REASON 1u
#define ABANDON_FIELD_READY_ID 1u
#define ABANDON_FIELD_REASON 2u
#define RESULT_FIELD_STATUS 1u
#define RESULT_FIELD_BODY 2u

typedef struct
{
  const uint8_t* buffer;
  size_t length;
  size_t offset;
} proto_reader;

/* Reads one base-128 varint. Fails on truncation and on a value that runs past
 * ten bytes, which is how a corrupt payload would otherwise spin. */
static bool read_varint(proto_reader* reader, uint64_t* out_value)
{
  uint64_t value = 0;
  unsigned shift = 0;
  size_t consumed = 0;

  while (reader->offset < reader->length)
  {
    uint8_t byte = reader->buffer[reader->offset++];
    consumed++;
    value |= ((uint64_t)(byte & VARINT_PAYLOAD_MASK)) << shift;
    if ((byte & VARINT_CONTINUATION_BIT) == 0u)
    {
      *out_value = value;
      return true;
    }
    if (consumed == VARINT_MAX_BYTES)
    {
      return false;
    }
    shift += VARINT_PAYLOAD_BITS;
  }
  return false;
}

/* Reads a length-delimited field as a view into the reader's buffer. */
static bool read_delimited(proto_reader* reader, const uint8_t** out_data, size_t* out_length)
{
  uint64_t length = 0;
  if (!read_varint(reader, &length))
  {
    return false;
  }
  /* Compare against the bytes left rather than adding to the offset: the
   * addition could wrap on a length near UINT64_MAX. */
  if (length > (uint64_t)(reader->length - reader->offset))
  {
    return false;
  }
  *out_data = reader->buffer + reader->offset;
  *out_length = (size_t)length;
  reader->offset += (size_t)length;
  return true;
}

/* Steps over a field this decoder does not know, so that a message extended by
 * a later protocol revision still decodes. */
static bool skip_field(proto_reader* reader, uint32_t wire_type)
{
  uint64_t scratch = 0;
  const uint8_t* data = NULL;
  size_t data_length = 0;

  switch (wire_type)
  {
    case WIRE_VARINT:
      return read_varint(reader, &scratch);
    case WIRE_64BIT:
      if (reader->length - reader->offset < WIRE_64BIT_BYTES)
      {
        return false;
      }
      reader->offset += WIRE_64BIT_BYTES;
      return true;
    case WIRE_LEN:
      return read_delimited(reader, &data, &data_length);
    case WIRE_32BIT:
      if (reader->length - reader->offset < WIRE_32BIT_BYTES)
      {
        return false;
      }
      reader->offset += WIRE_32BIT_BYTES;
      return true;
    default:
      return false;
  }
}

/* Reads the next field key. Returns false at end of message or on a malformed
 * key; `out_at_end` separates the two. */
static bool read_key(
    proto_reader* reader,
    uint32_t* out_field,
    uint32_t* out_wire_type,
    bool* out_at_end)
{
  uint64_t key = 0;
  *out_at_end = false;
  if (reader->offset >= reader->length)
  {
    *out_at_end = true;
    return false;
  }
  if (!read_varint(reader, &key))
  {
    return false;
  }
  *out_field = (uint32_t)(key >> FIELD_KEY_NUMBER_SHIFT);
  *out_wire_type = (uint32_t)(key & FIELD_KEY_WIRE_TYPE_MASK);
  /* Field number 0 is not assignable, so a zero key is a decode error rather
   * than an unknown field to skip. */
  return *out_field != 0u;
}

AZ_NODISCARD az_iot_result
az_iot_dm_proto_decode_probe(const uint8_t* buffer, size_t length, az_iot_dm_proto_probe* out_probe)
{
  if (out_probe == NULL || (length > 0u && buffer == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(out_probe, 0, sizeof(*out_probe));

  proto_reader reader = { buffer, length, 0u };
  for (;;)
  {
    uint32_t field = 0;
    uint32_t wire_type = 0;
    bool at_end = false;
    if (!read_key(&reader, &field, &wire_type, &at_end))
    {
      return at_end ? AZ_IOT_OK : AZ_IOT_ERR_PROTOCOL;
    }

    if (field == PROBE_FIELD_METHOD_NAME && wire_type == WIRE_LEN)
    {
      const uint8_t* data = NULL;
      size_t data_length = 0;
      if (!read_delimited(&reader, &data, &data_length))
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
      out_probe->method_name = (const char*)data;
      out_probe->method_name_len = data_length;
    }
    else if (field == PROBE_FIELD_RESPONSE_TIMEOUT && wire_type == WIRE_VARINT)
    {
      uint64_t value = 0;
      if (!read_varint(&reader, &value))
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
      /* proto3 uint32 is transmitted as a 64-bit varint; a peer that overflows
       * the declared width is misencoding, not describing a huge timeout. */
      if (value > UINT32_MAX)
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
      out_probe->response_timeout_seconds = (uint32_t)value;
    }
    else if (!skip_field(&reader, wire_type))
    {
      return AZ_IOT_ERR_PROTOCOL;
    }
  }
}

AZ_NODISCARD az_iot_result
az_iot_dm_proto_decode_exec(const uint8_t* buffer, size_t length, az_iot_dm_proto_exec* out_exec)
{
  if (out_exec == NULL || (length > 0u && buffer == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(out_exec, 0, sizeof(*out_exec));

  proto_reader reader = { buffer, length, 0u };
  for (;;)
  {
    uint32_t field = 0;
    uint32_t wire_type = 0;
    bool at_end = false;
    if (!read_key(&reader, &field, &wire_type, &at_end))
    {
      return at_end ? AZ_IOT_OK : AZ_IOT_ERR_PROTOCOL;
    }

    if ((field == EXEC_FIELD_READY_ID || field == EXEC_FIELD_PARAMS) && wire_type == WIRE_LEN)
    {
      const uint8_t* data = NULL;
      size_t data_length = 0;
      if (!read_delimited(&reader, &data, &data_length))
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
      if (field == EXEC_FIELD_READY_ID)
      {
        out_exec->ready_id = data;
        out_exec->ready_id_len = data_length;
      }
      else
      {
        out_exec->params = data;
        out_exec->params_len = data_length;
      }
    }
    else if (!skip_field(&reader, wire_type))
    {
      /* Field 3 (exec_start) lands here and is skipped like any other unknown
       * field, which is the intent: it is observability-only. */
      return AZ_IOT_ERR_PROTOCOL;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* encoding                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct
{
  uint8_t* buffer;
  size_t capacity;
  size_t offset;
  bool overflowed;
} proto_writer;

static void write_byte(proto_writer* writer, uint8_t value)
{
  if (writer->offset >= writer->capacity)
  {
    writer->overflowed = true;
    return;
  }
  writer->buffer[writer->offset++] = value;
}

static void write_varint(proto_writer* writer, uint64_t value)
{
  while (value >= VARINT_CONTINUATION_BIT)
  {
    write_byte(writer, (uint8_t)(value | VARINT_CONTINUATION_BIT));
    value >>= VARINT_PAYLOAD_BITS;
  }
  write_byte(writer, (uint8_t)value);
}

static void write_key(proto_writer* writer, uint32_t field, uint32_t wire_type)
{
  write_varint(writer, ((uint64_t)field << FIELD_KEY_NUMBER_SHIFT) | wire_type);
}

/* Encoded width of `value` as a varint. Needed to length-prefix a submessage,
 * whose size must be known before its first byte is written. */
static size_t varint_size(uint64_t value)
{
  size_t size = 1u;
  while (value >= VARINT_CONTINUATION_BIT)
  {
    value >>= VARINT_PAYLOAD_BITS;
    size++;
  }
  return size;
}

static void write_bytes_field(
    proto_writer* writer,
    uint32_t field,
    const uint8_t* data,
    size_t length)
{
  write_key(writer, field, WIRE_LEN);
  write_varint(writer, (uint64_t)length);
  if (writer->overflowed)
  {
    return;
  }
  if (writer->capacity - writer->offset < length)
  {
    writer->overflowed = true;
    return;
  }
  if (length > 0u)
  {
    memcpy(writer->buffer + writer->offset, data, length);
    writer->offset += length;
  }
}

static az_iot_result writer_finish(proto_writer* writer, size_t* out_length)
{
  if (writer->overflowed)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  *out_length = writer->offset;
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_dm_proto_encode_probe_ack_ready(
    uint8_t* buffer,
    size_t capacity,
    const uint8_t* ready_id,
    size_t ready_id_len,
    size_t* out_length)
{
  if (buffer == NULL || out_length == NULL || ready_id == NULL || ready_id_len == 0u)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  proto_writer writer = { buffer, capacity, 0u, false };
  /* ProbeAck.ready is a submessage, so its length prefix has to be known
   * before it is written. Ready holds one bytes field, whose size is its tag,
   * its own length prefix and the id. */
  size_t ready_len = 1u + varint_size((uint64_t)ready_id_len) + ready_id_len;
  write_key(&writer, PROBE_ACK_FIELD_READY, WIRE_LEN);
  write_varint(&writer, (uint64_t)ready_len);
  write_bytes_field(&writer, READY_FIELD_READY_ID, ready_id, ready_id_len);
  return writer_finish(&writer, out_length);
}

AZ_NODISCARD az_iot_result az_iot_dm_proto_encode_probe_ack_rejected(
    uint8_t* buffer,
    size_t capacity,
    az_iot_dm_proto_rejected_reason reason,
    size_t* out_length)
{
  if (buffer == NULL || out_length == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  proto_writer writer = { buffer, capacity, 0u, false };
  /* proto3 omits a default-valued scalar, so Rejected{UNSPECIFIED} is an empty
   * submessage -- but the oneof case is still present, which is what
   * distinguishes a rejection from an unset ProbeAck. */
  size_t rejected_len = (reason == AZ_IOT_DM_PROTO_REJECTED_UNSPECIFIED) ? 0u : 2u;
  write_key(&writer, PROBE_ACK_FIELD_REJECTED, WIRE_LEN);
  write_varint(&writer, (uint64_t)rejected_len);
  if (rejected_len > 0u)
  {
    write_key(&writer, REJECTED_FIELD_REASON, WIRE_VARINT);
    write_varint(&writer, (uint64_t)reason);
  }
  return writer_finish(&writer, out_length);
}

AZ_NODISCARD az_iot_result az_iot_dm_proto_encode_abandon(
    uint8_t* buffer,
    size_t capacity,
    const uint8_t* ready_id,
    size_t ready_id_len,
    az_iot_dm_proto_abandon_reason reason,
    size_t* out_length)
{
  if (buffer == NULL || out_length == NULL || ready_id == NULL || ready_id_len == 0u)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  proto_writer writer = { buffer, capacity, 0u, false };
  write_bytes_field(&writer, ABANDON_FIELD_READY_ID, ready_id, ready_id_len);
  if (reason != AZ_IOT_DM_PROTO_ABANDON_UNSPECIFIED)
  {
    write_key(&writer, ABANDON_FIELD_REASON, WIRE_VARINT);
    write_varint(&writer, (uint64_t)reason);
  }
  return writer_finish(&writer, out_length);
}

AZ_NODISCARD az_iot_result az_iot_dm_proto_encode_result(
    uint8_t* buffer,
    size_t capacity,
    int32_t status,
    const uint8_t* body,
    size_t body_len,
    size_t* out_length)
{
  if (buffer == NULL || out_length == NULL || (body_len > 0u && body == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  proto_writer writer = { buffer, capacity, 0u, false };
  if (status != 0)
  {
    write_key(&writer, RESULT_FIELD_STATUS, WIRE_VARINT);
    /* proto3 encodes a negative int32 as a 64-bit two's-complement varint, so
     * the value has to be widened rather than zero-extended. */
    write_varint(&writer, (uint64_t)(int64_t)status);
  }
  if (body_len > 0u)
  {
    write_bytes_field(&writer, 2u, body, body_len);
  }
  return writer_finish(&writer, out_length);
}
