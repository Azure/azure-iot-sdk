// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file adu_channel_internal.h
 * @brief INTERNAL. The transport seam between the ADU engine and whatever
 *        carries its update requests and status reports.
 *
 * NOT part of the public API. Applications do not implement this and do not see
 * it: they hand az_iot_adu_client_initialize() a connection client, and the SDK
 * builds the shipping channel itself. Putting the wire protocol behind an
 * application-supplied vtable would mean every application re-implemented the
 * device-update operations, their authentication and their error handling --
 * which is the SDK's job, not the application's.
 *
 * The seam exists for three internal reasons:
 *   1. the engine must name no transport, so a delivery mechanism can be
 *      replaced without touching the download/verify/install state machine;
 *   2. the bootstrap leg runs BEFORE the device is provisioned, so nothing here
 *      may assume a connected hub session underneath it;
 *   3. it lets the engine be exercised against a fake channel with no MQTT, no
 *      HTTP and no service.
 */

#ifndef AZ_IOT_ADU_CHANNEL_INTERNAL_H
#define AZ_IOT_ADU_CHANNEL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_adu.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* az_iot_adu_outcome / az_iot_adu_failure_origin / az_iot_adu_report_update_id
   * / az_iot_adu_report are declared in the public header: an application sees
   * the structured result, it just does not carry it. */

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

  /* --- The shipping channel ------------------------------------------------ */
  /*
   * State for the device-update channel the SDK builds from the application's
   * connection client. Lives inside az_iot_adu_client_t, so it is caller
   * allocated and needs no heap.
   */
  typedef struct az_iot_adu_channel_dps
  {
    az_iot_connection_client* connection;
    az_iot_adu_http_transport http;
    az_iot_adu_channel_update_cb update_cb;
    void* engine_ctx;
  } az_iot_adu_channel_dps;

  /* Bind the channel to a connection and an HTTPS transport and emit the vtable
   * pair the engine binds to. */
  az_iot_result az_iot_adu_channel_dps_init(
      az_iot_adu_channel_dps* channel_state,
      az_iot_connection_client* connection,
      const az_iot_adu_http_transport* http_transport,
      az_iot_adu_channel* out_channel);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADU_CHANNEL_INTERNAL_H */
