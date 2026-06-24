// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* az_iot_e2e_agent - device-side driver for the end-to-end test suite.
 *
 * Runs a SINGLE scriptable scenario against a real Azure IoT Hub / DPS instance
 * and returns 0 on success, non-zero on failure. The service-side half of each
 * scenario (sending C2D, invoking methods, reading telemetry from the
 * EventHub-compatible endpoint, twin reads/updates) and ALL assertions live in
 * the dotnet harness (c/tests/e2e/driver), which launches this process and
 * reuses the Microsoft.Azure.Devices service client.
 *
 * Device auth follows the SDK's real connect flow: DPS provisioning with an
 * X.509 individual enrollment (host == NULL + dps.id_scope set -> the
 * connection client provisions internally, then connects to the assigned hub).
 *
 * Configuration is taken entirely from environment variables set by the
 * harness:
 *   AZ_IOT_DPS_ID_SCOPE         DPS id scope                       (required)
 *   AZ_IOT_DPS_REGISTRATION_ID  individual enrollment reg id       (required)
 *   AZ_IOT_CLIENT_CERT          path to device X.509 cert PEM      (required)
 *   AZ_IOT_CLIENT_KEY           path to device X.509 key PEM       (required)
 *   AZ_IOT_TRUSTED_CA           path to CA bundle PEM (TLS server) (required)
 *   AZ_IOT_DPS_GLOBAL_ENDPOINT  DPS global endpoint override       (optional)
 *   AZ_IOT_E2E_PAYLOAD          telemetry payload (telemetry only) (optional)
 *
 * Usage: az_iot_e2e_agent <scenario>
 *   scenario = telemetry
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

/* Bounded pump budgets (each tick is a do_work(50ms) call). */
#define E2E_CONNECT_TICKS 1200 /* ~60s to provision via DPS + connect */
#define E2E_SEND_TICKS    600  /* ~30s for a single publish + ack     */
#define E2E_CLOSE_TICKS   100  /* ~5s graceful disconnect             */

/* Duplicate an environment variable into a heap buffer (portable). Returns
 * NULL when unset/empty. Caller frees. */
static char* env_dup(const char* name)
{
#ifdef _WIN32
    char* value = NULL;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == NULL || value[0] == '\0')
    {
        free(value);
        return NULL;
    }
    return value;
#else
    const char* value = getenv(name);
    if (value == NULL || value[0] == '\0')
    {
        return NULL;
    }
    {
        size_t n = strlen(value) + 1;
        char* copy = (char*)malloc(n);
        if (copy != NULL)
        {
            memcpy(copy, value, n);
        }
        return copy;
    }
#endif
}

typedef struct
{
    char* id_scope;
    char* reg_id;
    char* cert;
    char* key;
    char* ca;
    char* global_endpoint; /* optional */
    char* payload;         /* optional */
} e2e_config_t;

static void e2e_config_release(e2e_config_t* cfg)
{
    free(cfg->id_scope);
    free(cfg->reg_id);
    free(cfg->cert);
    free(cfg->key);
    free(cfg->ca);
    free(cfg->global_endpoint);
    free(cfg->payload);
    memset(cfg, 0, sizeof(*cfg));
}

static int e2e_config_load(e2e_config_t* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->id_scope        = env_dup("AZ_IOT_DPS_ID_SCOPE");
    cfg->reg_id          = env_dup("AZ_IOT_DPS_REGISTRATION_ID");
    cfg->cert            = env_dup("AZ_IOT_CLIENT_CERT");
    cfg->key             = env_dup("AZ_IOT_CLIENT_KEY");
    cfg->ca              = env_dup("AZ_IOT_TRUSTED_CA");
    cfg->global_endpoint = env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT");
    cfg->payload         = env_dup("AZ_IOT_E2E_PAYLOAD");

    if (cfg->id_scope == NULL || cfg->reg_id == NULL || cfg->cert == NULL
        || cfg->key == NULL || cfg->ca == NULL)
    {
        fprintf(stderr,
            "[e2e_agent] missing required env vars: "
            "AZ_IOT_DPS_ID_SCOPE/AZ_IOT_DPS_REGISTRATION_ID/AZ_IOT_CLIENT_CERT/"
            "AZ_IOT_CLIENT_KEY/AZ_IOT_TRUSTED_CA\n");
        return 1;
    }
    return 0;
}

typedef struct
{
    az_iot_connection_state_t conn_state;
    int                       send_done;
    az_iot_result_t           send_status;
} user_context_t;

static void on_conn_state(az_iot_connection_state_t s, az_iot_result_t reason, void* user_ctx)
{
    (void)reason;
    ((user_context_t*)user_ctx)->conn_state = s;
}

static void on_send_done(az_iot_result_t status, void* user_ctx)
{
    user_context_t* ctx = (user_context_t*)user_ctx;
    ctx->send_status = status;
    ctx->send_done = 1;
}

/* telemetry scenario: provision, connect, publish one telemetry message
 * carrying the harness-supplied payload (which embeds a unique correlation id),
 * then disconnect. Returns 0 only when the broker ACKs the publish. */
static int run_telemetry(az_iot_connection_client_t* conn, user_context_t* ctx, const char* payload)
{
    az_iot_telemetry_client_t telemetry_client;
    if (az_iot_telemetry_client_init(&telemetry_client, conn) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e_agent] telemetry client init failed\n");
        return 1;
    }

    int rc = 1;
    az_iot_telemetry_property_t props[] = {
        { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
    };
    az_iot_telemetry_message_t msg = { 0 };
    msg.payload = (const uint8_t*)payload;
    msg.payload_len = strlen(payload);
    msg.properties = props;
    msg.properties_count = sizeof(props) / sizeof(props[0]);

    if (az_iot_telemetry_client_send(&telemetry_client, &msg, on_send_done, ctx) == AZ_IOT_OK)
    {
        for (int i = 0; i < E2E_SEND_TICKS && !ctx->send_done; ++i)
        {
            (void)az_iot_connection_client_do_work(conn, 50);
        }
        if (ctx->send_done && ctx->send_status == AZ_IOT_OK)
        {
            rc = 0;
        }
        else
        {
            fprintf(stderr, "[e2e_agent] telemetry send not acked (done=%d status=%d)\n",
                ctx->send_done, (int)ctx->send_status);
        }
    }
    else
    {
        fprintf(stderr, "[e2e_agent] telemetry send call failed\n");
    }

    az_iot_telemetry_client_deinit(&telemetry_client);
    return rc;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: az_iot_e2e_agent <scenario>\n");
        return 2;
    }
    const char* scenario = argv[1];

    az_iot_log_sink_t log = az_iot_log_stderr_sink(AZ_IOT_LOG_ERROR);
    az_iot_log_set_global_sink(&log);

    e2e_config_t cfg;
    if (e2e_config_load(&cfg) != 0)
    {
        return 2;
    }

    int rc = 1;
    user_context_t ctx = { 0 };
    az_iot_certificate_provider_pem_t certs;
    az_iot_connection_client_t conn;
    int certs_ok = 0;
    int conn_ok = 0;

    az_iot_certificate_provider_pem_options_t pem = {
        .trusted_ca_pem_path = cfg.ca,
        .client_cert_pem_path = cfg.cert,
        .client_key_pem_path = cfg.key,
    };
    if (az_iot_certificate_provider_pem_init(&certs, &pem) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e_agent] certificate provider init failed\n");
        goto cleanup;
    }
    certs_ok = 1;

    az_iot_connection_client_options_t copts =
        az_iot_connection_client_options_get_default(cfg.id_scope, cfg.reg_id, &certs.base);
    if (cfg.global_endpoint != NULL)
    {
        copts.dps.global_endpoint = cfg.global_endpoint;
    }

    if (az_iot_connection_client_init(&conn, &copts) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e_agent] connection client init failed\n");
        goto cleanup;
    }
    conn_ok = 1;
    az_iot_connection_client_set_state_callback(&conn, on_conn_state, &ctx);

    if (az_iot_connection_client_register_mqtt_factory(&conn, az_iot_paho_factory_create_v3_1_1()) != AZ_IOT_OK
        || az_iot_connection_client_register_mqtt_factory(&conn, az_iot_paho_factory_create_v5()) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e_agent] MQTT factory registration failed\n");
        goto cleanup;
    }

    if (az_iot_connection_client_open(&conn) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e_agent] connection open failed\n");
        goto cleanup;
    }

    for (int i = 0; i < E2E_CONNECT_TICKS && ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
    {
        (void)az_iot_connection_client_do_work(&conn, 50);
        if (ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
        {
            break;
        }
    }

    if (ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED)
    {
        fprintf(stderr, "[e2e_agent] failed to reach CONNECTED (state=%d)\n", (int)ctx.conn_state);
        goto cleanup;
    }

    if (strcmp(scenario, "telemetry") == 0)
    {
        const char* payload = (cfg.payload != NULL) ? cfg.payload : "{\"e2e\":\"telemetry\"}";
        rc = run_telemetry(&conn, &ctx, payload);
    }
    else
    {
        fprintf(stderr, "[e2e_agent] unknown scenario '%s'\n", scenario);
        rc = 2;
    }

cleanup:
    if (conn_ok)
    {
        az_iot_connection_client_close(&conn);
        for (int i = 0; i < E2E_CLOSE_TICKS && ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
        {
            (void)az_iot_connection_client_do_work(&conn, 50);
        }
        az_iot_connection_client_deinit(&conn);
    }
    if (certs_ok)
    {
        az_iot_certificate_provider_pem_deinit(&certs);
    }
    e2e_config_release(&cfg);
    return rc;
}
