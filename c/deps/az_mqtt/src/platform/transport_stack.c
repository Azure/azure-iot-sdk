// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file transport_stack.c
 * @brief The platform transport, stacked in the caller's storage, top first:
 * TLS (if a backend is built), HTTP CONNECT proxy (unless AZ_MQTT_NO_PROXY), socket.
 */

#include "az_mqtt_io_layers_internal.h"

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/internal/az_result_internal.h>

/** @brief Offset alignment of each layer in the storage. */
#define _ALIGN(size) ((((size) + 15) / 16) * 16)

#ifdef AZ_MQTT_NO_PROXY
#define _PROXY_SIZE 0
#else
#define _PROXY_SIZE _ALIGN((int32_t)sizeof(_az_mqtt_proxy_transport))
#endif

AZ_NODISCARD int32_t az_mqtt_transport_sizeof(void)
{
  return _ALIGN(_az_mqtt_tls_transport_sizeof()) + _PROXY_SIZE
      + _ALIGN(_az_mqtt_socket_transport_sizeof());
}

AZ_NODISCARD az_result az_mqtt_transport_init(az_mqtt_transport* transport)
{
  _az_PRECONDITION_NOT_NULL(transport);
  uint8_t* const storage = (uint8_t*)transport;
  int32_t const tls_size = _ALIGN(_az_mqtt_tls_transport_sizeof());
  _az_mqtt_io_layer* const socket = (_az_mqtt_io_layer*)(void*)(storage + tls_size + _PROXY_SIZE);
  _az_RETURN_IF_FAILED(_az_mqtt_socket_transport_init(socket));
  _az_mqtt_io_layer* lower = socket;
#ifndef AZ_MQTT_NO_PROXY
  _az_mqtt_proxy_transport* const proxy = (_az_mqtt_proxy_transport*)(void*)(storage + tls_size);
  _az_RETURN_IF_FAILED(_az_mqtt_proxy_transport_init(proxy, lower));
  lower = &proxy->layer;
#endif
  if (tls_size > 0)
  {
    _az_RETURN_IF_FAILED(_az_mqtt_tls_transport_init(transport, lower));
  }
  return AZ_OK;
}

#if !defined(AZ_MQTT_TLS_OPENSSL) && !defined(AZ_MQTT_TLS_MBEDTLS) && !defined(AZ_MQTT_TLS_SCHANNEL)
// No TLS backend: no TLS layer; the socket refuses TLS options.
int32_t _az_mqtt_tls_transport_sizeof(void) { return 0; }

az_result _az_mqtt_tls_transport_init(az_mqtt_transport* transport, _az_mqtt_io_layer* lower)
{
  (void)transport;
  (void)lower;
  return AZ_MQTT_ERROR_NOT_SUPPORTED;
}
#endif
