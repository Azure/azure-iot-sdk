// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file az_iot_adu_channel.h
 * @brief The transport seam between the ADU engine (`adu_core`) and whatever
 *        carries its update requests and status reports.
 *
 * The engine names no transport. It receives an update payload through a
 * channel and hands back a *structured* result; turning that result into a wire
 * representation is the channel's job alone. This is what lets the engine be
 * exercised against a fake channel with no MQTT, no HTTP and no service.
 *
 * A channel is not "another hub feature". The shipping channel (ADUv2) hangs
 * off the provisioning path, and its bootstrap leg runs *before* the device is
 * provisioned at all, so nothing here may assume a connected hub session
 * underneath it.
 */

#ifndef AZ_IOT_ADU_CHANNEL_H
#define AZ_IOT_ADU_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Terminal and non-terminal outcomes a device reports for a workflow.
   *
   * These are the values the service accepts on the status-report operation.
   * `SKIPPED` replaces the ADUv1 accept/reject acknowledgement: an engine that
   * declines a deployment reports it rather than answering a protocol-level
   * "reject".
   */
  typedef enum az_iot_adu_outcome
  {
    AZ_IOT_ADU_OUTCOME_IN_PROGRESS = 0,
    AZ_IOT_ADU_OUTCOME_SUCCEEDED,
    AZ_IOT_ADU_OUTCOME_FAILED,
    AZ_IOT_ADU_OUTCOME_CANCELED,
    AZ_IOT_ADU_OUTCOME_SKIPPED,
  } az_iot_adu_outcome;

  /**
   * @brief Which layer a failure came from. `NOT_APPLICABLE` is used for every
   *        non-failure outcome.
   */
  typedef enum az_iot_adu_failure_origin
  {
    AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE = 0,
    AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE,
    AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_EXTENSION,
    AZ_IOT_ADU_FAILURE_ORIGIN_SERVICE,
  } az_iot_adu_failure_origin;

  /** @brief An update identity triple. Spans are NOT owned by this struct. */
  typedef struct az_iot_adu_report_update_id
  {
    const char* provider;
    const char* name;
    const char* version;
  } az_iot_adu_report_update_id;

  /**
   * @brief The structured result the engine hands a channel.
   *
   * `workflow_id` alone is the correlation key: reporting is idempotent on it,
   * and the ADUv1 `retryTimestamp` half of the old composite key does not exist
   * here.
   *
   * `installed_update_id` means "what is installed on the device *now*", not
   * "what this workflow is about". It is therefore the previously installed
   * update while a workflow is in progress or has failed, and the newly applied
   * update once the workflow has succeeded. It may be NULL when the device has
   * nothing installed (a day-0 onboarding device), in which case the channel
   * omits it rather than serializing a null.
   */
  typedef struct az_iot_adu_report
  {
    const char* workflow_id;

    /* NULL when the device has nothing installed. */
    const az_iot_adu_report_update_id* installed_update_id;

    az_iot_adu_outcome outcome;
    az_iot_adu_failure_origin failure_origin;

    /* Agent result code. The engine emits the values the contract defines:
     * 1 while in progress, 700 on success, negative on failure. */
    int32_t result_code;

    /* Comma-separated hex codes, e.g. "00000000" or "0x80000001". Never NULL. */
    const char* extended_result_codes;

    /* Free-form human-readable detail. May be NULL. */
    const char* result_details;
  } az_iot_adu_report;

  /**
   * @brief Invoked by a channel when an update payload has been delivered.
   *
   * @param update_payload      The raw, channel-specific update document. The
   *                            engine parses and verifies it; the channel does
   *                            not interpret it. Valid only for the duration of
   *                            the call — the engine copies what it needs.
   * @param update_payload_len  Length of @p update_payload in bytes.
   * @param engine_ctx          The context the engine passed to `open()`.
   */
  typedef void (*az_iot_adu_channel_update_cb)(
      const uint8_t* update_payload,
      size_t update_payload_len,
      void* engine_ctx);

  /**
   * @brief The delivery + reporting vtable.
   *
   * Every function takes the channel's own @p ctx. All are REQUIRED except
   * where noted; the engine validates them at initialization.
   */
  typedef struct az_iot_adu_channel_vtable
  {
    /**
     * @brief Bind the channel to the engine and begin delivery.
     *
     * For a push channel this is where a subscription is established. For a
     * pull channel it need only record the callback. Called once, from
     * az_iot_adu_client_initialize().
     */
    az_iot_result (*open)(void* ctx, az_iot_adu_channel_update_cb cb, void* engine_ctx);

    /**
     * @brief Unbind. Best-effort; the engine ignores the result during
     *        teardown. Called once, from az_iot_adu_client_destroy().
     */
    void (*close)(void* ctx);

    /**
     * @brief Ask the channel to check for an update now.
     *
     * A pull channel performs its fetch. A push channel may treat this as a
     * proactive get. The channel reports the outcome by invoking the update
     * callback; "no update available" is a success and simply means the
     * callback is not invoked.
     */
    az_iot_result (*request_update)(void* ctx);

    /**
     * @brief Deliver a structured status report.
     *
     * The channel owns serialization and any durability guarantee. @p report
     * and everything it points at are valid only for the duration of the call.
     */
    az_iot_result (*report)(void* ctx, const az_iot_adu_report* report);

    /**
     * @brief OPTIONAL. Driven from the engine's do_work() tick so a channel
     *        with its own asynchronous work has somewhere to run. May be NULL.
     */
    az_iot_result (*do_work)(void* ctx);
  } az_iot_adu_channel_vtable;

  /** @brief A channel instance: its vtable plus its own context. */
  typedef struct az_iot_adu_channel
  {
    const az_iot_adu_channel_vtable* vtable;
    void* ctx;
  } az_iot_adu_channel;

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADU_CHANNEL_H */
