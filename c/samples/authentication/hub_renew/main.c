// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* authentication/hub_renew
 *
 * Runtime operational-certificate renewal against a connected (Classic) hub
 * (increment 4, D7). While connected, the device produces a fresh CSR from its
 * managed provider and calls az_iot_connection_client_send_csr(). The hub
 * responds in two phases - ACCEPTED (202) then ISSUED (200) with the new chain -
 * and the sample persists the renewed chain back through the provider.
 *
 * Requires the managed provider (OpenSSL 3.0+).
 *
 * Environment:
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID
 *   AZ_IOT_CLIENT_CERT, AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA  (bootstrap X.509)
 *   AZ_IOT_OPERATIONAL_KEY   (optional; default operational_key.pem)
 *   AZ_IOT_OPERATIONAL_CERT  (optional; default operational_cert.pem)
 */
#include <stdio.h>
#include <stdlib.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "az_iot_certificate_provider_managed.h"

#include "sample_utils.h"

typedef struct
{
    az_iot_connection_state_t conn_state;
    az_iot_certificate_provider_managed_t* provider;
    int  csr_done;
    az_iot_result_t csr_status;
} user_context_t;

static void on_conn_state(az_iot_connection_state_t s, az_iot_result_t reason, void* user_ctx)
{
    (void)reason;
    ((user_context_t*)user_ctx)->conn_state = s;
}

static void on_csr_event(const az_iot_csr_event_t* evt, void* user_ctx)
{
    user_context_t* ctx = (user_context_t*)user_ctx;
    switch (evt->kind)
    {
        case AZ_IOT_CSR_ACCEPTED:
            fprintf(stderr, "[hub_renew] hub accepted CSR; signing in progress\n");
            break;
        case AZ_IOT_CSR_ISSUED:
            fprintf(stderr, "[hub_renew] renewed chain issued: %zu cert(s)\n",
                evt->issued ? evt->issued->count : (size_t)0);
            /* Persist the renewed chain through the provider. */
            if (evt->issued)
            {
                (void)ctx->provider->base.vtable->store_issued_certificate(
                    &ctx->provider->base, evt->issued);
            }
            ctx->csr_status = AZ_IOT_OK;
            ctx->csr_done = 1;
            break;
        case AZ_IOT_CSR_FAILED:
        default:
            fprintf(stderr, "[hub_renew] CSR failed: status=%d service_code=%d retry_after=%us\n",
                (int)evt->status, (int)evt->service_code, evt->retry_after_s);
            ctx->csr_status = evt->status;
            ctx->csr_done = 1;
            break;
    }
}

int main(void)
{
    az_iot_log_sink_t log = az_iot_log_stderr_sink(AZ_IOT_LOG_ERROR);
    az_iot_log_set_global_sink(&log);

    sample_config_t config = {0};
    if (sample_config_load(&config) != 0)
    {
        return 1;
    }

    char* op_key  = sample_env_dup("AZ_IOT_OPERATIONAL_KEY", "operational_key.pem");
    char* op_cert = sample_env_dup("AZ_IOT_OPERATIONAL_CERT", "operational_cert.pem");

    int rc = 1;
    user_context_t user_ctx = {0};
    az_iot_certificate_provider_managed_t provider = {0};
    az_iot_connection_client_t connection_client = {0};
    user_ctx.provider = &provider;

    az_iot_certificate_provider_managed_options_t mopts = {
        .bootstrap_cert_pem_path   = config.cert,
        .bootstrap_key_pem_path    = config.key,
        .trusted_ca_pem_path       = config.ca,
        .operational_key_pem_path  = op_key,
        .operational_cert_pem_path = op_cert,
        .key_type                  = AZ_IOT_MANAGED_KEY_EC_P256,
    };
    if (az_iot_certificate_provider_managed_init(&provider, &mopts) != AZ_IOT_OK)
    {
        fprintf(stderr, "[hub_renew] managed provider init failed\n");
        goto cleanup;
    }

    az_iot_connection_client_options_t copts =
        az_iot_connection_client_options_get_default(config.id_scope, config.reg_id, &provider.base);

    if (az_iot_connection_client_init(&connection_client, &copts) != AZ_IOT_OK)
        goto cleanup;

    az_iot_connection_client_set_state_callback(&connection_client, on_conn_state, &user_ctx);

    if (az_iot_connection_client_register_mqtt_factory(
            &connection_client, az_iot_paho_factory_create_v3_1_1()) != AZ_IOT_OK)
        goto cleanup;

    if (az_iot_connection_client_open(&connection_client) != AZ_IOT_OK)
        goto cleanup;

    for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
    {
        (void)az_iot_connection_client_do_work(&connection_client, 50);
        if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
            break;
    }

    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
    {
        /* Produce a CSR from the operational key and request renewal. */
        az_iot_certificate_signing_request_t csr = {0};
        if (provider.base.vtable->get_csr(&provider.base, config.reg_id, &csr) == AZ_IOT_OK)
        {
            az_iot_result_t send_rc = az_iot_connection_client_send_csr(
                &connection_client, &csr, NULL, NULL, on_csr_event, &user_ctx);
            provider.base.vtable->release_csr(&provider.base, &csr);

            if (send_rc == AZ_IOT_OK)
            {
                for (int i = 0; i < 1200 && !user_ctx.csr_done; ++i)
                    (void)az_iot_connection_client_do_work(&connection_client, 50);

                if (user_ctx.csr_done && user_ctx.csr_status == AZ_IOT_OK)
                    rc = 0;
            }
        }
    }

    az_iot_connection_client_close(&connection_client);
    for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
        (void)az_iot_connection_client_do_work(&connection_client, 50);

cleanup:
    az_iot_connection_client_deinit(&connection_client);
    az_iot_certificate_provider_managed_deinit(&provider);
    free(op_key);
    free(op_cert);
    sample_config_release(&config);
    return rc;
}
