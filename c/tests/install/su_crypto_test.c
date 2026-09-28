// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::su_crypto_*: the crypto hooks compute a known SHA-256 digest.
 * AZ_IOT_TEST_HOOKS_HEADER and AZ_IOT_TEST_HOOKS select the adapter. */
#include <stdint.h>
#include <string.h>

#include AZ_IOT_TEST_HOOKS_HEADER

#include "install_test.h"

int main(void)
{
  /* SHA-256("abc"), FIPS 180-2 appendix B.1. */
  static const uint8_t expected[32]
      = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
          0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
          0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
  static const uint8_t message[] = { 'a', 'b', 'c' };

  az_iot_su_crypto_hooks hooks = AZ_IOT_TEST_HOOKS();
  CHECK(hooks.sha256_fn != NULL);
  CHECK(hooks.verify_rs256_fn != NULL);

  uint8_t digest[32] = { 0 };
  CHECK(
      hooks.sha256_fn(message, sizeof(message), digest, hooks.user_ctx)
      == AZ_IOT_SU_RESULT_SUCCESS);
  CHECK(memcmp(digest, expected, sizeof(expected)) == 0);
  return 0;
}
