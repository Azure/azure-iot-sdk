// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::crypto_*: the backend computes a known SHA-256 digest.
 * AZ_IOT_TEST_CRYPTO_HEADER and AZ_IOT_TEST_CRYPTO select the backend. */
#include <stdint.h>
#include <string.h>

#include AZ_IOT_TEST_CRYPTO_HEADER

#include "install_test.h"

int main(void)
{
  /* SHA-256("abc"), FIPS 180-2 appendix B.1. */
  static const uint8_t expected[32]
      = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
          0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
          0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
  static const uint8_t message[] = { 'a', 'b', 'c' };

  const az_iot_crypto* crypto = AZ_IOT_TEST_CRYPTO();
  CHECK(crypto != NULL);
  CHECK(crypto->version == AZ_IOT_CRYPTO_VERSION);
  CHECK(crypto->verify_rs256 != NULL);

  az_iot_sha256_ctx ctx;
  uint8_t digest[AZ_IOT_SHA256_SIZE] = { 0 };
  CHECK(crypto->sha256_init(crypto, &ctx) == AZ_IOT_OK);
  CHECK(crypto->sha256_update(crypto, &ctx, message, sizeof(message)) == AZ_IOT_OK);
  CHECK(crypto->sha256_final(crypto, &ctx, digest) == AZ_IOT_OK);
  CHECK(memcmp(digest, expected, sizeof(expected)) == 0);
  return 0;
}
