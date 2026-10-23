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

/** @brief Bound on the overall result detail carried in a structured report. */
#ifndef AZ_IOT_SU_RESULT_DETAILS_SIZE
#define AZ_IOT_SU_RESULT_DETAILS_SIZE 256
#endif

  /** @brief A report built from the engine state, with the strings it points at. */
  typedef struct az_iot_su__report_view
  {
    az_iot_su_report report;
    char workflow_id[AZ_IOT_SU_WORKFLOW_ID_SIZE + 1];
    char extended[16];
    char details[AZ_IOT_SU_RESULT_DETAILS_SIZE];
    az_iot_su_report_update_id installed;
  } az_iot_su__report_view;

  /**
   * @brief Build the report the engine would send now into @p v.
   *
   * @return false when there is no active workflow, so nothing to report.
   */
  bool az_iot_su__build_report_view(const az_iot_su_client* client, az_iot_su__report_view* v);

  /**
   * @brief The terminal report last handed to the channel, rebuilt from its snapshot.
   *
   * @param[in] client The client.
   * @param[out] out The report; its strings point into the client.
   * @param[out] installed Storage for its installed update id.
   * @return false when no terminal report is kept.
   */
  bool az_iot_su__sent_terminal_report(
      const az_iot_su_client* client,
      az_iot_su_report* out,
      az_iot_su_report_update_id* installed);

  /* Assemble the current engine state into a structured az_iot_su_report and
   * hand it to the bound channel. Reporting is keyed on the active workflow id
   * and is idempotent on it; with no active workflow this is a no-op success,
   * because there is nothing for the service to attribute a report to.
   * Returns AZ_IOT_OK when the channel accepted the report. */
  az_iot_result az_iot_su__report_state(az_iot_su_client* client);

  /* Internal entry point: bind the engine to an explicit channel and crypto
   * backend. The public az_iot_su_client_init() builds the shipping channel,
   * takes the connection client's backend and calls this. Kept internal so
   * the engine can be exercised against a fake channel. */
  az_iot_result az_iot_su_client__initialize_with_channel(
      az_iot_su_client* client,
      const az_iot_su_channel* channel,
      const az_iot_crypto* crypto,
      const az_iot_su_client_config_options* options);
#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SU_INTERNAL_H */
