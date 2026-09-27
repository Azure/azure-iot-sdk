// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Internal declarations shared between the software updates core state machine
 * (su_client.c) and the structured reporting module (su_report.c). NOT part
 * of the public API. */
#ifndef AZ_IOT_SU_INTERNAL_H
#define AZ_IOT_SU_INTERNAL_H

#include "azure/iot/az_iot_su.h"

#include "su_channel_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Internal accessor shorthand. */
#define SU_I(c) ((c)->_internal)

/* Values for _internal.pending_fetch: which fetch route the application asked
 * for and the channel has not yet accepted. */
#define SU_FETCH_NONE 0u
#define SU_FETCH_ONBOARDING 1u
#define SU_FETCH_REGULAR 2u

  /* Map an internal fine-grained state to the protocol-defined agent state
   * (0=Idle, 6=DeploymentInProgress, 255=Failed). */
  az_iot_su_client_agent_state az_iot_su__agent_state(az_iot_su_state state);

  /* Assemble the current engine state into a structured az_iot_su_report and
   * hand it to the bound channel. Reporting is keyed on the active workflow id
   * and is idempotent on it; with no active workflow this is a no-op success,
   * because there is nothing for the service to attribute a report to.
   * Returns AZ_IOT_OK when the channel accepted the report. */
  az_iot_result az_iot_su__report_state(az_iot_su_client* client);

  /* Internal entry point: bind the engine to an explicit channel. The public
   * az_iot_su_client_initialize() builds the shipping channel and calls this.
   * Kept internal so the engine can be exercised against a fake channel. */
  az_iot_result az_iot_su_client__initialize_with_channel(
      az_iot_su_client* client,
      const az_iot_su_channel* channel,
      const az_iot_su_client_config_options* options);
#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SU_INTERNAL_H */
