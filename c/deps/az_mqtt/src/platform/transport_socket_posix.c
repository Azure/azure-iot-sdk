// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file transport_socket_posix.c
 * @brief Internal: POSIX TCP transport, the bottom of the platform transport.
 */

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE // recv, ssize_t under -std=c99
#endif

#include "az_mqtt_io_layers_internal.h"
#include "az_mqtt_socket_posix.h"

#include <azure/core/internal/az_precondition_internal.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
  _az_mqtt_io_layer layer; ///< Must be first.
  int fd;
  _az_mqtt_tcp_connect tcp;
  _az_mqtt_error_sink errors;
  bool connecting;
  bool connected;
} _socket_transport;

#define _S(t) ((_socket_transport*)(t))

static void _close(az_mqtt_transport* t)
{
  _socket_transport* const s = _S(t);
  _az_mqtt_tcp_connect_cancel(&s->tcp);
  if (s->fd >= 0)
  {
    close(s->fd);
    s->fd = -1;
  }
  s->connecting = false;
  s->connected = false;
}

static az_result _connect_start(
    az_mqtt_transport* t,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  _socket_transport* const s = _S(t);
  _close(t);
  s->errors.connect_attempt++;
  if (tls_options != NULL)
  {
    return AZ_MQTT_ERROR_NOT_SUPPORTED; // No TLS here: never fall back to plaintext.
  }
  az_result const rc = _az_mqtt_tcp_connect_start(&s->tcp, host, port, &s->errors);
  if (az_result_failed(rc))
  {
    _close(t);
    return rc;
  }
  s->connecting = true;
  return AZ_OK;
}

static az_result _connect_poll(az_mqtt_transport* t, int32_t timeout_ms)
{
  _socket_transport* const s = _S(t);
  if (!s->connecting)
  {
    return s->connected ? AZ_OK : AZ_MQTT_ERROR_INVALID_STATE;
  }
  az_result const rc = _az_mqtt_tcp_connect_poll(&s->tcp, timeout_ms, &s->errors);
  if (rc == AZ_MQTT_ERROR_TIMEOUT)
  {
    return rc;
  }
  if (az_result_failed(rc))
  {
    _close(t);
    return rc;
  }
  s->fd = s->tcp.fd;
  _az_mqtt_tcp_connect_init(&s->tcp); // Handed over.
  s->connecting = false;
  s->connected = true;
  return AZ_OK;
}

static az_result _send_some(
    az_mqtt_transport* t,
    az_span data,
    int32_t timeout_ms,
    int32_t* out_sent)
{
  _socket_transport* const s = _S(t);
  *out_sent = 0;
  if (!s->connected)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  if (az_span_size(data) == 0)
  {
    return AZ_OK;
  }
  int64_t const deadline = _az_mqtt_deadline(timeout_ms);
  for (;;)
  {
    int32_t const n = _az_mqtt_send_nosignal(s->fd, az_span_ptr(data), az_span_size(data));
    if (n > 0)
    {
      *out_sent = n;
      return AZ_OK;
    }
    int const w = n < 0
        ? -1
        : _az_mqtt_wait_fd(s->fd, _AZ_MQTT_WAIT_WRITE, _az_mqtt_remaining_ms(deadline));
    if (w == 0)
    {
      return AZ_OK; // Nothing could be sent in time.
    }
    if (w < 0)
    {
      s->connected = false;
      return _az_mqtt_socket_error(errno, &s->errors);
    }
  }
}

static az_result _send(az_mqtt_transport* t, az_span data)
{
  int64_t const deadline = _az_mqtt_deadline(AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS);
  while (az_span_size(data) > 0)
  {
    int32_t sent = 0;
    az_result const rc = _send_some(t, data, _az_mqtt_remaining_ms(deadline), &sent);
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (sent == 0)
    {
      _S(t)->connected = false; // A partial packet may be on the wire: unusable.
      return AZ_MQTT_ERROR_TIMEOUT;
    }
    data = az_span_slice_to_end(data, sent);
  }
  return AZ_OK;
}

static az_result
_receive(az_mqtt_transport* t, az_span buffer, int32_t timeout_ms, az_span* out_received)
{
  _socket_transport* const s = _S(t);
  *out_received = AZ_SPAN_EMPTY;
  if (!s->connected)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  int64_t const deadline = _az_mqtt_deadline(timeout_ms);
  for (;;)
  {
    int w = _az_mqtt_wait_fd(s->fd, _AZ_MQTT_WAIT_READ, _az_mqtt_remaining_ms(deadline));
    if (w > 0)
    {
      int32_t const n = _az_mqtt_recv_nonblocking(s->fd, az_span_ptr(buffer), az_span_size(buffer));
      if (n > 0)
      {
        *out_received = az_span_slice(buffer, 0, n);
        return AZ_OK;
      }
      w = n < 0 ? -1 : 1;
    }
    if (w == 0)
    {
      return AZ_OK; // Timed out: out_received stays empty.
    }
    if (w < 0)
    {
      s->connected = false;
      return _az_mqtt_socket_error(errno, &s->errors);
    }
  }
}

static void
_set_error_callback(az_mqtt_transport* t, az_mqtt_transport_error_fn callback, void* context)
{
  _S(t)->errors.callback = callback;
  _S(t)->errors.context = context;
}

static az_mqtt_transport_vtable const _vtable = {
  _connect_start, _connect_poll, _send, _receive, NULL, _close, NULL, _set_error_callback,
};

static _az_mqtt_io_layer_ops const _ops = { _send_some };

int32_t _az_mqtt_socket_transport_sizeof(void) { return (int32_t)sizeof(_socket_transport); }

az_result _az_mqtt_socket_transport_init(_az_mqtt_io_layer* storage)
{
  _az_PRECONDITION_NOT_NULL(storage);
  _socket_transport* const s = (_socket_transport*)storage;
  memset(s, 0, sizeof(*s));
  s->layer.base.vtable = &_vtable;
  s->layer.ops = &_ops;
  s->fd = -1;
  _az_mqtt_tcp_connect_init(&s->tcp);
  return AZ_OK;
}
