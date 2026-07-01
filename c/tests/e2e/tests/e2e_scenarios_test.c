// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* In-process end-to-end scenarios against a real Azure IoT Hub / DPS instance.
 *
 * ONE process plays both halves of every scenario:
 *   - Device: the shipping SDK over the Paho MQTT adapter (provisioned via DPS
 *     with an X.509 individual enrollment, exactly like a real device).
 *   - Service (cloud): the az_iot_e2e_service facade, which receives telemetry,
 *     sends C2D, invokes direct methods and reads/patches twins. That facade
 *     hides the transport it uses (vendored AMQP + HTTPS/SAS) entirely; this
 *     test never includes an AMQP header, so the SDK's MQTT-only device charter
 *     is preserved at the test boundary.
 *
 * The two halves are driven cooperatively: each wait loop interleaves the
 * device's do_work with the service's poll/pump so neither side blocks the
 * other. Everything is single-threaded and non-blocking.
 *
 * Configuration comes entirely from the environment (set by the e2e CI job):
 *   Device (materialized to files by CI):
 *     AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID,
 *     AZ_IOT_CLIENT_CERT, AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA,
 *     AZ_IOT_DPS_GLOBAL_ENDPOINT (optional)
 *   Service:
 *     IOTHUB_CONNECTION_STRING, IOTHUB_EVENTHUB_CONNECTION_STRING,
 *     IOTHUB_EVENTHUB_LISTEN_NAME (optional), IOTHUB_EVENTHUB_PARTITION_COUNT (optional)
 *
 * The device id targeted by the service side is the DPS registration id (the
 * SDK's default: device id == registration id for individual enrollments).
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

/* ---- timing budgets ------------------------------------------------------- */
#define E2E_CONNECT_TIMEOUT_S   90 /* DPS provision + MQTT connect            */
#define E2E_TELEMETRY_TIMEOUT_S 90 /* publish + EH-side receive               */
#define E2E_C2D_TIMEOUT_S       60
#define E2E_METHOD_TIMEOUT_S    60
#define E2E_TWIN_TIMEOUT_S      60
#define E2E_PUMP_MS             20 /* per do_work / poll slice                */

/* ---- environment ---------------------------------------------------------- */

/* Duplicate an environment variable into a heap buffer (portable). Returns NULL
 * when unset/empty. Caller frees. */
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

/* ---- shared fixture ------------------------------------------------------- */

typedef struct
{
    char* id_scope;
    char* reg_id;
    char* cert;
    char* key;
    char* ca;
    char* global_endpoint; /* optional */
} device_config_t;

typedef struct
{
    az_iot_connection_state_t conn_state;
} device_ctx_t;

typedef struct
{
    device_config_t                   cfg;
    device_ctx_t                      ctx;
    az_iot_certificate_provider_pem_t certs;
    az_iot_connection_client_t        conn;
    bool                              certs_ok;
    bool                              conn_ok;

    az_iot_e2e_service* service;

    const char* device_id; /* == cfg.reg_id */
} e2e_fixture_t;

static e2e_fixture_t g_fixture;

static void device_config_release(device_config_t* cfg)
{
    free(cfg->id_scope);
    free(cfg->reg_id);
    free(cfg->cert);
    free(cfg->key);
    free(cfg->ca);
    free(cfg->global_endpoint);
    memset(cfg, 0, sizeof(*cfg));
}

static int device_config_load(device_config_t* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->id_scope        = env_dup("AZ_IOT_DPS_ID_SCOPE");
    cfg->reg_id          = env_dup("AZ_IOT_DPS_REGISTRATION_ID");
    cfg->cert            = env_dup("AZ_IOT_CLIENT_CERT");
    cfg->key             = env_dup("AZ_IOT_CLIENT_KEY");
    cfg->ca              = env_dup("AZ_IOT_TRUSTED_CA");
    cfg->global_endpoint = env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT");

    if (cfg->id_scope == NULL || cfg->reg_id == NULL || cfg->cert == NULL
        || cfg->key == NULL || cfg->ca == NULL)
    {
        fprintf(stderr,
            "[e2e] missing required device env vars: AZ_IOT_DPS_ID_SCOPE/"
            "AZ_IOT_DPS_REGISTRATION_ID/AZ_IOT_CLIENT_CERT/AZ_IOT_CLIENT_KEY/"
            "AZ_IOT_TRUSTED_CA\n");
        return 1;
    }
    return 0;
}

static void on_conn_state(az_iot_connection_state_t s, az_iot_result_t reason, void* user_ctx)
{
    (void)reason;
    ((device_ctx_t*)user_ctx)->conn_state = s;
}

/* Pump the device MQTT stack for a single slice. */
static void device_pump(e2e_fixture_t* fx, int ms)
{
    (void)az_iot_connection_client_do_work(&fx->conn, ms);
}

/* Provision the device via DPS and connect it to the assigned hub. */
static int device_connect(e2e_fixture_t* fx)
{
    az_iot_certificate_provider_pem_options_t pem = {
        .trusted_ca_pem_path  = fx->cfg.ca,
        .client_cert_pem_path = fx->cfg.cert,
        .client_key_pem_path  = fx->cfg.key,
    };
    if (az_iot_certificate_provider_pem_init(&fx->certs, &pem) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e] certificate provider init failed\n");
        return 1;
    }
    fx->certs_ok = true;

    az_iot_connection_client_options_t copts =
        az_iot_connection_client_options_get_default(fx->cfg.id_scope, fx->cfg.reg_id, &fx->certs.base);
    if (fx->cfg.global_endpoint != NULL)
    {
        copts.dps.global_endpoint = fx->cfg.global_endpoint;
    }

    if (az_iot_connection_client_init(&fx->conn, &copts) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e] connection client init failed\n");
        return 1;
    }
    fx->conn_ok = true;
    az_iot_connection_client_set_state_callback(&fx->conn, on_conn_state, &fx->ctx);

    if (az_iot_connection_client_register_mqtt_factory(&fx->conn, az_iot_paho_factory_create_v3_1_1()) != AZ_IOT_OK
        || az_iot_connection_client_register_mqtt_factory(&fx->conn, az_iot_paho_factory_create_v5()) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e] MQTT factory registration failed\n");
        return 1;
    }

    if (az_iot_connection_client_open(&fx->conn) != AZ_IOT_OK)
    {
        fprintf(stderr, "[e2e] connection open failed\n");
        return 1;
    }

    time_t start = time(NULL);
    while (fx->ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED
           && (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S)
    {
        device_pump(fx, 50);
        if (fx->ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
        {
            break;
        }
    }

    if (fx->ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED)
    {
        fprintf(stderr, "[e2e] device did not reach CONNECTED (state=%d)\n", (int)fx->ctx.conn_state);
        return 1;
    }
    return 0;
}

/* ---- unique correlation markers ------------------------------------------- */

/* Build a short, run-unique marker like "tag-1a2b3c4d5e6f". A fresh hub plus a
 * unique marker per scenario means the telemetry watcher never confuses an old
 * message for the one under test. */
static void make_marker(char* out, size_t cap, const char* tag)
{
    static unsigned counter = 0;
    unsigned long long t = (unsigned long long)time(NULL);
    unsigned r = (unsigned)rand();
    snprintf(out, cap, "%s-%08llx%04x%03x", tag, t & 0xffffffffull, r & 0xffff, (counter++) & 0xfff);
}

/* ---- group setup / teardown ----------------------------------------------- */

static int group_setup(void** state)
{
    memset(&g_fixture, 0, sizeof(g_fixture));
    srand((unsigned)time(NULL));

    az_iot_log_sink_t log = az_iot_log_stderr_sink(AZ_IOT_LOG_ERROR);
    az_iot_log_set_global_sink(&log);

    if (device_config_load(&g_fixture.cfg) != 0)
    {
        return -1;
    }
    g_fixture.device_id = g_fixture.cfg.reg_id;

    const char* svc_err = NULL;
    g_fixture.service = az_iot_e2e_service_create(&svc_err);
    if (g_fixture.service == NULL)
    {
        fprintf(stderr, "[e2e] service client create failed: %s\n", (svc_err != NULL) ? svc_err : "unknown");
        return -1;
    }

    if (device_connect(&g_fixture) != 0)
    {
        az_iot_e2e_service_destroy(g_fixture.service);
        g_fixture.service = NULL;
        return -1;
    }

    *state = &g_fixture;
    return 0;
}

static int group_teardown(void** state)
{
    e2e_fixture_t* fx = (e2e_fixture_t*)*state;
    if (fx == NULL)
    {
        return 0;
    }
    if (fx->conn_ok)
    {
        az_iot_connection_client_close(&fx->conn);
        for (int i = 0; i < 100 && fx->ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
        {
            device_pump(fx, 50);
        }
        az_iot_connection_client_deinit(&fx->conn);
    }
    if (fx->certs_ok)
    {
        az_iot_certificate_provider_pem_deinit(&fx->certs);
    }
    if (fx->service != NULL)
    {
        az_iot_e2e_service_destroy(fx->service);
    }
    device_config_release(&fx->cfg);
    return 0;
}

/* ---- telemetry ------------------------------------------------------------ */

typedef struct
{
    int             done;
    az_iot_result_t status;
} send_ctx_t;

static void on_send_done(az_iot_result_t status, void* user_ctx)
{
    send_ctx_t* c = (send_ctx_t*)user_ctx;
    c->status = status;
    c->done = 1;
}

static void test_telemetry(void** state)
{
    e2e_fixture_t* fx = (e2e_fixture_t*)*state;

    char marker[64];
    make_marker(marker, sizeof(marker), "tele");

    char payload[128];
    snprintf(payload, sizeof(payload), "{\"e2e\":\"telemetry\",\"marker\":\"%s\"}", marker);

    /* Start listening on the Event Hub-compatible endpoint before sending so we
     * never miss the message. */
    assert_true(az_iot_e2e_service_telemetry_watch_begin(fx->service));

    az_iot_telemetry_client_t telemetry_client;
    assert_int_equal(az_iot_telemetry_client_init(&telemetry_client, &fx->conn), AZ_IOT_OK);

    az_iot_telemetry_property_t props[] = {
        { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
    };
    az_iot_telemetry_message_t msg = { 0 };
    msg.payload          = (const uint8_t*)payload;
    msg.payload_len      = strlen(payload);
    msg.properties       = props;
    msg.properties_count = sizeof(props) / sizeof(props[0]);

    send_ctx_t sc = { 0 };
    assert_int_equal(az_iot_telemetry_client_send(&telemetry_client, &msg, on_send_done, &sc), AZ_IOT_OK);

    bool seen = false;
    time_t start = time(NULL);
    while ((time(NULL) - start) < E2E_TELEMETRY_TIMEOUT_S)
    {
        device_pump(fx, E2E_PUMP_MS);
        assert_true(az_iot_e2e_service_do_work(fx->service, E2E_PUMP_MS));
        if (az_iot_e2e_service_telemetry_seen(fx->service, marker))
        {
            seen = true;
            break;
        }
    }

    /* The Event Hub can observe the message before the device's own send
     * acknowledgement (PUBACK) has been pumped in, so breaking on `seen` alone
     * races the send. Drain the device briefly until the send completes. */
    for (time_t ack = time(NULL); !sc.done && (time(NULL) - ack) < 5;)
    {
        device_pump(fx, E2E_PUMP_MS);
    }

    az_iot_telemetry_client_deinit(&telemetry_client);
    /* Release the AMQP/TLS connection before the next scenario (the Windows
     * reference transport allows only one TLS connection at a time). */
    az_iot_e2e_service_telemetry_watch_end(fx->service);

    assert_true(sc.done);
    assert_int_equal(sc.status, AZ_IOT_OK);
    if (!seen)
    {
        fprintf(stderr, "[e2e] telemetry marker '%s' not observed within %ds\n",
            marker, E2E_TELEMETRY_TIMEOUT_S);
    }
    assert_true(seen);
}

/* ---- cloud-to-device ------------------------------------------------------ */

typedef struct
{
    char expected[64];
    bool received;
    bool matched;
} c2d_ctx_t;

static void on_c2d(const uint8_t* payload, size_t payload_len, const char* content_type, void* user_ctx)
{
    (void)content_type;
    c2d_ctx_t* c = (c2d_ctx_t*)user_ctx;
    size_t expected_len = strlen(c->expected);
    c->matched = (payload_len == expected_len) && (memcmp(payload, c->expected, expected_len) == 0);
    c->received = true;
}

static void test_c2d(void** state)
{
    e2e_fixture_t* fx = (e2e_fixture_t*)*state;

    c2d_ctx_t cctx = { 0 };
    make_marker(cctx.expected, sizeof(cctx.expected), "c2d");

    az_iot_c2d_client_t c2d;
    assert_int_equal(az_iot_c2d_client_init(&c2d, &fx->conn), AZ_IOT_OK);
    assert_int_equal(az_iot_c2d_client_set_handler(&c2d, on_c2d, &cctx), AZ_IOT_OK);

    /* Give the subscription a few pumps to settle before the cloud sends. */
    for (int i = 0; i < 20; ++i)
    {
        device_pump(fx, E2E_PUMP_MS);
    }

    bool sent = az_iot_e2e_service_send_c2d(
        fx->service, fx->device_id, (const uint8_t*)cctx.expected, strlen(cctx.expected));
    if (!sent)
    {
        fprintf(stderr, "[e2e] c2d send failed: %s\n", az_iot_e2e_service_last_error(fx->service));
    }
    assert_true(sent);

    time_t start = time(NULL);
    while (!cctx.received && (time(NULL) - start) < E2E_C2D_TIMEOUT_S)
    {
        device_pump(fx, E2E_PUMP_MS);
    }

    az_iot_c2d_client_deinit(&c2d);

    assert_true(cctx.received);
    assert_true(cctx.matched);
}

/* ---- direct method -------------------------------------------------------- */

static void on_method(
    az_iot_direct_method_request_t* request,
    const char*                     method_name,
    const uint8_t*                  payload,
    size_t                          payload_len,
    void*                           user_ctx)
{
    (void)method_name;
    (void)user_ctx;
    /* Echo the request payload back with a 200. */
    (void)az_iot_direct_method_respond(request, 200, payload, payload_len);
}

static void test_direct_method(void** state)
{
    e2e_fixture_t* fx = (e2e_fixture_t*)*state;

    az_iot_direct_method_client_t dm;
    assert_int_equal(az_iot_direct_method_client_init(&dm, &fx->conn), AZ_IOT_OK);
    assert_int_equal(az_iot_direct_method_client_set_handler(&dm, on_method, NULL), AZ_IOT_OK);

    for (int i = 0; i < 20; ++i)
    {
        device_pump(fx, E2E_PUMP_MS);
    }

    assert_true(az_iot_e2e_service_method_invoke_begin(fx->service, fx->device_id, "echo", "\"ping\""));

    int  status = 0;
    int  rc = 0;
    char resp[512] = { 0 };
    time_t start = time(NULL);
    while ((time(NULL) - start) < E2E_METHOD_TIMEOUT_S)
    {
        device_pump(fx, E2E_PUMP_MS);
        rc = az_iot_e2e_service_request_poll(fx->service, &status, resp, sizeof(resp));
        if (rc != 0)
        {
            break;
        }
    }

    az_iot_direct_method_client_deinit(&dm);

    if (rc != 1)
    {
        fprintf(stderr, "[e2e] method poll rc=%d: %s\n", rc, az_iot_e2e_service_last_error(fx->service));
    }
    assert_int_equal(rc, 1);
    assert_int_equal(status, 200);
    assert_non_null(strstr(resp, "ping"));
}

/* ---- twin ----------------------------------------------------------------- */

typedef struct
{
    char expected[64];
    bool received;
    bool matched;
} desired_ctx_t;

static void on_desired(const uint8_t* patch, size_t patch_len, uint64_t version, void* user_ctx)
{
    (void)version;
    desired_ctx_t* d = (desired_ctx_t*)user_ctx;
    /* patch is not NUL-terminated; scan the delivered range for the marker. */
    size_t needle_len = strlen(d->expected);
    if (patch_len >= needle_len)
    {
        for (size_t i = 0; i + needle_len <= patch_len; ++i)
        {
            if (memcmp(patch + i, d->expected, needle_len) == 0)
            {
                d->matched = true;
                break;
            }
        }
    }
    d->received = true;
}

typedef struct
{
    int             done;
    az_iot_result_t status;
} patch_ack_ctx_t;

static void on_patch_ack(az_iot_result_t status, void* user_ctx)
{
    patch_ack_ctx_t* p = (patch_ack_ctx_t*)user_ctx;
    p->status = status;
    p->done = 1;
}

/* Drive an in-flight REST request to completion while keeping the device pumped. */
static int pump_request(e2e_fixture_t* fx, int* status, char* resp, size_t resp_size, int timeout_s)
{
    int rc = 0;
    time_t start = time(NULL);
    while ((time(NULL) - start) < timeout_s)
    {
        device_pump(fx, E2E_PUMP_MS);
        rc = az_iot_e2e_service_request_poll(fx->service, status, resp, resp_size);
        if (rc != 0)
        {
            break;
        }
    }
    return rc;
}

static void test_twin(void** state)
{
    e2e_fixture_t* fx = (e2e_fixture_t*)*state;

    az_iot_twin_client_t twin;
    assert_int_equal(az_iot_twin_client_init(&twin, &fx->conn), AZ_IOT_OK);

    /* --- desired: cloud patches, device observes ------------------------- */
    desired_ctx_t dctx = { 0 };
    make_marker(dctx.expected, sizeof(dctx.expected), "desired");
    assert_int_equal(az_iot_twin_client_subscribe_desired(&twin, on_desired, &dctx), AZ_IOT_OK);

    for (int i = 0; i < 20; ++i)
    {
        device_pump(fx, E2E_PUMP_MS);
    }

    char desired_json[128];
    snprintf(desired_json, sizeof(desired_json), "{\"cfg\":\"%s\"}", dctx.expected);
    assert_true(az_iot_e2e_service_twin_patch_desired_begin(fx->service, fx->device_id, desired_json));

    int  status = 0;
    char resp[2048] = { 0 };
    int  rc = pump_request(fx, &status, resp, sizeof(resp), E2E_TWIN_TIMEOUT_S);
    if (rc != 1)
    {
        fprintf(stderr, "[e2e] twin desired patch rc=%d: %s\n", rc, az_iot_e2e_service_last_error(fx->service));
    }
    assert_int_equal(rc, 1);
    assert_int_equal(status, 200);

    time_t start = time(NULL);
    while (!dctx.received && (time(NULL) - start) < E2E_TWIN_TIMEOUT_S)
    {
        device_pump(fx, E2E_PUMP_MS);
    }
    assert_true(dctx.received);
    assert_true(dctx.matched);

    /* --- reported: device patches, cloud reads --------------------------- */
    char reported_marker[64];
    make_marker(reported_marker, sizeof(reported_marker), "reported");
    char reported_json[128];
    snprintf(reported_json, sizeof(reported_json), "{\"rep\":\"%s\"}", reported_marker);

    patch_ack_ctx_t pack = { 0 };
    assert_int_equal(
        az_iot_twin_client_patch_reported(
            &twin, (const uint8_t*)reported_json, strlen(reported_json), on_patch_ack, &pack),
        AZ_IOT_OK);

    start = time(NULL);
    while (!pack.done && (time(NULL) - start) < E2E_TWIN_TIMEOUT_S)
    {
        device_pump(fx, E2E_PUMP_MS);
    }
    assert_true(pack.done);
    assert_int_equal(pack.status, AZ_IOT_OK);

    assert_true(az_iot_e2e_service_twin_get_begin(fx->service, fx->device_id));
    status = 0;
    memset(resp, 0, sizeof(resp));
    rc = pump_request(fx, &status, resp, sizeof(resp), E2E_TWIN_TIMEOUT_S);
    if (rc != 1)
    {
        fprintf(stderr, "[e2e] twin get rc=%d: %s\n", rc, az_iot_e2e_service_last_error(fx->service));
    }
    assert_int_equal(rc, 1);
    assert_int_equal(status, 200);
    assert_non_null(strstr(resp, reported_marker));

    az_iot_twin_client_deinit(&twin);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_telemetry),
        cmocka_unit_test(test_c2d),
        cmocka_unit_test(test_direct_method),
        cmocka_unit_test(test_twin),
    };
    return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
