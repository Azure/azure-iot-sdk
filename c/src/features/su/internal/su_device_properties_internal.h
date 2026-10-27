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

/**
 * @brief Longest string passed to the JSON writer.
 *
 * az_core's writer asserts on strings whose escaped form (up to six bytes per
 * input byte) exceeds 1,000,000,000 bytes; longer strings are rejected first.
 */
#define AZ_IOT_SU_MAX_JSON_STRING_SIZE (1000000000 / 6)

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
   * @brief Validates @p properties and measures their strings.
   *
   * @param[in] properties Properties to check.
   * @param[out] out_strings_size Bytes az_iot_su__copy_device_properties() writes, NUL
   *   terminators included.
   * @return AZ_IOT_OK, AZ_IOT_ERR_INVALID_ARG for malformed input, or
   *   AZ_IOT_ERR_NOT_ENOUGH_SPACE for more than AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES
   *   compatibility properties, a string over AZ_IOT_SU_MAX_JSON_STRING_SIZE, or an installed
   *   update ID over AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE.
   */
  az_iot_result az_iot_su__measure_device_properties(
      const az_iot_su_device_properties* properties,
      size_t* out_strings_size);

  /**
   * @brief Whether a string of @p properties lies in @p buffer.
   *
   * @param[in] properties Validated properties.
   * @param[in] buffer Destination of a copy.
   * @param[in] buffer_size Size of @p buffer.
   * @return true if copying @p properties into @p buffer could overwrite them.
   */
  bool az_iot_su__device_properties_overlap(
      const az_iot_su_device_properties* properties,
      const char* buffer,
      size_t buffer_size);

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
   * @brief Copies @p properties, packing their strings into @p strings.
   *
   * @param[in] properties Validated by az_iot_su__measure_device_properties().
   * @param[out] strings At least the measured size; no string of @p properties may lie in it
   *   (see az_iot_su__device_properties_overlap()).
   * @param[out] copy The copy; its strings point into @p strings. May be @p properties.
   * @param[out] custom_properties AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES entries, referenced by
   *   @p copy.
   */
  void az_iot_su__copy_device_properties(
      const az_iot_su_device_properties* properties,
      char* strings,
      az_iot_su_device_properties* copy,
      az_iot_su_custom_property* custom_properties);

#ifdef __cplusplus
}
#endif

#endif
