// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.
/* SPDX-License-Identifier: MIT */

/**
 * @file fuzz_parsers.c
 * @brief Fuzz target for the standalone parsers of service-supplied text.
 *
 * The first input byte selects the parser; the remaining bytes are its input.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "azure/core/az_json.h"
#include "azure/core/az_span.h"
#include "fuzz_common.h"
#include "internal/base64.h"
#include "internal/cert_util.h"
#include "internal/json_string.h"

/** @brief Longest parser input; longer inputs are truncated. Decoding never grows its input. */
#define FUZZ_PARSERS_INPUT_MAX 4096

/** @brief Certificates a chain may hold in this target. */
#define FUZZ_PARSERS_CHAIN_MAX 4

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/** @brief JSON string decoding, and comparison of the two input halves (escaped and not). */
static void fuzz_json_string(az_span in)
{
  uint8_t out[FUZZ_PARSERS_INPUT_MAX];
  az_span decoded;
  (void)az_iot_json_string_decode(in, AZ_SPAN_FROM_BUFFER(out), &decoded);

  int32_t half = az_span_size(in) / 2;
  az_span first = az_span_slice(in, 0, half);
  az_span second = az_span_slice_to_end(in, half);
  (void)az_iot_json_string_equals(first, second, true);
  (void)az_iot_json_string_equals(first, second, false);
}

/** @brief Base64url decoding, with and without standard base64, into a buffer and in place. */
static void fuzz_base64(az_span in)
{
  uint8_t out[FUZZ_PARSERS_INPUT_MAX];
  az_span decoded;
  (void)az_iot_base64_decode(in, false, AZ_SPAN_FROM_BUFFER(out), &decoded);
  (void)az_iot_base64_decode(in, true, AZ_SPAN_FROM_BUFFER(out), &decoded);

  int32_t size = az_span_size(in);
  if (size > 0)
  {
    memcpy(out, az_span_ptr(in), (size_t)size);
    az_span copy = az_span_create(out, size);
    (void)az_iot_base64_decode(copy, true, copy, &decoded);
  }
}

/** @brief The certificate helpers: base64 check, JSON completeness and chain collection. */
static void fuzz_cert_util(az_span in)
{
  char text[FUZZ_PARSERS_INPUT_MAX + 1];
  int32_t size = az_span_size(in);
  if (size > 0)
  {
    memcpy(text, az_span_ptr(in), (size_t)size);
  }
  text[size] = '\0';
  size_t length = 0;
  (void)az_iot_cert_util_is_base64(text, &length);

  (void)az_iot_cert_util_json_is_complete(in);

  az_json_reader reader;
  if (size > 0 && az_json_reader_init(&reader, in, NULL) == AZ_OK
      && az_json_reader_next_token(&reader) == AZ_OK
      && reader.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
  {
    az_span certs[FUZZ_PARSERS_CHAIN_MAX];
    size_t count = 0;
    (void)az_iot_cert_util_collect_chain_spans(&reader, certs, FUZZ_PARSERS_CHAIN_MAX, &count);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  static uint8_t empty;
  fuzz_abort_on_precondition_failure();
  if (size < 1)
  {
    return 0;
  }
  size_t length = size - 1;
  if (length > FUZZ_PARSERS_INPUT_MAX)
  {
    length = FUZZ_PARSERS_INPUT_MAX;
  }
  /* az_span_create() requires a non-NULL pointer, even for an empty span. */
  uint8_t* body = length > 0 ? (uint8_t*)(uintptr_t)(data + 1) : &empty;
  az_span in = az_span_create(body, (int32_t)length);

  switch (data[0] % 3)
  {
    case 0:
      fuzz_json_string(in);
      break;
    case 1:
      fuzz_base64(in);
      break;
    default:
      fuzz_cert_util(in);
      break;
  }
  return 0;
}
