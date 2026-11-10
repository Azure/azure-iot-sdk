// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Reads values out of the JSON the service returns (direct method responses, twins). */
#ifndef E2E_JSON_H
#define E2E_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Reads the unsigned integer at @p path (member names, outermost first, NULL-terminated)
 * in the JSON object @p json.
 *
 * @return false when there is none there, or it is not an unsigned integer.
 */
bool e2e_json_read_uint64(const char* json, const char* const* path, uint64_t* out);

/**
 * @brief Reads the string at @p path (as e2e_json_read_uint64()), unescaped, into @p out.
 *
 * @return false when there is none there, it is not a string, or it does not fit @p size.
 */
bool e2e_json_read_string(const char* json, const char* const* path, char* out, size_t size);

#endif /* E2E_JSON_H */
