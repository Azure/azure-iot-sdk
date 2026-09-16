// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

#ifndef AZ_IOT_ADU_DEVICE_PROPERTIES_INTERNAL_H
#define AZ_IOT_ADU_DEVICE_PROPERTIES_INTERNAL_H

#include "azure/iot/az_iot_adu.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define AZ_IOT_ADU_COMPATIBILITY_STORAGE_SIZE 256
#define AZ_IOT_ADU_INSTALLED_ID_STORAGE_SIZE 192

  typedef struct az_iot_adu_device_properties_snapshot
  {
    az_iot_adu_device_properties properties;
    az_iot_adu_custom_property custom_properties[AZ_IOT_ADU_MAX_COMPATIBILITY_PROPERTIES];
    char strings[AZ_IOT_ADU_COMPATIBILITY_STORAGE_SIZE + AZ_IOT_ADU_INSTALLED_ID_STORAGE_SIZE];
    size_t strings_size;
  } az_iot_adu_device_properties_snapshot;

  az_iot_result az_iot_adu__validate_compatibility_properties(
      const az_iot_adu_custom_property* properties,
      size_t count);

  /* NULL means nothing installed; a supplied ID must be a complete nonempty triple. */
  az_iot_result az_iot_adu__validate_installed_update_id(const az_iot_adu_report_update_id* id);

  /* Input must not overlap the scratch snapshot. No live state is changed on failure. */
  az_iot_result az_iot_adu__prepare_device_properties(
      const az_iot_adu_device_properties* properties,
      az_iot_adu_device_properties_snapshot* snapshot);

  /* The properties must already be validated; out has MAX_COMPATIBILITY_PROPERTIES slots. */
  size_t az_iot_adu__compatibility_properties(
      const az_iot_adu_device_properties* properties,
      az_iot_adu_custom_property* out);

  /* Commit a prepared snapshot into distinct, preflighted storage: strings has at
   * least snapshot->strings_size bytes and custom_properties has MAX slots. */
  void az_iot_adu__commit_device_properties(
      const az_iot_adu_device_properties_snapshot* snapshot,
      az_iot_adu_device_properties* properties,
      az_iot_adu_custom_property* custom_properties,
      char* strings);

#ifdef __cplusplus
}
#endif

#endif
