// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Internal declarations shared between the ADU core state machine
 * (adu_client.c) and the state reporter (adu_state_reporter.c). NOT part of the
 * public API. */
#ifndef AZ_IOT_ADU_INTERNAL_H
#define AZ_IOT_ADU_INTERNAL_H

#include "azure/iot/az_iot_adu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal accessor shorthand. */
#define ADU_I(c) ((c)->_internal)

/* Map an internal fine-grained state to the protocol-defined agent state
 * (0=Idle, 6=DeploymentInProgress, 255=Failed). */
az_iot_adu_client_agent_state az_iot_adu__agent_state(az_iot_adu_state state);

/* Format and publish the current agent state to the twin reported properties.
 * Uses az_iot_adu_client_get_agent_state_payload() to build the JSON and
 * az_iot_twin_client_patch_reported() to publish. workflow may be NULL when no
 * deployment is in progress; install_result may be NULL when no result yet.
 * Returns AZ_IOT_OK on a successful publish enqueue. */
az_iot_result az_iot_adu__report_state(az_iot_adu_client* client);

/* Deep-copy the caller's device properties into the client-owned cache buffer.
 * Lays out NUL-terminated strings packed into device_props_buffer and points the
 * cache descriptor at them. Returns AZ_IOT_ERR_NOT_ENOUGH_SPACE if the buffer is
 * too small, AZ_IOT_ERR_INVALID_ARG on bad input. */
az_iot_result az_iot_adu__cache_device_properties(
    az_iot_adu_client* client,
    const az_iot_adu_device_properties* device_props);

/* Build an az_iot_adu_client_device_properties (az_span view over the cache)
 * from the client's cached device properties, for handing to the upstream
 * formatter. The returned spans point into device_props_buffer. */
az_iot_adu_client_device_properties az_iot_adu__device_properties_view(
    const az_iot_adu_client* client);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADU_INTERNAL_H */
