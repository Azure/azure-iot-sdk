// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* INTERNAL. Update manifest parser. NOT part of the public API. */
#ifndef AZ_IOT_SU_MANIFEST_INTERNAL_H
#define AZ_IOT_SU_MANIFEST_INTERNAL_H

#include "azure/iot/az_iot_su.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Parse a decoded update manifest.
   *
   * Unknown members are skipped at every level. A known member of the wrong type, or anything
   * but whitespace after the root object, is malformed.
   *
   * @param manifest_text The decoded manifest JSON; @p out_manifest spans point into it.
   * @param out_manifest  Filled; cleared first.
   * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE (each exceeded limit logged) when steps,
   * files, files of a step or hashes of a file exceed their AZ_IOT_SU_MAX_* limit;
   * AZ_IOT_ERR_INVALID_ARG when malformed.
   */
  az_iot_result az_iot_su__parse_update_manifest(
      az_span manifest_text,
      az_iot_su_client_update_manifest* out_manifest);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SU_MANIFEST_INTERNAL_H */
