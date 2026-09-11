// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Implementation of the MQTTAsync stand-in. See mock_paho_async.h. */

#include "mock_paho_async.h"

#include <MQTTProperties.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The adapter only ever stores the handle and hands it back to us, so any
 * non-NULL address will do. */
static int s_handle_token;

static int s_create_rc;
static int s_set_callbacks_rc;
static int s_set_disconnected_rc;
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

void mock_paho_reset(void)
{
  s_create_rc = MQTTASYNC_SUCCESS;
  s_set_callbacks_rc = MQTTASYNC_SUCCESS;
  s_set_disconnected_rc = MQTTASYNC_SUCCESS;
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
}

void mock_paho_set_create_rc(int rc) { s_create_rc = rc; }
void mock_paho_set_set_callbacks_rc(int rc) { s_set_callbacks_rc = rc; }
void mock_paho_set_set_disconnected_rc(int rc) { s_set_disconnected_rc = rc; }
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
  (void)context;
  (void)cl;
  (void)ma;
  (void)dc;
  return s_set_callbacks_rc;
}

int MQTTAsync_setDisconnected(MQTTAsync handle, void* context, MQTTAsync_disconnected* co)
{
  (void)handle;
  (void)context;
  (void)co;
  return s_set_disconnected_rc;
}

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

void MQTTAsync_destroy(MQTTAsync* handle)
{
  ++s_destroy_calls;
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

int MQTTAsync_sendMessage(
    MQTTAsync handle,
    const char* destinationName,
    const MQTTAsync_message* msg,
    MQTTAsync_responseOptions* response)
{
  (void)handle;
  (void)destinationName;
  (void)msg;
  (void)response;
  return MQTTASYNC_SUCCESS;
}

void MQTTAsync_free(void* ptr) { (void)ptr; }

void MQTTAsync_freeMessage(MQTTAsync_message** msg) { (void)msg; }

void MQTTAsync_setTraceLevel(enum MQTTASYNC_TRACE_LEVELS level) { (void)level; }

void MQTTAsync_setTraceCallback(MQTTAsync_traceCallback* callback) { (void)callback; }

/* ------------------------------------------------------------------------- */
/* MQTTProperties. Reached only by the v5 branch of connect(); the adapter    */
/* just builds and frees them, so accepting and discarding is enough.         */
/* ------------------------------------------------------------------------- */

int MQTTProperties_add(MQTTProperties* props, const MQTTProperty* prop)
{
  (void)props;
  (void)prop;
  return 0;
}

void MQTTProperties_free(MQTTProperties* properties) { (void)properties; }

int MQTTProperties_propertyCount(MQTTProperties* props, enum MQTTPropertyCodes propid)
{
  (void)props;
  (void)propid;
  return 0;
}

MQTTProperty* MQTTProperties_getPropertyAt(
    MQTTProperties* props,
    enum MQTTPropertyCodes propid,
    int index)
{
  (void)props;
  (void)propid;
  (void)index;
  return NULL;
}
