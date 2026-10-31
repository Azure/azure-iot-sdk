// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_transport.c
 * @brief az_mqtt_transport_* calls, dispatched to the transport's az_mqtt_transport_vtable.
 */

#include <az_mqtt/az_mqtt_transport.h>

#include <azure/core/internal/az_precondition_internal.h>

AZ_NODISCARD az_result az_mqtt_transport_connect_start(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  _az_PRECONDITION_NOT_NULL(transport);
  return transport->vtable->connect_start(transport, host, port, tls_options);
}

AZ_NODISCARD az_result
az_mqtt_transport_connect_poll(az_mqtt_transport* transport, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(transport);
  return transport->vtable->connect_poll(transport, timeout_ms);
}

AZ_NODISCARD az_result az_mqtt_transport_connect(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  az_result rc = az_mqtt_transport_connect_start(transport, host, port, tls_options);
  if (az_result_succeeded(rc))
  {
    rc = az_mqtt_transport_connect_poll(transport, AZ_MQTT_TRANSPORT_CONNECT_TIMEOUT_MS);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      az_mqtt_transport_close(transport);
    }
  }
  return rc;
}

AZ_NODISCARD az_result az_mqtt_transport_send(az_mqtt_transport* transport, az_span data)
{
  _az_PRECONDITION_NOT_NULL(transport);
  return transport->vtable->send(transport, data);
}

AZ_NODISCARD az_result az_mqtt_transport_receive(
    az_mqtt_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received)
{
  _az_PRECONDITION_NOT_NULL(transport);
  return transport->vtable->receive(transport, buffer, timeout_ms, out_received);
}

void az_mqtt_transport_shutdown(az_mqtt_transport* transport)
{
  if (transport != NULL && transport->vtable->shutdown != NULL)
  {
    transport->vtable->shutdown(transport);
  }
}

void az_mqtt_transport_close(az_mqtt_transport* transport)
{
  if (transport != NULL)
  {
    transport->vtable->close(transport);
  }
}

AZ_NODISCARD az_result
az_mqtt_transport_set_proxy(az_mqtt_transport* transport, az_mqtt_proxy_options const* proxy)
{
  _az_PRECONDITION_NOT_NULL(transport);
  if (transport->vtable->set_proxy != NULL)
  {
    return transport->vtable->set_proxy(transport, proxy);
  }
  return proxy == NULL || az_span_size(proxy->host) == 0 ? AZ_OK : AZ_MQTT_ERROR_NOT_SUPPORTED;
}

void az_mqtt_transport_set_error_callback(
    az_mqtt_transport* transport,
    az_mqtt_transport_error_fn callback,
    void* context)
{
  _az_PRECONDITION_NOT_NULL(transport);
  if (transport->vtable->set_error_callback != NULL)
  {
    transport->vtable->set_error_callback(transport, callback, context);
  }
}

// ──────────────────────── Layers ─────────────────────────────

#include "az_mqtt_io_layers_internal.h"

static void _on_lower_error(az_mqtt_native_error const* error, void* context)
{
  _az_mqtt_io_layer_errors const* const errors = (_az_mqtt_io_layer_errors const*)context;
  if (errors->callback != NULL)
  {
    az_mqtt_native_error stamped = *error;
    stamped.connect_attempt = errors->connect_attempt;
    if (az_result_failed(errors->phase_result))
    {
      stamped.result = errors->phase_result;
    }
    errors->callback(&stamped, errors->context);
  }
}

void _az_mqtt_io_layer_errors_attach(_az_mqtt_io_layer_errors* errors, az_mqtt_transport* lower)
{
  az_mqtt_transport_set_error_callback(lower, _on_lower_error, errors);
}

void _az_mqtt_io_layer_report(
    _az_mqtt_io_layer_errors const* errors,
    az_mqtt_native_error_source source,
    int32_t code,
    az_result result)
{
  if (errors->callback != NULL)
  {
    az_mqtt_native_error const error = { source, code, result, errors->connect_attempt };
    errors->callback(&error, errors->context);
  }
}

az_result _az_mqtt_io_layer_send_some(
    _az_mqtt_io_layer* layer,
    az_span data,
    int32_t timeout_ms,
    int32_t* out_sent)
{
  return layer->ops->send_some(&layer->base, data, timeout_ms, out_sent);
}

int64_t _az_mqtt_io_layer_deadline(int32_t timeout_ms)
{
  return timeout_ms < 0 ? -1 : az_mqtt_transport_clock_ms() + timeout_ms;
}

int32_t _az_mqtt_io_layer_remaining(int64_t deadline_ms)
{
  if (deadline_ms < 0)
  {
    return -1;
  }
  int64_t const left = deadline_ms - az_mqtt_transport_clock_ms();
  return left <= 0 ? 0 : (left > INT32_MAX ? INT32_MAX : (int32_t)left);
}
