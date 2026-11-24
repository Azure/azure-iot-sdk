// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Checks a DPS individual enrollment the service returned. */
#ifndef E2E_DPS_ENROLLMENT_H
#define E2E_DPS_ENROLLMENT_H

#include <stdbool.h>

/**
 * @brief Whether @p enrollment_json is the complete individual enrollment of @p registration_id
 * and assigns @p device_id (NULL: @p registration_id).
 *
 * The assigned device is the top-level deviceId, or @p registration_id when it is absent or null.
 */
bool e2e_dps_enrollment_assigns_device(
    const char* enrollment_json,
    const char* registration_id,
    const char* device_id);

#endif /* E2E_DPS_ENROLLMENT_H */
