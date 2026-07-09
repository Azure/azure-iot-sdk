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

/**
 * @brief Callback invoked when a SAS URI response is received from IoT Hub.
 *
 * @param status          AZ_IOT_OK on success.
 * @param blob_sas_uri    The SAS URI to use for uploading the blob to Azure Storage.
 *                        Valid only for the lifetime of this callback. Copy if needed.
 * @param correlation_id  Opaque ID that must be passed back to notify_complete().
 *                        Valid only for the lifetime of this callback. Copy if needed.
 * @param user_ctx        User context passed to get_sas_uri().
 */
typedef void (*az_iot_file_upload_sas_callback)(
    az_iot_result status,
    const char* blob_sas_uri,
    const char* correlation_id,
    void* user_ctx);

/**
 * @brief Callback invoked when the upload completion notification is acknowledged.
 *
 * @param status    AZ_IOT_OK on success.
 * @param user_ctx  User context passed to notify_complete().
 */
typedef void (*az_iot_file_upload_complete_callback)(az_iot_result status, void* user_ctx);

#define AZ_IOT_FILE_UPLOAD_MAX_PENDING 4

typedef struct az_iot_file_upload_client
{
    struct
    {
        az_iot_connection_client* conn;
        uint32_t next_rid;
        struct
        {
            bool in_use;
            uint32_t rid;
            int kind; /* 0=none, 1=sas_uri, 2=notify */
            union
            {
                az_iot_file_upload_sas_callback sas_cb;
                az_iot_file_upload_complete_callback complete_cb;
            } cb;
            void* user_ctx;
        } pending[AZ_IOT_FILE_UPLOAD_MAX_PENDING];
    } _internal;
} az_iot_file_upload_client;

/**
 * @brief Initialize the file upload client.
 *
 * File upload is supported only on Classic IoT Hub (MQTT v3.1.1). Calling this
 * when connected to IoT/AEG Hub returns AZ_IOT_ERR_NOT_SUPPORTED.
 *
 * @param client  File upload client instance to initialize.
 * @param conn    Connection client (must already be initialized).
 * @return AZ_IOT_OK on success.
 */
az_iot_result az_iot_file_upload_client_init(
    az_iot_file_upload_client* client,
    az_iot_connection_client* conn);

/**
 * @brief Deinitialize the file upload client and unregister inbound handlers.
 */
void az_iot_file_upload_client_destroy(az_iot_file_upload_client* client);

/**
 * @brief Request a SAS URI for uploading a blob.
 *
 * Publishes a request to IoT Hub. The response arrives asynchronously via
 * the callback during a subsequent do_work() call.
 *
 * @param client     File upload client instance.
 * @param blob_name  Name of the blob to upload (e.g. "mydata/sensor.csv").
 * @param cb         Callback invoked with the SAS URI on success or error.
 * @param user_ctx   User context forwarded to the callback.
 * @return AZ_IOT_OK if the request was published successfully.
 */
az_iot_result az_iot_file_upload_client_get_sas_uri(
    az_iot_file_upload_client* client,
    const char* blob_name,
    az_iot_file_upload_sas_callback cb,
    void* user_ctx);

/**
 * @brief Notify IoT Hub that the file upload is complete.
 *
 * After the application has uploaded the blob to Azure Storage (HTTP PUT using
 * the SAS URI), call this to inform IoT Hub of the result.
 *
 * @param client          File upload client instance.
 * @param correlation_id  The correlation ID received in the SAS URI callback.
 * @param is_success      Whether the blob upload succeeded.
 * @param cb              Callback invoked when the notification is acknowledged.
 * @param user_ctx        User context forwarded to the callback.
 * @return AZ_IOT_OK if the notification was published successfully.
 */
az_iot_result az_iot_file_upload_client_notify_complete(
    az_iot_file_upload_client* client,
    const char* correlation_id,
    bool is_success,
    az_iot_file_upload_complete_callback cb,
    void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_FILE_UPLOAD_CLIENT_H */
