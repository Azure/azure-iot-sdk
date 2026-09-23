// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file json_string.h
 * @brief JSON string decoding.
 *
 * az_json_string_unescape() stops silently at a `\u` escape, returning a
 * truncated result that looks valid. This decoder handles every escape and
 * reports malformed input.
 */
#ifndef AZ_IOT_JSON_STRING_H
#define AZ_IOT_JSON_STRING_H

#include <azure/core/az_span.h>

#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Decode a JSON string body (without quotes) to UTF-8.
   *
   * Decodes the two-character escapes and `\uXXXX`, including surrogate
   * pairs. The output is never longer than the input, so @p out may alias
   * @p in (in-place decoding) when both start at the same address.
   *
   * @param in          The escaped string body, e.g. a token slice.
   * @param out         Destination.
   * @param out_decoded The decoded slice of @p out. Untouched on error; @p out
   *                    may then hold a partial result.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG on an unknown or truncated
   * escape, a lone or reversed surrogate, or `\u0000`;
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p out is too small.
   */
  az_iot_result az_iot_json_string_decode(az_span in, az_span out, az_span* out_decoded);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_JSON_STRING_H */
