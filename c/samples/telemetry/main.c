// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* telemetry - API A sample.
 *
 * Provision via DPS, open connection, send one telemetry message, close.
 * DPS is handled internally by the connection client when host == NULL and
 * dps.id_scope is set.
 */
#include <stdlib.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

typedef struct
{
    sample_config             config;
    az_iot_certificate_provider_pem certs;
    az_iot_connection_client  connection_client;
    az_iot_telemetry_client   telemetry_client;
} sample_state;

static void sample_state_destroy(sample_state* state)
{
    az_iot_telemetry_client_destroy(&state->telemetry_client);
    az_iot_connection_client_destroy(&state->connection_client);
    az_iot_certificate_provider_pem_destroy(&state->certs);
    sample_config_release(&state->config);
}

typedef struct
{
    az_iot_connection_state conn_state;
    int send_done;
    az_iot_result send_status;
} user_context;

static void on_conn_state(az_iot_connection_state s, az_iot_result reason, void* user_ctx)
{
    (void)s; (void)reason;
    ((user_context*)user_ctx)->conn_state = s;
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
    user_context* ctx = (user_context*)user_ctx;
    ctx->send_status = status;
    ctx->send_done = 1;
}

int main(void)
{
    az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
    az_iot_log_set_global_sink(&log);

    sample_state state = {0};
    if (sample_config_load(&state.config) != 0)
    {
        return 1;
    }

    int rc = 1;
    user_context user_ctx = {0};

    /* Certificate provider */
    az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
    pem.trusted_ca_pem_path = state.config.ca;
    pem.client_cert_pem_path = state.config.cert;
    pem.client_key_pem_path = state.config.key;

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
     * The connection client selects the appropriate factory based on the
     * session role. Both may be backed by different MQTT libraries. */
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

    /* Telemetry client */
    if (az_iot_telemetry_client_init(&state.telemetry_client, &state.connection_client) != AZ_IOT_OK)
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
        /* Send one telemetry message */
        static const uint8_t payload[] = "{\"temp\":23}";
        az_iot_telemetry_property props[] = {
            { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
        };
        az_iot_telemetry_message msg = {0};
        msg.payload = payload;
        msg.payload_len = sizeof(payload) - 1;
        msg.properties = props;
        msg.properties_count = sizeof(props) / sizeof(props[0]);

        if (az_iot_telemetry_client_send(&state.telemetry_client, &msg, on_send_done, &user_ctx) == AZ_IOT_OK)
        {
            for (int i = 0; i < 600 && !user_ctx.send_done; ++i)
                (void)az_iot_connection_client_do_work(&state.connection_client, 50);

            if (user_ctx.send_done && user_ctx.send_status == AZ_IOT_OK)
                rc = 0;
        }
    }

    /* Close connection */
    az_iot_connection_client_close(&state.connection_client);

    for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
        (void)az_iot_connection_client_do_work(&state.connection_client, 50);

    sample_state_destroy(&state);

    return rc;
}
