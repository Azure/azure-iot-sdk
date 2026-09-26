// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* IoT Hub Classic file upload unit tests. The client performs HTTPS through an
 * application transport hook, so these tests drive it offline via a mock hook +
 * an unopened direct-host (Classic) connection client -- no live hub, no MQTT.
 *
 * There is no mqttv5 counterpart to exercise: file upload is not carried on the
 * MQTT v5 hub, and this client pins Classic. What used to be the Next-dispatch
 * section is now a single test that the pin refuses an MQTT v5 connection. */
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
#include "azure/iot/az_iot_result.h"
#include "azure/iot/mqttv3/az_iot_file_upload_client.h"
/* Only to prove a released Classic pin admits the other generation. */
#include "azure/iot/mqttv5/az_iot_telemetry_client.h"

#define TEST_HUB "myhub.azure-devices.net"
#define TEST_DEVICE "dev1"

/* Sample SAS-URI response body, shaped like the IoT Hub REST response. */
static const char k_sas_json[]
    = "{"
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
  int call_count;
  char last_method[8];
  char last_url[512];
  char last_body[512];
  char last_authorization[64];
  char last_content_type[64];
  /* What the SDK offered as the response sink on the last call. */
  bool last_resp_body_null;
  size_t last_resp_body_capacity;
  az_iot_result transport_result; /* AZ_IOT_OK => return the programmed response */
  int resp_status;
  const char* resp_body;
  size_t resp_body_len; /* 0 => strlen(resp_body) */
  size_t resp_claim_body_len; /* >0 => report this body_len regardless of what was written */
  bool resp_redirect_body; /* true => point response->body at the hook's own buffer */
} mock_http;

static mock_http g_http;

/* Storage a misbehaving hook could redirect the SDK to. */
static uint8_t g_foreign_buffer[256];

static az_iot_result mock_send(
    const char* method,
    const char* url,
    const char* authorization,
    const char* content_type,
    const uint8_t* body,
    size_t body_len,
    az_iot_file_upload_http_response* response,
    void* hook_ctx)
{
  (void)hook_ctx;

  g_http.call_count++;
  snprintf(g_http.last_method, sizeof(g_http.last_method), "%s", method);
  snprintf(g_http.last_url, sizeof(g_http.last_url), "%s", url);
  snprintf(
      g_http.last_authorization,
      sizeof(g_http.last_authorization),
      "%s",
      authorization ? authorization : "(null)");
  snprintf(
      g_http.last_content_type,
      sizeof(g_http.last_content_type),
      "%s",
      content_type ? content_type : "(null)");
  g_http.last_resp_body_null = (response->body == NULL);
  g_http.last_resp_body_capacity = response->body_capacity;
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
  {
    return g_http.transport_result;
  }

  response->status_code = g_http.resp_status;
  if (response->body != NULL && response->body_capacity > 0 && g_http.resp_body != NULL)
  {
    size_t n = g_http.resp_body_len ? g_http.resp_body_len : strlen(g_http.resp_body);
    /* A real hook can never write past the capacity it was handed; the mock
     * clamps the same way, which is what produces a TRUNCATED response body
     * when a test programs an oversized payload. Guarding on a non-zero
     * capacity above keeps this subtraction from wrapping. */
    if (n > response->body_capacity - 1)
    {
      n = response->body_capacity - 1;
    }
    memcpy(response->body, g_http.resp_body, n);
    response->body_len = n;
  }
  if (g_http.resp_claim_body_len)
  {
    /* Simulate a hook that reports more bytes than it wrote. The rest of the
     * buffer is initialized so the span the SDK clamps to is fully defined --
     * what is under test is the clamp, not the contents of the filler. */
    if (response->body && response->body_capacity > response->body_len)
    {
      memset(
          response->body + response->body_len, ' ', response->body_capacity - response->body_len);
    }
    response->body_len = g_http.resp_claim_body_len;
  }
  if (g_http.resp_redirect_body)
  {
    /* Simulate a hook that answers with storage of its own instead of the
     * buffer the SDK provided. */
    memset(g_foreign_buffer, 'x', sizeof(g_foreign_buffer));
    response->body = g_foreign_buffer;
    response->body_capacity = sizeof(g_foreign_buffer);
    response->body_len = sizeof(g_foreign_buffer);
  }
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* result records                                                            */
/* ------------------------------------------------------------------------- */

typedef struct
{
  bool sas_done;
  az_iot_result sas_status;
  char sas_uri[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX];
  char correlation_id[AZ_IOT_FILE_UPLOAD_CORR_ID_MAX];
  bool notify_done;
  az_iot_result notify_status;
} rec;

static void on_sas(az_iot_result status, const char* uri, const char* corr, void* ctx)
{
  rec* r = (rec*)ctx;
  r->sas_done = true;
  r->sas_status = status;
  if (uri)
  {
    snprintf(r->sas_uri, sizeof(r->sas_uri), "%s", uri);
  }
  if (corr)
  {
    snprintf(r->correlation_id, sizeof(r->correlation_id), "%s", corr);
  }
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
  az_iot_connection_client conn;
  az_iot_mqttv3_file_upload_client fu;
} fixture;

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = TEST_HUB; /* direct host => Classic flavor; no open needed */
  opts.port = 8883;
  opts.client_id = TEST_DEVICE;
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  memset(&g_http, 0, sizeof(g_http));

  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(&fx->fu, &fx->conn, &http), AZ_IOT_OK);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    az_iot_mqttv3_file_upload_client_deinit(&fx->fu);
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
  az_iot_mqttv3_file_upload_client fu2;
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(&fu2, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(NULL, NULL, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void classic_init_requires_http_hook(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqttv3_file_upload_client fu2;
  /* A Classic connection with no HTTP transport is rejected. */
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_init(&fu2, &fx->conn, NULL), AZ_IOT_ERR_INVALID_ARG);
}

/* The request half of the exchange: method, URL and body handed to the hook. */
static void get_sas_uri_builds_the_request(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "sample-data/test.txt", on_sas, &r),
      AZ_IOT_OK);

  assert_int_equal(g_http.call_count, 1);
  assert_string_equal(g_http.last_method, "POST");
  assert_string_equal(
      g_http.last_url, "https://" TEST_HUB "/devices/" TEST_DEVICE "/files?api-version=2021-04-12");
  assert_string_equal(g_http.last_body, "{\"blobName\":\"sample-data/test.txt\"}");
}

/* The response half of the same exchange: the SAS URI assembled from the hub's
 * JSON fields, delivered with its correlation id. Kept apart from the request
 * test because it fails for a different reason -- response parsing rather than
 * request construction. */
static void get_sas_uri_delivers_the_sas_uri_and_correlation_id(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "sample-data/test.txt", on_sas, &r),
      AZ_IOT_OK);

  /* Delivered synchronously via the callback (Classic). */
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
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
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
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_true(r.sas_done);
  assert_int_equal(r.sas_status, AZ_IOT_ERR_MQTT);
}

static void get_sas_uri_rejects_bad_args(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, NULL, on_sas, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "", on_sas, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

static void notify_complete_builds_the_request(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 204;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "corr-9", true, on_notify, &r),
      AZ_IOT_OK);

  assert_int_equal(g_http.call_count, 1);
  assert_string_equal(g_http.last_method, "POST");
  assert_string_equal(
      g_http.last_url,
      "https://" TEST_HUB "/devices/" TEST_DEVICE "/files/notifications?api-version=2021-04-12");
  assert_string_equal(
      g_http.last_body,
      "{\"correlationId\":\"corr-9\",\"isSuccess\":true,"
      "\"statusCode\":200,\"statusDescription\":\"Succeeded\"}");
}

/* The hub acknowledges with 204 and no body; the client must still report
 * completion through the callback. Separate from the request test: this is the
 * status-mapping path, not the request builder. */
static void notify_complete_delivers_the_ack(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 204;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "corr-9", true, on_notify, &r),
      AZ_IOT_OK);

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
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "corr-9", false, on_notify, &r),
      AZ_IOT_OK);
  assert_string_equal(
      g_http.last_body,
      "{\"correlationId\":\"corr-9\",\"isSuccess\":false,"
      "\"statusCode\":0,\"statusDescription\":\"Failed\"}");
}

static void notify_complete_rejects_bad_args(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(NULL, "corr-9", true, on_notify, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, NULL, true, on_notify, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "", true, on_notify, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "corr-9", true, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
  /* None of the rejected calls may reach the transport. */
  assert_int_equal(g_http.call_count, 0);
}

static void get_sas_uri_rejects_null_client(void** state)
{
  (void)state;
  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(NULL, "b", on_sas, &r), AZ_IOT_ERR_INVALID_ARG);
  assert_false(r.sas_done);
}

/* ------------------------------------------------------------------------- */
/* init: argument validation and failure cleanup                             */
/* ------------------------------------------------------------------------- */

/* A transport struct whose send function is NULL is as good as no transport. */
static void classic_init_rejects_transport_with_null_send(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { NULL, NULL };
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_init(&fu2, &fx->conn, &http), AZ_IOT_ERR_INVALID_ARG);
}

/* A DPS-only connection that has not provisioned yet has no hub address. That is
 * the connection's state, not a bad argument, so it reports NOT_CONNECTED. */
static void init_rejects_unresolved_hub_address(void** state)
{
  (void)state;
  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "reg-1";
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);
  assert_null(az_iot_connection_client_get_iothub_address(&conn));

  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_init(&fu2, &conn, &http), AZ_IOT_ERR_NOT_CONNECTED);

  az_iot_connection_client_destroy(&conn);
}

static void init_rejects_missing_device_id(void** state)
{
  (void)state;
  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = TEST_HUB;
  opts.port = 8883;
  /* client_id deliberately left NULL. */
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_init(&fu2, &conn, &http), AZ_IOT_ERR_NOT_CONNECTED);

  az_iot_connection_client_destroy(&conn);
}

/* An endpoint too long to form a request URL is rejected where the length
 * actually matters -- at the operation -- not at init(). init() checks that the
 * connection HAS a hub address and device id; it cannot usefully check their
 * length, because the hub in force at init() need not be the one an operation
 * later addresses. Rejecting at init would also be over-strict: the real limit
 * is the whole URL fitting AZ_IOT_FILE_UPLOAD_URL_MAX, not any per-field bound.
 *
 * Both operations build a URL, so both are asserted; the notification URL is the
 * longer of the two, so a host that defeats the SAS request defeats it as well. */
static void oversized_hub_address_is_rejected_at_the_operation(void** state)
{
  (void)state;
  char long_host[AZ_IOT_FILE_UPLOAD_URL_MAX];
  memset(long_host, 'h', sizeof(long_host) - 1);
  long_host[sizeof(long_host) - 1] = '\0';

  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = long_host;
  opts.port = 8883;
  opts.client_id = TEST_DEVICE;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(&fu2, &conn, &http), AZ_IOT_OK);

  memset(&g_http, 0, sizeof(g_http));
  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fu2, "b", on_sas, &r),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fu2, "c", true, on_notify, &r),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  /* Refused locally: no truncated URL reached the transport. */
  assert_int_equal(g_http.call_count, 0);

  az_iot_mqttv3_file_upload_client_deinit(&fu2);
  az_iot_connection_client_destroy(&conn);
}

static void oversized_device_id_is_rejected_at_the_operation(void** state)
{
  (void)state;
  char long_id[AZ_IOT_FILE_UPLOAD_URL_MAX];
  memset(long_id, 'd', sizeof(long_id) - 1);
  long_id[sizeof(long_id) - 1] = '\0';

  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = TEST_HUB;
  opts.port = 8883;
  opts.client_id = long_id;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(&fu2, &conn, &http), AZ_IOT_OK);

  memset(&g_http, 0, sizeof(g_http));
  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fu2, "b", on_sas, &r),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fu2, "c", true, on_notify, &r),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  assert_int_equal(g_http.call_count, 0);

  az_iot_mqttv3_file_upload_client_deinit(&fu2);
  az_iot_connection_client_destroy(&conn);
}

/* A failed init must scrub the instance, not leave it half-wired: the operations
 * then report NOT_INITIALIZED instead of dereferencing a stale connection. */
static void failed_init_leaves_client_unusable(void** state)
{
  (void)state;
  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = TEST_HUB;
  opts.port = 8883;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_init(&fu2, &conn, &http), AZ_IOT_ERR_NOT_CONNECTED);

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fu2, "b", on_sas, &r),
      AZ_IOT_ERR_NOT_INITIALIZED);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fu2, "c", true, on_notify, &r),
      AZ_IOT_ERR_NOT_INITIALIZED);
  assert_int_equal(g_http.call_count, 0);

  az_iot_connection_client_destroy(&conn);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

static void calls_after_destroy_are_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqttv3_file_upload_client_deinit(&fx->fu);

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r),
      AZ_IOT_ERR_NOT_INITIALIZED);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
      AZ_IOT_ERR_NOT_INITIALIZED);
  assert_int_equal(g_http.call_count, 0);

  /* The fixture teardown destroys again: destroy must be idempotent. */
}

static void destroy_is_null_safe(void** state)
{
  (void)state;
  az_iot_mqttv3_file_upload_client_deinit(NULL);
}

/* Destroying twice must be harmless: the second call meets an already-scrubbed
 * instance. The fixture teardown makes it a third. */
static void destroy_is_idempotent(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqttv3_file_upload_client_deinit(&fx->fu);
  az_iot_mqttv3_file_upload_client_deinit(&fx->fu);
}

/* Re-initializing after deinit() rebinds the instance and leaves it usable.
 *
 * Deliberately NOT a double-init: init() takes a generation reference on the
 * connection, and initializing over a live instance would strand the one the
 * live instance still holds. The struct is caller-allocated, so the client
 * cannot tell a live instance from an uninitialized one and cannot refuse it --
 * which is why the header makes deinit-before-reinit the contract. */
static void reinit_after_deinit_succeeds(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_file_upload_http_transport http = { mock_send, NULL };

  az_iot_mqttv3_file_upload_client_deinit(&fx->fu);
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(&fx->fu, &fx->conn, &http), AZ_IOT_OK);

  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;
  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
}

/* Two independent upload clients may share one connection. */
static void two_clients_share_one_connection(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(&fu2, &fx->conn, &http), AZ_IOT_OK);

  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  rec r1;
  rec r2;
  memset(&r1, 0, sizeof(r1));
  memset(&r2, 0, sizeof(r2));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "one.txt", on_sas, &r1), AZ_IOT_OK);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fu2, "two.txt", on_sas, &r2), AZ_IOT_OK);
  assert_int_equal(r1.sas_status, AZ_IOT_OK);
  assert_int_equal(r2.sas_status, AZ_IOT_OK);
  assert_int_equal(g_http.call_count, 2);

  az_iot_mqttv3_file_upload_client_deinit(&fu2);
}

/* Destroying one client must not disturb another sharing the same connection.
 * destroy() unregisters from the connection, so a bug there would take the
 * survivor down with it -- a different failure than the sharing above. */
static void destroying_one_client_leaves_the_other_working(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(&fu2, &fx->conn, &http), AZ_IOT_OK);

  az_iot_mqttv3_file_upload_client_deinit(&fu2);

  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;
  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "three.txt", on_sas, &r), AZ_IOT_OK);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
}

/* The hub address and device id are read from the connection per operation, so a
 * client created before a DPS (re)assignment addresses the CURRENT hub -- caching
 * them at init() would keep aiming the REST calls at the previous one. The
 * connection re-points opts.host / opts.client_id at its own provisioned buffers
 * when an assignment lands; do the same here.
 *
 * Asserted once per operation, because each builds its own URL. */
static void sas_uri_requests_follow_a_hub_reassignment(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_string_equal(
      g_http.last_url, "https://" TEST_HUB "/devices/" TEST_DEVICE "/files?api-version=2021-04-12");

  fx->conn.opts.host = "otherhub.azure-devices.net";
  fx->conn.opts.client_id = "dev2";

  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
  assert_string_equal(
      g_http.last_url,
      "https://otherhub.azure-devices.net/devices/dev2/files?api-version=2021-04-12");
}

static void notifications_follow_a_hub_reassignment(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 204;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
      AZ_IOT_OK);
  assert_string_equal(
      g_http.last_url,
      "https://" TEST_HUB "/devices/" TEST_DEVICE "/files/notifications?api-version=2021-04-12");

  fx->conn.opts.host = "otherhub.azure-devices.net";
  fx->conn.opts.client_id = "dev2";

  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
      AZ_IOT_OK);
  assert_int_equal(r.notify_status, AZ_IOT_OK);
  assert_string_equal(
      g_http.last_url,
      "https://otherhub.azure-devices.net/devices/dev2/files/notifications?api-version=2021-04-12");
}

/* The connection may have no endpoint to hand out: it has not provisioned yet,
 * or -- once hub reassignment is supported -- it is between hubs. An operation
 * must refuse locally instead of building a request against an empty or stale
 * host.
 *
 * The return code is NOT_CONNECTED rather than INVALID_ARG on purpose: this is a
 * transient state of the connection, and an application told its arguments were
 * invalid would audit them instead of retrying.
 *
 * The two halves of an endpoint go missing independently, so each gets its own
 * test; recovery afterwards is a third, separate behaviour. */
static void requests_fail_while_the_hub_address_is_unavailable(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  fx->conn.opts.host = NULL;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r),
      AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
      AZ_IOT_ERR_NOT_CONNECTED);

  /* Refused locally: no callback fired and nothing reached the network. */
  assert_false(r.sas_done);
  assert_false(r.notify_done);
  assert_int_equal(g_http.call_count, 0);
}

/* An empty host is as unusable as a missing one, and reaches the check by a
 * different route -- opts.host pointing at a zero-length buffer rather than at
 * nothing. */
static void requests_fail_while_the_hub_address_is_empty(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  fx->conn.opts.host = "";

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r),
      AZ_IOT_ERR_NOT_CONNECTED);
  assert_false(r.sas_done);
  assert_int_equal(g_http.call_count, 0);
}

static void requests_fail_while_the_device_id_is_unavailable(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  fx->conn.opts.client_id = NULL;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r),
      AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
      AZ_IOT_ERR_NOT_CONNECTED);

  assert_false(r.sas_done);
  assert_false(r.notify_done);
  assert_int_equal(g_http.call_count, 0);
}

static void requests_fail_while_the_device_id_is_empty(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  fx->conn.opts.client_id = "";

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r),
      AZ_IOT_ERR_NOT_CONNECTED);
  assert_false(r.sas_done);
  assert_int_equal(g_http.call_count, 0);
}

/* Recovering without re-initialization is the whole point of resolving per
 * operation, so it is asserted rather than assumed: the same client instance
 * works again once the endpoint returns -- possibly a different hub than the one
 * it was initialized against. */
static void requests_resume_when_the_endpoint_returns(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  fx->conn.opts.host = NULL;
  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r),
      AZ_IOT_ERR_NOT_CONNECTED);

  fx->conn.opts.host = "otherhub.azure-devices.net";
  fx->conn.opts.client_id = "dev2";

  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
  assert_string_equal(
      g_http.last_url,
      "https://otherhub.azure-devices.net/devices/dev2/files?api-version=2021-04-12");
}

/* The Classic path is stateless per call, so starting the next request from
 * inside the completion callback is legal. Locking this in matters because the
 * Next/MQTT path will hold per-request state and must preserve the behaviour. */
typedef struct
{
  az_iot_mqttv3_file_upload_client* fu;
  int depth;
  az_iot_result nested_dispatch;
  bool nested_done;
} reentrant_ctx;

static void on_sas_reentrant(az_iot_result status, const char* uri, const char* corr, void* ctx)
{
  (void)uri;
  (void)corr;
  reentrant_ctx* rc = (reentrant_ctx*)ctx;
  assert_int_equal(status, AZ_IOT_OK);
  if (rc->depth++ == 0)
  {
    rc->nested_dispatch
        = az_iot_mqttv3_file_upload_client_get_sas_uri(rc->fu, "nested.txt", on_sas_reentrant, rc);
  }
  else
  {
    rc->nested_done = true;
  }
}

static void get_sas_uri_is_reentrant_from_callback(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  reentrant_ctx rc;
  memset(&rc, 0, sizeof(rc));
  rc.fu = &fx->fu;

  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "outer.txt", on_sas_reentrant, &rc),
      AZ_IOT_OK);
  assert_int_equal(rc.depth, 2);
  assert_int_equal(rc.nested_dispatch, AZ_IOT_OK);
  assert_true(rc.nested_done);
  assert_int_equal(g_http.call_count, 2);
  assert_string_equal(g_http.last_body, "{\"blobName\":\"nested.txt\"}");
}

/* ------------------------------------------------------------------------- */
/* generation pinning                                                        */
/* ------------------------------------------------------------------------- */

/* File upload is not carried on the MQTT v5 hub, so this client pins Classic.
 * A direct connection declares its generation up front, which is what lets the
 * pin be answered at init() rather than deferred to connect. */
static void init_against_an_mqtt_v5_connection_is_rejected(void** state)
{
  (void)state;

  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = TEST_HUB;
  opts.port = 8883;
  opts.client_id = TEST_DEVICE;
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_init(&fu2, &conn, &http),
      AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);

  az_iot_connection_client_destroy(&conn);
}

/* Refused up front rather than at the first upload: without an HTTP client the
 * SDK can never perform either operation, and a client that accepted the init
 * would fail every call with a less specific error much later. */
static void init_without_an_http_hook_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_mqttv3_file_upload_client fu2;
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_init(&fu2, &fx->conn, NULL), AZ_IOT_ERR_INVALID_ARG);

  az_iot_file_upload_http_transport empty = { NULL, NULL };
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_init(&fu2, &fx->conn, &empty), AZ_IOT_ERR_INVALID_ARG);
}

/* A failure AFTER the Classic pin is taken must release it.
 *
 * The endpoint check is the only failure that happens post-pin: the NULL and
 * missing-hook checks both run before __require_profile(), so they can never
 * exercise this. A DPS connection with no assigned hub fails there.
 *
 * Proved by admitting an mqttv5 client afterwards: a leaked Classic reference
 * would make that return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH, and the
 * connection would be stuck refusing a generation on behalf of a client that
 * does not exist. */
static void a_post_pin_init_failure_releases_the_profile_pin(void** state)
{
  (void)state;

  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "reg-1";
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  for (int i = 0; i < 4; ++i)
  {
    assert_int_equal(
        az_iot_mqttv3_file_upload_client_init(&fu2, &conn, &http), AZ_IOT_ERR_NOT_CONNECTED);
  }

  az_iot_mqttv5_telemetry_client t;
  assert_int_equal(az_iot_mqttv5_telemetry_client_init(&t, &conn), AZ_IOT_OK);
  az_iot_mqttv5_telemetry_client_destroy(&t);

  az_iot_connection_client_destroy(&conn);
}

/* The pre-pin refusals take no reference to release, so they cannot strand one.
 *
 * Uses its own DPS connection, not the fixture's: the fixture holds a live
 * Classic client, and its legitimate pin would refuse the mqttv5 client below for
 * an honest reason, hiding a leak. An unresolved connection with no live client
 * is the only state where "was a pin taken?" is observable. */
static void init_without_an_http_hook_takes_no_profile_pin(void** state)
{
  (void)state;

  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "reg-1";
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  az_iot_mqttv3_file_upload_client fu2;
  for (int i = 0; i < 4; ++i)
  {
    assert_int_equal(
        az_iot_mqttv3_file_upload_client_init(&fu2, &conn, NULL), AZ_IOT_ERR_INVALID_ARG);
  }

  /* Asserted through the OTHER generation: a leaked Classic reference is
   * invisible to another Classic client, which is admitted either way. */
  az_iot_mqttv5_telemetry_client t;
  assert_int_equal(az_iot_mqttv5_telemetry_client_init(&t, &conn), AZ_IOT_OK);
  az_iot_mqttv5_telemetry_client_destroy(&t);

  az_iot_connection_client_destroy(&conn);
}

/* ------------------------------------------------------------------------- */
/* HTTP status mapping                                                       */
/* ------------------------------------------------------------------------- */

typedef struct
{
  int status;
  az_iot_result expected;
} status_case;

/* The failure mapping is shared by both operations. 404 and 429 are called out
 * because callers act on them differently (missing hub/blob config vs. retry). */
static const status_case k_failure_status_cases[] = {
  { 301, AZ_IOT_ERR_PROTOCOL }, /* redirects are not followed */
  { 400, AZ_IOT_ERR_INVALID_ARG }, { 401, AZ_IOT_ERR_INVALID_ARG },
  { 403, AZ_IOT_ERR_INVALID_ARG }, { 404, AZ_IOT_ERR_NOT_FOUND },
  { 412, AZ_IOT_ERR_INVALID_ARG }, { 429, AZ_IOT_ERR_BUSY }, /* throttled */
  { 500, AZ_IOT_ERR_PROTOCOL },    { 503, AZ_IOT_ERR_PROTOCOL },
  { 0, AZ_IOT_ERR_PROTOCOL }, /* hook returned OK without setting a status */
};

static void get_sas_uri_maps_failure_status(void** state)
{
  fixture* fx = (fixture*)*state;
  for (size_t i = 0; i < sizeof(k_failure_status_cases) / sizeof(k_failure_status_cases[0]); ++i)
  {
    g_http.resp_status = k_failure_status_cases[i].status;
    /* A well-formed body is programmed on purpose: a failure status must win
     * and the body must never be parsed. */
    g_http.resp_body = k_sas_json;

    rec r;
    memset(&r, 0, sizeof(r));
    assert_int_equal(
        az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
    assert_true(r.sas_done);
    if (r.sas_status != k_failure_status_cases[i].expected)
    {
      fail_msg(
          "HTTP %d: expected result %d, got %d",
          k_failure_status_cases[i].status,
          (int)k_failure_status_cases[i].expected,
          (int)r.sas_status);
    }
    assert_string_equal(r.sas_uri, "");
    assert_string_equal(r.correlation_id, "");
  }
}

static void notify_complete_maps_failure_status(void** state)
{
  fixture* fx = (fixture*)*state;
  for (size_t i = 0; i < sizeof(k_failure_status_cases) / sizeof(k_failure_status_cases[0]); ++i)
  {
    g_http.resp_status = k_failure_status_cases[i].status;

    rec r;
    memset(&r, 0, sizeof(r));
    assert_int_equal(
        az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
        AZ_IOT_OK);
    assert_true(r.notify_done);
    if (r.notify_status != k_failure_status_cases[i].expected)
    {
      fail_msg(
          "HTTP %d: expected result %d, got %d",
          k_failure_status_cases[i].status,
          (int)k_failure_status_cases[i].expected,
          (int)r.notify_status);
    }
  }
}

/* IoT Hub answers 200 today, but any 2xx is a success per the mapping. */
static void get_sas_uri_accepts_any_2xx(void** state)
{
  fixture* fx = (fixture*)*state;
  static const int k_ok[] = { 200, 201, 202, 299 };
  for (size_t i = 0; i < sizeof(k_ok) / sizeof(k_ok[0]); ++i)
  {
    g_http.resp_status = k_ok[i];
    g_http.resp_body = k_sas_json;

    rec r;
    memset(&r, 0, sizeof(r));
    assert_int_equal(
        az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
    assert_int_equal(r.sas_status, AZ_IOT_OK);
    assert_string_equal(r.correlation_id, "corr-123");
  }
}

/* IoT Hub answers 204 to the notification; accept the whole 2xx range. */
static void notify_complete_accepts_any_2xx(void** state)
{
  fixture* fx = (fixture*)*state;
  static const int k_ok[] = { 200, 202, 204, 299 };
  for (size_t i = 0; i < sizeof(k_ok) / sizeof(k_ok[0]); ++i)
  {
    g_http.resp_status = k_ok[i];

    rec r;
    memset(&r, 0, sizeof(r));
    assert_int_equal(
        az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
        AZ_IOT_OK);
    assert_int_equal(r.notify_status, AZ_IOT_OK);
  }
}

static void notify_complete_transport_failure_delivers_error(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.transport_result = AZ_IOT_ERR_MQTT;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
      AZ_IOT_OK);
  assert_true(r.notify_done);
  assert_int_equal(r.notify_status, AZ_IOT_ERR_MQTT);
}

/* ------------------------------------------------------------------------- */
/* SAS response parsing                                                      */
/* ------------------------------------------------------------------------- */

/* Program a 200 + @p body and return the status delivered to the callback. */
static az_iot_result sas_result_for_body(fixture* fx, const char* body)
{
  g_http.resp_status = 200;
  g_http.resp_body = body;
  g_http.resp_body_len = 0;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_true(r.sas_done);
  return r.sas_status;
}

static void get_sas_uri_malformed_json_reports_protocol(void** state)
{
  fixture* fx = (fixture*)*state;
  static const char* const k_bad[] = {
    "", /* empty body */
    "   ",
    "not json at all",
    "{", /* truncated object */
    "{\"hostName\":\"h\"", /* unterminated */
    "[]", /* array, not an object */
    "\"just-a-string\"",
    "null",
  };
  for (size_t i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); ++i)
  {
    if (sas_result_for_body(fx, k_bad[i]) != AZ_IOT_ERR_PROTOCOL)
    {
      fail_msg("body '%s' should have reported PROTOCOL", k_bad[i]);
    }
  }
}

/* A 200 with no body at all (hook set neither body nor length). Handing an empty
 * span to the JSON reader would trip an az_core precondition whose handler never
 * returns, so this must be rejected before parsing. */
static void get_sas_uri_empty_response_reports_protocol(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = NULL;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_true(r.sas_done);
  assert_int_equal(r.sas_status, AZ_IOT_ERR_PROTOCOL);
}

/* A hook that reports more bytes than it was given must not make the parser read
 * past the buffer: the SDK clamps body_len to the capacity it handed out. Without
 * the clamp this reads far beyond the response buffer (caught by ASan/valgrind). */
static void get_sas_uri_clamps_overreported_body_len(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;
  g_http.resp_claim_body_len = AZ_IOT_FILE_UPLOAD_SAS_URI_MAX * 4;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_true(r.sas_done);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
  assert_string_equal(r.correlation_id, "corr-123");
}

/* Every field is mandatory: dropping any one of them fails the whole response
 * rather than producing a half-built URI. */
static void get_sas_uri_missing_field_reports_protocol(void** state)
{
  fixture* fx = (fixture*)*state;
  static const char* const k_missing[] = {
    /* no hostName */
    "{\"correlationId\":\"c\",\"containerName\":\"u\",\"blobName\":\"b\",\"sasToken\":\"?s\"}",
    /* no containerName */
    "{\"correlationId\":\"c\",\"hostName\":\"h\",\"blobName\":\"b\",\"sasToken\":\"?s\"}",
    /* no blobName */
    "{\"correlationId\":\"c\",\"hostName\":\"h\",\"containerName\":\"u\",\"sasToken\":\"?s\"}",
    /* no sasToken */
    "{\"correlationId\":\"c\",\"hostName\":\"h\",\"containerName\":\"u\",\"blobName\":\"b\"}",
    /* no correlationId */
    "{\"hostName\":\"h\",\"containerName\":\"u\",\"blobName\":\"b\",\"sasToken\":\"?s\"}",
    /* nothing at all */
    "{}",
  };
  for (size_t i = 0; i < sizeof(k_missing) / sizeof(k_missing[0]); ++i)
  {
    if (sas_result_for_body(fx, k_missing[i]) != AZ_IOT_ERR_PROTOCOL)
    {
      fail_msg("case %u: missing-field body should have reported PROTOCOL", (unsigned)i);
    }
  }
}

static void get_sas_uri_wrong_field_type_reports_protocol(void** state)
{
  fixture* fx = (fixture*)*state;
  static const char* const k_wrong[] = {
    "{\"correlationId\":\"c\",\"hostName\":42,\"containerName\":\"u\",\"blobName\":\"b\","
    "\"sasToken\":\"?s\"}",
    "{\"correlationId\":\"c\",\"hostName\":null,\"containerName\":\"u\",\"blobName\":\"b\","
    "\"sasToken\":\"?s\"}",
    "{\"correlationId\":\"c\",\"hostName\":\"h\",\"containerName\":{},\"blobName\":\"b\","
    "\"sasToken\":\"?s\"}",
    "{\"correlationId\":\"c\",\"hostName\":\"h\",\"containerName\":\"u\",\"blobName\":[],"
    "\"sasToken\":\"?s\"}",
    "{\"correlationId\":\"c\",\"hostName\":\"h\",\"containerName\":\"u\",\"blobName\":\"b\","
    "\"sasToken\":true}",
    "{\"correlationId\":7,\"hostName\":\"h\",\"containerName\":\"u\",\"blobName\":\"b\","
    "\"sasToken\":\"?s\"}",
  };
  for (size_t i = 0; i < sizeof(k_wrong) / sizeof(k_wrong[0]); ++i)
  {
    if (sas_result_for_body(fx, k_wrong[i]) != AZ_IOT_ERR_PROTOCOL)
    {
      fail_msg("case %u: wrong-typed field should have reported PROTOCOL", (unsigned)i);
    }
  }
}

/* Unknown nested objects/arrays must be skipped whole, not descended into --
 * otherwise an inner "hostName" would be picked up as a top-level field. */
static void get_sas_uri_skips_unknown_nested_members(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body
      = "{"
        "\"extra\":{\"hostName\":\"decoy.example.net\",\"nested\":{\"blobName\":\"decoy\"}},"
        "\"list\":[1,2,{\"containerName\":\"decoy\"}],"
        "\"hostName\":\"acct.blob.core.windows.net\","
        "\"containerName\":\"uploads\","
        "\"blobName\":\"real.txt\","
        "\"sasToken\":\"?sig=abc\","
        "\"correlationId\":\"corr-real\""
        "}";

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
  assert_string_equal(r.correlation_id, "corr-real");
  assert_string_equal(r.sas_uri, "https://acct.blob.core.windows.net/uploads/real.txt?sig=abc");
}

/* Single-byte JSON escapes in the response are unescaped before the URI is
 * assembled. */
static void get_sas_uri_unescapes_json_strings(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = "{"
                     "\"hostName\":\"acct.blob.core.windows.net\","
                     "\"containerName\":\"uploads\","
                     "\"blobName\":\"dir\\/a\\\"b.txt\","
                     "\"sasToken\":\"?sig=x\\/y\","
                     "\"correlationId\":\"corr\\/1\""
                     "}";

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
  assert_string_equal(r.correlation_id, "corr/1");
  assert_string_equal(r.sas_uri, "https://acct.blob.core.windows.net/uploads/dir/a\"b.txt?sig=x/y");
}

/* \uXXXX escapes are not implemented by the underlying azure-sdk-for-c JSON
 * reader, so a response using them cannot be parsed. IoT Hub percent-encodes
 * instead (%2F), so this is a documented limitation rather than a live problem;
 * the test pins the behaviour to a clean protocol error instead of a surprise. */
static void get_sas_uri_unicode_escape_reports_protocol(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      sas_result_for_body(
          fx,
          "{\"hostName\":\"h\",\"containerName\":\"u\",\"blobName\":\"\\u0041.txt\","
          "\"sasToken\":\"?s\",\"correlationId\":\"c\"}"),
      AZ_IOT_ERR_PROTOCOL);
}

/* First occurrence wins; the parser stops at the first match by design. */
static void get_sas_uri_duplicate_property_uses_first(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = "{"
                     "\"hostName\":\"first.blob.core.windows.net\","
                     "\"hostName\":\"second.blob.core.windows.net\","
                     "\"containerName\":\"uploads\","
                     "\"blobName\":\"b.txt\","
                     "\"sasToken\":\"?sig=abc\","
                     "\"correlationId\":\"corr-1\""
                     "}";

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
  assert_string_equal(r.sas_uri, "https://first.blob.core.windows.net/uploads/b.txt?sig=abc");
}

/* Fill @p out with @p n copies of @p c plus a NUL. */
static void fill_str(char* out, size_t n, char c)
{
  memset(out, c, n);
  out[n] = '\0';
}

/* A field longer than its parse buffer must be refused, never truncated into a
 * URI the caller would then upload to. */
static void get_sas_uri_oversized_field_reports_protocol(void** state)
{
  fixture* fx = (fixture*)*state;
  char big[1200];
  char body[2600];

  /* hostName parse buffer is 256 bytes. */
  fill_str(big, 300, 'h');
  snprintf(
      body,
      sizeof(body),
      "{\"hostName\":\"%s\",\"containerName\":\"u\",\"blobName\":\"b\","
      "\"sasToken\":\"?s\",\"correlationId\":\"c\"}",
      big);
  assert_int_equal(sas_result_for_body(fx, body), AZ_IOT_ERR_PROTOCOL);

  /* sasToken parse buffer is 1024 bytes. */
  fill_str(big, 1100, 's');
  snprintf(
      body,
      sizeof(body),
      "{\"hostName\":\"h\",\"containerName\":\"u\",\"blobName\":\"b\","
      "\"sasToken\":\"?%s\",\"correlationId\":\"c\"}",
      big);
  assert_int_equal(sas_result_for_body(fx, body), AZ_IOT_ERR_PROTOCOL);

  /* correlationId is delivered in a AZ_IOT_FILE_UPLOAD_CORR_ID_MAX buffer. */
  fill_str(big, AZ_IOT_FILE_UPLOAD_CORR_ID_MAX + 8, 'c');
  snprintf(
      body,
      sizeof(body),
      "{\"hostName\":\"h\",\"containerName\":\"u\",\"blobName\":\"b\","
      "\"sasToken\":\"?s\",\"correlationId\":\"%s\"}",
      big);
  assert_int_equal(sas_result_for_body(fx, body), AZ_IOT_ERR_PROTOCOL);
}

/* A response larger than the buffer the SDK hands the hook arrives truncated,
 * which must surface as a protocol error rather than a partial URI. */
static void get_sas_uri_truncated_response_reports_protocol(void** state)
{
  fixture* fx = (fixture*)*state;
  static char body[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX + 512];
  char filler[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX + 256];

  /* Valid JSON that simply does not fit: the trailing fields get cut off. */
  fill_str(filler, sizeof(filler) - 1, 'p');
  int n = snprintf(
      body,
      sizeof(body),
      "{\"padding\":\"%s\",\"hostName\":\"h\",\"containerName\":\"u\","
      "\"blobName\":\"b\",\"sasToken\":\"?s\",\"correlationId\":\"c\"}",
      filler);
  assert_true(n > 0 && (size_t)n > AZ_IOT_FILE_UPLOAD_SAS_URI_MAX);

  assert_int_equal(sas_result_for_body(fx, body), AZ_IOT_ERR_PROTOCOL);
}

/* The response buffer must accommodate the largest fields the parser accepts. */
static void get_sas_uri_accepts_large_fields(void** state)
{
  fixture* fx = (fixture*)*state;
  char host[128];
  char container[64];
  char blob[256];
  char sas[1024];
  static char body[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX];

  fill_str(host, 100, 'h');
  fill_str(container, 63, 'c');
  fill_str(blob, 200, 'b');
  fill_str(sas, 1000, 's');
  int n = snprintf(
      body,
      sizeof(body),
      "{\"hostName\":\"%s\",\"containerName\":\"%s\",\"blobName\":\"%s\","
      "\"sasToken\":\"?%s\",\"correlationId\":\"corr-big\"}",
      host,
      container,
      blob,
      sas);
  assert_true(n > 0 && (size_t)n < AZ_IOT_FILE_UPLOAD_SAS_URI_MAX);

  g_http.resp_status = 200;
  g_http.resp_body = body;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
  assert_string_equal(r.correlation_id, "corr-big");
  /* https:// + host + / + container + / + blob + ?sas */
  assert_int_equal((int)strlen(r.sas_uri), 8 + 100 + 1 + 63 + 1 + 200 + 1 + 1000);
}

/* A hook that answers with a buffer of its own is refused: the SDK parses only
 * the storage it handed out, so a redirected pointer cannot steer the reader. */
static void get_sas_uri_rejects_a_redirected_response_buffer(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;
  g_http.resp_redirect_body = true;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_true(r.sas_done);
  assert_int_equal(r.sas_status, AZ_IOT_ERR_PROTOCOL);
}

/* ------------------------------------------------------------------------- */
/* request building                                                          */
/* ------------------------------------------------------------------------- */

/* Both operations authenticate with mutual TLS in the application's hook, so the
 * SDK sends an empty Authorization and always a JSON content type. Asserted per
 * operation: each builds its own request, so each can regress on its own. */
static void sas_uri_request_carries_empty_auth_and_json_content_type(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_string_equal(g_http.last_authorization, "");
  assert_string_equal(g_http.last_content_type, "application/json");
}

static void notification_request_carries_empty_auth_and_json_content_type(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 204;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
      AZ_IOT_OK);
  assert_string_equal(g_http.last_authorization, "");
  assert_string_equal(g_http.last_content_type, "application/json");
}

/* get_sas_uri reads the response body, so the hook is handed a real buffer. */
static void sas_uri_request_supplies_a_response_buffer(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &r), AZ_IOT_OK);
  assert_false(g_http.last_resp_body_null);
  assert_int_equal((int)g_http.last_resp_body_capacity, AZ_IOT_FILE_UPLOAD_SAS_URI_MAX);
}

/* notify_complete reads no response body, so the hook gets an empty sink and
 * must tolerate it. The opposite expectation to the test above, which is why it
 * is not folded in with it. */
static void notification_request_supplies_no_response_buffer(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 204;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &r),
      AZ_IOT_OK);
  assert_true(g_http.last_resp_body_null);
  assert_int_equal((int)g_http.last_resp_body_capacity, 0);
}

/* Blob names are JSON string values: quotes and backslashes must be escaped so
 * the request body stays well-formed (and cannot be used to inject fields). */
static void get_sas_uri_escapes_blob_name(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "dir/a\"b\\c.txt", on_sas, &r),
      AZ_IOT_OK);
  assert_string_equal(g_http.last_body, "{\"blobName\":\"dir/a\\\"b\\\\c.txt\"}");

  /* An injection attempt stays a single string value. */
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "x\",\"evil\":\"y", on_sas, &r),
      AZ_IOT_OK);
  assert_string_equal(g_http.last_body, "{\"blobName\":\"x\\\",\\\"evil\\\":\\\"y\"}");
}

/* A blob name that cannot fit the request-body buffer is refused up front: the
 * call fails synchronously, nothing is sent and no callback fires. */
static void get_sas_uri_oversized_blob_name_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  char blob[AZ_IOT_FILE_UPLOAD_BODY_MAX + 64];
  fill_str(blob, sizeof(blob) - 1, 'b');

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, blob, on_sas, &r),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_false(r.sas_done);
  assert_int_equal(g_http.call_count, 0);
}

static void notify_complete_oversized_correlation_id_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  char corr[AZ_IOT_FILE_UPLOAD_BODY_MAX + 64];
  fill_str(corr, sizeof(corr) - 1, 'c');

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, corr, true, on_notify, &r),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_false(r.notify_done);
  assert_int_equal(g_http.call_count, 0);
}

/* The complement of the two oversized-endpoint tests: the longest hub host and
 * device id the connection client can hold still fit AZ_IOT_FILE_UPLOAD_URL_MAX,
 * so both URLs must build. (Formerly named ..._oversized_url_is_refused, which
 * described the opposite of what it asserts.) */
static void max_length_endpoint_still_builds_a_url(void** state)
{
  (void)state;
  char long_host[AZ_IOT_DPS_HOST_BUF];
  char long_id[AZ_IOT_DPS_DEVICE_ID_BUF];
  fill_str(long_host, sizeof(long_host) - 1, 'h');
  fill_str(long_id, sizeof(long_id) - 1, 'd');

  az_iot_connection_client conn;
  az_iot_connection_client_options opts = { 0 };
  opts.host = long_host;
  opts.port = 8883;
  opts.client_id = long_id;
  assert_int_equal(az_iot_connection_client_init(&conn, &opts), AZ_IOT_OK);

  memset(&g_http, 0, sizeof(g_http));
  az_iot_mqttv3_file_upload_client fu2;
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(&fu2, &conn, &http), AZ_IOT_OK);

  /* The longest hub host and device id the connection client can hold still fit
   * AZ_IOT_FILE_UPLOAD_URL_MAX, so both URLs must build. */
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json;
  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(az_iot_mqttv3_file_upload_client_get_sas_uri(&fu2, "b", on_sas, &r), AZ_IOT_OK);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
  assert_true(strlen(g_http.last_url) < AZ_IOT_FILE_UPLOAD_URL_MAX);

  g_http.resp_status = 204;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fu2, "c", true, on_notify, &r), AZ_IOT_OK);
  assert_int_equal(r.notify_status, AZ_IOT_OK);
  assert_true(strlen(g_http.last_url) < AZ_IOT_FILE_UPLOAD_URL_MAX);

  az_iot_mqttv3_file_upload_client_deinit(&fu2);
  az_iot_connection_client_destroy(&conn);
}

/* A SAS response with members the client does not care about, including a
 * nested object and an array, ahead of the ones it does. The reader has to skip
 * those at object scope rather than descending into them and matching a
 * same-named member of the wrong object. */
static const char k_sas_json_nested[]
    = "{"
      "\"meta\":{\"blobName\":\"decoy-from-nested-object\",\"n\":1},"
      "\"tags\":[\"a\",\"b\"],"
      "\"correlationId\":\"corr-nested\","
      "\"hostName\":\"acct.blob.core.windows.net\","
      "\"containerName\":\"uploads\","
      "\"blobName\":\"dev1/sample-data/test.txt\","
      "\"sasToken\":\"?sv=2021-04-12&sr=b&sig=ABC%2F123&se=2026-01-01&sp=rw\""
      "}";

static void a_sas_response_with_nested_members_still_finds_the_fields(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = k_sas_json_nested;

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "sample-data/test.txt", on_sas, &r),
      AZ_IOT_OK);

  assert_true(r.sas_done);
  assert_int_equal(r.sas_status, AZ_IOT_OK);
  assert_string_equal(r.correlation_id, "corr-nested");
  /* The decoy inside "meta" must not win. */
  assert_string_equal(
      r.sas_uri,
      "https://acct.blob.core.windows.net/uploads/dev1/sample-data/test.txt"
      "?sv=2021-04-12&sr=b&sig=ABC%2F123&se=2026-01-01&sp=rw");
}

/* A response that is not a JSON object at all cannot yield any field. */
static void a_sas_response_that_is_not_an_object_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  g_http.resp_status = 200;
  g_http.resp_body = "[\"not\",\"an\",\"object\"]";

  rec r;
  memset(&r, 0, sizeof(r));
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "sample-data/test.txt", on_sas, &r),
      AZ_IOT_OK);

  assert_true(r.sas_done);
  assert_int_not_equal(r.sas_status, AZ_IOT_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(init_rejects_null),
    cmocka_unit_test_setup_teardown(classic_init_requires_http_hook, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_builds_the_request, setup, teardown),
    cmocka_unit_test_setup_teardown(
        get_sas_uri_delivers_the_sas_uri_and_correlation_id, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_http_error_delivers_error, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_transport_failure_delivers_error, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_rejects_bad_args, setup, teardown),
    cmocka_unit_test_setup_teardown(notify_complete_builds_the_request, setup, teardown),
    cmocka_unit_test_setup_teardown(notify_complete_delivers_the_ack, setup, teardown),
    cmocka_unit_test_setup_teardown(notify_complete_failure_body, setup, teardown),
    cmocka_unit_test_setup_teardown(notify_complete_rejects_bad_args, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_rejects_null_client, setup, teardown),

    /* init argument validation + failure cleanup */
    cmocka_unit_test_setup_teardown(classic_init_rejects_transport_with_null_send, setup, teardown),
    cmocka_unit_test(init_rejects_unresolved_hub_address),
    cmocka_unit_test(init_rejects_missing_device_id),
    cmocka_unit_test(oversized_hub_address_is_rejected_at_the_operation),
    cmocka_unit_test(oversized_device_id_is_rejected_at_the_operation),
    cmocka_unit_test(failed_init_leaves_client_unusable),

    /* lifecycle */
    cmocka_unit_test_setup_teardown(calls_after_destroy_are_rejected, setup, teardown),
    cmocka_unit_test(destroy_is_null_safe),
    cmocka_unit_test_setup_teardown(destroy_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(reinit_after_deinit_succeeds, setup, teardown),
    cmocka_unit_test_setup_teardown(two_clients_share_one_connection, setup, teardown),
    cmocka_unit_test_setup_teardown(
        destroying_one_client_leaves_the_other_working, setup, teardown),

    /* endpoint resolution (per operation, never cached) */
    cmocka_unit_test_setup_teardown(sas_uri_requests_follow_a_hub_reassignment, setup, teardown),
    cmocka_unit_test_setup_teardown(notifications_follow_a_hub_reassignment, setup, teardown),
    cmocka_unit_test_setup_teardown(
        requests_fail_while_the_hub_address_is_unavailable, setup, teardown),
    cmocka_unit_test_setup_teardown(requests_fail_while_the_hub_address_is_empty, setup, teardown),
    cmocka_unit_test_setup_teardown(
        requests_fail_while_the_device_id_is_unavailable, setup, teardown),
    cmocka_unit_test_setup_teardown(requests_fail_while_the_device_id_is_empty, setup, teardown),
    cmocka_unit_test_setup_teardown(requests_resume_when_the_endpoint_returns, setup, teardown),

    cmocka_unit_test_setup_teardown(get_sas_uri_is_reentrant_from_callback, setup, teardown),

    /* HTTP status mapping */
    cmocka_unit_test_setup_teardown(get_sas_uri_maps_failure_status, setup, teardown),
    cmocka_unit_test_setup_teardown(notify_complete_maps_failure_status, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_accepts_any_2xx, setup, teardown),
    cmocka_unit_test_setup_teardown(notify_complete_accepts_any_2xx, setup, teardown),
    cmocka_unit_test_setup_teardown(
        notify_complete_transport_failure_delivers_error, setup, teardown),

    /* SAS response parsing */
    cmocka_unit_test_setup_teardown(get_sas_uri_malformed_json_reports_protocol, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_empty_response_reports_protocol, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_clamps_overreported_body_len, setup, teardown),
    cmocka_unit_test_setup_teardown(
        get_sas_uri_rejects_a_redirected_response_buffer, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_missing_field_reports_protocol, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_wrong_field_type_reports_protocol, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_skips_unknown_nested_members, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_unescapes_json_strings, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_unicode_escape_reports_protocol, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_duplicate_property_uses_first, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_oversized_field_reports_protocol, setup, teardown),
    cmocka_unit_test_setup_teardown(
        get_sas_uri_truncated_response_reports_protocol, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_accepts_large_fields, setup, teardown),

    /* request building */
    cmocka_unit_test_setup_teardown(
        sas_uri_request_carries_empty_auth_and_json_content_type, setup, teardown),
    cmocka_unit_test_setup_teardown(
        notification_request_carries_empty_auth_and_json_content_type, setup, teardown),
    cmocka_unit_test_setup_teardown(sas_uri_request_supplies_a_response_buffer, setup, teardown),
    cmocka_unit_test_setup_teardown(
        notification_request_supplies_no_response_buffer, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_escapes_blob_name, setup, teardown),
    cmocka_unit_test_setup_teardown(get_sas_uri_oversized_blob_name_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(
        notify_complete_oversized_correlation_id_is_refused, setup, teardown),
    cmocka_unit_test(max_length_endpoint_still_builds_a_url),

    /* generation pinning */
    cmocka_unit_test_setup_teardown(
        init_against_an_mqtt_v5_connection_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(init_without_an_http_hook_is_rejected, setup, teardown),
    cmocka_unit_test(a_post_pin_init_failure_releases_the_profile_pin),
    cmocka_unit_test(init_without_an_http_hook_takes_no_profile_pin),
    cmocka_unit_test_setup_teardown(
        a_sas_response_with_nested_members_still_finds_the_fields, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_sas_response_that_is_not_an_object_is_refused, setup, teardown),
  };
  return cmocka_run_group_tests_name("file_upload_client", tests, NULL, NULL);
}
