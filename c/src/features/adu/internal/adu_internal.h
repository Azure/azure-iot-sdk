// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Internal declarations shared between the ADU core state machine
 * (adu_client.c) and the structured reporting module (adu_report.c). NOT part
 * of the public API. */
#ifndef AZ_IOT_ADU_INTERNAL_H
#define AZ_IOT_ADU_INTERNAL_H

#include "azure/iot/az_iot_adu.h"

#include "adu_channel_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Internal accessor shorthand. */
#define ADU_I(c) ((c)->_internal)

  az_iot_result az_iot_adu__validate_install_result(const az_iot_adu_install_result* result);

  bool az_iot_adu__valid_result_fields(
      az_iot_adu_outcome outcome,
      az_iot_adu_failure_origin origin,
      const uint8_t* extended,
      int32_t extended_length,
      const uint8_t* details,
      int32_t details_length);

  void az_iot_adu__set_extended_result(az_span destination, int32_t* length, uint32_t code);

  /* Assemble the current engine state into a structured az_iot_adu_report and
   * hand it to the bound channel. Reporting is keyed on the active workflow id
   * and is idempotent on it; with no active workflow this is a no-op success,
   * because there is nothing for the service to attribute a report to.
   * Returns AZ_IOT_OK when the channel accepted the report. */
  az_iot_result az_iot_adu__report_state(az_iot_adu_client_t* client);

  /* Deep-copy the caller's device properties into the client-owned cache buffer.
   * Lays out NUL-terminated strings packed into device_props_buffer and points the
   * cache descriptor at them. Returns AZ_IOT_ERR_NOT_ENOUGH_SPACE if the buffer is
   * too small, AZ_IOT_ERR_INVALID_ARG on bad input. */
  az_iot_result az_iot_adu__cache_device_properties(
      az_iot_adu_client_t* client,
      const az_iot_adu_device_properties* device_props);

  /* Internal entry point: bind the engine to an explicit channel. The public
   * az_iot_adu_client_initialize() builds the shipping channel and calls this.
   * Kept internal so the engine can be exercised against a fake channel. */
  az_iot_result az_iot_adu_client__initialize_with_channel(
      az_iot_adu_client_t* client,
      const az_iot_adu_channel* channel,
      const az_iot_adu_client_config_options* options);
#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADU_INTERNAL_H */
