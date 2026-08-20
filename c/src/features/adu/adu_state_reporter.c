// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* ADU state reporter: formats the agent state into a twin reported-property
 * payload (via azure-sdk-for-c) and publishes it through the twin client. The
 * upstream az_iot_adu_client_device_properties type never escapes to the
 * application; it is built here, on demand, from the client-owned cache. */
#include <string.h>

#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_adu.h"
#include "azure/iot/az_iot_twin_client.h"

#include "internal/adu_internal.h"
#include "internal/span_writer.h"

/* Reported-property payload buffer. v5 manifests with the upper-bounded step
 * count fit comfortably; bounded and stack-local, no heap. */
#ifndef AZ_IOT_ADU_REPORT_BUFFER_SIZE
#define AZ_IOT_ADU_REPORT_BUFFER_SIZE 1024
#endif

az_iot_adu_client_agent_state az_iot_adu__agent_state(az_iot_adu_state state)
{
  /* The service knows three agent states; the workflow has twelve. Every one is
   * listed rather than folded into default:, so that adding a workflow state
   * forces a decision about how the service should see it -- the previous
   * default: would silently have reported it as in-progress, including for a
   * state that was actually terminal. */
  switch (state)
  {
    case AZ_IOT_ADU_STATE_IDLE:
      return AZ_IOT_ADU_CLIENT_AGENT_STATE_IDLE;

    case AZ_IOT_ADU_STATE_FAILED:
      return AZ_IOT_ADU_CLIENT_AGENT_STATE_FAILED;

    case AZ_IOT_ADU_STATE_MANIFEST_RECEIVED:
    case AZ_IOT_ADU_STATE_VERIFYING_MANIFEST:
    case AZ_IOT_ADU_STATE_DOWNLOAD_STARTED:
    case AZ_IOT_ADU_STATE_DOWNLOAD_COMPLETE:
    case AZ_IOT_ADU_STATE_BACKUP_STARTED:
    case AZ_IOT_ADU_STATE_BACKUP_COMPLETE:
    case AZ_IOT_ADU_STATE_INSTALL_STARTED:
    case AZ_IOT_ADU_STATE_INSTALL_COMPLETE:
    case AZ_IOT_ADU_STATE_APPLY_STARTED:
    case AZ_IOT_ADU_STATE_RESTORE_STARTED:
      return AZ_IOT_ADU_CLIENT_AGENT_STATE_DEPLOYMENT_IN_PROGRESS;

    default:
      /* A value from outside the enum: report the deployment as still running
       * rather than inventing a terminal outcome for it. */
      return AZ_IOT_ADU_CLIENT_AGENT_STATE_DEPLOYMENT_IN_PROGRESS;
  }
}

az_iot_adu_client_device_properties az_iot_adu__device_properties_view(
    const az_iot_adu_client_t* client)
{
  az_iot_adu_client_device_properties props = az_iot_adu_client_device_properties_default();

  /* The cache layout (see az_iot_adu__cache_device_properties) packs
   * NUL-terminated manufacturer/model/installed-update-id strings into the
   * caller's buffer. We rebuild az_span views over those C strings here. */
  const az_iot_adu_device_properties* cached
      = (const az_iot_adu_device_properties*)(const void*)ADU_I(client).device_props_buffer;

  if (ADU_I(client).device_props_buffer != NULL)
  {
    if (cached->manufacturer != NULL)
    {
      props.manufacturer = az_span_create_from_str((char*)(uintptr_t)cached->manufacturer);
    }
    if (cached->model != NULL)
    {
      props.model = az_span_create_from_str((char*)(uintptr_t)cached->model);
    }
  }

  /* The ADU service requires a non-empty serialized installed-update-id; the
   * upstream formatter has a precondition on it. Built once at initialize. */
  if (ADU_I(client).update_id_json_len > 0)
  {
    props.update_id = az_span_create(
        (uint8_t*)(uintptr_t)ADU_I(client).update_id_json,
        (int32_t)ADU_I(client).update_id_json_len);
  }

  props.adu_version = AZ_SPAN_FROM_STR(AZ_IOT_ADU_CLIENT_AGENT_VERSION);

  /* Hand the cached custom-property view (built at cache time) to the
   * formatter so they are emitted for deployment compatibility checks. */
  if (ADU_I(client).custom_props_view.count > 0)
  {
    props.custom_properties
        = (az_iot_adu_device_custom_properties*)(uintptr_t)&ADU_I(client).custom_props_view;
  }

  return props;
}

az_iot_result az_iot_adu__report_state(az_iot_adu_client_t* client)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (ADU_I(client).detached)
  {
    return AZ_IOT_ERR_DETACHED;
  }

  uint8_t buffer[AZ_IOT_ADU_REPORT_BUFFER_SIZE];
  az_json_writer jw;
  if (az_result_failed(az_json_writer_init(&jw, az_span_create(buffer, sizeof(buffer)), NULL)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_iot_adu_client_device_properties props = az_iot_adu__device_properties_view(client);

  /* Provide the workflow only when a deployment is in progress. */
  az_iot_adu_client_workflow* workflow = NULL;
  if (ADU_I(client).have_request)
  {
    workflow = &ADU_I(client).current_request.workflow;
  }

  /* Provide the accumulated install result once we have steps tracked. */
  az_iot_adu_client_install_result* result = NULL;
  if (ADU_I(client).install_result.step_results_count > 0)
  {
    result = &ADU_I(client).install_result;
  }

  az_result ar = az_iot_adu_client_get_agent_state_payload(
      &ADU_I(client).az,
      &props,
      az_iot_adu__agent_state(ADU_I(client).state),
      workflow,
      result,
      &jw);
  if (az_result_failed(ar))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_span payload = az_json_writer_get_bytes_used_in_destination(&jw);

  return az_iot_twin_client_patch_reported(
      ADU_I(client).twin, az_span_ptr(payload), (size_t)az_span_size(payload), NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* agent core-library API: standalone report builder                         */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_adu_build_report(
    const az_iot_adu_device_properties* device_props,
    const az_iot_adu_client_install_result* result,
    const az_iot_adu_client_update_request* request,
    az_iot_adu_state state,
    uint8_t* out_json,
    size_t out_size,
    size_t* out_len)
{
  if (device_props == NULL || out_json == NULL || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Stateless upstream formatter handle (no twin / state machine). */
  az_iot_adu_client az;
  if (az_result_failed(az_iot_adu_client_init(&az, NULL)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  /* Serialize the installed-update-id object the service expects. */
  char update_id_json[128];
  size_t update_id_json_len = 0;
  const char* prov = device_props->installed_update_id.provider
      ? device_props->installed_update_id.provider
      : "";
  const char* name
      = device_props->installed_update_id.name ? device_props->installed_update_id.name : "";
  const char* ver
      = device_props->installed_update_id.version ? device_props->installed_update_id.version : "";
  const char* update_id_parts[]
      = { "{\"provider\":\"", prov, "\",\"name\":\"", name, "\",\"version\":\"", ver, "\"}" };
  if (az_iot_span_writer_build_str(
          AZ_SPAN_FROM_BUFFER(update_id_json), &update_id_json_len, update_id_parts, 7)
      != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  az_iot_adu_client_device_properties props = az_iot_adu_client_device_properties_default();
  if (device_props->manufacturer != NULL)
  {
    props.manufacturer = az_span_create_from_str((char*)(uintptr_t)device_props->manufacturer);
  }
  if (device_props->model != NULL)
  {
    props.model = az_span_create_from_str((char*)(uintptr_t)device_props->model);
  }
  props.update_id = az_span_create((uint8_t*)update_id_json, (int32_t)update_id_json_len);
  props.adu_version = AZ_SPAN_FROM_STR(AZ_IOT_ADU_CLIENT_AGENT_VERSION);

  /* Custom properties (az_span views over the caller's strings; read-only for
   * the duration of this call). Clamped to the upstream array capacity. */
  az_iot_adu_device_custom_properties cprops;
  memset(&cprops, 0, sizeof(cprops));
  if (device_props->custom_properties != NULL && device_props->custom_properties_count > 0)
  {
    const size_t max_cp = sizeof(cprops.names) / sizeof(cprops.names[0]);
    size_t count = device_props->custom_properties_count;
    if (count > max_cp)
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    for (size_t i = 0; i < count; ++i)
    {
      if (device_props->custom_properties[i].name == NULL
          || device_props->custom_properties[i].value == NULL)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
      cprops.names[i]
          = az_span_create_from_str((char*)(uintptr_t)device_props->custom_properties[i].name);
      cprops.values[i]
          = az_span_create_from_str((char*)(uintptr_t)device_props->custom_properties[i].value);
    }
    cprops.count = (int32_t)count;
    props.custom_properties = &cprops;
  }

  /* Report the workflow id only when a deployment is in progress. */
  az_iot_adu_client_workflow* workflow = NULL;
  if (request != NULL && az_span_size(request->workflow.id) > 0)
  {
    workflow = &((az_iot_adu_client_update_request*)(uintptr_t)request)->workflow;
  }

  az_json_writer jw;
  if (az_result_failed(az_json_writer_init(&jw, az_span_create(out_json, (int32_t)out_size), NULL)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_result ar = az_iot_adu_client_get_agent_state_payload(
      &az,
      &props,
      az_iot_adu__agent_state(state),
      workflow,
      (az_iot_adu_client_install_result*)(uintptr_t)result,
      &jw);
  if (az_result_failed(ar))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE; /* destination too small for the payload */
  }

  az_span payload = az_json_writer_get_bytes_used_in_destination(&jw);
  if (out_len != NULL)
  {
    *out_len = (size_t)az_span_size(payload);
  }
  return AZ_IOT_OK;
}
