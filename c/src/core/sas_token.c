// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* See internal/sas_token.h. */
#include "internal/sas_token.h"

#include <string.h>

#include <azure/core/az_base64.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_log.h"

#include "internal/span_writer.h"

#define SAS_PREFIX "SharedAccessSignature sr="
#define SAS_SIG "&sig="
#define SAS_SE "&se="
/* Present on the provisioning audience only. The DPS device endpoint rejects a
 * token without it; a hub rejects one with it. */
#define SAS_SKN "&skn=registration"

az_iot_result az_iot_sas__build_resource(
    az_iot_sas_audience audience,
    const char* first,
    const char* second,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  if (first == NULL || second == NULL || out == NULL || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_span_writer w;
  az_iot_span_writer_init(&w, az_span_create((uint8_t*)out, (int32_t)out_size));
  az_iot_span_writer_append_str(&w, first);
  az_iot_span_writer_append_str(
      &w, audience == AZ_IOT_SAS_AUDIENCE_PROVISIONING ? "/registrations/" : "/devices/");
  az_iot_span_writer_append_str(&w, second);
  return az_iot_span_writer_end_str(&w, out_len);
}

az_iot_result az_iot_sas__url_encode(
    const char* in,
    size_t in_len,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  (void)in_len;
  if (in == NULL || out == NULL || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_span_writer w;
  az_iot_span_writer_init(&w, az_span_create((uint8_t*)out, (int32_t)out_size));
  az_iot_span_writer_append_url_encoded(&w, in);
  return az_iot_span_writer_end_str(&w, out_len);
}

/* Decimal, because the service reads `se` as a Unix timestamp. Written by hand
 * rather than through a formatter: the span writer has no integer append, and
 * printf-family calls are banned in this tree. */
static void append_u64(az_iot_span_writer* w, uint64_t value)
{
  char digits[21];
  size_t n = 0;
  if (value == 0)
  {
    digits[n++] = '0';
  }
  while (value > 0 && n < sizeof(digits))
  {
    digits[n++] = (char)('0' + (value % 10u));
    value /= 10u;
  }
  while (n > 0)
  {
    az_iot_span_writer_append_u8(w, (uint8_t)digits[--n]);
  }
}

az_iot_result az_iot_sas__build_string_to_sign(
    const char* escaped_resource,
    uint64_t expiry_unix_s,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  if (escaped_resource == NULL || out == NULL || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* The newline is part of the signed content, not a separator we are free to
   * choose. */
  az_iot_span_writer w;
  az_iot_span_writer_init(&w, az_span_create((uint8_t*)out, (int32_t)out_size));
  az_iot_span_writer_append_str(&w, escaped_resource);
  az_iot_span_writer_append_u8(&w, (uint8_t)'\n');
  append_u64(&w, expiry_unix_s);
  return az_iot_span_writer_end_str(&w, out_len);
}

az_iot_result az_iot_sas__build_token(
    az_iot_sas_audience audience,
    const char* escaped_resource,
    const uint8_t* signature,
    size_t signature_len,
    uint64_t expiry_unix_s,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  if (escaped_resource == NULL || signature == NULL || signature_len == 0 || out == NULL
      || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* base64 first, then percent-encode the result: '+', '/' and '=' all appear
   * in base64 and all change meaning in a query string. */
  char sig_b64[128];
  az_span b64_out = AZ_SPAN_FROM_BUFFER(sig_b64);
  az_span sig_in = az_span_create((uint8_t*)(uintptr_t)signature, (int32_t)signature_len);
  int32_t b64_len = 0;
  if (az_result_failed(az_base64_encode(b64_out, sig_in, &b64_len)))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  if ((size_t)b64_len >= sizeof(sig_b64))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  sig_b64[b64_len] = '\0';

  az_iot_span_writer w;
  az_iot_span_writer_init(&w, az_span_create((uint8_t*)out, (int32_t)out_size));
  az_iot_span_writer_append_str(&w, SAS_PREFIX);
  az_iot_span_writer_append_str(&w, escaped_resource);
  az_iot_span_writer_append_str(&w, SAS_SIG);
  az_iot_span_writer_append_url_encoded(&w, sig_b64);
  az_iot_span_writer_append_str(&w, SAS_SE);
  append_u64(&w, expiry_unix_s);
  if (audience == AZ_IOT_SAS_AUDIENCE_PROVISIONING)
  {
    az_iot_span_writer_append_str(&w, SAS_SKN);
  }
  return az_iot_span_writer_end_str(&w, out_len);
}

az_iot_result az_iot_sas__mint(
    az_iot_sas_audience audience,
    const char* first,
    const char* second,
    uint64_t expiry_unix_s,
    az_iot_sas_sign_callback sign,
    void* sign_ctx,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  if (sign == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  char resource[256];
  az_iot_result r
      = az_iot_sas__build_resource(audience, first, second, resource, sizeof(resource), NULL);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  char escaped[512];
  r = az_iot_sas__url_encode(resource, strlen(resource), escaped, sizeof(escaped), NULL);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  char to_sign[576];
  size_t to_sign_len = 0;
  r = az_iot_sas__build_string_to_sign(
      escaped, expiry_unix_s, to_sign, sizeof(to_sign), &to_sign_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  uint8_t mac[64];
  size_t mac_len = 0;
  r = sign((const uint8_t*)to_sign, to_sign_len, mac, sizeof(mac), &mac_len, sign_ctx);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("sas: the signing callback failed");
    return r;
  }
  if (mac_len == 0 || mac_len > sizeof(mac))
  {
    /* A callback that reports a length it did not write would otherwise send a
     * token signed with uninitialized stack. */
    AZ_IOT_LOG_ERROR("sas: the signing callback reported an invalid signature length");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  return az_iot_sas__build_token(
      audience, escaped, mac, mac_len, expiry_unix_s, out, out_size, out_len);
}
