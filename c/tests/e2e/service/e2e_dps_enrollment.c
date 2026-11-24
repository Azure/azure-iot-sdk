// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_dps_enrollment.h"

#include "e2e_json.h"

#include <string.h>

bool e2e_dps_enrollment_assigns_device(
    const char* enrollment_json,
    const char* registration_id,
    const char* device_id)
{
  static const char* const registration_id_path[] = { "registrationId", NULL };
  static const char* const device_id_path[] = { "deviceId", NULL };
  char value[256];
  return e2e_json_read_string(enrollment_json, registration_id_path, value, sizeof(value))
      && strcmp(value, registration_id) == 0
      && e2e_json_read_string_or_default(
             enrollment_json, device_id_path, registration_id, value, sizeof(value))
      && strcmp(value, device_id != NULL ? device_id : registration_id) == 0;
}
