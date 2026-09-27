// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

#ifndef AZ_IOT_SU_DEVICE_PROPERTIES_INTERNAL_H
#define AZ_IOT_SU_DEVICE_PROPERTIES_INTERNAL_H

#include "azure/iot/az_iot_su.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Bytes for compatibility-property strings, NUL terminators included. */
#define AZ_IOT_SU_COMPATIBILITY_STORAGE_SIZE 256
/** @brief Bytes for installed-update-ID strings, NUL terminators included. */
#define AZ_IOT_SU_INSTALLED_ID_STORAGE_SIZE 192

/**
 * @brief Longest string passed to the JSON writer.
 *
 * az_core's writer asserts on strings whose escaped form (up to six bytes per
 * input byte) exceeds 1,000,000,000 bytes; longer strings are rejected first.
 */
#define AZ_IOT_SU_MAX_JSON_STRING_SIZE (1000000000 / 6)

  /** @brief Self-contained copy of validated device properties. */
  typedef struct az_iot_su_device_properties_snapshot
  {
    /** Copied properties; strings point into strings. */
    az_iot_su_device_properties properties;
    /** Copied custom properties referenced by properties. */
    az_iot_su_custom_property custom_properties[AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES];
    /** Packed NUL-terminated strings. */
    char strings[AZ_IOT_SU_COMPATIBILITY_STORAGE_SIZE + AZ_IOT_SU_INSTALLED_ID_STORAGE_SIZE];
    size_t strings_size; /**< Bytes used in strings. */
  } az_iot_su_device_properties_snapshot;

  /**
   * @brief Validates the compatibility properties of an update check.
   *
   * @param[in] properties Properties to check.
   * @param[in] count Entries in @p properties.
   * @return AZ_IOT_OK if valid.
   * @retval AZ_IOT_ERR_INVALID_ARG NULL or empty list, empty or duplicate name,
   *   or NULL value.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE More than
   *   AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES entries, or a string over
   *   AZ_IOT_SU_MAX_JSON_STRING_SIZE.
   */
  az_iot_result az_iot_su__validate_compatibility_properties(
      const az_iot_su_custom_property* properties,
      size_t count);

  /**
   * @brief Validates an installed update ID.
   *
   * @param[in] id NULL for nothing installed; otherwise a complete nonempty triple.
   * @return AZ_IOT_OK if valid.
   * @retval AZ_IOT_ERR_INVALID_ARG A part is NULL or empty.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE A part exceeds AZ_IOT_SU_MAX_JSON_STRING_SIZE.
   */
  az_iot_result az_iot_su__validate_installed_update_id(const az_iot_su_report_update_id* id);

  /**
   * @brief Validates @p properties and copies them into @p snapshot.
   *
   * Changes no live state, so a failure leaves the current properties in place.
   *
   * @param[in] properties Caller's properties; must not overlap @p snapshot.
   * @param[out] snapshot Scratch copy.
   * @return AZ_IOT_OK, AZ_IOT_ERR_INVALID_ARG for malformed input, or
   *   AZ_IOT_ERR_NOT_ENOUGH_SPACE if a count or storage limit is exceeded.
   */
  az_iot_result az_iot_su__prepare_device_properties(
      const az_iot_su_device_properties* properties,
      az_iot_su_device_properties_snapshot* snapshot);

  /**
   * @brief Lists the emitted compatibility properties: manufacturer, model, then custom.
   *
   * @param[in] properties Validated properties.
   * @param[out] out AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES entries.
   * @return Entries written.
   */
  size_t az_iot_su__compatibility_properties(
      const az_iot_su_device_properties* properties,
      az_iot_su_custom_property* out);

  /**
   * @brief Copies a prepared snapshot into live storage. Cannot fail.
   *
   * @param[in] snapshot From az_iot_su__prepare_device_properties().
   * @param[out] properties Live properties, repointed at @p strings.
   * @param[out] custom_properties AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES entries.
   * @param[out] strings At least snapshot->strings_size bytes, separate from
   *   @p snapshot.
   */
  void az_iot_su__commit_device_properties(
      const az_iot_su_device_properties_snapshot* snapshot,
      az_iot_su_device_properties* properties,
      az_iot_su_custom_property* custom_properties,
      char* strings);

#ifdef __cplusplus
}
#endif

#endif
