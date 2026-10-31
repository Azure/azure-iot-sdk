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
#include "internal/json_string.h"
#include "internal/log_internal.h"

/** @brief Counts in an update manifest, including entries past a limit. */
typedef struct
{
  uint32_t step_count;
  uint32_t file_count;
  uint32_t largest_step_file_count;
  uint32_t largest_file_hash_count;
} manifest_counts;

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
 * @brief Parse one file of `files`.
 *
 * @param jr       Reader on the file object; left on its end.
 * @param file     Filled with at most AZ_IOT_SU_MAX_FILE_HASH_COUNT hashes.
 * @param counts   Largest file hash count updated.
 * @param is_error Set when malformed.
 */
static void parse_file(
    az_json_reader* jr,
    az_iot_su_client_update_manifest_file* file,
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
      if (hash_count > counts->largest_file_hash_count)
      {
        counts->largest_file_hash_count = hash_count;
      }
    }
    else if (az_json_token_is_text_equal(&name, AZ_SPAN_FROM_STR("mimeType")))
    {
      (void)json_token_is_kind(jr, AZ_JSON_TOKEN_STRING, is_error);
    }
    else
    {
      json_skip_value(jr, is_error);
    }
  }
}

/**
 * @brief Parse `files`.
 *
 * @param jr       Reader on the value; left on its end.
 * @param manifest Filled with at most AZ_IOT_SU_MAX_FILE_COUNT files.
 * @param counts   File counts updated, including files past the limit.
 * @param is_error Set when malformed.
 */
static void parse_files(
    az_json_reader* jr,
    az_iot_su_client_update_manifest* manifest,
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
    parse_file(jr, file, counts, is_error);
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
    az_iot_su_client_update_manifest* out_manifest)
{
  memset(out_manifest, 0, sizeof(*out_manifest));
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
      parse_files(&jr, out_manifest, &counts, &is_error);
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
  return check_manifest_counts(&counts);
}

const az_iot_su_client_update_manifest_file* az_iot_su__find_manifest_file(
    const az_iot_su_client_update_manifest* manifest,
    az_span id)
{
  for (uint32_t i = 0; i < manifest->files_count; ++i)
  {
    if (az_iot_json_string_equals(manifest->files[i].id, id, true))
    {
      return &manifest->files[i];
    }
  }
  return NULL;
}

const az_iot_su_client_file_url* az_iot_su__find_file_url(
    const az_iot_su_client_file_url* file_urls,
    uint32_t file_urls_count,
    az_span id)
{
  for (uint32_t i = 0; i < file_urls_count; ++i)
  {
    if (az_iot_json_string_equals(id, file_urls[i].id, false))
    {
      return &file_urls[i];
    }
  }
  return NULL;
}

az_iot_result az_iot_su__check_step_file_references(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t first_step,
    const az_iot_su_client_file_url* file_urls,
    uint32_t file_urls_count)
{
  for (uint32_t i = first_step; i < manifest->instructions.steps_count; ++i)
  {
    const az_iot_su_client_update_manifest_instructions_step* step
        = &manifest->instructions.steps[i];
    for (uint32_t j = 0; j < step->files_count; ++j)
    {
      az_span id = step->files[j];
      if (az_iot_su__find_manifest_file(manifest, id) == NULL)
      {
        AZ_IOT_LOG_ERRORF(
            AZ_IOT_LOG_COMPONENT_SU,
            "step %u names file '%.*s', which the manifest does not describe",
            (unsigned)i,
            (int)az_span_size(id),
            (const char*)az_span_ptr(id));
        return AZ_IOT_ERR_INVALID_ARG;
      }
      if (az_iot_su__find_file_url(file_urls, file_urls_count, id) == NULL)
      {
        AZ_IOT_LOG_ERRORF(
            AZ_IOT_LOG_COMPONENT_SU,
            "step %u names file '%.*s', which has no download URL",
            (unsigned)i,
            (int)az_span_size(id),
            (const char*)az_span_ptr(id));
        return AZ_IOT_ERR_INVALID_ARG;
      }
    }
  }
  return AZ_IOT_OK;
}
