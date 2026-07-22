// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* File upload end-to-end (Classic IoT Hub). Runs on the Linux e2e legs; skipped
 * on Windows (see the note in main()).
 *
 * Proves the device-side file-upload path against a real Azure IoT Hub that has
 * an Azure Storage account associated for file upload:
 *   1. The device provisions via DPS (X.509) and connects to the assigned hub.
 *   2. az_iot_file_upload_client_get_sas_uri() performs the HTTPS SAS-URI request
 *      (Classic control plane) through the application HTTP hook and returns a
 *      blob SAS URI + correlation id.
 *   3. The device PUTs a small blob to Azure Storage using that SAS URI.
 *   4. az_iot_file_upload_client_notify_complete() reports the outcome; the hub
 *      accepting it is proof the whole round-trip worked.
 *
 * The device HTTP hook (Classic control plane) and the Storage PUT go through
 * the e2e harness's own TLS transport (az_iot_e2e_https_request) -- the same
 * az_amqp transport the service facade uses -- so there is no external HTTP
 * client dependency. Mutual TLS with the device certificate authenticates the
 * hub REST calls; on Windows the harness's Schannel transport does not present
 * the self-signed device certificate on the CI runners, so the test self-skips
 * there (the OpenSSL transport on Linux presents it correctly).
 *
 * Requires the standard e2e device environment (provided by the e2e job):
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID,
 *   AZ_IOT_CLIENT_CERT, AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA  (device X.509),
 *   AZ_IOT_DPS_GLOBAL_ENDPOINT (optional),
 * and a hub with an Azure Storage account associated for file upload.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "az_iot_e2e_service.h"

#define E2E_CONNECT_TIMEOUT_S  120

static const char k_blob_content[] =
    "Hello from the Azure IoT C SDK file upload e2e test.\n";

/* ---- environment helpers -------------------------------------------------- */

#ifndef _WIN32
static char* dup_cstr(const char* s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* out = malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}
#endif

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
    const char* v = getenv(name);
    return (v && v[0]) ? dup_cstr(v) : NULL;
#endif
}

/* ---- HTTP (via the e2e harness transport) --------------------------------- */

/* Paths used by the HTTP hook to authenticate the Classic hub REST calls (mTLS). */
typedef struct
{
    const char* cert;
    const char* key;
} hub_http_ctx;

/* Split "https://host/path..." into host + origin-form path. */
static bool split_url(const char* url, char* host, size_t host_cap, char* path, size_t path_cap)
{
    const char* p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return false;

    const char* slash = strchr(p, '/');
    size_t host_len = (slash != NULL) ? (size_t)(slash - p) : strlen(p);
    if (host_len == 0 || host_len + 1 > host_cap) return false;
    memcpy(host, p, host_len);
    host[host_len] = '\0';

    const char* rest = (slash != NULL) ? slash : "/";
    size_t path_len = strlen(rest);
    if (path_len + 1 > path_cap) return false;
    memcpy(path, rest, path_len + 1);
    return true;
}

/* SDK HTTP transport hook for the Classic control plane: performs the hub REST
 * call over the harness TLS transport with the device certificate (mutual TLS). */
static az_iot_result e2e_http_send(
    const char* method, const char* url, const char* authorization,
    const char* content_type, const uint8_t* body, size_t body_len,
    az_iot_file_upload_http_response* response, void* hook_ctx)
{
    (void)authorization;
    hub_http_ctx* hc = (hub_http_ctx*)hook_ctx;

    char host[256];
    char path[1024];
    if (!split_url(url, host, sizeof(host), path, sizeof(path)))
        return AZ_IOT_ERR_INVALID_ARG;

    int status = 0;
    size_t resp_len = 0;
    if (!az_iot_e2e_https_request(
            host, method, path, hc->cert, hc->key, content_type, NULL,
            body, body_len, &status,
            (char*)response->body, response->body_capacity, &resp_len))
    {
        return AZ_IOT_ERR_MQTT;
    }
    response->status_code = status;
    response->body_len = resp_len;
    return AZ_IOT_OK;
}

/* ---- device callbacks ----------------------------------------------------- */

typedef struct
{
    az_iot_connection_state conn_state;
} conn_ctx;

static void on_conn_state(az_iot_connection_state s, az_iot_result reason, void* user_ctx)
{
    (void)reason;
    ((conn_ctx*)user_ctx)->conn_state = s;
}

typedef struct
{
    bool          sas_done;
    az_iot_result sas_status;
    char          sas_uri[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX];
    char          correlation_id[AZ_IOT_FILE_UPLOAD_CORR_ID_MAX];
    bool          notify_done;
    az_iot_result notify_status;
} upload_ctx;

static void on_sas(az_iot_result status, const char* uri, const char* corr, void* ctx)
{
    upload_ctx* u = (upload_ctx*)ctx;
    u->sas_done = true;
    u->sas_status = status;
    if (uri)  snprintf(u->sas_uri, sizeof(u->sas_uri), "%s", uri);
    if (corr) snprintf(u->correlation_id, sizeof(u->correlation_id), "%s", corr);
}

static void on_notify(az_iot_result status, void* ctx)
{
    upload_ctx* u = (upload_ctx*)ctx;
    u->notify_done = true;
    u->notify_status = status;
}

/* ---- scenario ------------------------------------------------------------- */

static void test_file_upload_classic(void** state)
{
    (void)state;

    char* id_scope = env_dup("AZ_IOT_DPS_ID_SCOPE");
    char* reg_id   = env_dup("AZ_IOT_DPS_REGISTRATION_ID");
    char* cert     = env_dup("AZ_IOT_CLIENT_CERT");
    char* key      = env_dup("AZ_IOT_CLIENT_KEY");
    char* ca       = env_dup("AZ_IOT_TRUSTED_CA");
    char* global   = env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT"); /* optional */

    assert_non_null(id_scope);
    assert_non_null(reg_id);
    assert_non_null(cert);
    assert_non_null(key);
    assert_non_null(ca);

    conn_ctx ctx = {0};
    az_iot_certificate_provider_pem certs = {0};
    az_iot_connection_client conn = {0};
    az_iot_file_upload_client fu = {0};

    az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
    pem.trusted_ca_pem_path  = ca;
    pem.client_cert_pem_path  = cert;
    pem.client_key_pem_path   = key;
    assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&certs, &pem));

    az_iot_connection_client_options copts = az_iot_connection_client_options_default();
    copts.dps.id_scope = id_scope;
    copts.dps.registration_id = reg_id;
    copts.certificate_provider = &certs.base;
    if (global != NULL) copts.dps.global_endpoint = global;

    assert_int_equal(AZ_IOT_OK, az_iot_connection_client_init(&conn, &copts));
    az_iot_connection_client_set_state_callback(&conn, on_conn_state, &ctx);
    assert_int_equal(AZ_IOT_OK,
        az_iot_connection_client_register_mqtt_factory(&conn, az_iot_paho_factory_create_v3_1_1()));

    assert_int_equal(AZ_IOT_OK, az_iot_connection_client_open(&conn));
    time_t start = time(NULL);
    while (ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED
           && (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S)
    {
        (void)az_iot_connection_client_do_work(&conn, 50);
        if (ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED) break;
    }
    assert_int_equal(ctx.conn_state, AZ_IOT_CONN_STATE_CONNECTED);

    /* File upload client with the harness-transport Classic HTTP hook. */
    hub_http_ctx http_ctx = { cert, key };
    az_iot_file_upload_http_transport http = { e2e_http_send, &http_ctx };
    assert_int_equal(AZ_IOT_OK, az_iot_file_upload_client_init(&fu, &conn, &http));

    /* A unique blob name per run avoids collisions across repeated e2e runs. */
    char blob_name[64];
    (void)snprintf(blob_name, sizeof(blob_name), "e2e-fileupload/%lld.txt",
                   (long long)time(NULL));

    /* Step 1: request the SAS URI (synchronous on Classic via the hook). */
    upload_ctx u = {0};
    assert_int_equal(AZ_IOT_OK,
        az_iot_file_upload_client_get_sas_uri(&fu, blob_name, on_sas, &u));
    assert_true(u.sas_done);
    assert_int_equal(u.sas_status, AZ_IOT_OK);
    assert_true(u.correlation_id[0] != '\0');
    assert_non_null(strstr(u.sas_uri, "https://"));

    /* Step 2: PUT the blob to Azure Storage (SAS token in the URI; no client
     * certificate -- the SAS token authenticates the request). */
    char blob_host[256];
    char blob_path[2048];
    assert_true(split_url(u.sas_uri, blob_host, sizeof(blob_host), blob_path, sizeof(blob_path)));
    int put_status = 0;
    assert_true(az_iot_e2e_https_request(
        blob_host, "PUT", blob_path, NULL, NULL, NULL, "x-ms-blob-type: BlockBlob",
        k_blob_content, strlen(k_blob_content), &put_status, NULL, 0, NULL));
    bool put_ok = (put_status >= 200 && put_status < 300);
    assert_true(put_ok);

    /* Step 3: notify the hub of completion. */
    assert_int_equal(AZ_IOT_OK,
        az_iot_file_upload_client_notify_complete(&fu, u.correlation_id, put_ok, on_notify, &u));
    assert_true(u.notify_done);
    assert_int_equal(u.notify_status, AZ_IOT_OK);

    az_iot_file_upload_client_destroy(&fu);
    az_iot_connection_client_close(&conn);
    for (int i = 0; i < 100 && ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
    {
        (void)az_iot_connection_client_do_work(&conn, 50);
    }
    az_iot_connection_client_destroy(&conn);
    az_iot_certificate_provider_pem_destroy(&certs);

    free(id_scope);
    free(reg_id);
    free(cert);
    free(key);
    free(ca);
    free(global);
}

int main(void)
{
    az_iot_log_level log_level = AZ_IOT_LOG_ERROR;
    char* lvl = env_dup("AZ_IOT_E2E_LOG_LEVEL");
    if (lvl != NULL)
    {
        if      (strcmp(lvl, "TRACE") == 0) log_level = AZ_IOT_LOG_TRACE;
        else if (strcmp(lvl, "DEBUG") == 0) log_level = AZ_IOT_LOG_DEBUG;
        else if (strcmp(lvl, "INFO")  == 0) log_level = AZ_IOT_LOG_INFO;
        else if (strcmp(lvl, "WARN")  == 0) log_level = AZ_IOT_LOG_WARN;
        free(lvl);
    }
    az_iot_log_sink log = az_iot_log_stderr_sink(log_level);
    az_iot_log_set_global_sink(&log);

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_file_upload_classic),
    };

#ifdef _WIN32
    /* Skipped on Windows: the e2e harness's Schannel TLS transport does not
     * present the self-signed X.509 device certificate during the mutual-TLS
     * hub REST handshake on the CI runners -- Schannel declines to send it even
     * though the private key is accessible, whereas the OpenSSL transport on
     * Linux presents it correctly. The file-upload SDK feature is covered by the
     * Linux e2e legs, and the file-upload sample builds and runs natively on
     * Windows. Return the CTest skip code (77) rather than fail. */
    (void)tests;
    fprintf(stderr,
        "az_iot_tests_e2e_fileupload: skipped on Windows (harness Schannel transport does "
        "not present the client certificate on CI); covered on the Linux e2e legs.\n");
    return 77;
#else
    return cmocka_run_group_tests(tests, NULL, NULL);
#endif
}
