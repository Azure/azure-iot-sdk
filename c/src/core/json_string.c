// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
 * @brief Decode the character at @p *i of a JSON string body to UTF-8.
 *
 * @param src  The escaped string body.
 * @param n    Size of @p src.
 * @param i    Position of the character; advanced past it on success.
 * @param utf8 Receives the UTF-8 bytes.
 * @return The number of bytes in @p utf8 (1 to 4); 0 on an unknown or truncated
 * escape, a lone or reversed surrogate, or `\u0000`.
 */
static int32_t decode_next_character(const uint8_t* src, int32_t n, int32_t* i, uint8_t utf8[4])
{
  int32_t at = *i;
  uint8_t c = src[at];
  if (c != '\\')
  {
    utf8[0] = c;
    *i = at + 1;
    return 1;
  }
  if (++at >= n)
  {
    return 0;
  }

  uint8_t simple = 0;
  switch (src[at])
  {
    case '"':
    case '\\':
    case '/':
      simple = src[at];
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
    utf8[0] = simple;
    *i = at + 1;
    return 1;
  }

  uint32_t cp;
  if (n - at - 1 < 4 || !read_hex4(&src[at + 1], &cp))
  {
    return 0;
  }
  at += 4;
  if (cp >= 0xD800 && cp <= 0xDBFF)
  {
    uint32_t lo;
    if (n - at - 1 < 6 || src[at + 1] != '\\' || src[at + 2] != 'u' || !read_hex4(&src[at + 3], &lo)
        || lo < 0xDC00 || lo > 0xDFFF)
    {
      return 0;
    }
    at += 6;
    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
  }
  else if ((cp >= 0xDC00 && cp <= 0xDFFF) || cp == 0)
  {
    return 0;
  }
  *i = at + 1;

  if (cp < 0x80)
  {
    utf8[0] = (uint8_t)cp;
    return 1;
  }
  if (cp < 0x800)
  {
    utf8[0] = (uint8_t)(0xC0 | (cp >> 6));
    utf8[1] = (uint8_t)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000)
  {
    utf8[0] = (uint8_t)(0xE0 | (cp >> 12));
    utf8[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    utf8[2] = (uint8_t)(0x80 | (cp & 0x3F));
    return 3;
  }
  utf8[0] = (uint8_t)(0xF0 | (cp >> 18));
  utf8[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
  utf8[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
  utf8[3] = (uint8_t)(0x80 | (cp & 0x3F));
  return 4;
}

az_iot_result az_iot_json_string_decode(az_span in, az_span out, az_span* out_decoded)
{
  if (out_decoded == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  const uint8_t* src = az_span_ptr(in);
  int32_t n = az_span_size(in);
  uint8_t* dst = az_span_ptr(out);
  int32_t cap = az_span_size(out);
  int32_t w = 0;

  for (int32_t i = 0; i < n;)
  {
    uint8_t utf8[4];
    int32_t length = decode_next_character(src, n, &i, utf8);
    if (length == 0)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    if (cap - w < length)
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    memcpy(&dst[w], utf8, (size_t)length);
    w += length;
  }

  *out_decoded = az_span_create(dst, w);
  return AZ_IOT_OK;
}

bool az_iot_json_string_equals(az_span escaped, az_span plain)
{
  const uint8_t* src = az_span_ptr(escaped);
  int32_t n = az_span_size(escaped);
  const uint8_t* expected = az_span_ptr(plain);
  int32_t remaining = az_span_size(plain);

  for (int32_t i = 0; i < n;)
  {
    uint8_t utf8[4];
    int32_t length = decode_next_character(src, n, &i, utf8);
    if (length == 0 || length > remaining || memcmp(utf8, expected, (size_t)length) != 0)
    {
      return false;
    }
    expected += length;
    remaining -= length;
  }
  return remaining == 0;
}
