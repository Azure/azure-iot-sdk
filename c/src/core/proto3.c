// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Minimal proto3 wire-format reader/writer. See internal/proto3.h. */
#include <string.h>

#include "internal/proto3.h"

bool az_iot_proto3_read_varint(const uint8_t* buf, size_t len, size_t* pos, uint64_t* out_value)
{
  if (!buf || !pos || !out_value)
  {
    return false;
  }

  uint64_t v = 0;
  unsigned shift = 0;
  while (*pos < len)
  {
    uint8_t b = buf[(*pos)++];
    /* The tenth byte can carry only bit 63, so its payload must be 0 or 1.
     * A larger one sets bits the value cannot hold; the shift below would
     * drop them and the wrapped remainder would pass for a valid value. */
    if (shift == AZ_IOT_PROTO3_VARINT_MAX_SHIFT && (b & 0x7Fu) > AZ_IOT_PROTO3_VARINT_TOP_BIT_MAX)
    {
      return false;
    }
    if (shift < 64u)
    {
      v |= ((uint64_t)(b & 0x7Fu)) << shift;
    }
    if ((b & 0x80u) == 0)
    {
      *out_value = v;
      return true;
    }
    shift += 7u;
    /* 10 bytes is the widest legal uint64: nine 7-bit groups cover bits
     * 0..62 and the tenth contributes bit 63 alone. So shift == 63 must
     * still be accepted, which is why this is checked after the shift
     * advances rather than before the byte is consumed. */
    if (shift > AZ_IOT_PROTO3_VARINT_MAX_SHIFT)
    {
      return false; /* > 10 bytes: malformed */
    }
  }
  return false; /* ran off the end mid-varint */
}

bool az_iot_proto3_read_tag(
    const uint8_t* buf,
    size_t len,
    size_t* pos,
    uint32_t* out_field,
    uint8_t* out_wire)
{
  if (!out_field || !out_wire)
  {
    return false;
  }

  uint64_t key = 0;
  if (!az_iot_proto3_read_varint(buf, len, pos, &key))
  {
    return false;
  }

  *out_field = (uint32_t)(key >> 3);
  *out_wire = (uint8_t)(key & 0x07u);
  return true;
}

bool az_iot_proto3_read_bytes(
    const uint8_t* buf,
    size_t len,
    size_t* pos,
    const uint8_t** out_bytes,
    size_t* out_len)
{
  if (!out_bytes || !out_len)
  {
    return false;
  }

  uint64_t n = 0;
  if (!az_iot_proto3_read_varint(buf, len, pos, &n))
  {
    return false;
  }
  if (n > (uint64_t)(len - *pos))
  {
    return false;
  }

  *out_bytes = (n > 0) ? (buf + *pos) : NULL;
  *out_len = (size_t)n;
  *pos += (size_t)n;
  return true;
}

bool az_iot_proto3_skip_field(const uint8_t* buf, size_t len, size_t* pos, uint8_t wire)
{
  if (!pos)
  {
    return false;
  }

  switch (wire)
  {
    case AZ_IOT_PROTO3_WIRE_VARINT:
    {
      uint64_t ignored = 0;
      return az_iot_proto3_read_varint(buf, len, pos, &ignored);
    }
    case AZ_IOT_PROTO3_WIRE_LEN:
    {
      const uint8_t* ignored_bytes = NULL;
      size_t ignored_len = 0;
      return az_iot_proto3_read_bytes(buf, len, pos, &ignored_bytes, &ignored_len);
    }
    case AZ_IOT_PROTO3_WIRE_32BIT:
      if (len - *pos < 4u)
      {
        return false;
      }
      *pos += 4u;
      return true;
    case AZ_IOT_PROTO3_WIRE_64BIT:
      if (len - *pos < 8u)
      {
        return false;
      }
      *pos += 8u;
      return true;
    default:
      return false; /* groups (3/4) and unknown wire types */
  }
}

/* Append a varint, or report that it does not fit without touching `pos`. */
static bool proto3_write_varint(uint8_t* buf, size_t cap, size_t* pos, uint64_t value)
{
  uint8_t tmp[10];
  size_t n = 0;
  do
  {
    uint8_t b = (uint8_t)(value & 0x7Fu);
    value >>= 7;
    if (value)
    {
      b = (uint8_t)(b | 0x80u);
    }
    tmp[n++] = b;
  } while (value);

  if (n > cap - *pos)
  {
    return false;
  }
  memcpy(buf + *pos, tmp, n);
  *pos += n;
  return true;
}

bool az_iot_proto3_write_tag(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, uint8_t wire)
{
  if (!buf || !pos || *pos > cap)
  {
    return false;
  }
  return proto3_write_varint(buf, cap, pos, ((uint64_t)field << 3) | wire);
}

bool az_iot_proto3_write_varint_field(
    uint8_t* buf,
    size_t cap,
    size_t* pos,
    uint32_t field,
    uint64_t value)
{
  if (!buf || !pos || *pos > cap)
  {
    return false;
  }

  /* Emit the key and the value as a unit: on overflow the caller must see the
   * buffer unchanged, not a dangling key with no value. */
  size_t start = *pos;
  if (!az_iot_proto3_write_tag(buf, cap, pos, field, AZ_IOT_PROTO3_WIRE_VARINT)
      || !proto3_write_varint(buf, cap, pos, value))
  {
    *pos = start;
    return false;
  }
  return true;
}

bool az_iot_proto3_write_bytes_field(
    uint8_t* buf,
    size_t cap,
    size_t* pos,
    uint32_t field,
    const uint8_t* bytes,
    size_t len)
{
  if (!buf || !pos || *pos > cap)
  {
    return false;
  }
  if (len > 0 && !bytes)
  {
    return false;
  }

  size_t start = *pos;
  if (!az_iot_proto3_write_tag(buf, cap, pos, field, AZ_IOT_PROTO3_WIRE_LEN)
      || !proto3_write_varint(buf, cap, pos, (uint64_t)len) || len > cap - *pos)
  {
    *pos = start;
    return false;
  }

  if (len)
  {
    memcpy(buf + *pos, bytes, len);
  }
  *pos += len;
  return true;
}
