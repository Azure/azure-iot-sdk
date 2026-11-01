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
   * @param manifest_text          The decoded manifest JSON; @p out_manifest spans point into it.
   * @param related_files          Storage for related files; may be NULL to keep none.
   * @param related_files_capacity Entries in @p related_files. Related files past it are left
   *                               out, with a warning. A related file keeps at most
   *                               AZ_IOT_SU_MAX_FILE_HASH_COUNT hashes.
   * @param out_manifest           Filled; cleared first.
   * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE (each exceeded limit logged) when steps,
   * files, files of a step or hashes of a file exceed their AZ_IOT_SU_MAX_* limit;
   * AZ_IOT_ERR_INVALID_ARG when malformed.
   */
  az_iot_result az_iot_su__parse_update_manifest(
      az_span manifest_text,
      az_iot_su_client_update_manifest_file* related_files,
      size_t related_files_capacity,
      az_iot_su_client_update_manifest* out_manifest);

  /**
   * @brief Check that every file of steps [@p first_step, end) is described by the manifest and
   * has a download URL.
   *
   * @param manifest        A parsed manifest.
   * @param first_step      First step checked; steps before it are not.
   * @param file_urls       Download URL of each file, from the request.
   * @param file_urls_count Entries in @p file_urls.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG (the offending step and file id logged) when a step
   * names a file id missing from `files` or from @p file_urls.
   */
  az_iot_result az_iot_su__check_step_file_references(
      const az_iot_su_client_update_manifest* manifest,
      uint32_t first_step,
      const az_iot_su_client_file_url* file_urls,
      uint32_t file_urls_count);

  /**
   * @brief Find the manifest file with id @p id.
   *
   * Ids are compared decoded: `f\u0031` and `f1` are the same id.
   *
   * @param manifest A parsed manifest.
   * @param id       A file id from the manifest, e.g. a step's `files` entry (JSON-escaped).
   * @return The file, or NULL when the manifest does not describe @p id.
   */
  const az_iot_su_client_update_manifest_file* az_iot_su__find_manifest_file(
      const az_iot_su_client_update_manifest* manifest,
      az_span id);

  /**
   * @brief Find the download URL of manifest file @p id.
   *
   * @param file_urls       Download URL of each file, with decoded ids, from the request.
   * @param file_urls_count Entries in @p file_urls.
   * @param id              A file id from the manifest (JSON-escaped); compared decoded.
   * @return The entry, or NULL when none has @p id.
   */
  const az_iot_su_client_file_url* az_iot_su__find_file_url(
      const az_iot_su_client_file_url* file_urls,
      uint32_t file_urls_count,
      az_span id);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SU_MANIFEST_INTERNAL_H */
