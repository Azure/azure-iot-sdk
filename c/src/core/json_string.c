// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>

#include "internal/json_string.h"

/**
 * @brief Read four hex digits.
 *
 * @param p   The digits; four bytes must be readable.
 * @param out The value.
 * @return true when all four are hex digits.
 */
static bool read_hex4(const uint8_t* p, uint32_t* out)
{
  uint32_t v = 0;
  for (int k = 0; k < 4; ++k)
  {
    uint8_t c = p[k];
    uint32_t d;
    if (c >= '0' && c <= '9')
    {
      d = (uint32_t)(c - '0');
    }
    else if (c >= 'a' && c <= 'f')
    {
      d = (uint32_t)(c - 'a' + 10);
    }
    else if (c >= 'A' && c <= 'F')
    {
      d = (uint32_t)(c - 'A' + 10);
    }
    else
    {
      return false;
    }
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

/**
 * @brief Decode the character at @p *position: a plain byte or one escape.
 *
 * @param src      The escaped string body.
 * @param size     Bytes in @p src.
 * @param position Index of the character; advanced past it. Must be below @p size.
 * @param out      Receives the decoded bytes.
 * @return Bytes written to @p out (1 to 4); 0 when the escape is malformed.
 */
static int32_t decode_next_character(
    const uint8_t* src,
    int32_t size,
    int32_t* position,
    uint8_t out[4])
{
  int32_t i = *position;
  if (src[i] != '\\')
  {
    out[0] = src[i];
    *position = i + 1;
    return 1;
  }
  if (++i >= size)
  {
    return 0;
  }

  uint8_t simple = 0;
  switch (src[i])
  {
    case '"':
    case '\\':
    case '/':
      simple = src[i];
      break;
    case 'b':
      simple = '\b';
      break;
    case 'f':
      simple = '\f';
      break;
    case 'n':
      simple = '\n';
      break;
    case 'r':
      simple = '\r';
      break;
    case 't':
      simple = '\t';
      break;
    case 'u':
      break;
    default:
      return 0;
  }
  if (simple != 0)
  {
    out[0] = simple;
    *position = i + 1;
    return 1;
  }

  uint32_t cp;
  if (size - i - 1 < 4 || !read_hex4(&src[i + 1], &cp))
  {
    return 0;
  }
  i += 4;
  if (cp >= 0xD800 && cp <= 0xDBFF)
  {
    uint32_t lo;
    if (size - i - 1 < 6 || src[i + 1] != '\\' || src[i + 2] != 'u' || !read_hex4(&src[i + 3], &lo)
        || lo < 0xDC00 || lo > 0xDFFF)
    {
      return 0;
    }
    i += 6;
    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
  }
  else if ((cp >= 0xDC00 && cp <= 0xDFFF) || cp == 0)
  {
    return 0;
  }
  *position = i + 1;

  if (cp < 0x80)
  {
    out[0] = (uint8_t)cp;
    return 1;
  }
  if (cp < 0x800)
  {
    out[0] = (uint8_t)(0xC0 | (cp >> 6));
    out[1] = (uint8_t)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000)
  {
    out[0] = (uint8_t)(0xE0 | (cp >> 12));
    out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (uint8_t)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (uint8_t)(0xF0 | (cp >> 18));
  out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (uint8_t)(0x80 | (cp & 0x3F));
  return 4;
}

az_iot_result az_iot_json_string_decode(az_span in, az_span out, az_span* out_decoded)
{
  if (out_decoded == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  const uint8_t* src = az_span_ptr(in);
  int32_t size = az_span_size(in);
  uint8_t* dst = az_span_ptr(out);
  int32_t capacity = az_span_size(out);
  int32_t written = 0;

  int32_t position = 0;
  while (position < size)
  {
    /* Decoded before it is written: in place, the write never passes the read. */
    uint8_t character[4];
    int32_t character_size = decode_next_character(src, size, &position, character);
    if (character_size == 0)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    if (capacity - written < character_size)
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    for (int32_t k = 0; k < character_size; ++k)
    {
      dst[written++] = character[k];
    }
  }

  *out_decoded = az_span_create(dst, written);
  return AZ_IOT_OK;
}

/** @brief Reads the decoded bytes of a JSON string body, or plain text, one byte at a time. */
typedef struct
{
  const uint8_t* text;
  int32_t size;
  int32_t position;
  bool is_escaped;
  uint8_t pending[4];
  int32_t pending_size;
  int32_t pending_position;
} json_string_byte_reader;

/** @brief End of the text, from json_string_byte_reader_next(). */
#define JSON_STRING_BYTE_READER_END (-1)
/** @brief A malformed escape, from json_string_byte_reader_next(). */
#define JSON_STRING_BYTE_READER_MALFORMED (-2)

/**
 * @brief Read the next decoded byte.
 *
 * @return The byte (0 to 255); JSON_STRING_BYTE_READER_END; or JSON_STRING_BYTE_READER_MALFORMED.
 */
static int32_t json_string_byte_reader_next(json_string_byte_reader* reader)
{
  if (reader->pending_position < reader->pending_size)
  {
    return reader->pending[reader->pending_position++];
  }
  if (reader->position >= reader->size)
  {
    return JSON_STRING_BYTE_READER_END;
  }
  if (!reader->is_escaped)
  {
    return reader->text[reader->position++];
  }
  reader->pending_size
      = decode_next_character(reader->text, reader->size, &reader->position, reader->pending);
  if (reader->pending_size == 0)
  {
    return JSON_STRING_BYTE_READER_MALFORMED;
  }
  reader->pending_position = 1;
  return reader->pending[0];
}

bool az_iot_json_string_equals(az_span escaped, az_span other, bool other_is_escaped)
{
  json_string_byte_reader readers[2] = {
    { az_span_ptr(escaped), az_span_size(escaped), 0, true, { 0 }, 0, 0 },
    { az_span_ptr(other), az_span_size(other), 0, other_is_escaped, { 0 }, 0, 0 },
  };
  for (;;)
  {
    int32_t a = json_string_byte_reader_next(&readers[0]);
    int32_t b = json_string_byte_reader_next(&readers[1]);
    if (a == JSON_STRING_BYTE_READER_MALFORMED || a != b)
    {
      return false;
    }
    if (a == JSON_STRING_BYTE_READER_END)
    {
      return true;
    }
  }
}
