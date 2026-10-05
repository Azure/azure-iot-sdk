// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter: what the MQTT 3.1.1 and MQTT 5 builds share, and the factory destructor. */

#include "az_iot_mqtt_az_mqtt_internal.h"

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"

#include <az_mqtt/az_mqtt_types.h>

#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_log_components.h"

#if defined(AZ_IOT_AZ_MQTT_OPENSSL)
#include <openssl/err.h>
#include <openssl/provider.h>
#endif

#include <stdlib.h>
#include <string.h>

char* az_iot_az_mqtt_strdup(const char* s)
{
  if (s == NULL)
  {
    return NULL;
  }
  size_t const n = strlen(s) + 1;
  char* copy = (char*)malloc(n);
  if (copy != NULL)
  {
    memcpy(copy, s, n);
  }
  return copy;
}

bool az_iot_az_mqtt_has_text(const char* s) { return s != NULL && s[0] != '\0'; }

az_iot_result az_iot_az_mqtt_request_result(az_result rc)
{
  if (az_result_succeeded(rc))
  {
    return AZ_IOT_OK;
  }
  if (rc == AZ_MQTT_ERROR_NOT_CONNECTED || rc == AZ_MQTT_ERROR_INVALID_STATE)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  // Full for now: no free in-flight entry, Receive Maximum reached, or no room for the copy.
  if (rc == AZ_MQTT_ERROR_FLOW_CONTROL || rc == AZ_MQTT_ERROR_OUT_OF_STORAGE)
  {
    return AZ_IOT_ERR_BUSY;
  }
  // AZ_ERROR_NOT_ENOUGH_SPACE: the packet does not fit the send buffer.
  if (rc == AZ_MQTT_ERROR_BUFFER_TOO_SMALL || rc == AZ_MQTT_ERROR_PACKET_TOO_LARGE
      || rc == AZ_ERROR_NOT_ENOUGH_SPACE)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  if (rc == AZ_MQTT_ERROR_INVALID_CONFIG || rc == AZ_ERROR_ARG)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (rc == AZ_MQTT_ERROR_NOT_SUPPORTED)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_ERR_MQTT;
}

az_iot_result az_iot_az_mqtt_session_result(az_result rc)
{
  if (az_result_succeeded(rc))
  {
    return AZ_IOT_OK;
  }
  if (rc == AZ_MQTT_ERROR_TLS_HANDSHAKE || rc == AZ_MQTT_ERROR_TLS_VERIFY)
  {
    return AZ_IOT_ERR_TLS;
  }
  if (rc == AZ_MQTT_ERROR_TIMEOUT || rc == AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT)
  {
    return AZ_IOT_ERR_TIMEOUT;
  }
  if (rc == AZ_MQTT_ERROR_NOT_SUPPORTED)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  // A CONNECT or an inbound packet larger than the buffers.
  if (rc == AZ_ERROR_NOT_ENOUGH_SPACE)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  // A CONNECT the encoder refuses (e.g. a field over 65,535 bytes).
  if (rc == AZ_MQTT_ERROR_INVALID_CONFIG || rc == AZ_ERROR_ARG)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return AZ_IOT_ERR_MQTT;
}

az_iot_result az_iot_az_mqtt_load_key_provider(const char* name)
{
#if defined(AZ_IOT_AZ_MQTT_OPENSSL)
  // Process-wide and never unloaded (as in the Paho adapter): loaded once, then reused, so
  // reconnects take no further reference, and keys of other sessions stay valid.
  if (OSSL_PROVIDER_available(NULL, name))
  {
    return AZ_IOT_OK;
  }
  if (OSSL_PROVIDER_try_load(NULL, name, 1) != NULL)
  {
    AZ_IOT_LOG_INFOF(AZ_IOT_LOG_COMPONENT_AZ_MQTT, "loaded OpenSSL provider '%s'", name);
    return AZ_IOT_OK;
  }
  ERR_clear_error();
  return AZ_IOT_ERR_NOT_SUPPORTED;
#else
  (void)name;
  return AZ_IOT_ERR_NOT_SUPPORTED;
#endif
}

const char* az_iot_az_mqtt_string_writer_add(az_iot_az_mqtt_string_writer* w, az_span s)
{
  size_t const n = (size_t)az_span_size(s);
  if ((size_t)(w->end - w->next) < n + 1)
  {
    return NULL;
  }
  char* out = w->next;
  if (n > 0)
  {
    memcpy(out, az_span_ptr(s), n);
  }
  out[n] = '\0';
  w->next += n + 1;
  return out;
}

void az_iot_az_mqtt_factory_free(void* factory_ctx) { free(factory_ctx); }

void az_iot_az_mqtt_factory_destroy(az_iot_mqtt_factory* factory) { free(factory); }
