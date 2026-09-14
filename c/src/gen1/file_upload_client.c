// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* IoT Hub Classic file upload.
 *
 * The control plane is two HTTPS REST calls to the hub, performed by the
 * application's HTTP transport hook -- this SDK ships no HTTP client. The SDK
 * builds the request and parses the response here. The blob bytes always go to
 * Azure Storage via an app HTTPS PUT to the SAS URI.
 *
 * There is no gen2 counterpart: file upload is not carried on the MQTT v5 hub
 * for now. This client pins the Classic profile, so an MQTT v5 connection is
 * refused at init() rather than at the first upload.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "azure/iot/gen1/az_iot_file_upload_client.h"

#include "internal/connection_client_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

/* IoT Hub device REST API version for the file-upload endpoints. */
#define AZ_IOT_FILEUPLOAD_API_VERSION "2021-04-12"

/* Request shape. Declared here so the URL that is built and the one documented
 * cannot drift apart. */
#define AZ_IOT_FILEUPLOAD_SCHEME "https://"
#define AZ_IOT_FILEUPLOAD_DEVICES_PATH "/devices/"
#define AZ_IOT_FILEUPLOAD_FILES_PATH "/files?api-version=" AZ_IOT_FILEUPLOAD_API_VERSION
#define AZ_IOT_FILEUPLOAD_NOTIFY_PATH \
  "/files/notifications?api-version=" AZ_IOT_FILEUPLOAD_API_VERSION
#define AZ_IOT_FILEUPLOAD_METHOD_POST "POST"
#define AZ_IOT_FILEUPLOAD_CONTENT_TYPE_JSON "application/json"

/* HTTP status-code boundaries used to map REST responses
 * (see http_status_to_result). */
#define AZ_IOT_FILEUPLOAD_HTTP_SUCCESS_MIN 200
#define AZ_IOT_FILEUPLOAD_HTTP_SUCCESS_MAX 300
#define AZ_IOT_FILEUPLOAD_HTTP_CLIENT_MIN 400
#define AZ_IOT_FILEUPLOAD_HTTP_CLIENT_MAX 500
#define AZ_IOT_FILEUPLOAD_HTTP_NOT_FOUND 404
#define AZ_IOT_FILEUPLOAD_HTTP_THROTTLED 429

/* statusCode reported in the completion-notification body. */
#define AZ_IOT_FILEUPLOAD_NOTIFY_STATUS_SUCCEEDED 200
#define AZ_IOT_FILEUPLOAD_NOTIFY_STATUS_FAILED 0

/* Field-size bounds for parsing the SAS-URI response. */
#define AZ_IOT_FILEUPLOAD_HOST_MAX 256
#define AZ_IOT_FILEUPLOAD_CONTAINER_MAX 256
#define AZ_IOT_FILEUPLOAD_BLOB_MAX 256
#define AZ_IOT_FILEUPLOAD_SAS_TOKEN_MAX 1024

/* Internal shorthand */
#define FI(c) ((c)->_internal)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

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
    if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      continue;
    }

    bool match = az_json_token_is_text_equal(&jr.token, name);
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return false;
    }

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
    if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      if (az_result_failed(az_json_reader_skip_children(&jr)))
      {
        return false;
      }
    }
  }
  return false;
}

/* Assemble the full SAS URI from the response JSON fields:
 * https://{hostName}/{containerName}/{blobName}{sasToken} */
static bool assemble_sas_uri(az_span json, char* out, size_t out_cap)
{
  char host[AZ_IOT_FILEUPLOAD_HOST_MAX] = { 0 };
  char container[AZ_IOT_FILEUPLOAD_CONTAINER_MAX] = { 0 };
  char blob[AZ_IOT_FILEUPLOAD_BLOB_MAX] = { 0 };
  char sas[AZ_IOT_FILEUPLOAD_SAS_TOKEN_MAX] = { 0 };

  if (!fileupload_json_str(json, AZ_SPAN_FROM_STR("hostName"), host, (int32_t)sizeof(host))
      || !fileupload_json_str(
          json, AZ_SPAN_FROM_STR("containerName"), container, (int32_t)sizeof(container))
      || !fileupload_json_str(json, AZ_SPAN_FROM_STR("blobName"), blob, (int32_t)sizeof(blob))
      || !fileupload_json_str(json, AZ_SPAN_FROM_STR("sasToken"), sas, (int32_t)sizeof(sas)))
  {
    return false;
  }

  const char* parts[] = { AZ_IOT_FILEUPLOAD_SCHEME, host, "/", container, "/", blob, sas };
  return az_iot_span_writer_build_str(
             az_span_create((uint8_t*)out, (int32_t)out_cap), NULL, parts, 7)
      == AZ_IOT_OK;
}

/* Build {"blobName":"<name>"} using the azure-sdk-for-c JSON writer. */
static az_iot_result build_sas_request_body(
    char* out,
    size_t cap,
    const char* blob_name,
    size_t* out_len)
{
  az_json_writer jw;
  if (az_result_failed(az_json_writer_init(&jw, az_span_create((uint8_t*)out, (int32_t)cap), NULL))
      || az_result_failed(az_json_writer_append_begin_object(&jw))
      || az_result_failed(az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("blobName")))
      || az_result_failed(
          az_json_writer_append_string(&jw, az_span_create_from_str((char*)(uintptr_t)blob_name)))
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
    char* out,
    size_t cap,
    const char* correlation_id,
    bool is_success,
    size_t* out_len)
{
  az_json_writer jw;
  if (az_result_failed(az_json_writer_init(&jw, az_span_create((uint8_t*)out, (int32_t)cap), NULL))
      || az_result_failed(az_json_writer_append_begin_object(&jw))
      || az_result_failed(
          az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("correlationId")))
      || az_result_failed(az_json_writer_append_string(
          &jw, az_span_create_from_str((char*)(uintptr_t)correlation_id)))
      || az_result_failed(az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("isSuccess")))
      || az_result_failed(az_json_writer_append_bool(&jw, is_success))
      || az_result_failed(az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("statusCode")))
      || az_result_failed(az_json_writer_append_int32(
          &jw,
          is_success ? AZ_IOT_FILEUPLOAD_NOTIFY_STATUS_SUCCEEDED
                     : AZ_IOT_FILEUPLOAD_NOTIFY_STATUS_FAILED))
      || az_result_failed(
          az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("statusDescription")))
      || az_result_failed(az_json_writer_append_string(
          &jw, is_success ? AZ_SPAN_FROM_STR("Succeeded") : AZ_SPAN_FROM_STR("Failed")))
      || az_result_failed(az_json_writer_append_end_object(&jw)))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  *out_len = (size_t)az_span_size(az_json_writer_get_bytes_used_in_destination(&jw));
  return AZ_IOT_OK;
}

/* Map an HTTP status code to an az_iot_result. */
static az_iot_result http_status_to_result(int status)
{
  if (status >= AZ_IOT_FILEUPLOAD_HTTP_SUCCESS_MIN && status < AZ_IOT_FILEUPLOAD_HTTP_SUCCESS_MAX)
  {
    return AZ_IOT_OK;
  }
  if (status == AZ_IOT_FILEUPLOAD_HTTP_NOT_FOUND)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  if (status == AZ_IOT_FILEUPLOAD_HTTP_THROTTLED)
  {
    return AZ_IOT_ERR_BUSY;
  }
  if (status >= AZ_IOT_FILEUPLOAD_HTTP_CLIENT_MIN && status < AZ_IOT_FILEUPLOAD_HTTP_CLIENT_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return AZ_IOT_ERR_PROTOCOL;
}

/* Borrow the connection's current hub address and device id.
 *
 * Read per operation rather than cached once at init(). Today that is not a
 * behavioural difference -- the connection assigns its hub exactly once, before
 * a file upload client can exist -- but the connection client is the owner of
 * both values, and keeping a second copy that stays correct only by accident of
 * the current control flow would be a trap for re-provisioning, hub failover, or
 * the IDLE-time host setter the connection already exposes internally.
 *
 * The strings are borrowed, not copied. That adds no lifetime dependency this
 * client does not already have -- it holds @p client->_internal.conn itself --
 * and the borrow is confined to the calling function, which formats the URL and
 * is done with the pointers before it returns; nothing in between pumps the
 * connection, invokes a callback, or otherwise lets the connection re-point
 * them. Storing them WOULD be unsafe, which is precisely why we do not. */
static az_iot_result fileupload_resolve_endpoint(
    az_iot_gen1_file_upload_client* client,
    const char** out_host,
    const char** out_device_id)
{
  const char* host = az_iot_connection_client_get_iothub_address(FI(client).conn);
  const char* device_id = az_iot_connection_client__device_id(FI(client).conn);

  /* The connection has no endpoint to give: it has not provisioned yet, or --
   * once hub reassignment is supported -- it is between hubs. That is a
   * transient state of the CONNECTION, not a mistake by the caller, and the
   * two call for opposite responses: an application told AZ_IOT_ERR_INVALID_ARG
   * would go auditing its own arguments, when what it should do is retry once
   * the connection is up. */
  if (!is_nonempty_cstr(host) || !is_nonempty_cstr(device_id))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  *out_host = host;
  *out_device_id = device_id;
  return AZ_IOT_OK;
}

/* Build one of the two request URLs: scheme + host + /devices/ + id + suffix. */
static az_iot_result build_request_url(
    const char* host,
    const char* device_id,
    const char* suffix,
    char (*url)[AZ_IOT_FILE_UPLOAD_URL_MAX])
{
  const char* parts[]
      = { AZ_IOT_FILEUPLOAD_SCHEME, host, AZ_IOT_FILEUPLOAD_DEVICES_PATH, device_id, suffix };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(*url), NULL, parts, 5) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR(
        "gen1_file_upload: the request URL did not fit AZ_IOT_FILE_UPLOAD_URL_MAX bytes");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_gen1_file_upload_client_init(
    az_iot_gen1_file_upload_client* client,
    az_iot_connection_client* conn,
    const az_iot_file_upload_http_transport* http_transport)
{
  if (!client || !conn)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* The SDK ships no HTTP client, so without a hook this client can never
   * perform either operation. Refuse at init rather than at the first upload. */
  if (!http_transport || !http_transport->send)
  {
    AZ_IOT_LOG_ERROR("gen1_file_upload: init requires an HTTP transport hook");
    memset(client, 0, sizeof(*client));
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_result result
      = az_iot_connection_client__require_profile(conn, AZ_IOT_CONNECTION_PROFILE_CLASSIC);
  if (result != AZ_IOT_OK)
  {
    memset(client, 0, sizeof(*client));
    return result;
  }

  memset(client, 0, sizeof(*client));
  FI(client).conn = conn;
  FI(client).http_send = http_transport->send;
  FI(client).http_ctx = http_transport->ctx;

  /* Fail a connection that cannot name a hub here rather than at the first
   * upload. This checks PRESENCE only -- whether the endpoint fits a request
   * URL is checked per operation, since the hub in force at init() need not be
   * the one an operation later addresses. */
  const char* host = NULL;
  const char* device_id = NULL;
  result = fileupload_resolve_endpoint(client, &host, &device_id);
  if (result != AZ_IOT_OK)
  {
    az_iot_connection_client__release_profile(conn);
    memset(client, 0, sizeof(*client));
    return result;
  }

  return AZ_IOT_OK;
}

void az_iot_gen1_file_upload_client_deinit(az_iot_gen1_file_upload_client* client)
{
  if (!client || !FI(client).conn)
  {
    return;
  }
  az_iot_connection_client__release_profile(FI(client).conn);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_gen1_file_upload_client_get_sas_uri(
    az_iot_gen1_file_upload_client* client,
    const char* blob_name,
    az_iot_file_upload_sas_callback cb,
    void* user_ctx)
{
  if (!client || !is_nonempty_cstr(blob_name) || !cb)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!FI(client).http_send)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  const char* host = NULL;
  const char* device_id = NULL;
  az_iot_result er = fileupload_resolve_endpoint(client, &host, &device_id);
  if (er != AZ_IOT_OK)
  {
    return er;
  }

  char url[AZ_IOT_FILE_UPLOAD_URL_MAX];
  er = build_request_url(host, device_id, AZ_IOT_FILEUPLOAD_FILES_PATH, &url);
  if (er != AZ_IOT_OK)
  {
    return er;
  }

  char body[AZ_IOT_FILE_UPLOAD_BODY_MAX];
  size_t body_len = 0;
  az_iot_result br = build_sas_request_body(body, sizeof(body), blob_name, &body_len);
  if (br != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARN("gen1_file_upload: the blob name does not fit AZ_IOT_FILE_UPLOAD_BODY_MAX");
    return br;
  }

  uint8_t rbuf[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX];
  az_iot_file_upload_http_response resp;
  memset(&resp, 0, sizeof(resp));
  resp.body = rbuf;
  resp.body_capacity = sizeof(rbuf);

  az_iot_result tr = FI(client).http_send(
      AZ_IOT_FILEUPLOAD_METHOD_POST,
      url,
      "",
      AZ_IOT_FILEUPLOAD_CONTENT_TYPE_JSON,
      (const uint8_t*)body,
      body_len,
      &resp,
      FI(client).http_ctx);
  if (tr != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen1_file_upload: the SAS-URI request failed in the transport hook (%s)",
        az_iot_result_to_string(tr));
    cb(tr, NULL, NULL, user_ctx);
    return AZ_IOT_OK;
  }

  az_iot_result r = http_status_to_result(resp.status_code);
  if (r == AZ_IOT_OK)
  {
    /* Never hand the JSON reader an empty span: az_json_reader_init requires
     * at least one byte and its precondition handler does not return. A
     * success status with no body is a protocol error, not a crash.
     *
     * The bounds come from THIS function's own buffer, not from the response
     * fields: the struct is mutable and the hook is application code, so a
     * hook that pointed `body` elsewhere or grew `body_capacity` must not be
     * able to widen the span the parser reads. Anything but the buffer we
     * handed out is refused outright. */
    size_t json_len = (resp.body_len > sizeof(rbuf)) ? sizeof(rbuf) : resp.body_len;
    if (resp.body != rbuf || json_len == 0)
    {
      AZ_IOT_LOG_WARN("gen1_file_upload: the hook returned success with no readable body");
      cb(AZ_IOT_ERR_PROTOCOL, NULL, NULL, user_ctx);
      return AZ_IOT_OK;
    }

    az_span json = az_span_create(rbuf, (int32_t)json_len);
    char sas_uri[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX];
    char corr_id[AZ_IOT_FILE_UPLOAD_CORR_ID_MAX];
    if (assemble_sas_uri(json, sas_uri, sizeof(sas_uri))
        && fileupload_json_str(
            json, AZ_SPAN_FROM_STR("correlationId"), corr_id, (int32_t)sizeof(corr_id)))
    {
      cb(AZ_IOT_OK, sas_uri, corr_id, user_ctx);
    }
    else
    {
      AZ_IOT_LOG_WARN("gen1_file_upload: the SAS-URI response did not carry the expected fields");
      cb(AZ_IOT_ERR_PROTOCOL, NULL, NULL, user_ctx);
    }
  }
  else
  {
    AZ_IOT_LOG_WARNF(
        "gen1_file_upload: the hub refused the SAS-URI request (HTTP %d)", resp.status_code);
    cb(r, NULL, NULL, user_ctx);
  }
  return AZ_IOT_OK;
}

az_iot_result az_iot_gen1_file_upload_client_notify_complete(
    az_iot_gen1_file_upload_client* client,
    const char* correlation_id,
    bool is_success,
    az_iot_file_upload_complete_callback cb,
    void* user_ctx)
{
  if (!client || !is_nonempty_cstr(correlation_id) || !cb)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!FI(client).http_send)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  const char* host = NULL;
  const char* device_id = NULL;
  az_iot_result er = fileupload_resolve_endpoint(client, &host, &device_id);
  if (er != AZ_IOT_OK)
  {
    return er;
  }

  char url[AZ_IOT_FILE_UPLOAD_URL_MAX];
  er = build_request_url(host, device_id, AZ_IOT_FILEUPLOAD_NOTIFY_PATH, &url);
  if (er != AZ_IOT_OK)
  {
    return er;
  }

  char body[AZ_IOT_FILE_UPLOAD_BODY_MAX];
  size_t body_len = 0;
  az_iot_result br
      = build_notification_body(body, sizeof(body), correlation_id, is_success, &body_len);
  if (br != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARN(
        "gen1_file_upload: the correlation id does not fit AZ_IOT_FILE_UPLOAD_BODY_MAX");
    return br;
  }

  az_iot_file_upload_http_response resp;
  memset(&resp, 0, sizeof(resp));

  az_iot_result tr = FI(client).http_send(
      AZ_IOT_FILEUPLOAD_METHOD_POST,
      url,
      "",
      AZ_IOT_FILEUPLOAD_CONTENT_TYPE_JSON,
      (const uint8_t*)body,
      body_len,
      &resp,
      FI(client).http_ctx);
  if (tr != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen1_file_upload: the completion notification failed in the transport hook (%s)",
        az_iot_result_to_string(tr));
    cb(tr, user_ctx);
    return AZ_IOT_OK;
  }

  az_iot_result r = http_status_to_result(resp.status_code);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_WARNF(
        "gen1_file_upload: the hub refused the completion notification (HTTP %d)",
        resp.status_code);
  }
  cb(r, user_ctx);
  return AZ_IOT_OK;
}
