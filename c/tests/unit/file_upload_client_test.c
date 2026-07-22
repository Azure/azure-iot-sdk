// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* FileUploadClient unit tests. The public API is one seamless async client; the
 * Classic path performs HTTPS through an application transport hook. These tests
 * drive the Classic path offline via a mock hook + an unopened direct-host
 * (Classic) connection client — no live hub, no MQTT. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_file_upload_client.h"
#include "azure/iot/az_iot_result.h"

#define TEST_HUB    "myhub.azure-devices.net"
#define TEST_DEVICE "dev1"

/* Sample SAS-URI response body, shaped like the IoT Hub REST response. */
static const char k_sas_json[] =
    "{"
    "\"correlationId\":\"corr-123\","
    "\"hostName\":\"acct.blob.core.windows.net\","
    "\"containerName\":\"uploads\","
    "\"blobName\":\"dev1/sample-data/test.txt\","
    "\"sasToken\":\"?sv=2021-04-12&sr=b&sig=ABC%2F123&se=2026-01-01&sp=rw\""
    "}";

/* ------------------------------------------------------------------------- */
/* mock HTTP transport                                                       */
/* ------------------------------------------------------------------------- */

typedef struct
{
    int           call_count;
    char          last_method[8];
    char          last_url[512];
    char          last_body[512];
    az_iot_result transport_result; /* AZ_IOT_OK => return the programmed response */
    int           resp_status;
    const char*   resp_body;
} mock_http;

static mock_http g_http;

static az_iot_result mock_send(
    const char* method, const char* url, const char* authorization,
    const char* content_type, const uint8_t* body, size_t body_len,
    az_iot_file_upload_http_response* response, void* hook_ctx)
{
    (void)authorization;
    (void)content_type;
    (void)hook_ctx;

    g_http.call_count++;
    snprintf(g_http.last_method, sizeof(g_http.last_method), "%s", method);
    snprintf(g_http.last_url, sizeof(g_http.last_url), "%s", url);
    if (body && body_len)
    {
        size_t n = body_len < sizeof(g_http.last_body) - 1 ? body_len : sizeof(g_http.last_body) - 1;
        memcpy(g_http.last_body, body, n);
        g_http.last_body[n] = '\0';
    }
    else
    {
        g_http.last_body[0] = '\0';
    }

    if (g_http.transport_result != AZ_IOT_OK)
        return g_http.transport_result;

    response->status_code = g_http.resp_status;
    if (response->body && g_http.resp_body)
    {
        size_t n = strlen(g_http.resp_body);
        if (n >= response->body_capacity) n = response->body_capacity - 1;
        memcpy(response->body, g_http.resp_body, n);
        response->body_len = n;
    }
    return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* result records                                                            */
/* ------------------------------------------------------------------------- */

typedef struct
{
    bool          sas_done;
    az_iot_result sas_status;
    char          sas_uri[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX];
    char          correlation_id[AZ_IOT_FILE_UPLOAD_CORR_ID_MAX];
    bool          notify_done;
    az_iot_result notify_status;
} rec;

static void on_sas(az_iot_result status, const char* uri, const char* corr, void* ctx)
{
    rec* r = (rec*)ctx;
    r->sas_done = true;
    r->sas_status = status;
    if (uri)  snprintf(r->sas_uri, sizeof(r->sas_uri), "%s", uri);
    if (corr) snprintf(r->correlation_id, sizeof(r->correlation_id), "%s", corr);
}

static void on_notify(az_iot_result status, void* ctx)
{
    rec* r = (rec*)ctx;
    r->notify_done = true;
    r->notify_status = status;
}

/* ------------------------------------------------------------------------- */
/* fixture: unopened direct-host (Classic) connection + file upload client    */
/* ------------------------------------------------------------------------- */

typedef struct
{
    az_iot_connection_client  conn;
    az_iot_file_upload_client fu;
} fixture;

static int setup(void** state)
{
    fixture* fx = (fixture*)calloc(1, sizeof(*fx));
    assert_non_null(fx);

    az_iot_connection_client_options opts = {0};
    opts.host = TEST_HUB;       /* direct host => Classic flavor; no open needed */
    opts.port = 8883;
    opts.client_id = TEST_DEVICE;
    assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

    memset(&g_http, 0, sizeof(g_http));

    az_iot_file_upload_http_transport http = { mock_send, NULL };
    assert_int_equal(
        az_iot_file_upload_client_init(&fx->fu, &fx->conn, &http), AZ_IOT_OK);

    *state = fx;
    return 0;
}

static int teardown(void** state)
{
    fixture* fx = (fixture*)*state;
    if (fx)
    {
        az_iot_file_upload_client_destroy(&fx->fu);
        az_iot_connection_client_destroy(&fx->conn);
        free(fx);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void init_rejects_null(void** state)
{
    (void)state;
    az_iot_file_upload_client fu2;
    assert_int_equal(az_iot_file_upload_client_init(&fu2, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(az_iot_file_upload_client_init(NULL, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void classic_init_requires_http_hook(void** state)
{
    fixture* fx = (fixture*)*state;
    az_iot_file_upload_client fu2;
    /* A Classic connection with no HTTP transport is rejected. */
    assert_int_equal(
        az_iot_file_upload_client_init(&fu2, &fx->conn, NULL),
        AZ_IOT_ERR_INVALID_ARG);
}

static void get_sas_uri_builds_request_and_delivers_uri(void** state)
{
    fixture* fx = (fixture*)*state;
    g_http.resp_status = 200;
    g_http.resp_body   = k_sas_json;

    rec r;
    memset(&r, 0, sizeof(r));
    assert_int_equal(
        az_iot_file_upload_client_get_sas_uri(&fx->fu, "sample-data/test.txt", on_sas, &r),
        AZ_IOT_OK);

    /* The request the SDK built and handed to the hook. */
    assert_int_equal(g_http.call_count, 1);
    assert_string_equal(g_http.last_method, "POST");
    assert_string_equal(
        g_http.last_url,
        "https://" TEST_HUB "/devices/" TEST_DEVICE "/files?api-version=2021-04-12");
    assert_string_equal(g_http.last_body, "{\"blobName\":\"sample-data/test.txt\"}");

    /* Result delivered synchronously via the callback (Classic). */
    assert_true(r.sas_done);
    assert_int_equal(r.sas_status, AZ_IOT_OK);
    assert_string_equal(r.correlation_id, "corr-123");
    assert_string_equal(
        r.sas_uri,
        "https://acct.blob.core.windows.net/uploads/dev1/sample-data/test.txt"
        "?sv=2021-04-12&sr=b&sig=ABC%2F123&se=2026-01-01&sp=rw");
}

static void get_sas_uri_http_error_delivers_error(void** state)
{
    fixture* fx = (fixture*)*state;
    g_http.resp_status = 403;

    rec r;
    memset(&r, 0, sizeof(r));
    assert_int_equal(
        az_iot_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r),
        AZ_IOT_OK);
    assert_true(r.sas_done);
    assert_int_not_equal(r.sas_status, AZ_IOT_OK);
}

static void get_sas_uri_transport_failure_delivers_error(void** state)
{
    fixture* fx = (fixture*)*state;
    g_http.transport_result = AZ_IOT_ERR_MQTT; /* hook could not perform the request */

    rec r;
    memset(&r, 0, sizeof(r));
    assert_int_equal(
        az_iot_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r),
        AZ_IOT_OK);
    assert_true(r.sas_done);
    assert_int_equal(r.sas_status, AZ_IOT_ERR_MQTT);
}

static void get_sas_uri_rejects_bad_args(void** state)
{
    fixture* fx = (fixture*)*state;
    assert_int_equal(
        az_iot_file_upload_client_get_sas_uri(&fx->fu, NULL, on_sas, NULL),
        AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(
        az_iot_file_upload_client_get_sas_uri(&fx->fu, "", on_sas, NULL),
        AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(
        az_iot_file_upload_client_get_sas_uri(&fx->fu, "b", NULL, NULL),
        AZ_IOT_ERR_INVALID_ARG);
}

static void notify_complete_builds_request_and_acks(void** state)
{
    fixture* fx = (fixture*)*state;
    g_http.resp_status = 204;

    rec r;
    memset(&r, 0, sizeof(r));
    assert_int_equal(
        az_iot_file_upload_client_notify_complete(&fx->fu, "corr-9", true, on_notify, &r),
        AZ_IOT_OK);

    assert_int_equal(g_http.call_count, 1);
    assert_string_equal(g_http.last_method, "POST");
    assert_string_equal(
        g_http.last_url,
        "https://" TEST_HUB "/devices/" TEST_DEVICE
        "/files/notifications?api-version=2021-04-12");
    assert_string_equal(
        g_http.last_body,
        "{\"correlationId\":\"corr-9\",\"isSuccess\":true,"
        "\"statusCode\":200,\"statusDescription\":\"Succeeded\"}");

    assert_true(r.notify_done);
    assert_int_equal(r.notify_status, AZ_IOT_OK);
}

static void notify_complete_failure_body(void** state)
{
    fixture* fx = (fixture*)*state;
    g_http.resp_status = 204;

    rec r;
    memset(&r, 0, sizeof(r));
    assert_int_equal(
        az_iot_file_upload_client_notify_complete(&fx->fu, "corr-9", false, on_notify, &r),
        AZ_IOT_OK);
    assert_string_equal(
        g_http.last_body,
        "{\"correlationId\":\"corr-9\",\"isSuccess\":false,"
        "\"statusCode\":0,\"statusDescription\":\"Failed\"}");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(init_rejects_null),
        cmocka_unit_test_setup_teardown(classic_init_requires_http_hook, setup, teardown),
        cmocka_unit_test_setup_teardown(get_sas_uri_builds_request_and_delivers_uri, setup, teardown),
        cmocka_unit_test_setup_teardown(get_sas_uri_http_error_delivers_error, setup, teardown),
        cmocka_unit_test_setup_teardown(get_sas_uri_transport_failure_delivers_error, setup, teardown),
        cmocka_unit_test_setup_teardown(get_sas_uri_rejects_bad_args, setup, teardown),
        cmocka_unit_test_setup_teardown(notify_complete_builds_request_and_acks, setup, teardown),
        cmocka_unit_test_setup_teardown(notify_complete_failure_body, setup, teardown),
    };
    return cmocka_run_group_tests_name("file_upload_client", tests, NULL, NULL);
}
