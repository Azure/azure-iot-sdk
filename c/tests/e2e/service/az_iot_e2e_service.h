// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

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
 *   IOTHUB_EVENTHUB_CONSUMER_GROUP      consumer group to read (optional; default $Default)
 */
#ifndef AZ_IOT_E2E_SERVICE_H
#define AZ_IOT_E2E_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
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

  /* The lookups below read the earliest received message whose body contains @p needle, among
   * the most recent 64; call them before az_iot_e2e_service_telemetry_watch_end(). */

  /* Arrival order (from 0, across partitions, since watch_begin) of the message, or -1 when
   * none has arrived. */
  int az_iot_e2e_service_telemetry_index(const az_iot_e2e_service* svc, const char* needle);

  /* Sets @p size to the full body size of the message. Returns false when none has arrived. */
  bool az_iot_e2e_service_telemetry_body_size(
      const az_iot_e2e_service* svc,
      const char* needle,
      size_t* size);

  /* Copies property @p name of the message into @p out (NUL-terminated, truncated to fit).
   * @p name names the section: "amqp:<field>" for an AMQP properties field ("amqp:message-id",
   * "amqp:content-type", ...), "app:<key>" for an application property, "annotation:<key>" for a
   * message annotation. Non-string values are given in a printable form; "amqp:creation-time" is
   * decimal epoch milliseconds. On a miss, prints every property captured for the message.
   * Returns false on a miss. */
  bool az_iot_e2e_service_telemetry_property(
      const az_iot_e2e_service* svc,
      const char* needle,
      const char* name,
      char* out,
      size_t out_size);

  /* Copies into @p name_out the section-qualified name (as above) of the first property of the
   * message whose value is exactly @p value. Returns false if there is none. */
  bool az_iot_e2e_service_telemetry_find_value(
      const az_iot_e2e_service* svc,
      const char* needle,
      const char* value,
      char* name_out,
      size_t name_out_size);

  /* Prints every property captured for the message, with its section-qualified name (or that
   * none arrived), to stdout. */
  void az_iot_e2e_service_telemetry_dump(const az_iot_e2e_service* svc, const char* needle);

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
   * az_iot_e2e_service_last_error).
   *
   * @p attach_refused_out, when non-NULL, distinguishes the ONE transient failure
   * -- the hub refusing the receiver link while enableFileUploadNotifications is
   * still propagating -- from every other cause. Retry on true; fail fast on
   * false, where a retry would only delay diagnosing a bad configuration. */
  bool az_iot_e2e_service_file_notification_watch_begin(
      az_iot_e2e_service* svc,
      const char* device_id,
      bool* attach_refused_out);

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
      int* out_stale,
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

  /* AMQP system properties of a C2D message; NULL / 0 leaves one unset, except
   * content_type, which defaults to "application/octet-stream". */
  typedef struct az_iot_e2e_c2d_properties
  {
    const char* message_id;
    const char* correlation_id;
    const char* content_type;
    const char* content_encoding;
    int64_t absolute_expiry_time_ms; /* ms since the Unix epoch */
  } az_iot_e2e_c2d_properties;

  /* As az_iot_e2e_service_send_c2d(), with @p properties (may be NULL). */
  bool az_iot_e2e_service_send_c2d_with_properties(
      az_iot_e2e_service* svc,
      const char* device_id,
      const uint8_t* payload,
      size_t payload_len,
      const az_iot_e2e_c2d_properties* properties);

  /* ---- Direct methods & twin (HTTPS REST, non-blocking / pumpable) ---------- */

  /**
   * @brief REST requests in flight at once. One slot backs the single-request
   *        functions (the *_begin calls and az_iot_e2e_service_request_poll);
   *        the rest are handles from az_iot_e2e_service_method_invoke_start.
   *
   * Each request is its own TLS connection. OpenSSL (Linux/macOS) keeps state
   * per connection; the Windows reference transport (Schannel) keeps ONE global
   * TLS session, so on Windows only one request -- handle or not -- may be in
   * flight at a time. Suites that use handles are Linux-only.
   */
#define AZ_IOT_E2E_SERVICE_MAX_REQUESTS 8

  /* Begin a direct-method invocation on @p device_id. @p json_payload is the raw
   * JSON value to deliver as the method payload (e.g. "\"ping\"" or "{\"a\":1}"),
   * or NULL for a JSON null. Both timeouts are 30 seconds. Drive to completion
   * with az_iot_e2e_service_request_poll. */
  bool az_iot_e2e_service_method_invoke_begin(
      az_iot_e2e_service* svc,
      const char* device_id,
      const char* method_name,
      const char* json_payload);

  /**
   * @brief As az_iot_e2e_service_method_invoke_begin(), with the request's
   *        responseTimeoutInSeconds and connectTimeoutInSeconds.
   */
  bool az_iot_e2e_service_method_invoke_with_timeout_begin(
      az_iot_e2e_service* svc,
      const char* device_id,
      const char* method_name,
      const char* json_payload,
      int response_timeout_s,
      int connect_timeout_s);

  /**
   * @brief As az_iot_e2e_service_method_invoke_with_timeout_begin(), on a free
   *        slot of its own so several invocations can be in flight together (see
   *        AZ_IOT_E2E_SERVICE_MAX_REQUESTS for the platform limit).
   *
   * @return A handle (> 0) for az_iot_e2e_service_request_poll_handle, or -1
   *         when no slot is free or the request cannot be built.
   */
  int az_iot_e2e_service_method_invoke_start(
      az_iot_e2e_service* svc,
      const char* device_id,
      const char* method_name,
      const char* json_payload,
      int response_timeout_s,
      int connect_timeout_s);

  /**
   * @brief As az_iot_e2e_service_request_poll(), for the request named by
   *        @p handle. The handle is released once this returns 1 or -1.
   */
  int az_iot_e2e_service_request_poll_handle(
      az_iot_e2e_service* svc,
      int handle,
      int* out_http_status,
      char* resp_buf,
      size_t resp_buf_size);

  /** @brief Abandon the request named by @p handle; safe once it completed. */
  void az_iot_e2e_service_request_cancel(az_iot_e2e_service* svc, int handle);

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
