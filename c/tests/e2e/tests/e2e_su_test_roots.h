// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file e2e_su_test_roots.h
 * @brief Device Update test root keys for the software updates e2e suite.
 */

#ifndef E2E_SU_TEST_ROOTS_H
#define E2E_SU_TEST_ROOTS_H

#include <stddef.h>

#include "azure/iot/az_iot_su.h"

/**
 * @brief Returns the test root keys (static storage).
 *
 * @param[out] out_count Number of keys returned.
 * @return The keys.
 */
const az_iot_su_root_key* e2e_su_test_roots(size_t* out_count);

#endif /* E2E_SU_TEST_ROOTS_H */
