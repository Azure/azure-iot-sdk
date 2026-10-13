// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* MQTTv3 file upload authorization: the hub SAS token handed to the HTTP
 * transport hook when the connection authenticates the hub with a key. Built
 * once per crypto backend; the reference token is the one connection_sas_test.c
 * checks as the CONNECT password. */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/mqttv3/az_iot_file_upload_client.h"
#include "support/test_provider.h"

#if defined(AZ_IOT_TEST_CRYPTO_MBEDTLS)
#include "az_iot_crypto_mbedtls.h"
#define TEST_CRYPTO() az_iot_crypto_mbedtls()
#else
#include "az_iot_crypto_openssl.h"
#define TEST_CRYPTO() az_iot_crypto_openssl()
#endif

/* Bytes 0..31. */
#define KEY_B64 "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
#define NOW 1700000000u
#define HUB_TOKEN                                                                       \
  "SharedAccessSignature sr=broker.example%2Fdevices%2Fut-device&sig=gqgacRa%2FNHRRK0%" \
  "2FCjw6CUls%2BkHtn3nhT2hrASNZQan0%3D&se=1700003600"

static const char k_sas_json[] = "{\"correlationId\":\"c\",\"hostName\":\"h\","
                                 "\"containerName\":\"k\",\"blobName\":\"b\",\"sasToken\":\"?s\"}";

static uint8_t g_sas_buffer[AZ_IOT_SAS_BUFFER_SIZE(1, AZ_IOT_SAS_TOKEN_SIZE(256))];

typedef struct
{
  int calls;
  char authorization[AZ_IOT_FILE_UPLOAD_AUTHORIZATION_MAX];
} mock_http;

static mock_http g_http;

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
  (void)method;
  (void)url;
  (void)content_type;
  (void)body;
  (void)body_len;
  (void)hook_ctx;
  g_http.calls++;
  assert_non_null(authorization);
  snprintf(g_http.authorization, sizeof(g_http.authorization), "%s", authorization);
  response->status_code = 200;
  if (response->body != NULL)
  {
    size_t n = sizeof(k_sas_json) - 1;
    memcpy(response->body, k_sas_json, n);
    response->body_len = n;
  }
  return AZ_IOT_OK;
}

static uint64_t fixed_time(void* user_ctx)
{
  (void)user_ctx;
  return NOW;
}

static uint64_t no_time(void* user_ctx)
{
  (void)user_ctx;
  return 0;
}

typedef struct
{
  az_iot_connection_client conn;
  az_iot_mqttv3_file_upload_client fu;
  az_iot_test_provider provider;
} fixture;

static az_iot_connection_client_options sas_options(void)
{
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.client_id = "ut-device";
  opts.crypto = TEST_CRYPTO();
  opts.hub_auth.sas.primary_key_base64 = KEY_B64;
  opts.unix_time.get_time = fixed_time;
  opts.sas_buffer.buffer = g_sas_buffer;
  opts.sas_buffer.size = sizeof(g_sas_buffer);
  return opts;
}

static void start(fixture* fx, const az_iot_connection_client_options* opts)
{
  memset(&g_http, 0, sizeof(g_http));
  assert_int_equal(az_iot_connection_client_init(&fx->conn, opts), AZ_IOT_OK);
  az_iot_file_upload_http_transport http = { mock_send, NULL };
  assert_int_equal(az_iot_mqttv3_file_upload_client_init(&fx->fu, &fx->conn, &http), AZ_IOT_OK);
}

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);
  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqttv3_file_upload_client_deinit(&fx->fu);
  az_iot_connection_client_deinit(&fx->conn);
  free(fx);
  return 0;
}

static void on_sas(az_iot_result status, const char* uri, const char* corr, void* ctx)
{
  (void)uri;
  (void)corr;
  *(az_iot_result*)ctx = status;
}

static void on_notify(az_iot_result status, void* ctx) { *(az_iot_result*)ctx = status; }

static void both_requests_carry_the_hub_sas_token(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = sas_options();
  start(fx, &opts);

  az_iot_result status = AZ_IOT_ERR_INTERNAL;
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &status), AZ_IOT_OK);
  assert_int_equal(status, AZ_IOT_OK);
  assert_string_equal(g_http.authorization, HUB_TOKEN);

  memset(g_http.authorization, 0, sizeof(g_http.authorization));
  status = AZ_IOT_ERR_INTERNAL;
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &status),
      AZ_IOT_OK);
  assert_int_equal(status, AZ_IOT_OK);
  assert_string_equal(g_http.authorization, HUB_TOKEN);
  assert_int_equal(g_http.calls, 2);
}

/* X.509: the hook authenticates with mutual TLS, so authorization stays "". */
static void an_x509_device_gets_an_empty_authorization(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_test_provider_init(&fx->provider, "ca.pem");
  az_iot_connection_client_options opts = sas_options();
  opts.certificate_provider = &fx->provider.base;
  start(fx, &opts);

  az_iot_result status = AZ_IOT_ERR_INTERNAL;
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &status), AZ_IOT_OK);
  assert_int_equal(status, AZ_IOT_OK);
  assert_string_equal(g_http.authorization, "");
}

/* A token that cannot be signed is not dispatched: no hook call, no callback. */
static void a_token_that_cannot_be_signed_is_not_dispatched(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_connection_client_options opts = sas_options();
  opts.unix_time.get_time = no_time;
  start(fx, &opts);

  az_iot_result status = AZ_IOT_ERR_INTERNAL;
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_get_sas_uri(&fx->fu, "b", on_sas, &status), AZ_IOT_ERR_BUSY);
  assert_int_equal(
      az_iot_mqttv3_file_upload_client_notify_complete(&fx->fu, "c", true, on_notify, &status),
      AZ_IOT_ERR_BUSY);
  assert_int_equal(status, AZ_IOT_ERR_INTERNAL);
  assert_int_equal(g_http.calls, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(both_requests_carry_the_hub_sas_token, setup, teardown),
    cmocka_unit_test_setup_teardown(an_x509_device_gets_an_empty_authorization, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_token_that_cannot_be_signed_is_not_dispatched, setup, teardown),
  };
  return cmocka_run_group_tests_name("mqttv3_file_upload_sas", tests, NULL, NULL);
}
