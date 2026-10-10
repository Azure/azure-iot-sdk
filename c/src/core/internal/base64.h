// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file base64.h
 * @brief Canonical base64 decoding, in place or not.
 *
 * Validates the input before handing it to az_core, which left-shifts a
 * negative value on a character outside the alphabet and accepts nonzero
 * trailing bits. Decodes in blocks through a small buffer, so the output may
 * overwrite the input.
 */
#ifndef AZ_IOT_BASE64_H
#define AZ_IOT_BASE64_H

#include <stdbool.h>

#include <azure/core/az_span.h>

#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Decode canonical unpadded base64url or, if @p allow_std, padded standard base64.
   *
   * Standard base64 is chosen when @p in has '+', '/' or '='. Its length must be a multiple
   * of 4, with at most two trailing '='. Unused trailing bits must be zero.
   *
   * @param in          Encoded text.
   * @param allow_std   Also accept standard base64.
   * @param out         Destination. May start at az_span_ptr(@p in) (in place); must not
   *                    otherwise overlap @p in.
   * @param out_decoded The decoded slice of @p out. Untouched on error; @p out may then hold
   *                    a partial result.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG if @p in is empty, has a character outside the
   * alphabet, misplaced padding, a bad length or nonzero trailing bits;
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p out is too small.
   */
  az_iot_result az_iot_base64_decode(az_span in, bool allow_std, az_span out, az_span* out_decoded);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_BASE64_H */
