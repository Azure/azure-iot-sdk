// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* authentication/dps_csr_managed
 *
 * DPS enrollment that obtains an OPERATIONAL certificate via CSR, using the
 * OpenSSL-backed "managed" certificate provider (increments 3, 5, 6).
 *
 * Flow:
 *   1. The device authenticates to DPS with an X.509 BOOTSTRAP identity.
 *   2. request_operational_certificate=true makes the connection client ask the
 *      provider for a CSR and send it in the DPS register body.
 *   3. DPS returns the issued operational certificate chain; the managed
 *      provider persists it to disk and the connection proceeds to the assigned
 *      hub using the OPERATIONAL identity.
 *   4. The optional operational-cert callback (D4) is notified of the new chain.
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
    int  issued;
} user_context_t;

static void on_conn_state(az_iot_connection_state_t s, az_iot_result_t reason, void* user_ctx)
{
    (void)reason;
    ((user_context_t*)user_ctx)->conn_state = s;
}

static void on_operational_cert(const az_iot_issued_certificate_t* issued, void* user_ctx)
{
    ((user_context_t*)user_ctx)->issued = 1;
    fprintf(stderr, "[dps_csr] operational certificate issued: %zu cert(s) in chain\n",
        issued ? issued->count : (size_t)0);
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
        fprintf(stderr, "[dps_csr] managed provider init failed\n");
        goto cleanup;
    }

    az_iot_connection_client_options_t copts =
        az_iot_connection_client_options_get_default(config.id_scope, config.reg_id, &provider.base);
    copts.dps.request_operational_certificate = true; /* opt in to CSR enrollment (D2) */

    if (az_iot_connection_client_init(&connection_client, &copts) != AZ_IOT_OK)
        goto cleanup;

    az_iot_connection_client_set_state_callback(&connection_client, on_conn_state, &user_ctx);
    az_iot_connection_client_set_operational_cert_callback(&connection_client, on_operational_cert, &user_ctx);

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
        fprintf(stderr, "[dps_csr] connected with %s identity\n",
            user_ctx.issued ? "operational" : "bootstrap");
        rc = 0;
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
