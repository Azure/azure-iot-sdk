// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file sas.c
 * @brief SAS token construction, matching the formats DPS and IoT Hub verify.
 */
#include "internal/sas.h"

#include <string.h>

#include <azure/core/az_base64.h>
#include <azure/core/az_span.h>

#include "internal/crypto.h"
#include "internal/span_writer.h"

/** @brief Base64 length of a SHA-256 MAC: 4 * ceil(32 / 3). */
#define SIG_BASE64_LEN 44u

void az_iot_sas__wipe(void* p, size_t len)
{
  volatile uint8_t* v = (volatile uint8_t*)p;
  while (len-- > 0)
  {
    *v++ = 0;
  }
}

AZ_NODISCARD az_iot_result
az_iot_sas__decode_key(const char* key_base64, uint8_t* out, size_t cap, size_t* out_len)
{
  *out_len = 0;
  if (out == NULL || cap == 0 || cap > (size_t)INT32_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_sas__wipe(out, cap);
  size_t n = key_base64 != NULL ? strlen(key_base64) : 0;
  if (n == 0 || n > 4u * ((cap + 2u) / 3u))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  int32_t written = 0;
  if (az_result_failed(az_base64_decode(
          az_span_create(out, (int32_t)cap),
          az_span_create((uint8_t*)(uintptr_t)key_base64, (int32_t)n),
          &written))
      || written <= 0)
  {
    az_iot_sas__wipe(out, cap);
    return AZ_IOT_ERR_INVALID_ARG;
  }
  *out_len = (size_t)written;
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_sas__derive_device_key(
    const az_iot_crypto* crypto,
    const uint8_t* group_key,
    size_t group_key_len,
    const char* id,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  return az_iot_crypto__hmac_sha256(
      crypto, group_key, group_key_len, (const uint8_t*)id, strlen(id), out);
}

/** @brief A span over @p cap bytes of @p out; empty when @p cap exceeds INT32_MAX. */
static az_span out_span(char* out, size_t cap)
{
  return cap <= (size_t)INT32_MAX ? az_span_create((uint8_t*)out, (int32_t)cap) : AZ_SPAN_EMPTY;
}

AZ_NODISCARD az_iot_result
az_iot_sas__resource_uri(bool is_dps, const char* first, const char* second, char* out, size_t cap)
{
  if (first == NULL || second == NULL || first[0] == '\0' || second[0] == '\0' || out == NULL
      || cap == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_span_writer w;
  az_iot_span_writer_init(&w, out_span(out, cap));
  az_iot_span_writer_append_url_encoded(&w, first);
  /* The infixes are the services' own; DPS's is lowercase. */
  az_iot_span_writer_append_str(&w, is_dps ? "%2fregistrations%2f" : "%2Fdevices%2F");
  az_iot_span_writer_append_url_encoded(&w, second);
  return az_iot_span_writer_end_str(&w, NULL);
}

AZ_NODISCARD az_iot_result az_iot_sas__build_token(
    const az_iot_crypto* crypto,
    const uint8_t* key,
    size_t key_len,
    const char* resource_uri,
    const char* key_name,
    uint64_t expiry,
    char* out,
    size_t cap)
{
  if (out == NULL || cap == 0)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  /* `<resource_uri>\n<expiry>` is built in @p out, then overwritten. */
  az_iot_span_writer w;
  az_iot_span_writer_init(&w, out_span(out, cap));
  az_iot_span_writer_append_str(&w, resource_uri);
  az_iot_span_writer_append_str(&w, "\n");
  az_iot_span_writer_append_u64(&w, expiry);
  size_t to_sign_len = 0;
  az_iot_result r = az_iot_span_writer_end_str(&w, &to_sign_len);
  if (r != AZ_IOT_OK)
  {
    az_iot_sas__wipe(out, cap);
    return r;
  }

  uint8_t mac[AZ_IOT_SHA256_SIZE];
  r = az_iot_crypto__hmac_sha256(crypto, key, key_len, (const uint8_t*)out, to_sign_len, mac);
  if (r != AZ_IOT_OK)
  {
    az_iot_sas__wipe(mac, sizeof(mac));
    az_iot_sas__wipe(out, cap);
    return r;
  }
  char sig[SIG_BASE64_LEN + 1u];
  int32_t sig_len = 0;
  az_result br = az_base64_encode(
      az_span_create((uint8_t*)sig, (int32_t)SIG_BASE64_LEN),
      az_span_create(mac, (int32_t)sizeof(mac)),
      &sig_len);
  az_iot_sas__wipe(mac, sizeof(mac));
  if (az_result_failed(br))
  {
    az_iot_sas__wipe(sig, sizeof(sig));
    az_iot_sas__wipe(out, cap);
    return AZ_IOT_ERR_INTERNAL;
  }
  sig[sig_len] = '\0';

  az_iot_span_writer_init(&w, out_span(out, cap));
  az_iot_span_writer_append_str(&w, "SharedAccessSignature sr=");
  az_iot_span_writer_append_str(&w, resource_uri);
  az_iot_span_writer_append_str(&w, "&sig=");
  az_iot_span_writer_append_url_encoded(&w, sig);
  az_iot_span_writer_append_str(&w, "&se=");
  az_iot_span_writer_append_u64(&w, expiry);
  if (key_name != NULL && key_name[0] != '\0')
  {
    az_iot_span_writer_append_str(&w, "&skn=");
    az_iot_span_writer_append_str(&w, key_name);
  }
  az_iot_sas__wipe(sig, sizeof(sig));
  r = az_iot_span_writer_end_str(&w, NULL);
  if (r != AZ_IOT_OK)
  {
    az_iot_sas__wipe(out, cap);
  }
  return r;
}
