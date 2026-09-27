// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MQTTV3_FILE_UPLOAD_CLIENT_H
#define AZ_IOT_MQTTV3_FILE_UPLOAD_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* IoT Hub Classic file upload.
 *
 * The control plane -- request a blob SAS URI, then notify the hub of completion
 * -- is two HTTPS REST calls to the hub. This SDK ships no HTTP client by
 * design, so the application provides one via an
 * az_iot_file_upload_http_transport hook registered at init(); the SDK builds
 * the request and parses the response.
 *
 * The blob bytes themselves go to Azure Storage via an HTTPS PUT to the returned
 * SAS URI. That is the application's job, not the SDK's -- it can reuse the same
 * HTTP client it provides for the hook.
 *
 * Usage:
 *   1. get_sas_uri(blob_name, cb)  -> cb delivers {blob_sas_uri, correlation_id}
 *   2. app PUTs the file to blob_sas_uri  (header "x-ms-blob-type: BlockBlob")
 *   3. notify_complete(correlation_id, is_success, cb)
 *
 * There is no mqttv5 counterpart. File upload is not carried on the MQTT v5 hub
 * for now, so rather than publish an API that cannot work there, this client
 * pins the Classic profile and a connection that resolves to MQTT v5 is refused
 * with AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH. When the AEG Files message
 * schema lands, an mqttv5 client is added beside this one -- the HTTP transport
 * hook and the buffers below stay Classic-only either way, which is why they
 * live in this header rather than a shared one.
 */

/** @brief Maximum length of a built request URL (incl. NUL). */
#ifndef AZ_IOT_FILE_UPLOAD_URL_MAX
#define AZ_IOT_FILE_UPLOAD_URL_MAX 512
#endif
/** @brief Size in bytes of the buffer used to build a request body.
 *
 * The body is passed to the transport hook as a (pointer, length) pair and is
 * NOT NUL-terminated, so a hook must never treat it as a C string.
 *
 * This bounds the blob name: the SAS-URI request body is {"blobName":"<name>"},
 * so a name whose JSON-ESCAPED form does not fit in this many bytes minus the 15
 * bytes of envelope is refused with AZ_IOT_ERR_NOT_ENOUGH_SPACE. Escaping is what
 * counts, not characters -- a quote or backslash costs two bytes, and non-ASCII
 * costs its UTF-8 length -- so the default admits up to ~495 bytes of escaped
 * name. That is well beyond typical names; raise it if the application uses
 * longer ones (Azure Storage permits blob names of up to 1024 characters). It
 * also bounds the correlation id accepted by notify_complete(). Each operation
 * places one buffer of this size on the stack. */
#ifndef AZ_IOT_FILE_UPLOAD_BODY_MAX
#define AZ_IOT_FILE_UPLOAD_BODY_MAX 512
#endif
/** @brief Maximum length of the assembled blob SAS URI (incl. NUL). */
#ifndef AZ_IOT_FILE_UPLOAD_SAS_URI_MAX
#define AZ_IOT_FILE_UPLOAD_SAS_URI_MAX 2048
#endif
/** @brief Maximum length of a correlation id (incl. NUL). */
#ifndef AZ_IOT_FILE_UPLOAD_CORR_ID_MAX
#define AZ_IOT_FILE_UPLOAD_CORR_ID_MAX 192
#endif

  /**
   * @brief Delivers the result of get_sas_uri().
   *
   * @param status          AZ_IOT_OK on success.
   * @param blob_sas_uri    URI to PUT the blob to (valid only during the callback).
   * @param correlation_id  Pass back to notify_complete() (valid only during the callback).
   * @param user_ctx        Context passed to get_sas_uri().
   */
  typedef void (*az_iot_file_upload_sas_callback)(
      az_iot_result status,
      const char* blob_sas_uri,
      const char* correlation_id,
      void* user_ctx);

  /**
   * @brief Delivers the result of notify_complete().
   */
  typedef void (*az_iot_file_upload_complete_callback)(az_iot_result status, void* user_ctx);

  /**
   * @brief Response buffer the application fills when performing an HTTP request
   *        through the transport hook.
   *
   * The SDK supplies @p body and @p body_capacity and the application sets
   * @p status_code and, when it wrote one, @p body_len.
   *
   * For a request whose response body the SDK does not read -- the completion
   * notification -- @p body is NULL and @p body_capacity is 0. A hook must tolerate
   * that and simply discard the body it received.
   *
   * For a request whose body the SDK does parse -- the SAS-URI request -- the SDK
   * ignores any change the hook made to @p body or @p body_capacity and bounds the
   * parse by the buffer it originally handed out, so an over-reported @p body_len
   * cannot read past it. A success status that arrives with no body at all is
   * reported to the caller as AZ_IOT_ERR_PROTOCOL.
   */
  typedef struct az_iot_file_upload_http_response
  {
    int status_code; /**< HTTP status the app observed (e.g. 200). */
    uint8_t* body; /**< SDK-provided buffer to receive the response body; may be NULL. */
    size_t body_capacity; /**< Capacity of @p body; 0 when no body is read. */
    size_t body_len; /**< Set by the app to the number of bytes written. */
  } az_iot_file_upload_http_response;

  /**
   * @brief Application HTTP transport for the control plane.
   *
   * Called synchronously by the SDK to perform one HTTPS request to IoT Hub and
   * return its response. The hook owns authentication: this SDK authenticates
   * devices with X.509, so the hook is expected to perform mutual TLS with the
   * device certificate and @p authorization is always "". The parameter is kept
   * for hooks that need to supply their own credential (for example a gateway
   * that fronts the hub) and for future token-based authentication.
   *
   * The hook must also validate the hub's server certificate; the SDK cannot do it
   * on the application's behalf on this transport.
   *
   * The call blocks the SDK for its whole duration. When an operation is started
   * from inside another callback it runs on the connection's do_work() thread, so
   * a hook without a bounded timeout stalls the MQTT pump.
   *
   * @return AZ_IOT_OK if the request was performed (even for a non-2xx status,
   *         which is reported via response.status_code); an error only on a
   *         transport-level failure.
   */
  typedef az_iot_result (*az_iot_file_upload_http_send_fn)(
      const char* method,
      const char* url,
      const char* authorization,
      const char* content_type,
      const uint8_t* body,
      size_t body_len,
      az_iot_file_upload_http_response* response,
      void* hook_ctx);

  /**
   * @brief HTTP transport hook. Required: the SDK ships no HTTP client.
   */
  typedef struct az_iot_file_upload_http_transport
  {
    az_iot_file_upload_http_send_fn send;
    void* ctx;
  } az_iot_file_upload_http_transport;

  typedef struct az_iot_mqttv3_file_upload_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_file_upload_http_send_fn http_send;
      void* http_ctx;
    } _internal;
  } az_iot_mqttv3_file_upload_client;

  /**
   * @brief Initialize the IoT Hub Classic file upload client.
   *
   * Call after the connection has resolved its hub (for a DPS client, once it
   * reaches CONNECTED) so the hub address and device id are known. Neither is
   * copied: both are read from the connection on each operation, so the connection
   * stays the single source of truth and a later hub assignment is picked up
   * without re-initializing this client.
   *
   * @p client must be a fresh instance or one that has been passed to
   * az_iot_mqttv3_file_upload_client_deinit(). Initializing over a live instance
   * cannot be detected -- the struct is caller-allocated, so an uninitialized one
   * is indistinguishable from a live one -- and would strand the generation
   * reference the live instance holds on @p conn.
   *
   * Pins the connection to the Classic profile. A connection already known to be
   * MQTT v5 -- a direct connection, or a DPS one past assignment -- is rejected
   * here with AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict
   * surfaces when the connection resolves, which fails it before it reports
   * CONNECTED. A failed init releases the pin it took.
   *
   * @param client          Instance to initialize.
   * @param conn            The (connected) connection client. Must outlive
   *                        @p client.
   * @param http_transport  HTTP transport for the control plane. Required.
   * @return AZ_IOT_OK on success;
   *         AZ_IOT_ERR_INVALID_ARG for a caller mistake -- a NULL argument, or no
   *         HTTP transport;
   *         AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH on an MQTT v5 connection;
   *         AZ_IOT_ERR_NOT_CONNECTED if the connection cannot yet supply a hub
   *         address and device id, which is transient: retry once it is connected.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv3_file_upload_client_init(
      az_iot_mqttv3_file_upload_client* client,
      az_iot_connection_client* conn,
      const az_iot_file_upload_http_transport* http_transport);

  /**
   * @brief Deinitialize the file upload client.
   */
  void az_iot_mqttv3_file_upload_client_deinit(az_iot_mqttv3_file_upload_client* client);

  /**
   * @brief Request a blob SAS URI (step 1).
   *
   * The result is delivered via @p cb during this call: the HTTP hook is
   * synchronous.
   *
   * @param client     File upload client instance.
   * @param blob_name  Name of the blob to upload (e.g. "mydata/sensor.csv"). Must
   *                   fit AZ_IOT_FILE_UPLOAD_BODY_MAX once JSON-escaped.
   * @param cb         Callback delivering the SAS URI + correlation id.
   * @param user_ctx   Context forwarded to @p cb.
   * @return AZ_IOT_OK if the request was dispatched (the result then arrives via
   *         @p cb, including for HTTP and transport failures); another error if it
   *         could not be dispatched at all -- AZ_IOT_ERR_NOT_ENOUGH_SPACE for a
   *         blob name that does not fit, or AZ_IOT_ERR_NOT_CONNECTED while the
   *         connection has no hub address and device id to address the request to
   *         (retry once it is connected). No callback fires when this returns
   *         anything but AZ_IOT_OK.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv3_file_upload_client_get_sas_uri(
      az_iot_mqttv3_file_upload_client* client,
      const char* blob_name,
      az_iot_file_upload_sas_callback cb,
      void* user_ctx);

  /**
   * @brief Notify IoT Hub that the upload finished (step 3), after the app PUT the
   *        blob to Azure Storage. Result delivered via @p cb (see get_sas_uri()).
   *
   * @param client          File upload client instance.
   * @param correlation_id  The correlation id from the get_sas_uri() callback.
   * @param is_success      Whether the blob upload succeeded.
   * @param cb              Callback delivering the acknowledgement status.
   * @param user_ctx        Context forwarded to @p cb.
   * @return AZ_IOT_OK if dispatched (the result then arrives via @p cb);
   *         AZ_IOT_ERR_NOT_ENOUGH_SPACE for a correlation id that does not fit
   *         AZ_IOT_FILE_UPLOAD_BODY_MAX once JSON-escaped;
   *         AZ_IOT_ERR_NOT_CONNECTED while the connection has no hub address and
   *         device id to address the request to (retry once it is connected). No
   *         callback fires when this returns anything but AZ_IOT_OK.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv3_file_upload_client_notify_complete(
      az_iot_mqttv3_file_upload_client* client,
      const char* correlation_id,
      bool is_success,
      az_iot_file_upload_complete_callback cb,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV3_FILE_UPLOAD_CLIENT_H */
