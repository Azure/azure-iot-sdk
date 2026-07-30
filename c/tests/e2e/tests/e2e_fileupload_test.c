// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* File upload end-to-end (Classic IoT Hub).
 *
 * Proves the device-side file-upload path against a real Azure IoT Hub that has
 * an Azure Storage account associated for file upload. The device provisions via
 * DPS (X.509) and connects to the assigned hub once, then every scenario runs
 * against that connection:
 *
 *   failure      an upload the device reports as FAILED is accepted by the hub
 *                and must NOT produce a notification.
 *   round trip   get_sas_uri -> PUT the blob -> read it back and compare bytes ->
 *                notify_complete(success).
 *   notification IoT Hub publishes the completion to the service-side file-upload
 *                notification endpoint. That notification is the only CLOUD-side
 *                proof the round trip completed -- the device alone merely sees
 *                the hub accept its completion message -- and the failed upload
 *                above must be absent from it.
 *   rejection    a completion notification carrying an unknown correlation id is
 *                rejected by the hub and surfaces as a mapped error.
 *   arguments    the client refuses malformed arguments locally, without
 *                touching the network.
 *
 * The first three run IN ORDER on purpose: reporting the failed upload before the
 * successful one makes the absence check deterministic, because once the later
 * notification arrives the hub has demonstrably processed both.
 *
 * The device HTTP hook (Classic control plane) and the Storage PUT/GET go through
 * the e2e harness's own TLS transport (az_iot_e2e_https_request) -- the same
 * az_amqp transport the service facade uses -- so there is no external HTTP
 * client dependency, and mutual TLS with the device certificate authenticates the
 * hub REST calls.
 *
 * On Windows the notification scenario self-skips: that harness transport keeps a
 * single process-wide Schannel session, so it cannot hold the notification
 * watcher's AMQP connection open across the HTTPS calls. Every other scenario --
 * including the entire upload round trip -- runs on both platforms.
 *
 * Requires the standard e2e device environment and the service connection
 * strings (both provided by the e2e job), and a hub with an Azure Storage
 * account associated for file upload AND file-upload notifications enabled.
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

#include "az_iot_e2e_service.h"
#include "e2e_device.h"

/* How long to wait for IoT Hub to post a file-upload notification. The hub
 * batches these, so the budget is tens of seconds, not milliseconds. */
#define E2E_NOTIFICATION_TIMEOUT_S 90
#define E2E_PUMP_SLICE_MS 50

static const char k_blob_content[] =
    "Hello from the Azure IoT C SDK file upload e2e test.\n";

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

/* ---- upload results ------------------------------------------------------- */

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

/* ---- fixture -------------------------------------------------------------- */

typedef struct
{
    e2e_device                dev;
    az_iot_e2e_service*       svc;
    az_iot_file_upload_client fu;
    hub_http_ctx              http_ctx;
    bool                      fu_ok;
    bool                      watching_notifications;
    /* Blob names produced by the first two scenarios, checked by the third. */
    char                      failed_blob[96];
    char                      uploaded_blob[96];
} fixture;

static fixture g_fx;

/* Keep the device MQTT session and the service AMQP watcher alive for @p ms. */
static void pump(fixture* fx, int ms)
{
    for (int elapsed = 0; elapsed < ms; elapsed += E2E_PUMP_SLICE_MS)
    {
        e2e_device_do_work(&fx->dev, E2E_PUMP_SLICE_MS);
        (void)az_iot_e2e_service_do_work(fx->svc, E2E_PUMP_SLICE_MS);
    }
}

/* A unique blob name per run and per scenario avoids collisions across repeated
 * e2e runs sharing one storage account. */
static void make_blob_name(char* out, size_t cap, const char* tag)
{
    (void)snprintf(out, cap, "e2e-fileupload/%s-%lld.txt", tag, (long long)time(NULL));
}

/* Request a SAS URI and assert the hub granted one. */
static void request_sas(fixture* fx, const char* blob_name, upload_ctx* u)
{
    memset(u, 0, sizeof(*u));
    assert_int_equal(AZ_IOT_OK,
        az_iot_file_upload_client_get_sas_uri(&fx->fu, blob_name, on_sas, u));
    assert_true(u->sas_done);
    assert_int_equal(u->sas_status, AZ_IOT_OK);
    assert_true(u->correlation_id[0] != '\0');
    assert_non_null(strstr(u->sas_uri, "https://"));
}

static int group_setup(void** state)
{
    (void)state;
    memset(&g_fx, 0, sizeof(g_fx));

    if (e2e_device_connect(&g_fx.dev) != 0)
    {
        fprintf(stderr, "file upload e2e: device failed to connect\n");
        return -1;
    }

    const char* err = NULL;
    g_fx.svc = az_iot_e2e_service_create(&err);
    if (g_fx.svc == NULL)
    {
        fprintf(stderr, "file upload e2e: service client unavailable: %s\n",
                (err != NULL) ? err : "unknown");
        return -1;
    }

    /* Watch before any completion is reported: notifications are delivered once.
     *
     * Not on Windows: the harness TLS transport keeps a single process-wide
     * Schannel session, so an AMQP connection held open here would be torn down by
     * the first HTTPS request (and vice versa). The notification scenario
     * self-skips there; every other scenario is sequential HTTPS and runs fine. */
#ifndef _WIN32
    if (!az_iot_e2e_service_file_notification_watch_begin(g_fx.svc, g_fx.dev.device_id))
    {
        fprintf(stderr, "file upload e2e: notification watch failed: %s\n",
                az_iot_e2e_service_last_error(g_fx.svc));
        return -1;
    }
    g_fx.watching_notifications = true;
#endif

    g_fx.http_ctx.cert = g_fx.dev.cert;
    g_fx.http_ctx.key = g_fx.dev.key;
    az_iot_file_upload_http_transport http = { e2e_http_send, &g_fx.http_ctx };
    if (az_iot_file_upload_client_init(&g_fx.fu, &g_fx.dev.conn, &http) != AZ_IOT_OK)
    {
        fprintf(stderr, "file upload e2e: file upload client init failed\n");
        return -1;
    }
    g_fx.fu_ok = true;
    return 0;
}

static int group_teardown(void** state)
{
    (void)state;
    if (g_fx.fu_ok) az_iot_file_upload_client_destroy(&g_fx.fu);
    if (g_fx.svc != NULL)
    {
        az_iot_e2e_service_file_notification_watch_end(g_fx.svc);
        az_iot_e2e_service_destroy(g_fx.svc);
        g_fx.svc = NULL;
    }
    e2e_device_disconnect(&g_fx.dev);
    return 0;
}

/* ---- scenarios ------------------------------------------------------------ */

/* An upload the device abandons and reports as FAILED. The hub must accept the
 * report; it must not publish a notification for it (asserted further down, once
 * a later successful upload's notification proves the hub got this far). */
static void test_failed_upload_is_accepted(void** state)
{
    (void)state;
    fixture* fx = &g_fx;

    make_blob_name(fx->failed_blob, sizeof(fx->failed_blob), "failed");
    upload_ctx failed;
    request_sas(fx, fx->failed_blob, &failed);

    /* Nothing is PUT: this upload never happened. */
    assert_int_equal(AZ_IOT_OK,
        az_iot_file_upload_client_notify_complete(
            &fx->fu, failed.correlation_id, false, on_notify, &failed));
    assert_true(failed.notify_done);
    assert_int_equal(failed.notify_status, AZ_IOT_OK);
}

/* The full device-side round trip, verified against storage: the blob really
 * lands with the bytes the device sent. The hub granting a URI is no proof that
 * the payload arrived intact, so it is read back and compared. */
static void test_upload_round_trip(void** state)
{
    (void)state;
    fixture* fx = &g_fx;

    make_blob_name(fx->uploaded_blob, sizeof(fx->uploaded_blob), "ok");
    upload_ctx u;
    request_sas(fx, fx->uploaded_blob, &u);

    char blob_host[256];
    char blob_path[2048];
    assert_true(split_url(u.sas_uri, blob_host, sizeof(blob_host), blob_path, sizeof(blob_path)));

    int put_status = 0;
    assert_true(az_iot_e2e_https_request(
        blob_host, "PUT", blob_path, NULL, NULL, NULL, "x-ms-blob-type: BlockBlob",
        k_blob_content, strlen(k_blob_content), &put_status, NULL, 0, NULL));
    assert_in_range(put_status, 200, 299);

    char readback[256];
    int get_status = 0;
    size_t read_len = 0;
    assert_true(az_iot_e2e_https_request(
        blob_host, "GET", blob_path, NULL, NULL, NULL, NULL,
        NULL, 0, &get_status, readback, sizeof(readback), &read_len));
    assert_in_range(get_status, 200, 299);
    assert_int_equal((int)read_len, (int)strlen(k_blob_content));
    assert_memory_equal(readback, k_blob_content, strlen(k_blob_content));

    assert_int_equal(AZ_IOT_OK,
        az_iot_file_upload_client_notify_complete(
            &fx->fu, u.correlation_id, true, on_notify, &u));
    assert_true(u.notify_done);
    assert_int_equal(u.notify_status, AZ_IOT_OK);
}

/* The cloud half: IoT Hub must publish the successful upload to the service-side
 * notification endpoint, and must not publish the failed one. */
static void test_hub_publishes_a_completion_notification(void** state)
{
    (void)state;
    fixture* fx = &g_fx;

    if (!fx->watching_notifications)
    {
        /* Windows: the harness TLS transport cannot hold this watcher's AMQP
         * connection open across the HTTPS calls the scenarios above make. */
        skip();
    }

    assert_true(fx->uploaded_blob[0] != '\0');
    assert_true(fx->failed_blob[0] != '\0');

    time_t start = time(NULL);
    while (!az_iot_e2e_service_file_notification_seen(fx->svc, fx->uploaded_blob)
           && (time(NULL) - start) < E2E_NOTIFICATION_TIMEOUT_S)
    {
        pump(fx, 500);
    }
    assert_true(az_iot_e2e_service_file_notification_seen(fx->svc, fx->uploaded_blob));

    /* The hub processed the failed upload BEFORE this one, so its notification
     * would already have arrived if the hub emitted one. */
    assert_false(az_iot_e2e_service_file_notification_seen(fx->svc, fx->failed_blob));
}

/* A completion notification for a correlation id the hub never issued is
 * rejected, and the SDK maps the hub's 4xx onto a caller-visible error rather
 * than reporting success. */
static void test_notify_with_unknown_correlation_id_is_rejected(void** state)
{
    (void)state;
    fixture* fx = &g_fx;

    upload_ctx u;
    memset(&u, 0, sizeof(u));
    assert_int_equal(AZ_IOT_OK,
        az_iot_file_upload_client_notify_complete(
            &fx->fu, "e2e-correlation-id-that-does-not-exist", true, on_notify, &u));
    assert_true(u.notify_done);
    assert_int_equal(u.notify_status, AZ_IOT_ERR_INVALID_ARG);
}

/* Malformed arguments are refused locally, so a live client never turns them
 * into a request. */
static void test_client_rejects_invalid_arguments(void** state)
{
    (void)state;
    fixture* fx = &g_fx;

    upload_ctx u;
    memset(&u, 0, sizeof(u));
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_file_upload_client_get_sas_uri(&fx->fu, "", on_sas, &u));
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_file_upload_client_get_sas_uri(&fx->fu, "blob.txt", NULL, &u));
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_file_upload_client_notify_complete(&fx->fu, "", true, on_notify, &u));
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_file_upload_client_notify_complete(&fx->fu, "corr", true, NULL, &u));
    assert_false(u.sas_done);
    assert_false(u.notify_done);
}

/* One client serves repeated uploads, and the hub issues a distinct correlation
 * id for each. */
static void test_sequential_uploads_reuse_the_client(void** state)
{
    (void)state;
    fixture* fx = &g_fx;

    char first_blob[96];
    char second_blob[96];
    make_blob_name(first_blob, sizeof(first_blob), "seq1");
    make_blob_name(second_blob, sizeof(second_blob), "seq2");

    upload_ctx first;
    upload_ctx second;
    request_sas(fx, first_blob, &first);
    request_sas(fx, second_blob, &second);
    assert_string_not_equal(first.correlation_id, second.correlation_id);

    /* Release both so the hub does not keep them pending against the account. */
    assert_int_equal(AZ_IOT_OK, az_iot_file_upload_client_notify_complete(
        &fx->fu, first.correlation_id, false, on_notify, &first));
    assert_int_equal(first.notify_status, AZ_IOT_OK);
    assert_int_equal(AZ_IOT_OK, az_iot_file_upload_client_notify_complete(
        &fx->fu, second.correlation_id, false, on_notify, &second));
    assert_int_equal(second.notify_status, AZ_IOT_OK);
}

int main(void)
{
    az_iot_log_level log_level = AZ_IOT_LOG_ERROR;
#ifdef _WIN32
    char* lvl = NULL;
    size_t lvl_len = 0;
    if (_dupenv_s(&lvl, &lvl_len, "AZ_IOT_E2E_LOG_LEVEL") != 0) lvl = NULL;
#else
    const char* lvl = getenv("AZ_IOT_E2E_LOG_LEVEL");
#endif
    if (lvl != NULL && lvl[0] != '\0')
    {
        if      (strcmp(lvl, "TRACE") == 0) log_level = AZ_IOT_LOG_TRACE;
        else if (strcmp(lvl, "DEBUG") == 0) log_level = AZ_IOT_LOG_DEBUG;
        else if (strcmp(lvl, "INFO")  == 0) log_level = AZ_IOT_LOG_INFO;
        else if (strcmp(lvl, "WARN")  == 0) log_level = AZ_IOT_LOG_WARN;
    }
#ifdef _WIN32
    free(lvl);
#endif
    az_iot_log_sink log = az_iot_log_stderr_sink(log_level);
    az_iot_log_set_global_sink(&log);

    /* The first three are ORDER-DEPENDENT: the failed upload must be reported
     * before the successful one so that the notification check can assert its
     * absence deterministically (see the file header). */
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_failed_upload_is_accepted),
        cmocka_unit_test(test_upload_round_trip),
        cmocka_unit_test(test_hub_publishes_a_completion_notification),
        cmocka_unit_test(test_notify_with_unknown_correlation_id_is_rejected),
        cmocka_unit_test(test_client_rejects_invalid_arguments),
        cmocka_unit_test(test_sequential_uploads_reuse_the_client),
    };

    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
