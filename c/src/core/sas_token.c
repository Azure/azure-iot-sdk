// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

#include "azure/iot/az_iot_sas_token.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "internal/crypto.h"

/** @brief Base64 of a 32-byte MAC, padding included. */
#define SAS_SIGNATURE_B64_LEN 44u

/** @brief Whether @p s is a URL-encoded value: unreserved characters and `%`. */
static bool is_encoded_value(const char* s)
{
  for (; *s != '\0'; ++s)
  {
    char ch = *s;
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
          || ch == '-' || ch == '.' || ch == '_' || ch == '~' || ch == '%'))
    {
      return false;
    }
  }
  return true;
}

/** @brief Appends @p n bytes of @p src at @p *pos; false if @p out_size is exceeded,
 * keeping room for the terminator. */
static bool put(char* out, size_t out_size, size_t* pos, const char* src, size_t n)
{
  if (n >= out_size - *pos)
  {
    return false;
  }
  memcpy(out + *pos, src, n);
  *pos += n;
  return true;
}

/** @brief Writes @p expiry in decimal to @p text; returns its length. */
static size_t format_expiry(uint64_t expiry, char text[21])
{
  char digits[20];
  size_t n = 0;
  do
  {
    digits[n++] = (char)('0' + (expiry % 10u));
    expiry /= 10u;
  } while (expiry != 0u);
  for (size_t i = 0; i < n; ++i)
  {
    text[i] = digits[n - 1u - i];
  }
  text[n] = '\0';
  return n;
}

/** @brief Base64 of @p mac into @p b64 (SAS_SIGNATURE_B64_LEN characters). */
static void base64_mac(const uint8_t mac[AZ_IOT_SHA256_SIZE], char b64[SAS_SIGNATURE_B64_LEN])
{
  static const char k_alphabet[]
      = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="; /* [64]: padding */
  size_t o = 0;
  for (size_t i = 0; i < AZ_IOT_SHA256_SIZE; i += 3u)
  {
    uint32_t v = (uint32_t)mac[i] << 16;
    size_t left = AZ_IOT_SHA256_SIZE - i;
    v |= left > 1u ? (uint32_t)mac[i + 1u] << 8 : 0u;
    v |= left > 2u ? (uint32_t)mac[i + 2u] : 0u;
    b64[o++] = k_alphabet[(v >> 18) & 0x3Fu];
    b64[o++] = k_alphabet[(v >> 12) & 0x3Fu];
    size_t third = left > 1u ? (size_t)((v >> 6) & 0x3Fu) : 64u;
    size_t fourth = left > 2u ? (size_t)(v & 0x3Fu) : 64u;
    b64[o++] = k_alphabet[third];
    b64[o++] = k_alphabet[fourth];
  }
}

AZ_NODISCARD az_iot_result az_iot_sas_token_string_to_sign(
    const char* resource_uri,
    uint64_t expiry_unix_seconds,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  if (resource_uri == NULL || resource_uri[0] == '\0' || !is_encoded_value(resource_uri)
      || expiry_unix_seconds == 0u || out == NULL || out_len == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  char expiry[21];
  size_t expiry_len = format_expiry(expiry_unix_seconds, expiry);
  size_t pos = 0;
  if (out_size == 0u || !put(out, out_size, &pos, resource_uri, strlen(resource_uri))
      || !put(out, out_size, &pos, "\n", 1u) || !put(out, out_size, &pos, expiry, expiry_len))
  {
    if (out_size != 0u)
    {
      out[0] = '\0';
    }
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  out[pos] = '\0';
  *out_len = pos;
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_sas_token_from_signature(
    const char* resource_uri,
    const char* key_name,
    uint64_t expiry_unix_seconds,
    const uint8_t signature[AZ_IOT_SHA256_SIZE],
    char* out,
    size_t out_size,
    size_t* out_len)
{
  if (resource_uri == NULL || resource_uri[0] == '\0' || !is_encoded_value(resource_uri)
      || (key_name != NULL && !is_encoded_value(key_name)) || expiry_unix_seconds == 0u
      || signature == NULL || out == NULL || out_len == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  char b64[SAS_SIGNATURE_B64_LEN];
  base64_mac(signature, b64);
  char expiry[21];
  size_t expiry_len = format_expiry(expiry_unix_seconds, expiry);
  static const char k_prefix[] = "SharedAccessSignature sr=";
  size_t pos = 0;
  bool ok = out_size != 0u && put(out, out_size, &pos, k_prefix, sizeof(k_prefix) - 1u)
      && put(out, out_size, &pos, resource_uri, strlen(resource_uri))
      && put(out, out_size, &pos, "&sig=", 5u);
  for (size_t i = 0; ok && i < SAS_SIGNATURE_B64_LEN; ++i)
  {
    const char* enc = b64[i] == '+' ? "%2B" : b64[i] == '/' ? "%2F" : b64[i] == '=' ? "%3D" : NULL;
    ok = enc != NULL ? put(out, out_size, &pos, enc, 3u) : put(out, out_size, &pos, &b64[i], 1u);
  }
  ok = ok && put(out, out_size, &pos, "&se=", 4u) && put(out, out_size, &pos, expiry, expiry_len);
  if (ok && key_name != NULL && key_name[0] != '\0')
  {
    ok = put(out, out_size, &pos, "&skn=", 5u)
        && put(out, out_size, &pos, key_name, strlen(key_name));
  }
  az_iot_crypto__wipe(b64, sizeof(b64));
  if (!ok)
  {
    az_iot_crypto__wipe(out, out_size);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  out[pos] = '\0';
  *out_len = pos;
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_sas_token_sign(
    const az_iot_crypto* crypto,
    const uint8_t* key,
    size_t key_len,
    const char* resource_uri,
    const char* key_name,
    uint64_t expiry_unix_seconds,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  if (key == NULL || key_len == 0u || az_iot_crypto__validate(crypto) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* The string to sign is built in @p out, then replaced by the token. */
  size_t to_sign_len = 0;
  az_iot_result r = az_iot_sas_token_string_to_sign(
      resource_uri, expiry_unix_seconds, out, out_size, &to_sign_len);
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  if (r == AZ_IOT_OK)
  {
    r = az_iot_crypto__hmac_sha256(crypto, key, key_len, (const uint8_t*)out, to_sign_len, mac);
  }
  if (r == AZ_IOT_OK)
  {
    r = az_iot_sas_token_from_signature(
        resource_uri, key_name, expiry_unix_seconds, mac, out, out_size, out_len);
  }
  else if (out != NULL && out_size != 0u)
  {
    az_iot_crypto__wipe(out, out_size);
  }
  az_iot_crypto__wipe(mac, sizeof(mac));
  return r;
}

AZ_NODISCARD az_iot_result az_iot_sas_derive_device_key(
    const az_iot_crypto* crypto,
    const uint8_t* group_key,
    size_t group_key_len,
    const char* id,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  if (out == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_result r = group_key == NULL || group_key_len == 0u || id == NULL || id[0] == '\0'
          || az_iot_crypto__validate(crypto) != AZ_IOT_OK
      ? AZ_IOT_ERR_INVALID_ARG
      : az_iot_crypto__hmac_sha256(
            crypto, group_key, group_key_len, (const uint8_t*)id, strlen(id), out);
  if (r != AZ_IOT_OK)
  {
    az_iot_crypto__wipe(out, AZ_IOT_SHA256_SIZE);
  }
  return r;
}
