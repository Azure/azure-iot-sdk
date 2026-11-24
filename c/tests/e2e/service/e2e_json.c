// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_json.h"

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
  if (az_json_reader_init(r, az_span_create((uint8_t*)(uintptr_t)json, (int32_t)strlen(json)), NULL)
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
