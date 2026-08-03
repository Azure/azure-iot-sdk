// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Internal certificate/CSR helpers shared within the connection client. */
#ifndef AZ_IOT_INTERNAL_CERT_UTIL_H
#define AZ_IOT_INTERNAL_CERT_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <azure/az_core.h>

#include "azure/iot/az_iot_result.h"

/* True when `s` is a non-empty base64 string (RFC 4648 alphabet + padding),
 * writing its length to *out_len; false otherwise. */
bool az_iot_cert_util_is_base64(const char* s, size_t* out_len);

/* Generate a short, run-unique request id ("%08x-%08x") into buf, using an LCG
 * seeded by the monotonic clock and advanced through *rng_state. */
void az_iot_cert_util_gen_request_id(uint64_t* rng_state, char* buf, size_t cap);

/* Collect the base64 DER cert strings of a JSON array (the reader positioned so
 * the next tokens are the array's string elements) as ZERO-COPY spans into the
 * payload. Fills certs[0..*out_count) and returns AZ_IOT_OK, or a *_PROTOCOL /
 * *_NOT_ENOUGH_SPACE error. */
az_iot_result az_iot_cert_util_collect_chain_spans(
    az_json_reader* jr,
    az_span* certs,
    size_t max,
    size_t* out_count);

#endif /* AZ_IOT_INTERNAL_CERT_UTIL_H */
