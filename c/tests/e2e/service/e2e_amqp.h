// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Internal helper for the e2e service client: the AMQP 1.0 paths (telemetry
 * receive from the Event Hub-compatible endpoint, and cloud-to-device send to
 * the IoT Hub service endpoint), built on the vendored `az_amqp` library and
 * its sample reference transport.
 *
 * This header is INTERNAL to the az_iot_e2e_service static library. Test
 * translation units never include it, so the `az_amqp` dependency stays behind
 * the service-client facade.
 */
#ifndef AZ_IOT_E2E_AMQP_H
#define AZ_IOT_E2E_AMQP_H

#include <azure/az_amqp.h>

#include "az_amqp_sample_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define E2E_AMQP_MAX_PARTITIONS 32
#define E2E_AMQP_CAPTURE_MAX 16
#define E2E_AMQP_CAPTURE_BODY_MAX 1024

/* A telemetry watcher: an AMQP connection to the IoT Hub Event Hub-compatible
 * endpoint with one receiver link per partition. Received message bodies are
 * captured into a small ring so the test can poll for a correlation marker.
 *
 * The struct is large (frame + per-partition buffers) and therefore intended to
 * be heap-allocated as part of the owning service object. */
typedef struct e2e_amqp_telemetry_tag
{
    az_amqp_sample_transport transport_storage;
    az_amqp_transport transport;
    az_amqp_connection connection;
    az_amqp_session session;
    az_amqp_cbs cbs;
    az_amqp_link receivers[E2E_AMQP_MAX_PARTITIONS];
    int partition_count;
    bool started;
    bool connection_failed;

    /* Capture ring of recently received bodies (NUL-terminated for substring search). */
    char captured[E2E_AMQP_CAPTURE_MAX][E2E_AMQP_CAPTURE_BODY_MAX];
    int captured_count;

    /* Backing storage referenced by the az_amqp objects above. */
    uint8_t incoming_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    uint8_t outgoing_buffer[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
    az_amqp_session* session_slots[1];
    az_amqp_link* link_slots[E2E_AMQP_MAX_PARTITIONS + 2];
    uint8_t cbs_reply_buffer[1024];
    uint8_t recv_buffers[E2E_AMQP_MAX_PARTITIONS][2048];
    char source_addr[E2E_AMQP_MAX_PARTITIONS][192];
    char link_name[E2E_AMQP_MAX_PARTITIONS][32];
    char audience_buffer[256];
} e2e_amqp_telemetry_t;

/* Connect to @p eh_host:5671, CBS-authorize @p sas_token against the entity, and
 * attach one earliest-position receiver to each of @p partition_count partitions.
 * On failure, returns false and (when non-NULL) points @p err_out at a static
 * message. */
bool e2e_amqp_telemetry_begin(
    e2e_amqp_telemetry_t* t,
    const char* eh_host,
    const char* entity_path,
    const char* sas_token,
    int partition_count,
    const char** err_out);

/* Pump the telemetry connection once, waiting up to @p wait_ms for socket I/O.
 * Returns false if the connection has failed. */
bool e2e_amqp_telemetry_pump(e2e_amqp_telemetry_t* t, int wait_ms);

/* Returns true if any captured telemetry body contains @p needle. */
bool e2e_amqp_telemetry_seen(const e2e_amqp_telemetry_t* t, const char* needle);

/* Detach receivers and close the telemetry connection (best-effort). */
void e2e_amqp_telemetry_end(e2e_amqp_telemetry_t* t);

/* Send one cloud-to-device message to @p device_id via the IoT Hub service AMQP
 * endpoint (@p hub_host:5671), authorizing with @p sas_token (audience = hub
 * host). Blocks (pumping internally) until IoT Hub accepts the message or an
 * error/timeout occurs. Opens and tears down its own short-lived connection.
 * Returns true only when the delivery outcome is `accepted`. */
bool e2e_amqp_send_c2d(
    const char* hub_host,
    const char* sas_token,
    const char* device_id,
    const uint8_t* payload,
    size_t payload_len,
    const char** err_out);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_E2E_AMQP_H */
