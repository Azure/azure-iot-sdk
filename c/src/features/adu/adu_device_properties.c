// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

#include <string.h>

#include "internal/adu_device_properties_internal.h"
#include "internal/span_writer.h"

az_iot_result az_iot_adu__validate_compatibility_properties(
    const az_iot_adu_custom_property* properties,
    size_t count)
{
  if (properties == NULL || count == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (count > AZ_IOT_ADU_MAX_COMPATIBILITY_PROPERTIES)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  for (size_t i = 0; i < count; ++i)
  {
    if (!is_nonempty_cstr(properties[i].name) || properties[i].value == NULL)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    if (strlen(properties[i].name) > INT32_MAX || strlen(properties[i].value) > INT32_MAX)
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

az_iot_result az_iot_adu__validate_installed_update_id(const az_iot_adu_report_update_id* id)
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
  if (strlen(id->provider) > INT32_MAX || strlen(id->name) > INT32_MAX
      || strlen(id->version) > INT32_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  return AZ_IOT_OK;
}

size_t az_iot_adu__compatibility_properties(
    const az_iot_adu_device_properties* properties,
    az_iot_adu_custom_property* out)
{
  size_t count = 0;
  if (properties->manufacturer != NULL)
  {
    out[count++] = (az_iot_adu_custom_property){ "manufacturer", properties->manufacturer };
  }
  if (properties->model != NULL)
  {
    out[count++] = (az_iot_adu_custom_property){ "model", properties->model };
  }
  for (size_t i = 0; i < properties->custom_properties_count; ++i)
  {
    out[count++] = properties->custom_properties[i];
  }
  return count;
}

static const char* pack_string(az_iot_span_writer* writer, char* strings, const char* value)
{
  if (value == NULL)
  {
    return NULL;
  }
  const char* stored = strings + az_iot_span_writer_length(writer);
  az_iot_span_writer_append_str(writer, value);
  az_iot_span_writer_append_u8(writer, 0);
  return stored;
}

az_iot_result az_iot_adu__prepare_device_properties(
    const az_iot_adu_device_properties* properties,
    az_iot_adu_device_properties_snapshot* snapshot)
{
  if (properties == NULL || snapshot == NULL
      || (properties->custom_properties_count > 0 && properties->custom_properties == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t builtins
      = (properties->manufacturer != NULL ? 1u : 0u) + (properties->model != NULL ? 1u : 0u);
  if (properties->custom_properties_count > AZ_IOT_ADU_MAX_COMPATIBILITY_PROPERTIES - builtins)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  az_iot_adu_custom_property compatibility[AZ_IOT_ADU_MAX_COMPATIBILITY_PROPERTIES];
  size_t count = az_iot_adu__compatibility_properties(properties, compatibility);
  az_iot_result r = az_iot_adu__validate_compatibility_properties(compatibility, count);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  az_iot_adu_report_update_id id = { properties->installed_update_id.provider,
                                     properties->installed_update_id.name,
                                     properties->installed_update_id.version };
  r = az_iot_adu__validate_installed_update_id(
      id.provider == NULL && id.name == NULL && id.version == NULL ? NULL : &id);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  az_iot_adu_device_properties* out = &snapshot->properties;
  az_iot_span_writer writer;
  az_iot_span_writer_init(
      &writer, az_span_create((uint8_t*)snapshot->strings, AZ_IOT_ADU_COMPATIBILITY_STORAGE_SIZE));
  out->manufacturer = pack_string(&writer, snapshot->strings, properties->manufacturer);
  out->model = pack_string(&writer, snapshot->strings, properties->model);
  out->custom_properties_count = properties->custom_properties_count;
  out->custom_properties = out->custom_properties_count > 0 ? snapshot->custom_properties : NULL;
  for (size_t i = 0; i < out->custom_properties_count; ++i)
  {
    snapshot->custom_properties[i].name
        = pack_string(&writer, snapshot->strings, properties->custom_properties[i].name);
    snapshot->custom_properties[i].value
        = pack_string(&writer, snapshot->strings, properties->custom_properties[i].value);
  }
  r = az_iot_span_writer_end(&writer, NULL);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  size_t compatibility_size = az_iot_span_writer_length(&writer);
  char* installed = snapshot->strings + compatibility_size;
  az_iot_span_writer_init(
      &writer, az_span_create((uint8_t*)installed, AZ_IOT_ADU_INSTALLED_ID_STORAGE_SIZE));
  out->installed_update_id.provider = pack_string(&writer, installed, id.provider);
  out->installed_update_id.name = pack_string(&writer, installed, id.name);
  out->installed_update_id.version = pack_string(&writer, installed, id.version);
  r = az_iot_span_writer_end(&writer, NULL);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  snapshot->strings_size = compatibility_size + az_iot_span_writer_length(&writer);
  return AZ_IOT_OK;
}

static const char* rebase_string(
    const az_iot_adu_device_properties_snapshot* snapshot,
    char* strings,
    const char* value)
{
  return value != NULL ? strings + (value - snapshot->strings) : NULL;
}

void az_iot_adu__commit_device_properties(
    const az_iot_adu_device_properties_snapshot* snapshot,
    az_iot_adu_device_properties* properties,
    az_iot_adu_custom_property* custom_properties,
    char* strings)
{
  memcpy(strings, snapshot->strings, snapshot->strings_size);
  *properties = snapshot->properties;
  properties->manufacturer = rebase_string(snapshot, strings, properties->manufacturer);
  properties->model = rebase_string(snapshot, strings, properties->model);
  properties->installed_update_id.provider
      = rebase_string(snapshot, strings, properties->installed_update_id.provider);
  properties->installed_update_id.name
      = rebase_string(snapshot, strings, properties->installed_update_id.name);
  properties->installed_update_id.version
      = rebase_string(snapshot, strings, properties->installed_update_id.version);
  for (size_t i = 0; i < properties->custom_properties_count; ++i)
  {
    custom_properties[i].name
        = rebase_string(snapshot, strings, snapshot->custom_properties[i].name);
    custom_properties[i].value
        = rebase_string(snapshot, strings, snapshot->custom_properties[i].value);
  }
  properties->custom_properties
      = properties->custom_properties_count > 0 ? custom_properties : NULL;
}
