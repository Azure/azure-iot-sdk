// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file crypto.c
 * @brief One-shot SHA-256 and HMAC-SHA256 built on an az_iot_crypto backend.
 */
#include "internal/crypto.h"

#include <string.h>

/** @brief SHA-256 block size, in bytes. */
#define SHA256_BLOCK_SIZE 64u

void az_iot_crypto__wipe(void* p, size_t len)
{
  volatile uint8_t* v = (volatile uint8_t*)p;
  while (len-- > 0)
  {
    *v++ = 0;
  }
}

AZ_NODISCARD az_iot_result az_iot_crypto__validate(const az_iot_crypto* crypto)
{
  if (crypto == NULL || crypto->version != AZ_IOT_CRYPTO_VERSION || crypto->sha256_init == NULL
      || crypto->sha256_update == NULL || crypto->sha256_final == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return AZ_IOT_OK;
}

/** @brief Hashes the concatenation of up to two buffers. */
static az_iot_result sha256_2(
    const az_iot_crypto* crypto,
    const uint8_t* a,
    size_t a_len,
    const uint8_t* b,
    size_t b_len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  az_iot_sha256_ctx ctx;
  az_iot_result r = crypto->sha256_init(crypto, &ctx);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  r = crypto->sha256_update(crypto, &ctx, a, a_len);
  if (r == AZ_IOT_OK)
  {
    r = crypto->sha256_update(crypto, &ctx, b, b_len);
  }
  /* final() releases the context on every path. */
  az_iot_result fr = crypto->sha256_final(crypto, &ctx, r == AZ_IOT_OK ? out : NULL);
  az_iot_crypto__wipe(&ctx, sizeof(ctx));
  return r != AZ_IOT_OK ? r : fr;
}

AZ_NODISCARD az_iot_result az_iot_crypto__sha256(
    const az_iot_crypto* crypto,
    const uint8_t* data,
    size_t data_len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  if (crypto == NULL || out == NULL || (data == NULL && data_len != 0))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return sha256_2(crypto, data, data_len, NULL, 0, out);
}

AZ_NODISCARD az_iot_result az_iot_crypto__hmac_sha256(
    const az_iot_crypto* crypto,
    const uint8_t* key,
    size_t key_len,
    const uint8_t* data,
    size_t data_len,
    uint8_t out[AZ_IOT_SHA256_SIZE])
{
  if (crypto == NULL || out == NULL || (key == NULL && key_len != 0)
      || (data == NULL && data_len != 0))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* K0: the key, hashed first if longer than a block, zero-padded. */
  uint8_t k0[SHA256_BLOCK_SIZE];
  memset(k0, 0, sizeof(k0));
  az_iot_result r = AZ_IOT_OK;
  if (key_len > SHA256_BLOCK_SIZE)
  {
    r = sha256_2(crypto, key, key_len, NULL, 0, k0);
  }
  else if (key_len > 0)
  {
    memcpy(k0, key, key_len);
  }

  uint8_t pad[SHA256_BLOCK_SIZE];
  uint8_t inner[AZ_IOT_SHA256_SIZE];
  if (r == AZ_IOT_OK)
  {
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; ++i)
    {
      pad[i] = (uint8_t)(k0[i] ^ 0x36u);
    }
    r = sha256_2(crypto, pad, sizeof(pad), data, data_len, inner);
  }
  if (r == AZ_IOT_OK)
  {
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; ++i)
    {
      pad[i] = (uint8_t)(k0[i] ^ 0x5cu);
    }
    r = sha256_2(crypto, pad, sizeof(pad), inner, sizeof(inner), out);
  }

  az_iot_crypto__wipe(k0, sizeof(k0));
  az_iot_crypto__wipe(pad, sizeof(pad));
  az_iot_crypto__wipe(inner, sizeof(inner));
  return r;
}
