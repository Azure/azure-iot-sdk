// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_sas.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <string.h>
#include <stdio.h>

/* --- base64 (via OpenSSL libcrypto, which we already link) ---------------- */

/* Decode a NUL-terminated base64 string into @p out. Returns the number of raw
 * bytes written, or -1 on error. */
static int e2e_base64_decode(const char* in, uint8_t* out, int out_cap)
{
  int in_len = (int)strlen(in);
  if (in_len == 0 || (in_len % 4) != 0)
  {
    return -1;
  }
  /* EVP_DecodeBlock writes 3 bytes for every 4 input chars; padding '=' are
   * decoded as trailing zero bytes that we then trim. */
  if ((in_len / 4) * 3 > out_cap)
  {
    return -1;
  }
  int decoded = EVP_DecodeBlock(out, (const unsigned char*)in, in_len);
  if (decoded < 0)
  {
    return -1;
  }
  if (in[in_len - 1] == '=')
  {
    decoded--;
  }
  if (in[in_len - 2] == '=')
  {
    decoded--;
  }
  return decoded;
}

/* Encode @p in_len raw bytes into a NUL-terminated base64 string in @p out.
 * @p out_cap must be at least 4*ceil(in_len/3)+1. Returns true on success. */
static bool e2e_base64_encode(const uint8_t* in, int in_len, char* out, int out_cap)
{
  int needed = 4 * ((in_len + 2) / 3) + 1;
  if (out_cap < needed)
  {
    return false;
  }
  (void)EVP_EncodeBlock((unsigned char*)out, in, in_len); /* NUL-terminates */
  return true;
}

/* --- HMAC-SHA256 (via OpenSSL EVP_MAC, the non-deprecated 3.0+ API) -------- */

static bool e2e_hmac_sha256(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data,
    size_t data_len,
    uint8_t out[32])
{
  EVP_MAC* mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
  if (mac == NULL)
  {
    return false;
  }
  EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
  EVP_MAC_free(mac);
  if (ctx == NULL)
  {
    return false;
  }

  char digest[] = "SHA256";
  OSSL_PARAM params[2];
  params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0);
  params[1] = OSSL_PARAM_construct_end();

  bool ok = false;
  size_t out_len = 0;
  if (EVP_MAC_init(ctx, key, key_len, params) == 1 && EVP_MAC_update(ctx, data, data_len) == 1
      && EVP_MAC_final(ctx, out, &out_len, 32) == 1 && out_len == 32)
  {
    ok = true;
  }
  EVP_MAC_CTX_free(ctx);
  return ok;
}

/* --- percent-encoding ----------------------------------------------------- */

size_t e2e_url_encode(const char* src, char* dst, size_t dst_size)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)src; *p != '\0'; p++)
  {
    unsigned char c = *p;
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved)
    {
      if (o + 1 >= dst_size)
      {
        break;
      }
      dst[o++] = (char)c;
    }
    else
    {
      if (o + 3 >= dst_size)
      {
        break;
      }
      dst[o++] = '%';
      dst[o++] = hex[c >> 4];
      dst[o++] = hex[c & 0x0F];
    }
  }
  if (dst_size > 0)
  {
    dst[(o < dst_size) ? o : (dst_size - 1)] = '\0';
  }
  return o;
}

/* --- connection-string parsing -------------------------------------------- */

/* Copy @p len bytes of @p value into @p dst (capacity @p cap), NUL-terminated. */
static void e2e_copy_bounded(char* dst, size_t cap, const char* value, size_t len)
{
  if (len >= cap)
  {
    len = cap - 1;
  }
  memcpy(dst, value, len);
  dst[len] = '\0';
}

bool e2e_conn_info_parse(const char* connection_string, e2e_conn_info* out)
{
  if (connection_string == NULL || out == NULL)
  {
    return false;
  }
  memset(out, 0, sizeof(*out));

  const char* seg = connection_string;
  while (*seg != '\0')
  {
    const char* seg_end = strchr(seg, ';');
    size_t seg_len = (seg_end != NULL) ? (size_t)(seg_end - seg) : strlen(seg);

    const char* eq = memchr(seg, '=', seg_len);
    if (eq != NULL)
    {
      size_t key_len = (size_t)(eq - seg);
      const char* val = eq + 1;
      size_t val_len = seg_len - key_len - 1;

      if (key_len == 8 && strncmp(seg, "HostName", 8) == 0)
      {
        e2e_copy_bounded(out->host, sizeof(out->host), val, val_len);
      }
      else if (key_len == 8 && strncmp(seg, "Endpoint", 8) == 0)
      {
        /* Endpoint=sb://<host>/ -> strip scheme and trailing slash. */
        if (val_len >= 5 && strncmp(val, "sb://", 5) == 0)
        {
          val += 5;
          val_len -= 5;
        }
        while (val_len > 0 && val[val_len - 1] == '/')
        {
          val_len--;
        }
        e2e_copy_bounded(out->host, sizeof(out->host), val, val_len);
      }
      else if (key_len == 19 && strncmp(seg, "SharedAccessKeyName", 19) == 0)
      {
        e2e_copy_bounded(out->key_name, sizeof(out->key_name), val, val_len);
      }
      else if (key_len == 15 && strncmp(seg, "SharedAccessKey", 15) == 0)
      {
        e2e_copy_bounded(out->key, sizeof(out->key), val, val_len);
      }
      else if (key_len == 10 && strncmp(seg, "EntityPath", 10) == 0)
      {
        e2e_copy_bounded(out->entity_path, sizeof(out->entity_path), val, val_len);
      }
    }

    if (seg_end == NULL)
    {
      break;
    }
    seg = seg_end + 1;
  }

  return out->host[0] != '\0' && out->key[0] != '\0';
}

/* --- SAS token ------------------------------------------------------------ */

bool e2e_sas_token_create(
    const char* resource_uri,
    const char* key_name,
    const char* key,
    bool key_base64_decode,
    int64_t expiry_unix_sec,
    char* out,
    size_t out_size)
{
  if (resource_uri == NULL || key_name == NULL || key == NULL || out == NULL)
  {
    return false;
  }

  char sr[512];
  e2e_url_encode(resource_uri, sr, sizeof(sr));

  char to_sign[640];
  int n = snprintf(to_sign, sizeof(to_sign), "%s\n%lld", sr, (long long)expiry_unix_sec);
  if (n < 0 || n >= (int)sizeof(to_sign))
  {
    return false;
  }

  /* Derive the HMAC key. */
  const uint8_t* hmac_key;
  size_t hmac_key_len;
  uint8_t decoded_key[256];
  if (key_base64_decode)
  {
    int decoded_len = e2e_base64_decode(key, decoded_key, (int)sizeof(decoded_key));
    if (decoded_len < 0)
    {
      return false;
    }
    hmac_key = decoded_key;
    hmac_key_len = (size_t)decoded_len;
  }
  else
  {
    hmac_key = (const uint8_t*)key;
    hmac_key_len = strlen(key);
  }

  uint8_t mac[32];
  if (!e2e_hmac_sha256(hmac_key, hmac_key_len, (const uint8_t*)to_sign, strlen(to_sign), mac))
  {
    return false;
  }

  char sig_b64[64];
  if (!e2e_base64_encode(mac, (int)sizeof(mac), sig_b64, (int)sizeof(sig_b64)))
  {
    return false;
  }

  char sig_enc[128];
  e2e_url_encode(sig_b64, sig_enc, sizeof(sig_enc));

  int m = snprintf(
      out,
      out_size,
      "SharedAccessSignature sr=%s&sig=%s&se=%lld&skn=%s",
      sr,
      sig_enc,
      (long long)expiry_unix_sec,
      key_name);
  return m > 0 && m < (int)out_size;
}
