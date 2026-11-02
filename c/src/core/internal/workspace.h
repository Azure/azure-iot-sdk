// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file workspace.h
 * @brief Internal: lays out a client's entries in a workspace the application provides.
 *
 * The entries start at the workspace's first 16-byte-aligned byte, and each array is rounded up
 * to 16 bytes (AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN()). A workspace size is therefore 15
 * plus each array's aligned size, so the workspace itself needs no alignment.
 */
#ifndef AZ_IOT_WORKSPACE_H
#define AZ_IOT_WORKSPACE_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Checks a workspace passed to init(). The caller logs the failure.
   *
   * @retval AZ_IOT_ERR_INVALID_ARG @p workspace is NULL.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE @p size is below @p needed.
   */
  static inline az_iot_result az_iot_workspace__check(
      const uint8_t* workspace,
      size_t size,
      size_t needed)
  {
    if (workspace == NULL)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    return size < needed ? AZ_IOT_ERR_NOT_ENOUGH_SPACE : AZ_IOT_OK;
  }

  /** @brief The first 16-byte-aligned byte of @p workspace, where its entries start. */
  static inline uint8_t* az_iot_workspace__start(uint8_t* workspace)
  {
    return (uint8_t*)(((uintptr_t)workspace + 15u) & ~(uintptr_t)15u);
  }

  /** @brief Takes @p count entries of @p entry_size at @p *cursor, which moves past them. */
  static inline void* az_iot_workspace__take(uint8_t** cursor, size_t count, size_t entry_size)
  {
    void* const entries = *cursor;
    *cursor += AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(count * entry_size);
    return entries;
  }

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_WORKSPACE_H */
