// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* c2d_receiver - API A sample.
 *
 * Provision via DPS, open connection, subscribe for cloud-to-device messages,
 * print received payloads. Runs for ~60 seconds then exits. DPS is handled
 * internally by the connection client when host == NULL and dps.id_scope is set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

typedef struct
{
    sample_config_t                   config;
    az_iot_certificate_provider_pem_t certs;
    az_iot_connection_client_t        connection_client;
    az_iot_c2d_client_t              c2d_client;
} sample_state_t;

static void sample_state_destroy(sample_state_t* s)
{
    az_iot_c2d_client_deinit(&s->c2d_client);
    az_iot_connection_client_deinit(&s->connection_client);
    az_iot_certificate_provider_pem_deinit(&s->certs);
    sample_config_release(&s->config);
}

typedef struct
{
    az_iot_connection_state_t conn_state;
    int messages_received;
} user_context_t;

static void on_conn_state(az_iot_connection_state_t s, az_iot_result_t reason, void* user_ctx)
{
    (void)reason;
    ((user_context_t*)user_ctx)->conn_state = s;
}

static void on_c2d(
    const uint8_t* payload,
    size_t payload_len,
    const char* content_type,
    void* user_ctx)
{
    user_context_t* ctx = (user_context_t*)user_ctx;
    ctx->messages_received++;

    printf("C2D #%d: %zu bytes", ctx->messages_received, payload_len);
    if (content_type)
        printf(" [%s]", content_type);
    if (payload_len > 0 && payload_len <= 256)
        printf(" => %.*s", (int)payload_len, (const char*)payload);
    printf("\n");
}

int main(void)
{
    az_iot_log_sink_t log = az_iot_log_stderr_sink(AZ_IOT_LOG_ERROR);
    az_iot_log_set_global_sink(&log);

    sample_state_t sample_state = {0};
    if (sample_config_load(&sample_state.config) != 0)
    {
        return 1;
    }

    int rc = 1;
    user_context_t user_ctx = {0};

    /* Certificate provider */
    az_iot_certificate_provider_pem_options_t pem = {
        .trusted_ca_pem_path = sample_state.config.ca,
        .client_cert_pem_path = sample_state.config.cert,
        .client_key_pem_path = sample_state.config.key };

    if (az_iot_certificate_provider_pem_init(&sample_state.certs, &pem) != AZ_IOT_OK)
    {
        sample_state_destroy(&sample_state);
        return 1;
    }

    /* Connection client (DPS provisioning is internal when host==NULL) */
    az_iot_connection_client_options_t copts =
        az_iot_connection_client_options_get_default(
            sample_state.config.id_scope, sample_state.config.reg_id, &sample_state.certs.base);

    if (az_iot_connection_client_init(&sample_state.connection_client, &copts) != AZ_IOT_OK)
    {
        sample_state_destroy(&sample_state);
        return 1;
    }
    az_iot_connection_client_set_state_callback(&sample_state.connection_client, on_conn_state, &user_ctx);

    /* MQTT adapters: register both v3.1.1 (DPS + Classic) and v5 (Next). */
    if (az_iot_connection_client_register_mqtt_factory(
            &sample_state.connection_client, az_iot_paho_factory_create_v3_1_1()) != AZ_IOT_OK)
    {
        sample_state_destroy(&sample_state);
        return 1;
    }
    if (az_iot_connection_client_register_mqtt_factory(
            &sample_state.connection_client, az_iot_paho_factory_create_v5()) != AZ_IOT_OK)
    {
        sample_state_destroy(&sample_state);
        return 1;
    }

    /* C2D client */
    if (az_iot_c2d_client_init(&sample_state.c2d_client, &sample_state.connection_client) != AZ_IOT_OK)
    {
        sample_state_destroy(&sample_state);
        return 1;
    }
    az_iot_c2d_client_set_handler(&sample_state.c2d_client, on_c2d, &user_ctx);

    /* Open (internally provisions via DPS then connects to assigned hub) */
    if (az_iot_connection_client_open(&sample_state.connection_client) != AZ_IOT_OK)
    {
        sample_state_destroy(&sample_state);
        return 1;
    }

    for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
    {
        (void)az_iot_connection_client_do_work(&sample_state.connection_client, 50);
        if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
            break;
    }

    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
    {
        printf("Connected. Listening for C2D messages (~60s)...\n");

        /* Pump for ~60 seconds (600 * 100ms) */
        for (int i = 0; i < 600; ++i)
            (void)az_iot_connection_client_do_work(&sample_state.connection_client, 100);

        printf("Done. Received %d C2D message(s).\n", user_ctx.messages_received);
        rc = 0;
    }

    /* Close connection */
    az_iot_connection_client_close(&sample_state.connection_client);

    for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
        (void)az_iot_connection_client_do_work(&sample_state.connection_client, 50);

    sample_state_destroy(&sample_state);
    return rc;
}
