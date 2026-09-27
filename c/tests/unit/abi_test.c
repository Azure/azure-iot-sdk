// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for az_iot_abi.h: field presence on size-stamped structs, and the
 * profile/fingerprint the application sees matching the library's. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include "azure/iot/az_iot_abi.h"

typedef struct stamped
{
  uint32_t _internal_size;
  uint8_t first;
  uint64_t last;
} stamped;

static void struct_has_field_accepts_a_full_stamp(void** state)
{
  (void)state;
  stamped s = { ._internal_size = sizeof(stamped) };
  assert_true(AZ_IOT_STRUCT_HAS_FIELD(&s, stamped, first));
  assert_true(AZ_IOT_STRUCT_HAS_FIELD(&s, stamped, last));
}

static void struct_has_field_rejects_fields_past_an_older_stamp(void** state)
{
  (void)state;
  stamped s = { ._internal_size = (uint32_t)offsetof(stamped, last) };
  assert_true(AZ_IOT_STRUCT_HAS_FIELD(&s, stamped, first));
  assert_false(AZ_IOT_STRUCT_HAS_FIELD(&s, stamped, last));

  s._internal_size = (uint32_t)(offsetof(stamped, last) + sizeof(s.last) - 1u);
  assert_false(AZ_IOT_STRUCT_HAS_FIELD(&s, stamped, last));
}

static void struct_has_field_rejects_a_zero_stamp(void** state)
{
  (void)state;
  stamped s = { 0 };
  assert_false(AZ_IOT_STRUCT_HAS_FIELD(&s, stamped, first));
}

static void fingerprint_matches_the_library(void** state)
{
  (void)state;
  assert_int_equal(az_iot_abi_fingerprint(), AZ_IOT_ABI_FINGERPRINT);
}

static void fingerprint_distinguishes_profiles(void** state)
{
  (void)state;
  uint32_t const other = AZ_IOT_ABI_MIX(
      AZ_IOT_ABI_MIX(AZ_IOT_ABI_MIX(2166136261u, AZ_IOT_ABI_VERSION), !AZ_IOT_ABI_SHARED),
      sizeof(void*));
  assert_int_not_equal(other, AZ_IOT_ABI_FINGERPRINT);
}

static void reserves_follow_the_profile(void** state)
{
  (void)state;
  size_t const reserves[] = {
    AZ_IOT_CONNECTION_CLIENT_RESERVE,           AZ_IOT_SU_CLIENT_RESERVE,
    AZ_IOT_MQTTV5_DIRECT_METHOD_CLIENT_RESERVE, AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_RESERVE,
    AZ_IOT_MQTTV5_TWIN_CLIENT_RESERVE,          AZ_IOT_MQTTV3_TWIN_CLIENT_RESERVE,
    AZ_IOT_MQTTV3_FILE_UPLOAD_CLIENT_RESERVE,   AZ_IOT_MQTTV3_TELEMETRY_CLIENT_RESERVE,
    AZ_IOT_MQTTV5_TELEMETRY_CLIENT_RESERVE,     AZ_IOT_MQTTV3_C2D_CLIENT_RESERVE,
    AZ_IOT_CERTIFICATE_PROVIDER_PEM_RESERVE,
  };
  for (size_t i = 0; i < sizeof(reserves) / sizeof(reserves[0]); i++)
  {
#if AZ_IOT_ABI_SHARED
    assert_true(reserves[i] > 0u);
    assert_int_equal(reserves[i] % 8u, 0u);
#else
    assert_int_equal(reserves[i], 0u);
#endif
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(struct_has_field_accepts_a_full_stamp),
    cmocka_unit_test(struct_has_field_rejects_fields_past_an_older_stamp),
    cmocka_unit_test(struct_has_field_rejects_a_zero_stamp),
    cmocka_unit_test(fingerprint_matches_the_library),
    cmocka_unit_test(fingerprint_distinguishes_profiles),
    cmocka_unit_test(reserves_follow_the_profile),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
