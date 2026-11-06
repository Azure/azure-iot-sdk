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
        = az_iot_json_string_equals(jr.token.slice, AZ_SPAN_FROM_STR("updateManifest"), false);
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

AZ_NODISCARD az_iot_result az_iot_su__verify_and_parse_detached_manifest_file(
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
  if (out_manifest == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(out_manifest, 0, sizeof(*out_manifest));
  if (az_iot_crypto__validate(crypto) != AZ_IOT_OK || detached_file == NULL
      || (file_urls == NULL && file_urls_count > 0u))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

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

const az_iot_su_client_update_manifest_file* az_iot_su__find_detached_manifest_file(
    const az_iot_su_client_update_manifest* mini_manifest)
{
  if (mini_manifest == NULL || !az_iot_su_manifest_is_detached(mini_manifest)
      || mini_manifest->files_count != 1u)
  {
    return NULL;
  }
  return az_iot_su__find_manifest_file(mini_manifest, mini_manifest->detached_manifest_file_id);
}

/** @brief Whether two parsed (JSON-escaped) update ids are equal, compared decoded. */
static bool update_ids_equal(
    const az_iot_su_client_update_manifest_update_id* first,
    const az_iot_su_client_update_manifest_update_id* second)
{
  return az_iot_json_string_equals(first->provider, second->provider, true)
      && az_iot_json_string_equals(first->name, second->name, true)
      && az_iot_json_string_equals(first->version, second->version, true);
}

/**
 * @brief The JSON object at the start of @p region, without what follows it.
 *
 * @param region The `updateManifest` value after its in-place decode: the full manifest text, then
 *               what is left of the escaped value.
 * @return The full manifest text; empty when @p region does not start with a JSON object.
 */
static az_span leading_json_object(az_span region)
{
  az_json_reader jr;
  if (az_span_size(region) <= 0 || az_result_failed(az_json_reader_init(&jr, region, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT
      || az_result_failed(az_json_reader_skip_children(&jr)))
  {
    return AZ_SPAN_EMPTY;
  }
  const uint8_t* end = az_span_ptr(jr.token.slice) + az_span_size(jr.token.slice);
  return az_span_slice(region, 0, (int32_t)(end - az_span_ptr(region)));
}

/**
 * @brief Log why the full manifest is rejected, clear @p out_manifest and set
 * @p out_manifest_error.
 *
 * @return AZ_IOT_ERR_INVALID_ARG.
 */
static az_iot_result reject_full_manifest(
    uint32_t manifest_error,
    const char* reason,
    az_iot_su_client_update_manifest* out_manifest,
    uint32_t* out_manifest_error)
{
  AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_SU, "detached manifest rejected: %s", reason);
  memset(out_manifest, 0, sizeof(*out_manifest));
  *out_manifest_error = manifest_error;
  return AZ_IOT_ERR_INVALID_ARG;
}

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
    uint32_t* out_manifest_error)
{
  if (out_manifest_text != NULL)
  {
    *out_manifest_text = AZ_SPAN_EMPTY;
  }
  if (mini_manifest == NULL || out_manifest == NULL || out_manifest_error == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  *out_manifest_error = 0;
  const az_iot_su_client_update_manifest_file* entry
      = az_iot_su__find_detached_manifest_file(mini_manifest);
  if (entry == NULL)
  {
    return reject_full_manifest(
        AZ_IOT_SU_MANIFEST_ERROR_DETACHED_FILE_ENTRY,
        "the mini manifest files is not exactly its detached manifest file",
        out_manifest,
        out_manifest_error);
  }
  /* Copied first: out_manifest may be mini_manifest, which the parse overwrites. */
  az_iot_su_client_update_manifest_file detached_file = *entry;
  az_iot_su_client_update_manifest_update_id mini_update_id = mini_manifest->update_id;
  /* Located before the in-place decode: the full manifest text starts there. */
  az_span escaped = AZ_SPAN_EMPTY;
  (void)find_update_manifest_property(file_content, &escaped);

  az_iot_su__detached_manifest_error file_error = AZ_IOT_SU__DETACHED_MANIFEST_ERROR_NONE;
  az_iot_result result = az_iot_su__verify_and_parse_detached_manifest_file(
      crypto,
      &detached_file,
      file_content,
      file_urls,
      file_urls_count,
      related_files,
      related_files_capacity,
      out_manifest,
      &file_error);
  if (result != AZ_IOT_OK)
  {
    *out_manifest_error = (file_error == AZ_IOT_SU__DETACHED_MANIFEST_ERROR_SIZE_MISMATCH
                           || file_error == AZ_IOT_SU__DETACHED_MANIFEST_ERROR_HASH_MISMATCH)
        ? AZ_IOT_SU_MANIFEST_ERROR_DETACHED_FILE_HASH
        : AZ_IOT_SU_MANIFEST_ERROR_DETACHED_FILE_INVALID;
    return result;
  }
  if (!update_ids_equal(&out_manifest->update_id, &mini_update_id))
  {
    return reject_full_manifest(
        AZ_IOT_SU_MANIFEST_ERROR_DETACHED_UPDATE_ID,
        "the full manifest updateId differs from the mini manifest's",
        out_manifest,
        out_manifest_error);
  }
  if (az_iot_su_manifest_is_detached(out_manifest))
  {
    return reject_full_manifest(
        AZ_IOT_SU_MANIFEST_ERROR_DETACHED_NESTED,
        "the full manifest is itself a mini manifest",
        out_manifest,
        out_manifest_error);
  }
  result = az_iot_su__check_manifest_supported(out_manifest, out_manifest_error);
  if (result != AZ_IOT_OK)
  {
    memset(out_manifest, 0, sizeof(*out_manifest));
    return result;
  }
  if (out_manifest_text != NULL)
  {
    *out_manifest_text = leading_json_object(escaped);
  }
  return AZ_IOT_OK;
}
