// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

#include "azure/iot/az_iot_sas_token.h"

#include <stdbool.h>
#include <string.h>

#include <azure/core/az_base64.h>

#include "internal/crypto.h"
#include "internal/span_writer.h"

/** @brief Base64 of a 32-byte MAC, padding included. */
#define SAS_SIGNATURE_B64_LEN 44

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

/** @brief Zeroes @p out (@p out_size bytes) when there is one. */
static void wipe_out(char* out, size_t out_size)
{
  if (out != NULL)
  {
    az_iot_crypto__wipe(out, out_size);
  }
}

/** @brief A writer over @p out; sizes past INT32_MAX are capped. */
static void writer_over(az_iot_span_writer* writer, char* out, size_t out_size)
{
  int32_t size = out_size > (size_t)INT32_MAX ? INT32_MAX : (int32_t)out_size;
  az_iot_span_writer_init(writer, az_span_create((uint8_t*)out, size));
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
    wipe_out(out, out_size);
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_span_writer writer;
  writer_over(&writer, out, out_size);
  az_iot_span_writer_append_str(&writer, resource_uri);
  az_iot_span_writer_append_str(&writer, "\n");
  az_iot_span_writer_append_u64(&writer, expiry_unix_seconds);
  az_iot_result r = az_iot_span_writer_end_str(&writer, out_len);
  if (r != AZ_IOT_OK)
  {
    wipe_out(out, out_size);
  }
  return r;
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
    wipe_out(out, out_size);
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Fixed sizes: az_base64_encode()'s preconditions hold. */
  uint8_t mac[AZ_IOT_SHA256_SIZE];
  memcpy(mac, signature, sizeof(mac));
  uint8_t b64[SAS_SIGNATURE_B64_LEN + 1];
  int32_t b64_len = 0;
  az_iot_result r = az_result_succeeded(az_base64_encode(
                        az_span_create(b64, SAS_SIGNATURE_B64_LEN),
                        az_span_create(mac, AZ_IOT_SHA256_SIZE),
                        &b64_len))
          && b64_len == SAS_SIGNATURE_B64_LEN
      ? AZ_IOT_OK
      : AZ_IOT_ERR_INTERNAL;
  if (r == AZ_IOT_OK)
  {
    /* `+`, `/` and `=` are percent-encoded, as by the service SDKs. */
    b64[SAS_SIGNATURE_B64_LEN] = '\0';
    az_iot_span_writer writer;
    writer_over(&writer, out, out_size);
    az_iot_span_writer_append_str(&writer, "SharedAccessSignature sr=");
    az_iot_span_writer_append_str(&writer, resource_uri);
    az_iot_span_writer_append_str(&writer, "&sig=");
    az_iot_span_writer_append_url_encoded(&writer, (const char*)b64);
    az_iot_span_writer_append_str(&writer, "&se=");
    az_iot_span_writer_append_u64(&writer, expiry_unix_seconds);
    if (key_name != NULL && key_name[0] != '\0')
    {
      az_iot_span_writer_append_str(&writer, "&skn=");
      az_iot_span_writer_append_str(&writer, key_name);
    }
    r = az_iot_span_writer_end_str(&writer, out_len);
  }
  az_iot_crypto__wipe(mac, sizeof(mac));
  az_iot_crypto__wipe(b64, sizeof(b64));
  if (r != AZ_IOT_OK)
  {
    wipe_out(out, out_size);
  }
  return r;
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
    wipe_out(out, out_size);
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
  else
  {
    wipe_out(out, out_size);
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
