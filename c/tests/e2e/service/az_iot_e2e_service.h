// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* az_iot_e2e_service - the service-side half of the in-process end-to-end suite.
 *
 * This facade lets a test act as the cloud side of a scenario (receive device
 * telemetry, send cloud-to-device messages, invoke direct methods, read/patch
 * device twins) WITHOUT exposing the transport it uses. All of the vendored AMQP
 * (`az_amqp`) and HTTPS machinery is confined to the implementation; a test only
 * ever sees the plain-C types below. This keeps the SDK's MQTT-only device
 * client charter intact: AMQP lives strictly in test code, behind this boundary.
 *
 * Configuration is read from the environment on create():
 *   IOTHUB_CONNECTION_STRING            IoT Hub service policy connection string
 *   IOTHUB_EVENTHUB_CONNECTION_STRING   Event Hub-compatible endpoint connection string
 *   IOTHUB_EVENTHUB_LISTEN_NAME         Event Hub entity name (optional; else from the CS)
 *   IOTHUB_EVENTHUB_PARTITION_COUNT     partition count to watch (optional; default 4)
 */
#ifndef AZ_IOT_E2E_SERVICE_H
#define AZ_IOT_E2E_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque service-side client. Heap-allocated so no transport type leaks here. */
typedef struct az_iot_e2e_service az_iot_e2e_service;

/* Create the service client from the environment. On failure returns NULL and,
 * when @p error_out is non-NULL, points it at a static description. */
az_iot_e2e_service* az_iot_e2e_service_create(const char** error_out);

/* Destroy the service client and release any open transport. */
void az_iot_e2e_service_destroy(az_iot_e2e_service* svc);

/* The most recent error description, or NULL. */
const char* az_iot_e2e_service_last_error(const az_iot_e2e_service* svc);

/* ---- Telemetry (AMQP receive, pumped concurrently with the device) -------- */

/* Begin watching the Event Hub-compatible endpoint for device telemetry.
 * Returns false on setup failure (see az_iot_e2e_service_last_error). */
bool az_iot_e2e_service_telemetry_watch_begin(az_iot_e2e_service* svc);

/* Pump the service transport for up to @p timeout_ms. Call this interleaved with
 * the device's own do_work while waiting for telemetry or a file-upload
 * notification. Returns false if a service connection has failed. */
bool az_iot_e2e_service_do_work(az_iot_e2e_service* svc, int timeout_ms);

/* Returns true once a telemetry body containing @p needle has been received. */
bool az_iot_e2e_service_telemetry_seen(const az_iot_e2e_service* svc, const char* needle);

/* Stop watching telemetry and release the underlying connection. Safe to call
 * when not watching. */
void az_iot_e2e_service_telemetry_watch_end(az_iot_e2e_service* svc);

/* ---- File-upload notifications (AMQP receive, pumped like telemetry) ------ */

/* Begin watching the IoT Hub file-upload notification endpoint. IoT Hub posts
 * one notification per upload a device reported as SUCCESSFUL, so this is the
 * only cloud-side proof that a file-upload round trip really completed -- the
 * device itself only sees the hub accept its completion notification.
 *
 * The endpoint is HUB-WIDE: only notifications mentioning @p device_id are
 * consumed, the rest are released back for other watchers. Start watching BEFORE
 * the device notifies completion; notifications are delivered once and are not
 * replayed to a later watcher. Returns false on setup failure (see
 * az_iot_e2e_service_last_error). */
bool az_iot_e2e_service_file_notification_watch_begin(
    az_iot_e2e_service* svc,
    const char* device_id);

/* Returns true once a received notification body contains @p needle (e.g. the
 * blob name). Pump with az_iot_e2e_service_do_work while waiting. */
bool az_iot_e2e_service_file_notification_seen(const az_iot_e2e_service* svc, const char* needle);

/* Delivery counters for the notification watcher. A notification that never
 * arrives looks exactly like one that arrived and was filtered out or could not
 * be decoded, so a test whose wait times out should report these. Any output
 * pointer may be NULL. */
void az_iot_e2e_service_file_notification_stats(
    const az_iot_e2e_service* svc,
    int* out_delivered,
    int* out_captured,
    int* out_released,
    int* out_unparsed);

/* Stop watching file-upload notifications. Safe to call when not watching. */
void az_iot_e2e_service_file_notification_watch_end(az_iot_e2e_service* svc);

/* ---- Cloud-to-device (AMQP send; blocks until IoT Hub accepts) ------------ */

/* Send a cloud-to-device message to @p device_id. Blocks (pumping internally)
 * until IoT Hub accepts the message. Returns false on failure. */
bool az_iot_e2e_service_send_c2d(
    az_iot_e2e_service* svc,
    const char* device_id,
    const uint8_t* payload,
    size_t payload_len);

/* ---- Direct methods & twin (HTTPS REST, non-blocking / pumpable) ---------- */

/* Begin a direct-method invocation on @p device_id. @p json_payload is the raw
 * JSON value to deliver as the method payload (e.g. "\"ping\"" or "{\"a\":1}").
 * Drive to completion with az_iot_e2e_service_request_poll. */
bool az_iot_e2e_service_method_invoke_begin(
    az_iot_e2e_service* svc,
    const char* device_id,
    const char* method_name,
    const char* json_payload);

/* Begin a twin GET for @p device_id. Drive with az_iot_e2e_service_request_poll. */
bool az_iot_e2e_service_twin_get_begin(az_iot_e2e_service* svc, const char* device_id);

/* Begin a twin desired-properties PATCH for @p device_id. @p desired_json is the
 * raw JSON object of desired properties (e.g. "{\"interval\":5}"). Drive with
 * az_iot_e2e_service_request_poll. */
bool az_iot_e2e_service_twin_patch_desired_begin(
    az_iot_e2e_service* svc,
    const char* device_id,
    const char* desired_json);

/* Advance the in-flight REST request without blocking the device. On completion
 * (returns 1) writes the HTTP status to @p out_http_status and the response body
 * (NUL-terminated, truncated to fit) to @p resp_buf. Returns 0 while pending,
 * 1 when complete, -1 on error. */
int az_iot_e2e_service_request_poll(
    az_iot_e2e_service* svc,
    int* out_http_status,
    char* resp_buf,
    size_t resp_buf_size);

/* ---- Device-side HTTPS (for e2e device features such as file upload) ------- */

/* Perform a blocking HTTPS request using the harness's TLS transport, so an e2e
 * test can drive the device side of an HTTPS feature without an external HTTP
 * client. Enables mutual TLS when @p client_cert_path / @p client_key_path are
 * non-NULL (e.g. a device authenticating to IoT Hub); pass NULL for an anonymous
 * request (e.g. an Azure Storage PUT authenticated by a SAS token already in
 * @p path). @p content_type and @p extra_header (a single header line, e.g.
 * "x-ms-blob-type: BlockBlob") are optional. On completion, writes the HTTP
 * status to @p out_status and the response body (NUL-terminated, truncated to
 * fit) to @p resp_buf with its length in @p out_resp_len (all optional). The
 * peer is validated against the platform's default trust store. Returns true
 * when the exchange completed (any HTTP status), false on transport failure. */
bool az_iot_e2e_https_request(
    const char* host,
    const char* method,
    const char* path,
    const char* client_cert_path,
    const char* client_key_path,
    const char* content_type,
    const char* extra_header,
    const void* body,
    size_t body_len,
    int* out_status,
    char* resp_buf,
    size_t resp_buf_size,
    size_t* out_resp_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_E2E_SERVICE_H */
