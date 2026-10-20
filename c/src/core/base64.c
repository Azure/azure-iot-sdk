// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>

#include <azure/core/az_base64.h>

#include "internal/base64.h"

/** @brief Characters decoded per az_core call; a multiple of 4. */
#define AZ_IOT_BASE64_BLOCK 64

/**
 * @brief 6-bit value of a base64 character.
 *
 * @param c   The character.
 * @param std Standard alphabet ('+', '/') rather than base64url ('-', '_').
 * @return The value; -1 outside the alphabet, including '='.
 */
static int32_t b64_value(uint8_t c, bool std)
{
  if (c >= 'A' && c <= 'Z')
  {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z')
  {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9')
  {
    return c - '0' + 52;
  }
  if (c == (std ? '+' : '-'))
  {
    return 62;
  }
  if (c == (std ? '/' : '_'))
  {
    return 63;
  }
  return -1;
}

az_iot_result az_iot_base64_decode(az_span in, bool allow_std, az_span out, az_span* out_decoded)
{
  const uint8_t* p = az_span_ptr(in);
  int32_t len = az_span_size(in);
  if (out_decoded == NULL || len <= 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  bool std = false;
  for (int32_t i = 0; allow_std && i < len; ++i)
  {
    std = std || p[i] == '+' || p[i] == '/' || p[i] == '=';
  }
  int32_t data_len = len;
  if (std)
  {
    if (len % 4 != 0)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    while (data_len > len - 2 && p[data_len - 1] == '=')
    {
      --data_len;
    }
  }
  int32_t rem = data_len % 4;
  if (data_len <= 0 || rem == 1 || (std && len - data_len != (4 - rem) % 4))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  for (int32_t i = 0; i < data_len; ++i)
  {
    if (b64_value(p[i], std) < 0)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }
  /* Canonical form: the bits past the last whole byte are zero. */
  int32_t last = b64_value(p[data_len - 1], std);
  if ((rem == 2 && (last & 0x0F) != 0) || (rem == 3 && (last & 0x03) != 0))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if ((data_len / 4) * 3 + (rem * 3) / 4 > az_span_size(out))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  /* Each block is read before its bytes are written, and output never passes
   * input, so decoding in place is safe. */
  uint8_t block[AZ_IOT_BASE64_BLOCK / 4 * 3];
  uint8_t* dst = az_span_ptr(out);
  int32_t w = 0;
  for (int32_t i = 0; i < len; i += AZ_IOT_BASE64_BLOCK)
  {
    int32_t end = (len - i < AZ_IOT_BASE64_BLOCK) ? len : i + AZ_IOT_BASE64_BLOCK;
    az_span src = az_span_slice(in, i, end);
    int32_t written = 0;
    az_result r = std ? az_base64_decode(AZ_SPAN_FROM_BUFFER(block), src, &written)
                      : az_base64_url_decode(AZ_SPAN_FROM_BUFFER(block), src, &written);
    if (az_result_failed(r))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    memmove(dst + w, block, (size_t)written);
    w += written;
  }
  *out_decoded = az_span_create(dst, w);
  return AZ_IOT_OK;
}
