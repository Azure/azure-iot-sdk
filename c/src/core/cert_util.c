// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "internal/cert_util.h"

#include <stdio.h>

#include "internal/log_internal.h"
#include "internal/reconnect.h"     /* az_iot_time_mono_ms */

/* LCG mixing constants for the request-id nonce generator. */
#define CERT_RNG_LCG_MULTIPLIER 6364136223846793005ull
#define CERT_RNG_LCG_INCREMENT  1442695040888963407ull

bool az_iot_cert_util_is_base64(const char* s, size_t* out_len)
{
    size_t n = 0;
    for (; s[n]; ++n)
    {
        char ch = s[n];
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
               || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=';
        if (!ok) return false;
    }
    *out_len = n;
    return n > 0;
}

void az_iot_cert_util_gen_request_id(uint64_t* rng_state, char* buf, size_t cap)
{
    uint64_t x = az_iot_time_mono_ms()
               ^ (*rng_state * CERT_RNG_LCG_MULTIPLIER + CERT_RNG_LCG_INCREMENT);
    *rng_state = x;
    (void)snprintf(buf, cap, "%08x-%08x",
                   (unsigned)(x >> 32), (unsigned)(x & 0xffffffffu));
}

az_iot_result az_iot_cert_util_collect_chain_spans(
    az_json_reader* jr, az_span* certs, size_t max, size_t* out_count)
{
    size_t count = 0;
    while (az_result_succeeded(az_json_reader_next_token(jr))
           && jr->token.kind != AZ_JSON_TOKEN_END_ARRAY)
    {
        if (jr->token.kind != AZ_JSON_TOKEN_STRING)
        {
            return AZ_IOT_ERR_PROTOCOL;
        }
        if (count >= max)
        {
            AZ_IOT_LOG_ERROR("issued cert chain: more certificates than the supported maximum");
            return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
        }
        certs[count++] = jr->token.slice;
    }
    *out_count = count;
    return AZ_IOT_OK;
}
