// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* INTERNAL. Detached update manifest file: verify, unwrap and parse. NOT part of the public API. */
#ifndef AZ_IOT_SU_DETACHED_MANIFEST_INTERNAL_H
#define AZ_IOT_SU_DETACHED_MANIFEST_INTERNAL_H

#include "azure/iot/az_iot_crypto.h"
#include "azure/iot/az_iot_su.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief Why az_iot_su__verify_and_parse_detached_manifest_file() failed. */
  typedef enum az_iot_su__detached_manifest_error
  {
    /** No error. */
    AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NONE = 0,
    /** The file length differs from the signed `sizeInBytes`. */
    AZ_IOT_SU__DETACHED_MANIFEST_ERROR_SIZE_MISMATCH,
    /** The file SHA-256 differs from the signed one, or cannot be computed or compared. */
    AZ_IOT_SU__DETACHED_MANIFEST_ERROR_HASH_MISMATCH,
    /** The file is not a JSON object. */
    AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NOT_JSON_OBJECT,
    /** The file has no non-empty string property `updateManifest`. */
    AZ_IOT_SU__DETACHED_MANIFEST_ERROR_UPDATE_MANIFEST_MISSING,
    /** `updateManifest` has an invalid JSON string escape. */
    AZ_IOT_SU__DETACHED_MANIFEST_ERROR_UPDATE_MANIFEST_UNDECODABLE,
    /** The manifest parser rejected the manifest, or it exceeds a limit. */
    AZ_IOT_SU__DETACHED_MANIFEST_ERROR_MANIFEST_INVALID,
    /** A step names a file missing from the manifest `files` or from the file URLs. */
    AZ_IOT_SU__DETACHED_MANIFEST_ERROR_FILE_REFERENCE_UNRESOLVED,
  } az_iot_su__detached_manifest_error;

  /**
   * @brief Verify a downloaded detached manifest file against the signed mini manifest entry
   * describing it, then unwrap and parse the full update manifest it carries.
   *
   * The file is `{"updateManifest": "<serialized full manifest>", ...}`; other properties are
   * ignored. It carries no signature: the size and SHA-256 signed in the mini manifest bind it.
   * Callers still check that the full manifest is not itself detached and that its `updateId`
   * matches the mini manifest's.
   *
   * @param crypto                 Backend; needs only the SHA-256 functions.
   * @param detached_file          The mini manifest `files` entry of the detached manifest.
   * @param file_content           The whole file, writable: `updateManifest` is decoded in place,
   *                               also on failure. @p out_manifest spans point into it.
   * @param file_urls              Download URL of each file, from the request.
   * @param file_urls_count        Entries in @p file_urls.
   * @param related_files          Storage for related files; may be NULL to keep none.
   * @param related_files_capacity Entries in @p related_files.
   * @param out_manifest           The full manifest; zeroed on failure.
   * @param out_error              Why it failed; AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NONE on
   *                               success. May be NULL.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG on bad arguments or a malformed file or manifest;
   * AZ_IOT_ERR_AUTH on a size or SHA-256 mismatch; AZ_IOT_ERR_NOT_ENOUGH_SPACE when the manifest
   * exceeds an AZ_IOT_SU_MAX_* limit.
   */
  AZ_NODISCARD az_iot_result az_iot_su__verify_and_parse_detached_manifest_file(
      const az_iot_crypto* crypto,
      const az_iot_su_client_update_manifest_file* detached_file,
      az_span file_content,
      const az_iot_su_client_file_url* file_urls,
      uint32_t file_urls_count,
      az_iot_su_client_update_manifest_file* related_files,
      size_t related_files_capacity,
      az_iot_su_client_update_manifest* out_manifest,
      az_iot_su__detached_manifest_error* out_error);

  /**
   * @brief Find the detached manifest file of a mini manifest: its only `files` entry, whose id
   * is its `detachedManifestFileId`.
   *
   * @param mini_manifest A parsed mini manifest.
   * @return The entry; NULL when @p mini_manifest is not a mini manifest or its `files` is not
   * exactly that entry.
   */
  const az_iot_su_client_update_manifest_file* az_iot_su__find_detached_manifest_file(
      const az_iot_su_client_update_manifest* mini_manifest);

  /**
   * @brief Get the full update manifest of a mini manifest from its detached manifest file.
   *
   * Runs az_iot_su__find_detached_manifest_file() and
   * az_iot_su__verify_and_parse_detached_manifest_file(), then checks that the full manifest has
   * the mini manifest's `updateId`, is not itself a mini manifest, and is supported
   * (az_iot_su__check_manifest_supported()).
   *
   * @param crypto                 Backend; needs only the SHA-256 functions.
   * @param mini_manifest          The verified mini manifest. May be @p out_manifest; its spans
   *                               must not point into @p file_content.
   * @param file_content           The whole detached manifest file, writable; decoded in place.
   * @param file_urls              Download URL of each file, from the request.
   * @param file_urls_count        Entries in @p file_urls.
   * @param related_files          Storage for related files; may be NULL to keep none.
   * @param related_files_capacity Entries in @p related_files.
   * @param out_manifest           The full manifest; zeroed on failure.
   * @param out_manifest_text      The full manifest text within @p file_content; empty on
   *                               failure. May be NULL.
   * @param out_manifest_error     The AZ_IOT_SU_MANIFEST_ERROR_* sub-code on failure; else 0.
   * @return AZ_IOT_OK; AZ_IOT_ERR_AUTH on a size or SHA-256 mismatch; AZ_IOT_ERR_NOT_SUPPORTED
   * for a `reference` step; AZ_IOT_ERR_NOT_ENOUGH_SPACE when the manifest exceeds an
   * AZ_IOT_SU_MAX_* limit; AZ_IOT_ERR_INVALID_ARG otherwise.
   */
  AZ_NODISCARD az_iot_result az_iot_su__parse_detached_manifest(
      const az_iot_crypto* crypto,
      const az_iot_su_client_update_manifest* mini_manifest,
      az_span file_content,
      const az_iot_su_client_file_url* file_urls,
      uint32_t file_urls_count,
      az_iot_su_client_update_manifest_file* related_files,
      size_t related_files_capacity,
      az_iot_su_client_update_manifest* out_manifest,
      az_span* out_manifest_text,
      uint32_t* out_manifest_error);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SU_DETACHED_MANIFEST_INTERNAL_H */
