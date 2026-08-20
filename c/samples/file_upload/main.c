// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* file_upload - sample.
 *
 * File upload uses ONE seamless SDK API regardless of hub flavor:
 *   1. az_iot_file_upload_client_get_sas_uri()   -> SAS URI + correlation id
 *   2. the app PUTs the file to that SAS URI on Azure Storage (HTTPS)
 *   3. az_iot_file_upload_client_notify_complete()
 *
 * The control-plane transport is chosen by the SDK from the connection's hub
 * flavor: on IoT Hub Classic it is HTTPS to the hub, performed through the
 * application HTTP hook this sample registers (the SDK ships no HTTP client); on
 * IoT Hub Next (AEG) it travels over the MQTT connection (handled by the SDK).
 * The blob PUT to Azure Storage is always the application's own HTTPS call.
 *
 * This sample uses libcurl (compiled in when CMake's find_package(CURL) succeeds
 * and defines AZ_IOT_SAMPLE_WITH_CURL) both as the HTTP hook and for the Storage
 * PUT. Without libcurl it still provisions and connects, but the HTTPS calls are
 * unavailable and the sample explains how to enable them.
 *
 * Authentication: the Classic hub REST calls use the X.509 device certificate
 * (mutual TLS) -- the same cert/key used for MQTT. The Storage PUT is
 * authenticated by the SAS token embedded in the blob URI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

#ifdef AZ_IOT_SAMPLE_WITH_CURL
#include <curl/curl.h>
#endif

/* The blob to create and the content to upload. */
static const char k_blob_name[] = "sample-data/test.txt";
static const char k_blob_content[] = "Hello from the Azure IoT C SDK file upload sample.\n";

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_file_upload_client file_upload_client;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  az_iot_file_upload_client_destroy(&s->file_upload_client);
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  az_iot_connection_state conn_state;
} user_context;

static void on_conn_state(az_iot_connection_state s, az_iot_result reason, void* user_ctx)
{
  (void)reason;
  ((user_context*)user_ctx)->conn_state = s;
}

#ifdef AZ_IOT_SAMPLE_WITH_CURL

typedef struct
{
  char* buf;
  size_t cap;
  size_t len;
} response_sink;

static size_t on_curl_write(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  response_sink* sink = (response_sink*)userdata;
  size_t chunk = size * nmemb;
  if (sink->buf && sink->cap)
  {
    size_t room = (sink->len + 1 < sink->cap) ? (sink->cap - 1 - sink->len) : 0;
    size_t copy = (chunk < room) ? chunk : room;
    if (copy)
    {
      memcpy(sink->buf + sink->len, ptr, copy);
      sink->len += copy;
      sink->buf[sink->len] = '\0';
    }
  }
  return chunk; /* report full consumption so curl does not abort the transfer */
}

/* Perform one HTTPS request with libcurl. Returns the HTTP status code, or -1 on
 * a transport failure. When client_cert/client_key are non-NULL, mutual-TLS auth
 * is used (required for the IoT Hub REST calls). */
static long https_request(
    const char* method,
    const char* url,
    const char* content_type,
    const char* authorization,
    const void* body,
    size_t body_len,
    const char* extra_header,
    const char* client_cert,
    const char* client_key,
    char* resp,
    size_t resp_cap,
    size_t* resp_len)
{
  CURL* curl = curl_easy_init();
  if (!curl)
  {
    return -1;
  }

  struct curl_slist* headers = NULL;
  if (content_type && content_type[0])
  {
    char h[128];
    snprintf(h, sizeof(h), "Content-Type: %s", content_type);
    headers = curl_slist_append(headers, h);
  }
  if (authorization && authorization[0])
  {
    char h[576];
    snprintf(h, sizeof(h), "Authorization: %s", authorization);
    headers = curl_slist_append(headers, h);
  }
  if (extra_header && extra_header[0])
  {
    headers = curl_slist_append(headers, extra_header);
  }

  response_sink sink = { resp, resp_cap, 0 };
  if (resp && resp_cap)
  {
    resp[0] = '\0';
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  if (body && body_len)
  {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
  }
  if (headers)
  {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  if (client_cert && client_key)
  {
    curl_easy_setopt(curl, CURLOPT_SSLCERT, client_cert);
    curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLKEY, client_key);
    curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
  }

  long status = -1;
  CURLcode cc = curl_easy_perform(curl);
  if (cc == CURLE_OK)
  {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  }
  else
  {
    fprintf(stderr, "HTTP %s failed: %s\n", method, curl_easy_strerror(cc));
  }

  if (resp_len)
  {
    *resp_len = sink.len;
  }
  if (headers)
  {
    curl_slist_free_all(headers);
  }
  curl_easy_cleanup(curl);
  return status;
}

#endif /* AZ_IOT_SAMPLE_WITH_CURL */

/* Paths used by the HTTP hook to authenticate the Classic hub REST calls (mTLS). */
typedef struct
{
  const char* cert;
  const char* key;
} hub_http_ctx;

/* HTTP transport hook the SDK calls for the Classic control plane (SAS-URI
 * request + completion notification). Always defined so the file upload client
 * can be initialized on Classic even in a build without libcurl. */
static az_iot_result curl_http_send(
    const char* method,
    const char* url,
    const char* authorization,
    const char* content_type,
    const uint8_t* body,
    size_t body_len,
    az_iot_file_upload_http_response* response,
    void* hook_ctx)
{
#ifdef AZ_IOT_SAMPLE_WITH_CURL
  hub_http_ctx* hc = (hub_http_ctx*)hook_ctx;
  long status = https_request(
      method,
      url,
      content_type,
      authorization,
      body,
      body_len,
      NULL,
      hc->cert,
      hc->key,
      (char*)response->body,
      response->body_capacity,
      &response->body_len);
  if (status < 0)
  {
    return AZ_IOT_ERR_MQTT; /* transport failure */
  }
  response->status_code = (int)status;
  return AZ_IOT_OK;
#else
  (void)url;
  (void)authorization;
  (void)content_type;
  (void)body;
  (void)body_len;
  (void)response;
  (void)hook_ctx;
  fprintf(stderr, "libcurl not built in; cannot perform HTTPS %s.\n", method);
  return AZ_IOT_ERR_MQTT;
#endif
}

typedef struct
{
  bool sas_done;
  az_iot_result sas_status;
  char sas_uri[AZ_IOT_FILE_UPLOAD_SAS_URI_MAX];
  char correlation_id[AZ_IOT_FILE_UPLOAD_CORR_ID_MAX];
  bool notify_done;
  az_iot_result notify_status;
} upload_ctx;

static void on_sas(
    az_iot_result status,
    const char* blob_sas_uri,
    const char* correlation_id,
    void* user_ctx)
{
  upload_ctx* u = (upload_ctx*)user_ctx;
  u->sas_done = true;
  u->sas_status = status;
  if (status == AZ_IOT_OK && blob_sas_uri && correlation_id)
  {
    snprintf(u->sas_uri, sizeof(u->sas_uri), "%s", blob_sas_uri);
    snprintf(u->correlation_id, sizeof(u->correlation_id), "%s", correlation_id);
  }
}

static void on_notify(az_iot_result status, void* user_ctx)
{
  upload_ctx* u = (upload_ctx*)user_ctx;
  u->notify_done = true;
  u->notify_status = status;
}

/* Drive the three-step upload through the seamless SDK API. Returns 0 on a
 * fully successful upload. */
static int run_file_upload(sample_state* state)
{
  az_iot_file_upload_client* fu = &state->file_upload_client;
  upload_ctx u;
  memset(&u, 0, sizeof(u));

  /* Step 1: request a SAS URI. On Classic, on_sas fires synchronously here. */
  printf("Requesting SAS URI for '%s'...\n", k_blob_name);
  az_iot_result gr = az_iot_file_upload_client_get_sas_uri(fu, k_blob_name, on_sas, &u);
  if (gr == AZ_IOT_ERR_NOT_SUPPORTED)
  {
    printf("File upload is not yet available on this hub "
           "(AEG/Next: pending the Files message schema).\n");
    return 1;
  }
  if (gr != AZ_IOT_OK)
  {
    printf("Failed to start the SAS URI request: %s\n", az_iot_result_to_string(gr));
    return 1;
  }
  if (!u.sas_done || u.sas_status != AZ_IOT_OK)
  {
    printf("SAS URI request did not succeed: %s\n", az_iot_result_to_string(u.sas_status));
#ifndef AZ_IOT_SAMPLE_WITH_CURL
    printf(
        "(Rebuild with libcurl to upload the %lu bytes of '%s'.)\n",
        (unsigned long)strlen(k_blob_content),
        k_blob_name);
#endif
    return 1;
  }
  printf("Received SAS URI (correlation id: %s).\n", u.correlation_id);

  /* Step 2: PUT the blob to Azure Storage (SAS token in the URI; no cert). */
  bool put_ok = false;
#ifdef AZ_IOT_SAMPLE_WITH_CURL
  printf("Uploading %lu bytes to Azure Storage...\n", (unsigned long)strlen(k_blob_content));
  long put = https_request(
      "PUT",
      u.sas_uri,
      NULL,
      NULL,
      k_blob_content,
      strlen(k_blob_content),
      "x-ms-blob-type: BlockBlob",
      NULL,
      NULL,
      NULL,
      0,
      NULL);
  put_ok = (put >= 200 && put < 300);
  printf("Storage PUT completed (HTTP %ld).\n", put);
#endif

  /* Step 3: notify IoT Hub of the outcome (always, success or failure). */
  printf("Notifying IoT Hub of completion...\n");
  az_iot_result nr
      = az_iot_file_upload_client_notify_complete(fu, u.correlation_id, put_ok, on_notify, &u);
  if (nr != AZ_IOT_OK)
  {
    printf("Failed to send the completion notification: %s\n", az_iot_result_to_string(nr));
    return 1;
  }
  if (!u.notify_done || u.notify_status != AZ_IOT_OK)
  {
    printf(
        "Completion notification did not succeed: %s\n", az_iot_result_to_string(u.notify_status));
    return 1;
  }

  if (!put_ok)
  {
    printf("Blob upload failed; IoT Hub was notified of the failure.\n");
    return 1;
  }
  printf("File upload completed successfully.\n");
  return 0;
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sample_state state = { 0 };
  if (sample_config_load(&state.config) != 0)
  {
    return 1;
  }

  int rc = 1;
  user_context user_ctx = { 0 };

  /* Certificate provider (X.509). */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = state.config.ca;
  pem.client_cert_pem_path = state.config.cert;
  pem.client_key_pem_path = state.config.key;

  if (az_iot_certificate_provider_pem_init(&state.certs, &pem) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Connection client (DPS provisioning is internal when host==NULL). */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = state.config.id_scope;
  copts.dps.registration_id = state.config.reg_id;
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_set_state_callback(&state.connection_client, on_conn_state, &user_ctx);

  /* DPS provisioning and the Classic hub session both run over MQTT v3.1.1. */
  if (az_iot_connection_client_register_mqtt_factory(
          &state.connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Open (internally provisions via DPS then connects to the assigned hub). */
  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    printf(
        "Connected. IoT Hub: %s\n",
        az_iot_connection_client_get_iothub_address(&state.connection_client));

    /* The Classic control plane needs an app HTTP transport (mTLS with the
     * device cert). On Next the SDK uses MQTT and ignores this hook. */
    hub_http_ctx http_ctx = { state.config.cert, state.config.key };
    az_iot_file_upload_http_transport http = { curl_http_send, &http_ctx };

    if (az_iot_file_upload_client_init(&state.file_upload_client, &state.connection_client, &http)
        != AZ_IOT_OK)
    {
      printf("Failed to initialize the file upload client.\n");
    }
    else
    {
#ifdef AZ_IOT_SAMPLE_WITH_CURL
      curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
      rc = run_file_upload(&state);
#ifdef AZ_IOT_SAMPLE_WITH_CURL
      curl_global_cleanup();
#endif
    }
  }
  else
  {
    printf("Failed to reach CONNECTED (state=%d).\n", (int)user_ctx.conn_state);
  }

  /* Close connection. */
  az_iot_connection_client_close(&state.connection_client);

  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
