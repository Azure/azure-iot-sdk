// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_FILE_UPLOAD_CLIENT_H
#define AZ_IOT_FILE_UPLOAD_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"
#include "az_iot_connection_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Azure IoT Hub file upload — one seamless API, transport chosen by hub flavor.
 *
 * The control plane (request a blob SAS URI, then notify the hub of completion)
 * uses a different transport depending on the hub the connection resolved to,
 * but the API below is identical for both (the client dispatches internally on
 * the connection's protocol profile, like az_iot_twin_client):
 *
 *   - IoT Hub Classic: the two operations are HTTPS REST calls to the hub. This
 *     SDK ships no HTTP client by design, so the application provides one via an
 *     az_iot_file_upload_http_transport hook registered at init(); the SDK
 *     builds the request and parses the response.
 *   - IoT Hub Next (AEG): the two operations travel over the existing MQTT
 *     connection (topics ih/{deviceId}/srv|dev/files). The SDK handles this
 *     internally; no HTTP hook is used for the control plane.
 *
 * The blob bytes themselves ALWAYS go to Azure Storage via an HTTPS PUT to the
 * returned SAS URI — the application's responsibility on both flavors (it can
 * reuse the same HTTP client it provides for the Classic hook).
 *
 * Usage (identical regardless of flavor):
 *   1. get_sas_uri(blob_name, cb)  -> cb delivers {blob_sas_uri, correlation_id}
 *   2. app PUTs the file to blob_sas_uri  (header "x-ms-blob-type: BlockBlob")
 *   3. notify_complete(correlation_id, is_success, cb)
 */

/** @brief Maximum length of a built request URL (incl. NUL). */
#ifndef AZ_IOT_FILE_UPLOAD_URL_MAX
#define AZ_IOT_FILE_UPLOAD_URL_MAX 512
#endif
/** @brief Maximum length of a built request body (incl. NUL).
 *
 * This bounds the blob name: the SAS-URI request body is {"blobName":"<name>"},
 * so a name (after JSON escaping) longer than this minus ~15 bytes is refused
 * with AZ_IOT_ERR_NOT_ENOUGH_SPACE. The default leaves room for ~495 characters,
 * well beyond typical names; raise it if the application uses longer ones (Azure
 * Storage permits up to 1024). It also bounds the correlation id accepted by
 * notify_complete(). Each operation allocates one buffer of this size on the
 * stack. */
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
typedef void (*az_iot_file_upload_complete_callback)(
    az_iot_result status,
    void* user_ctx);

/**
 * @brief Response buffer the application fills when performing a Classic HTTP
 *        request through the transport hook.
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
    int      status_code;    /**< HTTP status the app observed (e.g. 200). */
    uint8_t* body;           /**< SDK-provided buffer to receive the response body; may be NULL. */
    size_t   body_capacity;  /**< Capacity of @p body; 0 when no body is read. */
    size_t   body_len;       /**< Set by the app to the number of bytes written. */
} az_iot_file_upload_http_response;

/**
 * @brief Application HTTP transport for the Classic control plane.
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
 * @brief HTTP transport hook: required on a Classic hub, ignored on Next.
 */
typedef struct az_iot_file_upload_http_transport
{
    az_iot_file_upload_http_send_fn send;
    void*                           ctx;
} az_iot_file_upload_http_transport;

typedef struct az_iot_file_upload_client
{
    struct
    {
        az_iot_connection_client*       conn;
        az_iot_file_upload_http_send_fn http_send;
        void*                           http_ctx;
        char hub_hostname[AZ_IOT_DPS_HOST_BUF];
        char device_id[AZ_IOT_DPS_DEVICE_ID_BUF];
    } _internal;
} az_iot_file_upload_client;

/**
 * @brief Initialize the file upload client.
 *
 * Call after the connection has resolved its hub (for a DPS client, once it
 * reaches CONNECTED) so the hub address and device id are known. Both are
 * re-read from the connection on every operation, so a client stays valid if the
 * connection is later reassigned to a different hub.
 *
 * @param client          Instance to initialize.
 * @param conn            The (connected) connection client.
 * @param http_transport  HTTP transport for the Classic control plane. REQUIRED
 *                        on a Classic hub; may be NULL on Next.
 * @return AZ_IOT_OK on success; AZ_IOT_ERR_INVALID_ARG if a Classic connection is
 *         missing the HTTP transport or the hub/device id are not yet available.
 */
AZ_NODISCARD az_iot_result az_iot_file_upload_client_init(
    az_iot_file_upload_client* client,
    az_iot_connection_client* conn,
    const az_iot_file_upload_http_transport* http_transport);

/**
 * @brief Deinitialize the file upload client.
 */
void az_iot_file_upload_client_destroy(az_iot_file_upload_client* client);

/**
 * @brief Request a blob SAS URI (step 1).
 *
 * Asynchronous: the result is delivered via @p cb — during this call on Classic
 * (synchronous HTTP hook), or during a later do_work() on Next.
 *
 * @param client     File upload client instance.
 * @param blob_name  Name of the blob to upload (e.g. "mydata/sensor.csv"). Must
 *                   fit AZ_IOT_FILE_UPLOAD_BODY_MAX once JSON-escaped.
 * @param cb         Callback delivering the SAS URI + correlation id.
 * @param user_ctx   Context forwarded to @p cb.
 * @return AZ_IOT_OK if the request was dispatched (the result then arrives via
 *         @p cb, including for HTTP and transport failures);
 *         AZ_IOT_ERR_NOT_SUPPORTED on a Next/AEG hub until the AEG Files message
 *         schema is implemented; another error if it could not be dispatched at
 *         all — notably AZ_IOT_ERR_NOT_ENOUGH_SPACE for a blob name that does not
 *         fit. No callback fires when this returns anything but AZ_IOT_OK.
 */
AZ_NODISCARD az_iot_result az_iot_file_upload_client_get_sas_uri(
    az_iot_file_upload_client* client,
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
 *         AZ_IOT_ERR_NOT_SUPPORTED on Next until the AEG Files schema is
 *         implemented. No callback fires when this returns anything but
 *         AZ_IOT_OK.
 */
AZ_NODISCARD az_iot_result az_iot_file_upload_client_notify_complete(
    az_iot_file_upload_client* client,
    const char* correlation_id,
    bool is_success,
    az_iot_file_upload_complete_callback cb,
    void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_FILE_UPLOAD_CLIENT_H */
