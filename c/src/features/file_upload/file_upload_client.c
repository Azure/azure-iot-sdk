// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* FileUploadClient — one seamless API, transport chosen by hub flavor.
 *
 * The public API (get_sas_uri + notify_complete + callbacks) is identical for
 * both hub flavors; the transport is selected internally from the connection's
 * protocol profile (like twin_client):
 *   - Classic: HTTPS REST to the hub, performed by the application's HTTP
 *     transport hook (this SDK ships no HTTP client). The SDK builds the request
 *     and parses the response here.
 *   - Next (AEG): the control plane travels over the MQTT connection
 *     (ih/{deviceId}/srv|dev/files). Not yet implemented — pending the AEG Files
 *     message schema — so the API returns AZ_IOT_ERR_NOT_SUPPORTED on Next.
 * The blob bytes always go to Azure Storage via an app HTTPS PUT to the SAS URI.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_file_upload_client.h"

#include "internal/connection_client_internal.h"
#include "internal/protocol_profile.h"

/* IoT Hub device REST API version for the Classic file-upload endpoints. */
#define AZ_IOT_FILEUPLOAD_API_VERSION "2021-04-12"

/* HTTP status-code boundaries used to map Classic REST responses
 * (see http_status_to_result). */
#define AZ_IOT_FILEUPLOAD_HTTP_SUCCESS_MIN 200
#define AZ_IOT_FILEUPLOAD_HTTP_SUCCESS_MAX 300
#define AZ_IOT_FILEUPLOAD_HTTP_CLIENT_MIN  400
#define AZ_IOT_FILEUPLOAD_HTTP_CLIENT_MAX  500
#define AZ_IOT_FILEUPLOAD_HTTP_NOT_FOUND   404
#define AZ_IOT_FILEUPLOAD_HTTP_THROTTLED   429

/* statusCode reported in the completion-notification body. */
#define AZ_IOT_FILEUPLOAD_NOTIFY_STATUS_SUCCEEDED 200
#define AZ_IOT_FILEUPLOAD_NOTIFY_STATUS_FAILED    0

/* Field-size bounds for parsing the SAS-URI response. */
#define AZ_IOT_FILEUPLOAD_HOST_MAX      256
#define AZ_IOT_FILEUPLOAD_CONTAINER_MAX 256
#define AZ_IOT_FILEUPLOAD_BLOB_MAX      256
#define AZ_IOT_FILEUPLOAD_SAS_TOKEN_MAX 1024

/* Internal shorthand */
#define FI(c) ((c)->_internal)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Copy a non-empty NUL-terminated string into a fixed buffer, bounds-checked. */
static az_iot_result fileupload_copy(char* dst, size_t cap, const char* src)
{
    if (!src || !src[0]) return AZ_IOT_ERR_INVALID_ARG;
    size_t n = strlen(src);
    if (n + 1 > cap) return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    memcpy(dst, src, n + 1);
    return AZ_IOT_OK;
}

/* Read a top-level string property @p name from JSON object @p json, copying its
 * (unescaped) value into @p out (NUL-terminated). Returns true on success. Uses
 * the azure-sdk-for-c JSON reader rather than hand-rolled parsing. */
static bool fileupload_json_str(az_span json, az_span name, char* out, int32_t cap)
{
    az_json_reader jr;
    if (az_result_failed(az_json_reader_init(&jr, json, NULL))
        || az_result_failed(az_json_reader_next_token(&jr))
        || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
        return false;
    }

    while (az_result_succeeded(az_json_reader_next_token(&jr))
           && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
    {
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME) continue;

        bool match = az_json_token_is_text_equal(&jr.token, name);
        if (az_result_failed(az_json_reader_next_token(&jr))) return false;

        if (match)
        {
            int32_t len = 0;
            if (jr.token.kind != AZ_JSON_TOKEN_STRING
                || az_result_failed(az_json_token_get_string(&jr.token, out, cap, &len)))
            {
                return false;
            }
            return true;
        }

        /* Skip a non-matching nested object/array so we stay at object scope. */
        if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT
            || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
        {
            if (az_result_failed(az_json_reader_skip_children(&jr))) return false;
        }
    }
    return false;
}

/* Assemble the full SAS URI from the response JSON fields:
 * https://{hostName}/{containerName}/{blobName}{sasToken} */
static bool assemble_sas_uri(az_span json, char* out, size_t out_cap)
{
    char host[AZ_IOT_FILEUPLOAD_HOST_MAX] = {0};
    char container[AZ_IOT_FILEUPLOAD_CONTAINER_MAX] = {0};
    char blob[AZ_IOT_FILEUPLOAD_BLOB_MAX] = {0};
    char sas[AZ_IOT_FILEUPLOAD_SAS_TOKEN_MAX] = {0};

    if (!fileupload_json_str(json, AZ_SPAN_FROM_STR("hostName"), host, (int32_t)sizeof(host))
        || !fileupload_json_str(json, AZ_SPAN_FROM_STR("containerName"), container, (int32_t)sizeof(container))
        || !fileupload_json_str(json, AZ_SPAN_FROM_STR("blobName"), blob, (int32_t)sizeof(blob))
        || !fileupload_json_str(json, AZ_SPAN_FROM_STR("sasToken"), sas, (int32_t)sizeof(sas)))
    {
        return false;
    }

    int n = snprintf(out, out_cap, "https://%s/%s/%s%s", host, container, blob, sas);
    return (n > 0 && (size_t)n < out_cap);
}

/* Build {"blobName":"<name>"} using the azure-sdk-for-c JSON writer. */
static az_iot_result build_sas_request_body(char* out, size_t cap, const char* blob_name, size_t* out_len)
{
    az_json_writer jw;
    if (az_result_failed(az_json_writer_init(&jw, az_span_create((uint8_t*)out, (int32_t)cap), NULL))
        || az_result_failed(az_json_writer_append_begin_object(&jw))
        || az_result_failed(az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("blobName")))
        || az_result_failed(az_json_writer_append_string(&jw, az_span_create_from_str((char*)(uintptr_t)blob_name)))
        || az_result_failed(az_json_writer_append_end_object(&jw)))
    {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    *out_len = (size_t)az_span_size(az_json_writer_get_bytes_used_in_destination(&jw));
    return AZ_IOT_OK;
}

/* Build the completion-notification body using the azure-sdk-for-c JSON writer:
 * {"correlationId":..,"isSuccess":..,"statusCode":..,"statusDescription":..} */
static az_iot_result build_notification_body(
    char* out, size_t cap, const char* correlation_id, bool is_success, size_t* out_len)
{
    az_json_writer jw;
    if (az_result_failed(az_json_writer_init(&jw, az_span_create((uint8_t*)out, (int32_t)cap), NULL))
        || az_result_failed(az_json_writer_append_begin_object(&jw))
        || az_result_failed(az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("correlationId")))
        || az_result_failed(az_json_writer_append_string(&jw, az_span_create_from_str((char*)(uintptr_t)correlation_id)))
        || az_result_failed(az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("isSuccess")))
        || az_result_failed(az_json_writer_append_bool(&jw, is_success))
        || az_result_failed(az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("statusCode")))
        || az_result_failed(az_json_writer_append_int32(&jw,
               is_success ? AZ_IOT_FILEUPLOAD_NOTIFY_STATUS_SUCCEEDED : AZ_IOT_FILEUPLOAD_NOTIFY_STATUS_FAILED))
        || az_result_failed(az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("statusDescription")))
        || az_result_failed(az_json_writer_append_string(&jw,
               is_success ? AZ_SPAN_FROM_STR("Succeeded") : AZ_SPAN_FROM_STR("Failed")))
        || az_result_failed(az_json_writer_append_end_object(&jw)))
    {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    *out_len = (size_t)az_span_size(az_json_writer_get_bytes_used_in_destination(&jw));
    return AZ_IOT_OK;
}

/* Map a Classic HTTP status code to an az_iot_result. */
static az_iot_result http_status_to_result(int status)
{
    if (status >= AZ_IOT_FILEUPLOAD_HTTP_SUCCESS_MIN && status < AZ_IOT_FILEUPLOAD_HTTP_SUCCESS_MAX)
        return AZ_IOT_OK;
    if (status == AZ_IOT_FILEUPLOAD_HTTP_NOT_FOUND) return AZ_IOT_ERR_NOT_FOUND;
    if (status == AZ_IOT_FILEUPLOAD_HTTP_THROTTLED) return AZ_IOT_ERR_BUSY;
    if (status >= AZ_IOT_FILEUPLOAD_HTTP_CLIENT_MIN && status < AZ_IOT_FILEUPLOAD_HTTP_CLIENT_MAX)
        return AZ_IOT_ERR_INVALID_ARG;
    return AZ_IOT_ERR_PROTOCOL;
}

/* Copy the connection's current hub address and device id into the client.
 *
 * Read per operation rather than cached once at init(): the connection re-points
 * both when DPS assigns -- or reassigns -- a hub, so a client created before a
 * reassignment would otherwise keep addressing the previous hub's REST endpoint
 * with the previous device id. */
static az_iot_result fileupload_resolve_endpoint(az_iot_file_upload_client* client)
{
    az_iot_result r = fileupload_copy(
        FI(client).hub_hostname, sizeof(FI(client).hub_hostname),
        az_iot_connection_client_get_iothub_address(FI(client).conn));
    if (r != AZ_IOT_OK) return r;

    return fileupload_copy(
        FI(client).device_id, sizeof(FI(client).device_id),
        az_iot_connection_client__device_id(FI(client).conn));
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_file_upload_client_init(
    az_iot_file_upload_client* client,
    az_iot_connection_client* conn,
    const az_iot_file_upload_http_transport* http_transport)
{
    if (!client || !conn) return AZ_IOT_ERR_INVALID_ARG;

    const az_iot_protocol_profile* profile = az_iot_connection_client__profile(conn);
    if (!profile) return AZ_IOT_ERR_NOT_SUPPORTED;

    memset(client, 0, sizeof(*client));
    FI(client).conn = conn;
    if (http_transport)
    {
        FI(client).http_send = http_transport->send;
        FI(client).http_ctx  = http_transport->ctx;
    }

    /* Classic has no in-SDK transport: the application must supply an HTTP hook.
     * Next carries the control plane over the existing MQTT connection. */
    if (profile->flavor != AZ_IOT_HUB_FLAVOR_NEXT && !FI(client).http_send)
    {
        memset(client, 0, sizeof(*client));
        return AZ_IOT_ERR_INVALID_ARG;
    }

    az_iot_result r = fileupload_resolve_endpoint(client);
    if (r != AZ_IOT_OK) { memset(client, 0, sizeof(*client)); return r; }

    return AZ_IOT_OK;
}

void az_iot_file_upload_client_destroy(az_iot_file_upload_client* client)
{
    if (!client) return;
    /* Harmless today (no handlers are registered yet); keeps teardown correct
     * once the Next/MQTT path registers response handlers. */
    if (FI(client).conn)
        (void)az_iot_connection_client__unregister_inbound_handlers(FI(client).conn, client);
    memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_file_upload_client_get_sas_uri(
    az_iot_file_upload_client* client,
    const char* blob_name,
    az_iot_file_upload_sas_callback cb,
    void* user_ctx)
{
    if (!client || !blob_name || !blob_name[0] || !cb) return AZ_IOT_ERR_INVALID_ARG;

    const az_iot_protocol_profile* profile = az_iot_connection_client__profile(FI(client).conn);
    if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        /* TODO(AEG): file upload over the MQTT connection. Publish to
         * "ih/{device_id}/srv/files", response on "ih/{device_id}/dev/files",
         * correlated via MQTT5 Correlation Data with a `type` property and a
         * protobuf body (AEG RFC implementation.md 3.5). Blocked on the AEG
         * Files message schema. */
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    /* Classic: synchronous HTTPS request via the application's transport hook. */
    if (!FI(client).http_send) return AZ_IOT_ERR_NOT_INITIALIZED;

    az_iot_result er = fileupload_resolve_endpoint(client);
    if (er != AZ_IOT_OK) return er;

    char url[AZ_IOT_FILE_UPLOAD_URL_MAX];
    int n = snprintf(url, sizeof(url), "https://%s/devices/%s/files?api-version=%s",
                     FI(client).hub_hostname, FI(client).device_id, AZ_IOT_FILEUPLOAD_API_VERSION);
    if (n < 0 || (size_t)n >= sizeof(url)) return AZ_IOT_ERR_NOT_ENOUGH_SPACE;

    char body[AZ_IOT_FILE_UPLOAD_BODY_MAX];
    size_t body_len = 0;
    az_iot_result br = build_sas_request_body(body, sizeof(body), blob_name, &body_len);
    if (br != AZ_IOT_OK) return br;

    uint8_t rbuf[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX];
    az_iot_file_upload_http_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.body = rbuf;
    resp.body_capacity = sizeof(rbuf);

    az_iot_result tr = FI(client).http_send(
        "POST", url, "", "application/json",
        (const uint8_t*)body, body_len, &resp, FI(client).http_ctx);
    if (tr != AZ_IOT_OK)
    {
        cb(tr, NULL, NULL, user_ctx);
        return AZ_IOT_OK;
    }

    az_iot_result r = http_status_to_result(resp.status_code);
    if (r == AZ_IOT_OK)
    {
        /* Never hand the JSON reader an empty span: az_json_reader_init requires
         * at least one byte and its precondition handler does not return. A
         * success status with no body is a protocol error, not a crash. The
         * length is also clamped to the buffer the hook was given, so a hook that
         * over-reports body_len cannot make the parser read past the buffer. */
        size_t json_len = (resp.body_len > resp.body_capacity) ? resp.body_capacity : resp.body_len;
        if (resp.body == NULL || json_len == 0)
        {
            cb(AZ_IOT_ERR_PROTOCOL, NULL, NULL, user_ctx);
            return AZ_IOT_OK;
        }

        az_span json = az_span_create(resp.body, (int32_t)json_len);
        char sas_uri[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX];
        char corr_id[AZ_IOT_FILE_UPLOAD_CORR_ID_MAX];
        if (assemble_sas_uri(json, sas_uri, sizeof(sas_uri))
            && fileupload_json_str(json, AZ_SPAN_FROM_STR("correlationId"),
                                   corr_id, (int32_t)sizeof(corr_id)))
            cb(AZ_IOT_OK, sas_uri, corr_id, user_ctx);
        else
            cb(AZ_IOT_ERR_PROTOCOL, NULL, NULL, user_ctx);
    }
    else
    {
        cb(r, NULL, NULL, user_ctx);
    }
    return AZ_IOT_OK;
}

az_iot_result az_iot_file_upload_client_notify_complete(
    az_iot_file_upload_client* client,
    const char* correlation_id,
    bool is_success,
    az_iot_file_upload_complete_callback cb,
    void* user_ctx)
{
    if (!client || !correlation_id || !correlation_id[0] || !cb) return AZ_IOT_ERR_INVALID_ARG;

    const az_iot_protocol_profile* profile = az_iot_connection_client__profile(FI(client).conn);
    if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED; /* TODO(AEG): notify over MQTT (see get_sas_uri). */
    }

    if (!FI(client).http_send) return AZ_IOT_ERR_NOT_INITIALIZED;

    az_iot_result er = fileupload_resolve_endpoint(client);
    if (er != AZ_IOT_OK) return er;

    char url[AZ_IOT_FILE_UPLOAD_URL_MAX];
    int n = snprintf(url, sizeof(url), "https://%s/devices/%s/files/notifications?api-version=%s",
                     FI(client).hub_hostname, FI(client).device_id, AZ_IOT_FILEUPLOAD_API_VERSION);
    if (n < 0 || (size_t)n >= sizeof(url)) return AZ_IOT_ERR_NOT_ENOUGH_SPACE;

    char body[AZ_IOT_FILE_UPLOAD_BODY_MAX];
    size_t body_len = 0;
    az_iot_result br = build_notification_body(body, sizeof(body), correlation_id, is_success, &body_len);
    if (br != AZ_IOT_OK) return br;

    az_iot_file_upload_http_response resp;
    memset(&resp, 0, sizeof(resp));

    az_iot_result tr = FI(client).http_send(
        "POST", url, "", "application/json",
        (const uint8_t*)body, body_len, &resp, FI(client).http_ctx);
    if (tr != AZ_IOT_OK)
    {
        cb(tr, user_ctx);
        return AZ_IOT_OK;
    }

    cb(http_status_to_result(resp.status_code), user_ctx);
    return AZ_IOT_OK;
}
