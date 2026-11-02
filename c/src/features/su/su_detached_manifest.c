// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Detached update manifest file: check it against the signed mini manifest, then unwrap and
 * parse the full update manifest it carries. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "internal/su_detached_manifest_internal.h"
#include "internal/su_manifest_internal.h"
#include "internal/crypto.h"
#include "internal/json_string.h"
#include "internal/log_internal.h"

/**
 * @brief az_iot_su_read_chunk_callback over a file held in memory.
 *
 * @param read_ctx The file, as an az_span*.
 */
static int32_t read_chunk_from_span(
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* read_ctx)
{
  const az_span* content = (const az_span*)read_ctx;
  size_t size = (size_t)az_span_size(*content);
  size_t count = offset < size ? size - offset : 0u;
  if (count > buffer_size)
  {
    count = buffer_size;
  }
  if (count > 0u)
  {
    memcpy(buffer, az_span_ptr(*content) + offset, count);
  }
  *out_read = count;
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/**
 * @brief Find the escaped value of top-level string property `updateManifest` of @p file_content.
 *
 * The whole file must be one JSON object. When `updateManifest` repeats, the last one wins.
 *
 * @param file_content The detached manifest file.
 * @param out_escaped  The value, still JSON-escaped; empty when missing or not a string.
 * @return AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NONE, or why the file is rejected.
 */
static az_iot_su__detached_manifest_error find_update_manifest_property(
    az_span file_content,
    az_span* out_escaped)
{
  *out_escaped = AZ_SPAN_EMPTY;
  az_json_reader jr;
  if (az_span_size(file_content) <= 0
      || az_result_failed(az_json_reader_init(&jr, file_content, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NOT_JSON_OBJECT;
  }
  for (;;)
  {
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NOT_JSON_OBJECT;
    }
    if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      break;
    }
    bool is_update_manifest
        = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("updateManifest"));
    if (az_result_failed(az_json_reader_next_token(&jr))
        || az_result_failed(az_json_reader_skip_children(&jr)))
    {
      return AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NOT_JSON_OBJECT;
    }
    if (is_update_manifest)
    {
      *out_escaped = jr.token.kind == AZ_JSON_TOKEN_STRING ? jr.token.slice : AZ_SPAN_EMPTY;
    }
  }
  /* Nothing but whitespace may follow the root object. */
  if (az_json_reader_next_token(&jr) != AZ_ERROR_JSON_READER_DONE)
  {
    return AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NOT_JSON_OBJECT;
  }
  return az_span_size(*out_escaped) > 0
      ? AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NONE
      : AZ_IOT_SU__DETACHED_MANIFEST_ERROR_UPDATE_MANIFEST_MISSING;
}

/**
 * @brief Log why the file is rejected, clear @p out_manifest and set @p out_error.
 *
 * @return @p result.
 */
static az_iot_result reject_detached_manifest_file(
    az_iot_result result,
    az_iot_su__detached_manifest_error error,
    const char* reason,
    az_iot_su_client_update_manifest* out_manifest,
    az_iot_su__detached_manifest_error* out_error)
{
  AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_SU, "detached manifest file rejected: %s", reason);
  memset(out_manifest, 0, sizeof(*out_manifest));
  if (out_error != NULL)
  {
    *out_error = error;
  }
  return result;
}

az_iot_result az_iot_su__verify_and_parse_detached_manifest_file(
    const az_iot_crypto* crypto,
    const az_iot_su_client_update_manifest_file* detached_file,
    az_span file_content,
    const az_iot_su_client_file_url* file_urls,
    uint32_t file_urls_count,
    az_iot_su_client_update_manifest_file* related_files,
    size_t related_files_capacity,
    az_iot_su_client_update_manifest* out_manifest,
    az_iot_su__detached_manifest_error* out_error)
{
  if (out_error != NULL)
  {
    *out_error = AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NONE;
  }
  if (az_iot_crypto__validate(crypto) != AZ_IOT_OK || detached_file == NULL || out_manifest == NULL
      || (file_urls == NULL && file_urls_count > 0u))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(out_manifest, 0, sizeof(*out_manifest));

  if (detached_file->size_in_bytes != (int64_t)az_span_size(file_content))
  {
    return reject_detached_manifest_file(
        AZ_IOT_ERR_AUTH,
        AZ_IOT_SU__DETACHED_MANIFEST_ERROR_SIZE_MISMATCH,
        "size differs from the signed sizeInBytes",
        out_manifest,
        out_error);
  }
  if (az_iot_su_verify_file_hash(detached_file, crypto, read_chunk_from_span, &file_content)
      != AZ_IOT_OK)
  {
    return reject_detached_manifest_file(
        AZ_IOT_ERR_AUTH,
        AZ_IOT_SU__DETACHED_MANIFEST_ERROR_HASH_MISMATCH,
        "SHA-256 differs from the signed one",
        out_manifest,
        out_error);
  }

  az_span escaped;
  az_iot_su__detached_manifest_error error = find_update_manifest_property(file_content, &escaped);
  if (error != AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NONE)
  {
    return reject_detached_manifest_file(
        AZ_IOT_ERR_INVALID_ARG,
        error,
        "not a JSON object with a string updateManifest",
        out_manifest,
        out_error);
  }

  az_span manifest_text;
  if (az_iot_json_string_decode(escaped, escaped, &manifest_text) != AZ_IOT_OK)
  {
    return reject_detached_manifest_file(
        AZ_IOT_ERR_INVALID_ARG,
        AZ_IOT_SU__DETACHED_MANIFEST_ERROR_UPDATE_MANIFEST_UNDECODABLE,
        "updateManifest has an invalid escape",
        out_manifest,
        out_error);
  }

  az_iot_result result = az_iot_su__parse_update_manifest(
      manifest_text, related_files, related_files_capacity, out_manifest);
  if (result != AZ_IOT_OK)
  {
    return reject_detached_manifest_file(
        result,
        AZ_IOT_SU__DETACHED_MANIFEST_ERROR_MANIFEST_INVALID,
        "update manifest is malformed or over a limit",
        out_manifest,
        out_error);
  }
  if (az_iot_su__check_step_file_references(out_manifest, 0, file_urls, file_urls_count)
      != AZ_IOT_OK)
  {
    return reject_detached_manifest_file(
        AZ_IOT_ERR_INVALID_ARG,
        AZ_IOT_SU__DETACHED_MANIFEST_ERROR_FILE_REFERENCE_UNRESOLVED,
        "a step file is not in files or has no download URL",
        out_manifest,
        out_error);
  }
  return AZ_IOT_OK;
}
