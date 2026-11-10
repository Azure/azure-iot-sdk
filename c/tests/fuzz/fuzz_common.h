// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.
/* SPDX-License-Identifier: MIT */

/**
 * @file fuzz_common.h
 * @brief Helpers shared by the fuzz targets.
 */

#ifndef AZ_IOT_FUZZ_COMMON_H
#define AZ_IOT_FUZZ_COMMON_H

#include <stdlib.h>

#include "azure/core/az_precondition.h"

/** @brief Precondition failure handler that aborts, so the fuzzer reports a crash. */
static void fuzz_on_precondition_failed(void) { abort(); }

/**
 * @brief Make azure-sdk-for-c precondition failures abort. Its default handler spins forever,
 * which a fuzzer would report as a timeout without a stack.
 */
static inline void fuzz_abort_on_precondition_failure(void)
{
  az_precondition_failed_set_callback(fuzz_on_precondition_failed);
}

#endif /* AZ_IOT_FUZZ_COMMON_H */
