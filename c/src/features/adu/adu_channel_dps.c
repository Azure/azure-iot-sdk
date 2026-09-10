// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* The shipping device-update channel.
 *
 * This is the transport the SDK builds for the application when it calls
 * az_iot_adu_client_initialize() with its connection client. The application
 * supplies only an HTTPS send primitive; everything above it -- URL
 * construction, authentication, request bodies, response parsing, the ETag
 * round-trip, retry and error classification -- belongs here, so that no
 * application has to re-implement the protocol.
 *
 * STATUS: the channel is wired end to end (lifecycle, engine binding, transport
 * and connection ownership) but the two wire operations are NOT implemented yet
 * and return AZ_IOT_ERR_NOT_SUPPORTED. They are the remaining work, and they are
 * gated on two decisions that are not settled:
 *
 *   - the device credential for these calls (the measured reference derives a
 *     SAS from an enrollment-group key; the connection client is X.509-only);
 *   - confirmation of the error-code and ETag semantics, which the reference
 *     material does not currently exercise.
 *
 * Nothing here fabricates a result: an unimplemented operation says so rather
 * than reporting success and silently doing nothing.
 */

#include <string.h>

#include "azure/iot/az_iot_adu.h"

#include "internal/adu_channel_internal.h"
#include "internal/log_internal.h"

static az_iot_result channel_open(void* ctx, az_iot_adu_channel_update_cb cb, void* engine_ctx)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL || cb == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  c->update_cb = cb;
  c->engine_ctx = engine_ctx;
  return AZ_IOT_OK;
}

static void channel_close(void* ctx)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL)
  {
    return;
  }
  c->update_cb = NULL;
  c->engine_ctx = NULL;
}

/* Device-initiated update check. Not implemented yet; see the file header. */
static az_iot_result channel_request_update(void* ctx)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return AZ_IOT_ERR_NOT_SUPPORTED;
}

/* Status report for a workflow. Not implemented yet; see the file header. */
static az_iot_result channel_report(void* ctx, const az_iot_adu_report* report)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL || report == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return AZ_IOT_ERR_NOT_SUPPORTED;
}

static const az_iot_adu_channel_vtable k_channel_vtable = {
  .open = channel_open,
  .close = channel_close,
  .request_update = channel_request_update,
  .report = channel_report,
  .do_work = NULL,
};

az_iot_result az_iot_adu_channel_dps_init(
    az_iot_adu_channel_dps* channel_state,
    az_iot_connection_client* connection,
    az_iot_adu_channel* out_channel)
{
  if (channel_state == NULL || connection == NULL || out_channel == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(channel_state, 0, sizeof(*channel_state));
  channel_state->connection = connection;

  out_channel->vtable = &k_channel_vtable;
  out_channel->ctx = channel_state;
  return AZ_IOT_OK;
}
