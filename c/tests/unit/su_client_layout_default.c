// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* Built with the SDK's default capacities, unlike the mismatch tests that link it. */

#include <stddef.h>

#include "azure/iot/az_iot_su.h"

size_t az_iot_test_su_default_client_size(void);

/**
 * @brief sizeof(az_iot_su_client) with default capacities.
 */
size_t az_iot_test_su_default_client_size(void) { return sizeof(az_iot_su_client); }
