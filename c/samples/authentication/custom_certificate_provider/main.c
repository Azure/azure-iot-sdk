// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* authentication/custom_certificate_provider
 *
 * The COMPLETE, app-owned certificate-provider path (vs. the shipped "managed"
 * provider used by dps_csr_managed). It wires sample_cert_provider - a real
 * az_iot_certificate_provider implemented in samples/common with platform-
 * native CSR issuance (OpenSSL on Linux, CNG on Windows) - into a full DPS CSR
 * enrollment: authenticate to DPS with the bootstrap identity, send a real
 * PKCS#10 CSR, persist the issued operational chain, and connect to the assigned
 * hub with the OPERATIONAL identity.
 *
 * Use this as the copy-paste starting point for integrating your own crypto.
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

#include "sample_cert_provider.h"
#include "sample_utils.h"

typedef struct
{
    az_iot_connection_state conn_state;
    int  issued;
} user_context;

static void on_conn_state(az_iot_connection_state s, az_iot_result reason, void* user_ctx)
{
    (void)reason;
    ((user_context*)user_ctx)->conn_state = s;
}

static void on_operational_cert(const az_iot_issued_certificate* issued, void* user_ctx)
{
    ((user_context*)user_ctx)->issued = 1;
    fprintf(stderr, "[custom_cert] operational certificate issued: %zu cert(s) in chain\n",
        issued ? issued->count : (size_t)0);
}

int main(void)
{
    az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
    az_iot_log_set_global_sink(&log);

    sample_config config = {0};
    if (sample_config_load(&config) != 0)
    {
        return 1;
    }

    char* op_key  = sample_env_dup("AZ_IOT_OPERATIONAL_KEY", "operational_key.pem");
    char* op_cert = sample_env_dup("AZ_IOT_OPERATIONAL_CERT", "operational_cert.pem");

    int rc = 1;
    user_context user_ctx = {0};
    sample_cert_provider provider = {0};
    az_iot_connection_client connection_client = {0};

    sample_cert_provider_options popts = {
        .bootstrap_cert_path   = config.cert,
        .bootstrap_key_path    = config.key,
        .trusted_ca_path       = config.ca,
        .operational_key_path  = op_key,
        .operational_cert_path = op_cert,
    };
    if (sample_cert_provider_init(&provider, &popts) != AZ_IOT_OK)
    {
        fprintf(stderr, "[custom_cert] provider init failed\n");
        goto cleanup;
    }

    az_iot_connection_client_options copts = az_iot_connection_client_options_default();
    copts.dps.id_scope = config.id_scope;
    copts.dps.registration_id = config.reg_id;
    copts.certificate_provider = &provider.base;
    AZ_IOT_CSR_PAYLOAD_STORAGE(csr_payload_buf);
    copts.csr_payload_buffer = az_span_create(csr_payload_buf, sizeof(csr_payload_buf));
    copts.dps.request_operational_certificate = true; /* opt in to CSR enrollment */

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
        fprintf(stderr, "[custom_cert] connected with %s identity\n",
            user_ctx.issued ? "operational" : "bootstrap");
        rc = 0;
    }

    az_iot_connection_client_close(&connection_client);
    for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
        (void)az_iot_connection_client_do_work(&connection_client, 50);

cleanup:
    az_iot_connection_client_destroy(&connection_client);
    sample_cert_provider_destroy(&provider);
    free(op_key);
    free(op_cert);
    sample_config_release(&config);
    return rc;
}
