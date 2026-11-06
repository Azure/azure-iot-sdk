// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Implementation of the MQTTAsync stand-in. See mock_paho_async.h. */

#include "mock_paho_async.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The adapter only ever stores the handle and hands it back to us, so any
 * non-NULL address will do. */
static int s_handle_token;

static int s_create_rc;
static int s_set_callbacks_rc;
static int s_connect_rc;

static int s_disconnect_rc;
static bool s_disconnect_completion_success;
static int s_disconnect_calls;
static bool s_disconnect_had_callbacks;

static int s_create_calls;
static int s_connect_calls;
static int s_destroy_calls;

static char s_last_server_uri[256];
static bool s_last_had_ssl;
static int s_last_enable_server_cert_auth;
static int s_last_verify;
static char s_last_private_key[512];
static bool s_last_had_private_key;
static char s_last_http_proxy[256];
static char s_last_https_proxy[256];
static int s_last_struct_version;
static int s_last_connect_timeout;

/* The inbound callbacks the adapter registered, and the context it expects
 * back. Written once by connect(), read by the test's producer threads. */
static void* s_callback_context;
static MQTTAsync_messageArrived* s_message_arrived;
static MQTTAsync_connectionLost* s_connection_lost;

static int s_send_calls;
static int s_send_rc;
static bool s_send_completes_at_once;
static MQTTAsync_responseOptions s_last_send_response;

/* Accepted sends. Paho completes each exactly once: by a callback a test drives, during
 * MQTTAsync_sendMessage() when s_send_completes_at_once, or else in MQTTAsync_destroy(). */
#define MOCK_PAHO_MAX_SENDS 64
typedef struct
{
  MQTTAsync_responseOptions response;
  bool pending;
} mock_paho_send;
static mock_paho_send s_sends[MOCK_PAHO_MAX_SENDS];
static int s_send_count;

void mock_paho_reset(void)
{
  s_create_rc = MQTTASYNC_SUCCESS;
  s_set_callbacks_rc = MQTTASYNC_SUCCESS;
  s_connect_rc = MQTTASYNC_SUCCESS;
  s_disconnect_rc = MQTTASYNC_SUCCESS;
  s_disconnect_completion_success = true;
  s_disconnect_calls = 0;
  s_disconnect_had_callbacks = false;
  s_create_calls = 0;
  s_connect_calls = 0;
  s_destroy_calls = 0;
  s_last_server_uri[0] = '\0';
  s_last_had_ssl = false;
  s_last_enable_server_cert_auth = -1;
  s_last_verify = -1;
  s_last_private_key[0] = '\0';
  s_last_had_private_key = false;
  s_last_http_proxy[0] = '\0';
  s_last_https_proxy[0] = '\0';
  s_last_struct_version = -1;
  s_last_connect_timeout = -1;
  s_callback_context = NULL;
  s_message_arrived = NULL;
  s_connection_lost = NULL;
  s_send_calls = 0;
  s_send_rc = MQTTASYNC_SUCCESS;
  s_send_completes_at_once = false;
  memset(s_sends, 0, sizeof(s_sends));
  s_send_count = 0;
  memset(&s_last_send_response, 0, sizeof(s_last_send_response));
}

void mock_paho_set_create_rc(int rc) { s_create_rc = rc; }
void mock_paho_set_set_callbacks_rc(int rc) { s_set_callbacks_rc = rc; }
void mock_paho_set_connect_rc(int rc) { s_connect_rc = rc; }

void mock_paho_set_disconnect_rc(int rc) { s_disconnect_rc = rc; }
void mock_paho_set_disconnect_completion(bool success)
{
  s_disconnect_completion_success = success;
}
int mock_paho_disconnect_calls(void) { return s_disconnect_calls; }
bool mock_paho_disconnect_had_callbacks(void) { return s_disconnect_had_callbacks; }

int mock_paho_create_calls(void) { return s_create_calls; }
int mock_paho_connect_calls(void) { return s_connect_calls; }
int mock_paho_destroy_calls(void) { return s_destroy_calls; }

const char* mock_paho_last_server_uri(void)
{
  return s_last_server_uri[0] != '\0' ? s_last_server_uri : NULL;
}

bool mock_paho_last_connect_had_ssl(void) { return s_last_had_ssl; }
int mock_paho_last_enable_server_cert_auth(void) { return s_last_enable_server_cert_auth; }
int mock_paho_last_verify(void) { return s_last_verify; }

const char* mock_paho_last_private_key(void)
{
  return s_last_had_private_key ? s_last_private_key : NULL;
}

const char* mock_paho_last_http_proxy(void)
{
  return s_last_http_proxy[0] != '\0' ? s_last_http_proxy : NULL;
}

const char* mock_paho_last_https_proxy(void)
{
  return s_last_https_proxy[0] != '\0' ? s_last_https_proxy : NULL;
}

int mock_paho_last_connect_struct_version(void) { return s_last_struct_version; }
int mock_paho_last_connect_timeout(void) { return s_last_connect_timeout; }

/* ------------------------------------------------------------------------- */
/* MQTTAsync                                                                 */
/* ------------------------------------------------------------------------- */

int MQTTAsync_createWithOptions(
    MQTTAsync* handle,
    const char* serverURI,
    const char* clientId,
    int persistence_type,
    void* persistence_context,
    MQTTAsync_createOptions* options)
{
  (void)serverURI;
  (void)clientId;
  (void)persistence_type;
  (void)persistence_context;
  (void)options;
  ++s_create_calls;
  if (serverURI)
  {
    snprintf(s_last_server_uri, sizeof(s_last_server_uri), "%s", serverURI);
  }
  if (s_create_rc != MQTTASYNC_SUCCESS)
  {
    return s_create_rc;
  }
  if (handle)
  {
    *handle = &s_handle_token;
  }
  return MQTTASYNC_SUCCESS;
}

int MQTTAsync_setCallbacks(
    MQTTAsync handle,
    void* context,
    MQTTAsync_connectionLost* cl,
    MQTTAsync_messageArrived* ma,
    MQTTAsync_deliveryComplete* dc)
{
  (void)handle;
  (void)dc;
  s_callback_context = context;
  s_connection_lost = cl;
  s_message_arrived = ma;
  return s_set_callbacks_rc;
}

void* mock_paho_callback_context(void) { return s_callback_context; }

MQTTAsync_messageArrived* mock_paho_message_arrived(void) { return s_message_arrived; }

MQTTAsync_connectionLost* mock_paho_connection_lost(void) { return s_connection_lost; }

int MQTTAsync_connect(MQTTAsync handle, const MQTTAsync_connectOptions* options)
{
  (void)handle;
  ++s_connect_calls;
  s_last_had_ssl = options != NULL && options->ssl != NULL;
  if (s_last_had_ssl)
  {
    s_last_enable_server_cert_auth = options->ssl->enableServerCertAuth;
    s_last_verify = options->ssl->verify;
    s_last_had_private_key = options->ssl->privateKey != NULL;
    if (s_last_had_private_key)
    {
      snprintf(s_last_private_key, sizeof(s_last_private_key), "%s", options->ssl->privateKey);
    }
  }
  else
  {
    s_last_enable_server_cert_auth = -1;
    s_last_verify = -1;
    s_last_had_private_key = false;
  }
  s_last_struct_version = options != NULL ? options->struct_version : -1;
  s_last_connect_timeout = options != NULL ? options->connectTimeout : -1;
  s_last_http_proxy[0] = '\0';
  s_last_https_proxy[0] = '\0';
  if (options != NULL && options->httpProxy != NULL)
  {
    snprintf(s_last_http_proxy, sizeof(s_last_http_proxy), "%s", options->httpProxy);
  }
  if (options != NULL && options->httpsProxy != NULL)
  {
    snprintf(s_last_https_proxy, sizeof(s_last_https_proxy), "%s", options->httpsProxy);
  }
  return s_connect_rc;
}

int MQTTAsync_disconnect(MQTTAsync handle, const MQTTAsync_disconnectOptions* options)
{
  (void)handle;
  ++s_disconnect_calls;
  s_disconnect_had_callbacks = options != NULL && options->onSuccess != NULL
      && options->onFailure != NULL && options->context != NULL;

  if (s_disconnect_rc != MQTTASYNC_SUCCESS)
  {
    /* Refused: the real client runs no completion callback in this case. */
    return s_disconnect_rc;
  }
  if (options != NULL)
  {
    if (s_disconnect_completion_success && options->onSuccess)
    {
      MQTTAsync_successData data;
      memset(&data, 0, sizeof(data));
      options->onSuccess(options->context, &data);
    }
    else if (!s_disconnect_completion_success && options->onFailure)
    {
      MQTTAsync_failureData data;
      memset(&data, 0, sizeof(data));
      data.code = MQTTASYNC_FAILURE;
      data.message = "mock disconnect failure";
      options->onFailure(options->context, &data);
    }
  }
  return MQTTASYNC_SUCCESS;
}

int MQTTAsync_isConnected(MQTTAsync handle)
{
  (void)handle;
  /* MQTTAsync_connect only queues the attempt, so "not connected yet" is the
   * honest answer everywhere this suite looks. */
  return 0;
}

static void mock_paho_fail_pending_sends(int code);

void MQTTAsync_destroy(MQTTAsync* handle)
{
  ++s_destroy_calls;
  mock_paho_fail_pending_sends(MQTTASYNC_OPERATION_INCOMPLETE);
  if (handle)
  {
    *handle = NULL;
  }
}

int MQTTAsync_subscribe(
    MQTTAsync handle,
    const char* topic,
    int qos,
    MQTTAsync_responseOptions* response)
{
  (void)handle;
  (void)topic;
  (void)qos;
  (void)response;
  return MQTTASYNC_SUCCESS;
}

int MQTTAsync_unsubscribe(MQTTAsync handle, const char* topic, MQTTAsync_responseOptions* response)
{
  (void)handle;
  (void)topic;
  (void)response;
  return MQTTASYNC_SUCCESS;
}

/* The send a completion is for; aborts on a second completion, which Paho never makes. */
static mock_paho_send* mock_paho_send_complete(void* context)
{
  mock_paho_send* send = (mock_paho_send*)context;
  if (!send->pending)
  {
    fprintf(stderr, "mock_paho: a send was completed twice\n");
    abort();
  }
  send->pending = false;
  return send;
}

static void mock_paho_send_success(void* context, MQTTAsync_successData* data)
{
  mock_paho_send* send = mock_paho_send_complete(context);
  send->response.onSuccess(send->response.context, data);
}

static void mock_paho_send_failure(void* context, MQTTAsync_failureData* data)
{
  mock_paho_send* send = mock_paho_send_complete(context);
  send->response.onFailure(send->response.context, data);
}

/* Fails @p send with @p code and token 0, as Paho does for a write that fails. */
static void mock_paho_fail_send(mock_paho_send* send, int code)
{
  if (send->response.onFailure != NULL)
  {
    MQTTAsync_failureData data;
    memset(&data, 0, sizeof(data));
    data.code = code;
    mock_paho_send_failure(send, &data);
  }
  else
  {
    send->pending = false;
  }
}

static void mock_paho_fail_pending_sends(int code)
{
  for (int i = 0; i < s_send_count; ++i)
  {
    if (s_sends[i].pending)
    {
      mock_paho_fail_send(&s_sends[i], code);
    }
  }
  s_send_count = 0;
}

int MQTTAsync_sendMessage(
    MQTTAsync handle,
    const char* destinationName,
    const MQTTAsync_message* msg,
    MQTTAsync_responseOptions* response)
{
  ++s_send_calls;
  (void)handle;
  (void)destinationName;
  (void)msg;
  if (s_send_rc != MQTTASYNC_SUCCESS)
  {
    return s_send_rc;
  }
  if (response != NULL)
  {
    if (s_send_count == MOCK_PAHO_MAX_SENDS)
    {
      fprintf(
          stderr,
          "mock_paho: more than %d sends before MQTTAsync_destroy()\n",
          MOCK_PAHO_MAX_SENDS);
      abort();
    }
    /* Paho sets the token to the packet id it assigned. */
    response->token = s_send_calls;
    mock_paho_send* send = &s_sends[s_send_count++];
    send->response = *response;
    send->pending = true;
    /* What a test drives completes the send through the mock, which counts it. */
    s_last_send_response = *response;
    s_last_send_response.context = send;
    s_last_send_response.onSuccess = response->onSuccess ? mock_paho_send_success : NULL;
    s_last_send_response.onFailure = response->onFailure ? mock_paho_send_failure : NULL;
    if (s_send_completes_at_once)
    {
      /* As Paho's send thread may, before MQTTAsync_sendMessage() returns. */
      mock_paho_fail_send(send, MQTTASYNC_FAILURE);
    }
  }
  return MQTTASYNC_SUCCESS;
}

void MQTTAsync_free(void* ptr) { (void)ptr; }

/* Frees the struct only: tests allocate it with malloc() and point its payload
 * at storage they own. */
void MQTTAsync_freeMessage(MQTTAsync_message** msg)
{
  if (msg != NULL)
  {
    free(*msg);
    *msg = NULL;
  }
}

void MQTTAsync_setTraceLevel(enum MQTTASYNC_TRACE_LEVELS level) { (void)level; }

static MQTTAsync_traceCallback* s_trace_callback;

void MQTTAsync_setTraceCallback(MQTTAsync_traceCallback* callback) { s_trace_callback = callback; }

MQTTAsync_traceCallback* mock_paho_trace_callback(void) { return s_trace_callback; }

int mock_paho_send_calls(void) { return s_send_calls; }

void mock_paho_set_send_rc(int rc) { s_send_rc = rc; }

void mock_paho_set_send_completes_at_once(bool completes_at_once)
{
  s_send_completes_at_once = completes_at_once;
}

const MQTTAsync_responseOptions* mock_paho_last_send_response(void)
{
  return &s_last_send_response;
}
