// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* file_upload - sample.
 *
 * Provision via DPS, open connection, request a SAS URI for file upload,
 * simulate a blob upload (the actual HTTP PUT is a placeholder), then notify
 * IoT Hub of completion. DPS is handled internally by the connection client
 * when host == NULL and dps.id_scope is set.
 *
 * NOTE: File Upload is supported only on Classic IoT Hub.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

typedef struct
{
    sample_config                      config;
    az_iot_certificate_provider_pem    certs;
    az_iot_connection_client           connection_client;
    az_iot_file_upload_client          file_upload_client;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
    az_iot_file_upload_client_destroy(&s->file_upload_client);
    az_iot_connection_client_destroy(&s->connection_client);
    az_iot_certificate_provider_pem_destroy(&s->certs);
    sample_config_release(&s->config);
}

typedef struct
{
    az_iot_connection_state conn_state;
    int sas_done;
    int notify_done;
    az_iot_result sas_status;
    az_iot_result notify_status;
    char sas_uri[2048];
    char correlation_id[128];
} user_context;

static void on_conn_state(az_iot_connection_state s, az_iot_result reason, void* user_ctx)
{
    (void)reason;
    ((user_context*)user_ctx)->conn_state = s;
}

static void on_sas_uri(
    az_iot_result status,
    const char* blob_sas_uri,
    const char* correlation_id,
    void* user_ctx)
{
    user_context* ctx = (user_context*)user_ctx;
    ctx->sas_status = status;
    ctx->sas_done = 1;

    if (status == AZ_IOT_OK && blob_sas_uri && correlation_id)
    {
        snprintf(ctx->sas_uri, sizeof(ctx->sas_uri), "%s", blob_sas_uri);
        snprintf(ctx->correlation_id, sizeof(ctx->correlation_id), "%s", correlation_id);
        printf("SAS URI received: %s\n", blob_sas_uri);
        printf("Correlation ID:   %s\n", correlation_id);
    }
    else
    {
        printf("SAS URI request failed: %s\n", az_iot_result_to_string(status));
    }
}

static void on_notify_complete(az_iot_result status, void* user_ctx)
{
    user_context* ctx = (user_context*)user_ctx;
    ctx->notify_status = status;
    ctx->notify_done = 1;
    printf("Upload notification: %s\n", az_iot_result_to_string(status));
}

int main(void)
{
    az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_ERROR);
    az_iot_log_set_global_sink(&log);

    sample_state state = {0};
    if (sample_config_load(&state.config) != 0)
    {
        return 1;
    }

    int rc = 1;
    user_context user_ctx = {0};

    /* Certificate provider */
    az_iot_certificate_provider_pem_options pem = {
        .trusted_ca_pem_path = state.config.ca,
        .client_cert_pem_path = state.config.cert,
        .client_key_pem_path = state.config.key };

    if (az_iot_certificate_provider_pem_init(&state.certs, &pem) != AZ_IOT_OK)
    {
        sample_state_destroy(&state);
        return 1;
    }

    /* Connection client (DPS provisioning is internal when host==NULL) */
    az_iot_connection_client_options copts = az_iot_connection_client_options_default();
    copts.dps.id_scope = state.config.id_scope;
    copts.dps.registration_id = state.config.reg_id;
    copts.certificate_provider = &state.certs.base;

    if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
    {
        sample_state_destroy(&state);
        return 1;
    }
    az_iot_connection_client_set_state_callback(&state.connection_client, on_conn_state, &user_ctx);

    /* MQTT adapters: register both v3.1.1 (DPS + Classic) and v5 (Next).
     * File upload only works on Classic, but we register both for DPS support. */
    if (az_iot_connection_client_register_mqtt_factory(
            &state.connection_client, az_iot_paho_factory_create_v3_1_1()) != AZ_IOT_OK)
    {
        sample_state_destroy(&state);
        return 1;
    }
    if (az_iot_connection_client_register_mqtt_factory(
            &state.connection_client, az_iot_paho_factory_create_v5()) != AZ_IOT_OK)
    {
        sample_state_destroy(&state);
        return 1;
    }

    /* File upload client */
    if (az_iot_file_upload_client_init(&state.file_upload_client, &state.connection_client) != AZ_IOT_OK)
    {
        sample_state_destroy(&state);
        return 1;
    }

    /* Open (internally provisions via DPS then connects to assigned hub) */
    if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
    {
        sample_state_destroy(&state);
        return 1;
    }

    for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
    {
        (void)az_iot_connection_client_do_work(&state.connection_client, 50);
        if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
            break;
    }

    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
    {
        /* Step 1: Request SAS URI */
        printf("Requesting SAS URI for blob 'sample-data/test.txt'...\n");
        if (az_iot_file_upload_client_get_sas_uri(
                &state.file_upload_client,
                "sample-data/test.txt",
                on_sas_uri, &user_ctx) != AZ_IOT_OK)
        {
            printf("Failed to send SAS URI request.\n");
            goto cleanup;
        }

        /* Wait for SAS URI response */
        for (int i = 0; i < 600 && !user_ctx.sas_done; ++i)
            (void)az_iot_connection_client_do_work(&state.connection_client, 50);

        if (!user_ctx.sas_done || user_ctx.sas_status != AZ_IOT_OK)
        {
            printf("SAS URI request did not complete successfully.\n");
            goto cleanup;
        }

        /* Step 2: Upload blob (HTTP PUT — simulated here) */
        printf("Simulating blob upload to Azure Storage...\n");
        printf("(In production, perform HTTP PUT to: %s)\n", user_ctx.sas_uri);
        /* In a real application, you would use an HTTP client library here
         * to upload your file content to the SAS URI. */
        bool upload_success = true; /* simulated success */

        /* Step 3: Notify IoT Hub of completion */
        printf("Notifying IoT Hub of upload completion...\n");
        if (az_iot_file_upload_client_notify_complete(
                &state.file_upload_client,
                user_ctx.correlation_id,
                upload_success,
                on_notify_complete, &user_ctx) != AZ_IOT_OK)
        {
            printf("Failed to send completion notification.\n");
            goto cleanup;
        }

        /* Wait for notification acknowledgement */
        for (int i = 0; i < 600 && !user_ctx.notify_done; ++i)
            (void)az_iot_connection_client_do_work(&state.connection_client, 50);

        if (user_ctx.notify_done && user_ctx.notify_status == AZ_IOT_OK)
        {
            printf("File upload completed successfully.\n");
            rc = 0;
        }
    }

cleanup:
    /* Close connection */
    az_iot_connection_client_close(&state.connection_client);

    for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
        (void)az_iot_connection_client_do_work(&state.connection_client, 50);

    sample_state_destroy(&state);

    return rc;
}
