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

/** @brief Base64 length of a SHA-256 MAC: 4 * ceil(32 / 3). */
#define SIG_BASE64_LEN 44u
/** @brief Longest decimal uint64_t. */
#define U64_DEC_MAX 20u

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
  size_t n = key_base64 != NULL ? strlen(key_base64) : 0;
  if (n == 0 || cap == 0 || cap > (size_t)INT32_MAX || n > 4u * ((cap + 2u) / 3u))
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

/** @brief Whether @p c is outside the URL-unreserved set. */
static bool needs_encoding(char c)
{
  return !(
      (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'
      || c == '_' || c == '.' || c == '~');
}

/**
 * @brief Appends @p n bytes of @p src at @p *pos, URL-encoding them when
 * @p encode is set. Leaves room for a terminator.
 */
static bool append(char* out, size_t cap, size_t* pos, const char* src, size_t n, bool encode)
{
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < n; ++i)
  {
    char c = src[i];
    if (encode && needs_encoding(c))
    {
      if (cap - *pos < 4u)
      {
        return false;
      }
      out[(*pos)++] = '%';
      out[(*pos)++] = hex[((uint8_t)c) >> 4];
      out[(*pos)++] = hex[((uint8_t)c) & 0x0Fu];
    }
    else
    {
      if (cap - *pos < 2u)
      {
        return false;
      }
      out[(*pos)++] = c;
    }
  }
  return true;
}

AZ_NODISCARD az_iot_result
az_iot_sas__resource_uri(bool is_dps, const char* first, const char* second, char* out, size_t cap)
{
  if (first == NULL || second == NULL || first[0] == '\0' || second[0] == '\0' || cap == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* The infixes are the services' own; DPS's is lowercase. */
  const char* infix = is_dps ? "%2fregistrations%2f" : "%2Fdevices%2F";
  size_t pos = 0;
  if (!append(out, cap, &pos, first, strlen(first), true)
      || !append(out, cap, &pos, infix, strlen(infix), false)
      || !append(out, cap, &pos, second, strlen(second), true))
  {
    out[0] = '\0';
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  out[pos] = '\0';
  return AZ_IOT_OK;
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
  if (cap == 0)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  char expiry_text[U64_DEC_MAX];
  az_span expiry_rest = AZ_SPAN_EMPTY;
  if (az_result_failed(az_span_u64toa(AZ_SPAN_FROM_BUFFER(expiry_text), expiry, &expiry_rest)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  size_t expiry_len = sizeof(expiry_text) - (size_t)az_span_size(expiry_rest);

  /* `<resource_uri>\n<expiry>` is built in @p out, then overwritten. */
  size_t pos = 0;
  size_t uri_len = strlen(resource_uri);
  if (!append(out, cap, &pos, resource_uri, uri_len, false)
      || !append(out, cap, &pos, "\n", 1, false)
      || !append(out, cap, &pos, expiry_text, expiry_len, false))
  {
    out[0] = '\0';
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  az_iot_result r = az_iot_crypto__hmac_sha256(crypto, key, key_len, (const uint8_t*)out, pos, mac);
  if (r != AZ_IOT_OK)
  {
    az_iot_sas__wipe(mac, sizeof(mac));
    az_iot_sas__wipe(out, cap);
    return r;
  }
  char sig[SIG_BASE64_LEN];
  int32_t sig_len = 0;
  az_result br = az_base64_encode(
      az_span_create((uint8_t*)sig, (int32_t)sizeof(sig)),
      az_span_create(mac, (int32_t)sizeof(mac)),
      &sig_len);
  az_iot_sas__wipe(mac, sizeof(mac));
  if (az_result_failed(br))
  {
    az_iot_sas__wipe(out, cap);
    return AZ_IOT_ERR_INTERNAL;
  }

  static const char k_prefix[] = "SharedAccessSignature sr=";
  pos = 0;
  bool ok = append(out, cap, &pos, k_prefix, sizeof(k_prefix) - 1u, false)
      && append(out, cap, &pos, resource_uri, uri_len, false)
      && append(out, cap, &pos, "&sig=", 5, false)
      && append(out, cap, &pos, sig, (size_t)sig_len, true)
      && append(out, cap, &pos, "&se=", 4, false)
      && append(out, cap, &pos, expiry_text, expiry_len, false);
  if (ok && key_name != NULL && key_name[0] != '\0')
  {
    ok = append(out, cap, &pos, "&skn=", 5, false)
        && append(out, cap, &pos, key_name, strlen(key_name), false);
  }
  az_iot_sas__wipe(sig, sizeof(sig));
  if (!ok)
  {
    az_iot_sas__wipe(out, cap);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  out[pos] = '\0';
  return AZ_IOT_OK;
}
