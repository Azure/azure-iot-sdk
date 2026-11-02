// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_su_delta_internal.h
 * @brief Helpers of the `microsoft/delta:1` download handler, exposed for its unit tests.
 */
#ifndef AZ_IOT_SU_DELTA_INTERNAL_H
#define AZ_IOT_SU_DELTA_INTERNAL_H

#include <stdbool.h>

/**
 * @brief Copy @p source_path to @p target_path through a new `<target_path>.XXXXXX` file,
 * renamed when complete; no existing file is replaced but @p target_path.
 *
 * @return true when copied; false otherwise, e.g. when @p source_path is not a regular file (a
 * link is not followed), with @p source_path kept and the temporary file removed.
 */
bool az_iot_su_delta__copy_file(const char* source_path, const char* target_path);

/**
 * @brief Remove @p path if it still names the file open at @p descriptor, which is not a
 * directory and no handler holds a lock on.
 *
 * @return true when removed.
 */
bool az_iot_su_delta__remove_opened_file_unless_locked(int descriptor, const char* path);

#endif /* AZ_IOT_SU_DELTA_INTERNAL_H */
