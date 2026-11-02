// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

#include <string.h>

#include "internal/su_device_properties_internal.h"
#include "internal/span_writer.h"
#include "internal/log_internal.h"

az_iot_result az_iot_su__validate_compatibility_properties(
    const az_iot_su_custom_property* properties,
    size_t count)
{
  if (properties == NULL || count == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (count > AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  for (size_t i = 0; i < count; ++i)
  {
    if (!is_nonempty_cstr(properties[i].name) || properties[i].value == NULL)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    if (strlen(properties[i].name) > AZ_IOT_SU_MAX_JSON_STRING_SIZE
        || strlen(properties[i].value) > AZ_IOT_SU_MAX_JSON_STRING_SIZE)
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    for (size_t j = 0; j < i; ++j)
    {
      if (strcmp(properties[i].name, properties[j].name) == 0)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
    }
  }
  return AZ_IOT_OK;
}

az_iot_result az_iot_su__validate_installed_update_id(const az_iot_su_report_update_id* id)
{
  if (id == NULL)
  {
    return AZ_IOT_OK;
  }
  if (!is_nonempty_cstr(id->provider) || !is_nonempty_cstr(id->name)
      || !is_nonempty_cstr(id->version))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (strlen(id->provider) > AZ_IOT_SU_MAX_JSON_STRING_SIZE
      || strlen(id->name) > AZ_IOT_SU_MAX_JSON_STRING_SIZE
      || strlen(id->version) > AZ_IOT_SU_MAX_JSON_STRING_SIZE)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  return AZ_IOT_OK;
}

size_t az_iot_su__compatibility_properties(
    const az_iot_su_device_properties* properties,
    az_iot_su_custom_property* out)
{
  size_t count = 0;
  if (properties->manufacturer != NULL)
  {
    out[count++] = (az_iot_su_custom_property){ "manufacturer", properties->manufacturer };
  }
  if (properties->model != NULL)
  {
    out[count++] = (az_iot_su_custom_property){ "model", properties->model };
  }
  for (size_t i = 0; i < properties->custom_properties_count; ++i)
  {
    out[count++] = properties->custom_properties[i];
  }
  return count;
}

static size_t string_size(const char* value) { return value != NULL ? strlen(value) + 1u : 0u; }

az_iot_result az_iot_su__measure_device_properties(
    const az_iot_su_device_properties* properties,
    size_t* out_strings_size)
{
  if (properties == NULL || out_strings_size == NULL
      || (properties->custom_properties_count > 0 && properties->custom_properties == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t builtins
      = (properties->manufacturer != NULL ? 1u : 0u) + (properties->model != NULL ? 1u : 0u);
  if (properties->custom_properties_count > AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES - builtins)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  az_iot_su_custom_property compatibility[AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES];
  size_t count = az_iot_su__compatibility_properties(properties, compatibility);
  az_iot_result r = az_iot_su__validate_compatibility_properties(compatibility, count);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  const az_iot_su_update_id_info* installed = &properties->installed_update_id;
  az_iot_su_report_update_id id = { installed->provider, installed->name, installed->version };
  r = az_iot_su__validate_installed_update_id(
      id.provider == NULL && id.name == NULL && id.version == NULL ? NULL : &id);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* Each string is at most AZ_IOT_SU_MAX_JSON_STRING_SIZE, so the sum of at
   * most 13 of them cannot overflow. */
  size_t installed_size = string_size(id.provider) + string_size(id.name) + string_size(id.version);
  if (installed_size > AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "installed update ID needs %lu bytes; AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE is %lu",
        (unsigned long)installed_size,
        (unsigned long)AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  size_t size
      = string_size(properties->manufacturer) + string_size(properties->model) + installed_size;
  for (size_t i = 0; i < properties->custom_properties_count; ++i)
  {
    size += string_size(properties->custom_properties[i].name)
        + string_size(properties->custom_properties[i].value);
  }
  *out_strings_size = size;
  return AZ_IOT_OK;
}

/* Compared as integers: relational operators on pointers into different objects are undefined. */
static bool string_in_buffer(const char* value, const char* buffer, size_t buffer_size)
{
  if (value == NULL)
  {
    return false;
  }
  uintptr_t start = (uintptr_t)buffer;
  uintptr_t end = (uintptr_t)(value + strlen(value) + 1u);
  uintptr_t begin = (uintptr_t)value;
  return begin < start + buffer_size && end > start;
}

bool az_iot_su__device_properties_overlap(
    const az_iot_su_device_properties* properties,
    const char* buffer,
    size_t buffer_size)
{
  const char* strings[3 + 2 + 2 * AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES] = {
    properties->manufacturer,
    properties->model,
    properties->installed_update_id.provider,
    properties->installed_update_id.name,
    properties->installed_update_id.version,
  };
  size_t count = 5;
  for (size_t i = 0; i < properties->custom_properties_count; ++i)
  {
    strings[count++] = properties->custom_properties[i].name;
    strings[count++] = properties->custom_properties[i].value;
  }
  for (size_t i = 0; i < count; ++i)
  {
    if (string_in_buffer(strings[i], buffer, buffer_size))
    {
      return true;
    }
  }
  return false;
}

static const char* copy_string(char** cursor, const char* value)
{
  if (value == NULL)
  {
    return NULL;
  }
  size_t size = strlen(value) + 1u;
  char* stored = *cursor;
  memcpy(stored, value, size);
  *cursor += size;
  return stored;
}

void az_iot_su__copy_device_properties(
    const az_iot_su_device_properties* properties,
    char* strings,
    az_iot_su_device_properties* copy,
    az_iot_su_custom_property* custom_properties)
{
  const az_iot_su_device_properties source = *properties;
  char* cursor = strings;
  az_iot_su_device_properties out = { 0 };
  out.manufacturer = copy_string(&cursor, source.manufacturer);
  out.model = copy_string(&cursor, source.model);
  out.custom_properties_count = source.custom_properties_count;
  for (size_t i = 0; i < source.custom_properties_count; ++i)
  {
    az_iot_su_custom_property property = source.custom_properties[i];
    custom_properties[i].name = copy_string(&cursor, property.name);
    custom_properties[i].value = copy_string(&cursor, property.value);
  }
  out.custom_properties = source.custom_properties_count > 0 ? custom_properties : NULL;
  out.installed_update_id.provider = copy_string(&cursor, source.installed_update_id.provider);
  out.installed_update_id.name = copy_string(&cursor, source.installed_update_id.name);
  out.installed_update_id.version = copy_string(&cursor, source.installed_update_id.version);
  *copy = out;
}
