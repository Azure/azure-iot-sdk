// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_json.h"

#include <stdio.h>
#include <string.h>

#include <azure/core/az_json.h>

/** @brief Moves @p r, on an object's BEGIN_OBJECT, to the value of member @p name. */
static bool json_enter(az_json_reader* r, const char* name)
{
  if (r->token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return false;
  }
  while (az_json_reader_next_token(r) == AZ_OK && r->token.kind == AZ_JSON_TOKEN_PROPERTY_NAME)
  {
    bool const match = az_json_token_is_text_equal(
        &r->token, az_span_create((uint8_t*)(uintptr_t)name, (int32_t)strlen(name)));
    if (az_json_reader_next_token(r) != AZ_OK)
    {
      return false;
    }
    if (match)
    {
      return true;
    }
    if (az_json_reader_skip_children(r) != AZ_OK)
    {
      return false;
    }
  }
  return false;
}

/** @brief Moves @p r to the value at @p path in the JSON object @p json. */
static bool json_find(const char* json, const char* const* path, az_json_reader* r)
{
  /* An empty buffer fails az_json_reader_init's precondition, which does not return. */
  if (*json == '\0'
      || az_json_reader_init(
             r, az_span_create((uint8_t*)(uintptr_t)json, (int32_t)strlen(json)), NULL)
          != AZ_OK
      || az_json_reader_next_token(r) != AZ_OK)
  {
    return false;
  }
  for (; *path != NULL; ++path)
  {
    if (!json_enter(r, *path))
    {
      return false;
    }
  }
  return true;
}

bool e2e_json_read_uint64(const char* json, const char* const* path, uint64_t* out)
{
  az_json_reader r;
  return json_find(json, path, &r) && r.token.kind == AZ_JSON_TOKEN_NUMBER
      && az_json_token_get_uint64(&r.token, out) == AZ_OK;
}

bool e2e_json_read_string(const char* json, const char* const* path, char* out, size_t size)
{
  az_json_reader r;
  return json_find(json, path, &r) && r.token.kind == AZ_JSON_TOKEN_STRING
      && az_json_token_get_string(&r.token, out, (int32_t)size, NULL) == AZ_OK;
}

/**
 * @brief Whether @p json is exactly one well-formed JSON value whose member names have no
 * \\uXXXX escape, which az_json cannot match (such a member would look absent).
 */
static bool json_is_complete_and_matchable(const char* json)
{
  az_json_reader r;
  if (*json == '\0'
      || az_json_reader_init(
             &r, az_span_create((uint8_t*)(uintptr_t)json, (int32_t)strlen(json)), NULL)
          != AZ_OK)
  {
    return false;
  }
  az_result result;
  while ((result = az_json_reader_next_token(&r)) == AZ_OK)
  {
    if (r.token.kind == AZ_JSON_TOKEN_PROPERTY_NAME
        && az_span_find(r.token.slice, AZ_SPAN_FROM_STR("\\u")) >= 0)
    {
      return false;
    }
  }
  return result == AZ_ERROR_JSON_READER_DONE;
}

bool e2e_json_read_string_or_default(
    const char* json,
    const char* const* path,
    const char* default_value,
    char* out,
    size_t size)
{
  if (!json_is_complete_and_matchable(json))
  {
    return false;
  }
  az_json_reader r;
  if (!json_find(json, path, &r) || r.token.kind == AZ_JSON_TOKEN_NULL)
  {
    int n = snprintf(out, size, "%s", default_value);
    return n >= 0 && (size_t)n < size;
  }
  return r.token.kind == AZ_JSON_TOKEN_STRING
      && az_json_token_get_string(&r.token, out, (int32_t)size, NULL) == AZ_OK;
}
