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

  /* Map an internal fine-grained state to the protocol-defined agent state
   * (0=Idle, 6=DeploymentInProgress, 255=Failed). */
  az_iot_adu_client_agent_state az_iot_adu__agent_state(az_iot_adu_state state);

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

  /* Build an az_iot_adu_client_device_properties (az_span view over the cache)
   * from the client's cached device properties, for handing to the upstream
   * formatter. The returned spans point into device_props_buffer. */
  az_iot_adu_client_device_properties az_iot_adu__device_properties_view(
      const az_iot_adu_client_t* client);

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
