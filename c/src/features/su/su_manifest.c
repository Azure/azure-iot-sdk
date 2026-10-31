// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Update manifest parser: one pass over the decoded manifest JSON into
 * az_iot_su_client_update_manifest, bounded by the AZ_IOT_SU_MAX_* limits. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "internal/su_manifest_internal.h"
#include "internal/log_internal.h"

/** @brief Counts in an update manifest, including entries past a limit. */
typedef struct
{
  uint32_t step_count;
  uint32_t file_count;
  uint32_t largest_step_file_count;
  uint32_t largest_file_hash_count;
  uint32_t related_file_count;
} manifest_counts;

/** @brief Caller storage for related files and how many it holds. */
typedef struct
{
  az_iot_su_client_update_manifest_file* files;
  uint32_t capacity;
} related_file_storage;

/**
 * @brief Move to the next property of the object being read.
 *
 * @param jr       Reader inside an object, on its start or on the end of a value.
 * @param name     Set to the property name.
 * @param is_error Set when the JSON is malformed.
 * @return true with the reader on the property value; false at the object end or on error.
 */
static bool json_next_property(az_json_reader* jr, az_json_token* name, bool* is_error)
{
  if (az_result_failed(az_json_reader_next_token(jr)))
  {
    *is_error = true;
    return false;
  }
  if (jr->token.kind == AZ_JSON_TOKEN_END_OBJECT)
  {
    return false;
  }
  if (jr->token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
  {
    *is_error = true;
    return false;
  }
  *name = jr->token;
  if (az_result_failed(az_json_reader_next_token(jr)))
  {
    *is_error = true;
    return false;
  }
  return true;
}

/**
 * @brief Move to the next element of the array being read.
 *
 * @param jr       Reader inside an array, on its start or on the end of an element.
 * @param is_error Set when the JSON is malformed.
 * @return true with the reader on the element; false at the array end or on error.
 */
static bool json_next_element(az_json_reader* jr, bool* is_error)
{
  if (az_result_failed(az_json_reader_next_token(jr)))
  {
    *is_error = true;
    return false;
  }
  return jr->token.kind != AZ_JSON_TOKEN_END_ARRAY;
}

/** @brief Move the reader to the end of the current value; sets @p is_error on failure. */
static void json_skip_value(az_json_reader* jr, bool* is_error)
{
  if (az_result_failed(az_json_reader_skip_children(jr)))
  {
    *is_error = true;
  }
}

/**
 * @brief Check the kind of the current token.
 *
 * @return true when it is @p kind; otherwise false with @p is_error set.
 */
static bool json_token_is_kind(const az_json_reader* jr, az_json_token_kind kind, bool* is_error)
{
  if (jr->token.kind != kind)
  {
    *is_error = true;
    return false;
  }
  return true;
}

/** @brief Read the current string token into @p out; sets @p is_error when not a string. */
static void json_read_string(const az_json_reader* jr, az_span* out, bool* is_error)
{
  if (json_token_is_kind(jr, AZ_JSON_TOKEN_STRING, is_error))
  {
    *out = jr->token.slice;
  }
}

/**
 * @brief Parse `updateId`.
 *
 * @param jr       Reader on the value; left on its end.
 * @param id       Filled.
 * @param is_error Set when malformed.
 */
static void parse_update_id(
    az_json_reader* jr,
    az_iot_su_client_update_manifest_update_id* id,
    bool* is_error)
{
  az_json_token name;
  if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
  {
    return;
  }
  while (!*is_error && json_next_property(jr, &name, is_error))
  {
    if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("provider")))
    {
      json_read_string(jr, &id->provider, is_error);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("name")))
    {
      json_read_string(jr, &id->name, is_error);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("version")))
    {
      json_read_string(jr, &id->version, is_error);
    }
    else
    {
      json_skip_value(jr, is_error);
    }
  }
}

/**
 * @brief Parse one step of `instructions.steps`.
 *
 * @param jr       Reader on the step; left on its end.
 * @param step     Filled with at most AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP file ids.
 * @param counts   Largest step file count updated.
 * @param is_error Set when malformed.
 */
static void parse_step(
    az_json_reader* jr,
    az_iot_su_client_update_manifest_instructions_step* step,
    manifest_counts* counts,
    bool* is_error)
{
  az_json_token name;
  if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
  {
    return;
  }
  while (!*is_error && json_next_property(jr, &name, is_error))
  {
    if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("handler")))
    {
      json_read_string(jr, &step->handler, is_error);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("files")))
    {
      if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_ARRAY, is_error))
      {
        return;
      }
      uint32_t file_count = 0;
      while (!*is_error && json_next_element(jr, is_error))
      {
        if (file_count < (uint32_t)AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP)
        {
          json_read_string(jr, &step->files[file_count], is_error);
        }
        else
        {
          (void)json_token_is_kind(jr, AZ_JSON_TOKEN_STRING, is_error);
        }
        file_count++;
      }
      step->files_count = file_count < (uint32_t)AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP
          ? file_count
          : (uint32_t)AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP;
      if (file_count > counts->largest_step_file_count)
      {
        counts->largest_step_file_count = file_count;
      }
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("handlerProperties")))
    {
      az_json_token property;
      if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
      {
        return;
      }
      while (!*is_error && json_next_property(jr, &property, is_error))
      {
        if (az_json_token_is_text_equal(&property, AZ_SPAN_FROM_STR("installedCriteria")))
        {
          json_read_string(jr, &step->handler_properties.installed_criteria, is_error);
        }
        else
        {
          json_skip_value(jr, is_error);
        }
      }
    }
    else
    {
      json_skip_value(jr, is_error);
    }
  }
}

/**
 * @brief Parse `instructions`.
 *
 * @param jr           Reader on the value; left on its end.
 * @param instructions Filled with at most AZ_IOT_SU_MAX_STEP_COUNT steps.
 * @param counts       Step counts updated, including steps past the limit.
 * @param is_error     Set when malformed.
 */
static void parse_instructions(
    az_json_reader* jr,
    az_iot_su_client_update_manifest_instructions* instructions,
    manifest_counts* counts,
    bool* is_error)
{
  az_json_token name;
  if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
  {
    return;
  }
  while (!*is_error && json_next_property(jr, &name, is_error))
  {
    if (!az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("steps")))
    {
      json_skip_value(jr, is_error);
      continue;
    }
    if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_ARRAY, is_error))
    {
      return;
    }
    while (!*is_error && json_next_element(jr, is_error))
    {
      /* Steps past the limit are still parsed, so they are checked and counted. */
      az_iot_su_client_update_manifest_instructions_step past_limit;
      memset(&past_limit, 0, sizeof(past_limit));
      az_iot_su_client_update_manifest_instructions_step* step
          = counts->step_count < (uint32_t)AZ_IOT_SU_MAX_STEP_COUNT
          ? &instructions->steps[counts->step_count]
          : &past_limit;
      parse_step(jr, step, counts, is_error);
      counts->step_count++;
    }
    instructions->steps_count = counts->step_count < (uint32_t)AZ_IOT_SU_MAX_STEP_COUNT
        ? counts->step_count
        : (uint32_t)AZ_IOT_SU_MAX_STEP_COUNT;
  }
}

/**
 * @brief Parse `downloadHandler`: an object whose `id` is the handler id.
 *
 * @param jr       Reader on the value; left on its end.
 * @param id       Set to the id.
 * @param is_error Set when malformed.
 */
static void parse_download_handler(az_json_reader* jr, az_span* id, bool* is_error)
{
  az_json_token name;
  if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
  {
    return;
  }
  while (!*is_error && json_next_property(jr, &name, is_error))
  {
    if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("id")))
    {
      json_read_string(jr, id, is_error);
    }
    else
    {
      json_skip_value(jr, is_error);
    }
  }
}

/**
 * @brief Read the current object, unparsed, into @p out; sets @p is_error when not an object.
 *
 * @param jr       Reader on the object; left on its end.
 * @param out      Set to the object text, braces included.
 * @param is_error Set when malformed.
 */
static void json_read_object_text(az_json_reader* jr, az_span* out, bool* is_error)
{
  if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
  {
    return;
  }
  uint8_t* begin = az_span_ptr(jr->token.slice);
  json_skip_value(jr, is_error);
  if (!*is_error)
  {
    uint8_t* end = az_span_ptr(jr->token.slice) + az_span_size(jr->token.slice);
    *out = az_span_create(begin, (int32_t)(end - begin));
  }
}

static void parse_related_files(
    az_json_reader* jr,
    az_iot_su_client_update_manifest_file* file,
    const related_file_storage* related,
    manifest_counts* counts,
    bool* is_error);

/**
 * @brief Parse one file of `files`, or one related file.
 *
 * @param jr       Reader on the file object; left on its end.
 * @param file     Filled with at most AZ_IOT_SU_MAX_FILE_HASH_COUNT hashes.
 * @param related  Storage for its related files; NULL for a related file, whose
 *                 `relatedFiles` and `downloadHandler` are skipped.
 * @param counts   Largest file hash count (files only) and related file count updated.
 * @param is_error Set when malformed.
 */
static void parse_file(
    az_json_reader* jr,
    az_iot_su_client_update_manifest_file* file,
    const related_file_storage* related,
    manifest_counts* counts,
    bool* is_error)
{
  az_json_token name;
  if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
  {
    return;
  }
  while (!*is_error && json_next_property(jr, &name, is_error))
  {
    if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("fileName")))
    {
      json_read_string(jr, &file->file_name, is_error);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("sizeInBytes")))
    {
      if (json_token_is_kind(jr, AZ_JSON_TOKEN_NUMBER, is_error)
          && az_result_failed(az_json_token_get_int64(&jr->token, &file->size_in_bytes)))
      {
        *is_error = true;
      }
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("hashes")))
    {
      az_json_token hash_type;
      if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
      {
        return;
      }
      uint32_t hash_count = 0;
      while (!*is_error && json_next_property(jr, &hash_type, is_error))
      {
        if (hash_count < (uint32_t)AZ_IOT_SU_MAX_FILE_HASH_COUNT)
        {
          file->hashes[hash_count].hash_type = hash_type.slice;
          json_read_string(jr, &file->hashes[hash_count].hash_value, is_error);
        }
        else
        {
          (void)json_token_is_kind(jr, AZ_JSON_TOKEN_STRING, is_error);
        }
        hash_count++;
      }
      file->hashes_count = hash_count < (uint32_t)AZ_IOT_SU_MAX_FILE_HASH_COUNT
          ? hash_count
          : (uint32_t)AZ_IOT_SU_MAX_FILE_HASH_COUNT;
      /* A related file is optional: past the limit, its extra hashes are dropped instead. */
      if (related != NULL && hash_count > counts->largest_file_hash_count)
      {
        counts->largest_file_hash_count = hash_count;
      }
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("mimeType")))
    {
      json_read_string(jr, &file->mime_type, is_error);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("properties")))
    {
      json_read_object_text(jr, &file->properties, is_error);
    }
    else if (
        related != NULL && az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("downloadHandler")))
    {
      parse_download_handler(jr, &file->download_handler_id, is_error);
    }
    else if (
        related != NULL && az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("relatedFiles")))
    {
      parse_related_files(jr, file, related, counts, is_error);
    }
    else
    {
      json_skip_value(jr, is_error);
    }
  }
}

/**
 * @brief Parse a file's `relatedFiles`: an object keyed by related file id.
 *
 * @param jr       Reader on the value; left on its end.
 * @param file     Its related_files_index and related_files_count set.
 * @param related  Filled while it has room; later related files are counted only.
 * @param counts   Related file count updated, including those past the capacity.
 * @param is_error Set when malformed.
 */
static void parse_related_files(
    az_json_reader* jr,
    az_iot_su_client_update_manifest_file* file,
    const related_file_storage* related,
    manifest_counts* counts,
    bool* is_error)
{
  az_json_token related_id;
  if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
  {
    return;
  }
  file->related_files_index = counts->related_file_count < related->capacity
      ? counts->related_file_count
      : related->capacity;
  file->related_files_count = 0;
  while (!*is_error && json_next_property(jr, &related_id, is_error))
  {
    az_iot_su_client_update_manifest_file past_capacity;
    memset(&past_capacity, 0, sizeof(past_capacity));
    az_iot_su_client_update_manifest_file* related_file = &past_capacity;
    if (counts->related_file_count < related->capacity)
    {
      related_file = &related->files[counts->related_file_count];
      memset(related_file, 0, sizeof(*related_file));
      file->related_files_count++;
    }
    related_file->id = related_id.slice;
    parse_file(jr, related_file, NULL, counts, is_error);
    counts->related_file_count++;
  }
}

/**
 * @brief Parse `files`.
 *
 * @param jr       Reader on the value; left on its end.
 * @param manifest Filled with at most AZ_IOT_SU_MAX_FILE_COUNT files.
 * @param related  Storage for related files.
 * @param counts   File counts updated, including files past the limit.
 * @param is_error Set when malformed.
 */
static void parse_files(
    az_json_reader* jr,
    az_iot_su_client_update_manifest* manifest,
    const related_file_storage* related,
    manifest_counts* counts,
    bool* is_error)
{
  az_json_token file_id;
  if (!json_token_is_kind(jr, AZ_JSON_TOKEN_BEGIN_OBJECT, is_error))
  {
    return;
  }
  while (!*is_error && json_next_property(jr, &file_id, is_error))
  {
    /* Files past the limit are still parsed, so they are checked and counted. */
    az_iot_su_client_update_manifest_file past_limit;
    memset(&past_limit, 0, sizeof(past_limit));
    az_iot_su_client_update_manifest_file* file
        = counts->file_count < (uint32_t)AZ_IOT_SU_MAX_FILE_COUNT
        ? &manifest->files[counts->file_count]
        : &past_limit;
    file->id = file_id.slice;
    parse_file(jr, file, related, counts, is_error);
    counts->file_count++;
  }
  manifest->files_count = counts->file_count < (uint32_t)AZ_IOT_SU_MAX_FILE_COUNT
      ? counts->file_count
      : (uint32_t)AZ_IOT_SU_MAX_FILE_COUNT;
}

/**
 * @brief Check a manifest's counts against the AZ_IOT_SU_MAX_* limits, logging each exceeded.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE when any count exceeds its limit.
 */
static az_iot_result check_manifest_counts(const manifest_counts* counts)
{
  az_iot_result result = AZ_IOT_OK;
  if (counts->step_count > (uint32_t)AZ_IOT_SU_MAX_STEP_COUNT)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "manifest has %u steps; AZ_IOT_SU_MAX_STEP_COUNT is %u",
        (unsigned)counts->step_count,
        (unsigned)AZ_IOT_SU_MAX_STEP_COUNT);
    result = AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  if (counts->file_count > (uint32_t)AZ_IOT_SU_MAX_FILE_COUNT)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "manifest has %u files; AZ_IOT_SU_MAX_FILE_COUNT is %u",
        (unsigned)counts->file_count,
        (unsigned)AZ_IOT_SU_MAX_FILE_COUNT);
    result = AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  if (counts->largest_step_file_count > (uint32_t)AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "manifest step has %u files; AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP is %u",
        (unsigned)counts->largest_step_file_count,
        (unsigned)AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP);
    result = AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  if (counts->largest_file_hash_count > (uint32_t)AZ_IOT_SU_MAX_FILE_HASH_COUNT)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "manifest file has %u hashes; AZ_IOT_SU_MAX_FILE_HASH_COUNT is %u",
        (unsigned)counts->largest_file_hash_count,
        (unsigned)AZ_IOT_SU_MAX_FILE_HASH_COUNT);
    result = AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  return result;
}

az_iot_result az_iot_su__parse_update_manifest(
    az_span manifest_text,
    az_iot_su_client_update_manifest_file* related_files,
    size_t related_files_capacity,
    az_iot_su_client_update_manifest* out_manifest)
{
  memset(out_manifest, 0, sizeof(*out_manifest));
  related_file_storage related
      = { related_files, related_files == NULL ? 0u : (uint32_t)related_files_capacity };
  az_json_reader jr;
  if (az_span_size(manifest_text) <= 0
      || az_result_failed(az_json_reader_init(&jr, manifest_text, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  manifest_counts counts = { 0 };
  bool is_error = false;
  az_json_token name;
  while (!is_error && json_next_property(&jr, &name, &is_error))
  {
    if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("manifestVersion")))
    {
      json_read_string(&jr, &out_manifest->manifest_version, &is_error);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("updateId")))
    {
      parse_update_id(&jr, &out_manifest->update_id, &is_error);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("instructions")))
    {
      parse_instructions(&jr, &out_manifest->instructions, &counts, &is_error);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("files")))
    {
      parse_files(&jr, out_manifest, &related, &counts, &is_error);
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("createdDateTime")))
    {
      json_read_string(&jr, &out_manifest->create_date_time, &is_error);
    }
    else
    {
      json_skip_value(&jr, &is_error);
    }
  }
  /* Nothing but whitespace may follow the root object. */
  if (is_error || az_json_reader_next_token(&jr) != AZ_ERROR_JSON_READER_DONE)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  out_manifest->related_files = related.files;
  out_manifest->related_files_count
      = counts.related_file_count < related.capacity ? counts.related_file_count : related.capacity;
  if (counts.related_file_count > related.capacity)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_SU,
        "manifest has %u related files; kept %u (max_related_files)",
        (unsigned)counts.related_file_count,
        (unsigned)related.capacity);
  }
  return check_manifest_counts(&counts);
}
