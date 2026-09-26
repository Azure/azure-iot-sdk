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
 * @brief Append one byte, bounds-checked.
 *
 * @param dst  Destination.
 * @param cap  Destination capacity.
 * @param w    Write position; advanced on success.
 * @param byte The byte.
 * @return true when it fit.
 */
static bool put(uint8_t* dst, int32_t cap, int32_t* w, uint8_t byte)
{
  if (*w >= cap)
  {
    return false;
  }
  dst[(*w)++] = byte;
  return true;
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

  for (int32_t i = 0; i < n; ++i)
  {
    uint8_t c = src[i];
    if (c != '\\')
    {
      if (!put(dst, cap, &w, c))
      {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
      }
      continue;
    }
    if (++i >= n)
    {
      return AZ_IOT_ERR_INVALID_ARG;
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
        return AZ_IOT_ERR_INVALID_ARG;
    }
    if (simple != 0)
    {
      if (!put(dst, cap, &w, simple))
      {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
      }
      continue;
    }

    uint32_t cp;
    if (n - i - 1 < 4 || !read_hex4(&src[i + 1], &cp))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    i += 4;
    if (cp >= 0xD800 && cp <= 0xDBFF)
    {
      uint32_t lo;
      if (n - i - 1 < 6 || src[i + 1] != '\\' || src[i + 2] != 'u' || !read_hex4(&src[i + 3], &lo)
          || lo < 0xDC00 || lo > 0xDFFF)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
      i += 6;
      cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
    }
    else if ((cp >= 0xDC00 && cp <= 0xDFFF) || cp == 0)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }

    bool ok;
    if (cp < 0x80)
    {
      ok = put(dst, cap, &w, (uint8_t)cp);
    }
    else if (cp < 0x800)
    {
      ok = put(dst, cap, &w, (uint8_t)(0xC0 | (cp >> 6)))
          && put(dst, cap, &w, (uint8_t)(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
      ok = put(dst, cap, &w, (uint8_t)(0xE0 | (cp >> 12)))
          && put(dst, cap, &w, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)))
          && put(dst, cap, &w, (uint8_t)(0x80 | (cp & 0x3F)));
    }
    else
    {
      ok = put(dst, cap, &w, (uint8_t)(0xF0 | (cp >> 18)))
          && put(dst, cap, &w, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)))
          && put(dst, cap, &w, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)))
          && put(dst, cap, &w, (uint8_t)(0x80 | (cp & 0x3F)));
    }
    if (!ok)
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
  }

  *out_decoded = az_span_create(dst, w);
  return AZ_IOT_OK;
}
