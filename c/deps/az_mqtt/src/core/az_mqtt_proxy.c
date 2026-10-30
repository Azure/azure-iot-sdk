// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_proxy.c
 * @brief Internal: HTTP CONNECT proxy layer over another transport (RFC 9110 §9.3.6).
 */

#include "az_mqtt_io_layers_internal.h"

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/internal/az_result_internal.h>

#include <string.h>

#ifndef AZ_MQTT_NO_PROXY

/** @brief _az_mqtt_proxy_transport.stage. */
enum
{
  _IDLE, ///< Not connecting.
  _LOWER, ///< The lower transport connects (to the proxy, or directly).
  _REQUEST, ///< Sending the CONNECT request (resumable).
  _REPLY, ///< CONNECT sent; reading the reply.
  _OPEN, ///< Tunnel (or direct connection) up.
};

#define _P(t) ((_az_mqtt_proxy_transport*)(t))
#define _LOWER_T(p) (&(p)->lower->base)

static az_result _connect_start(
    az_mqtt_transport* t,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  _az_mqtt_proxy_transport* const p = _P(t);
  p->errors.connect_attempt++;
  p->stage = _IDLE;
  p->stash_start = 0;
  p->stash_end = 0;
  p->active = p->proxy; // A started connect keeps its proxy.
  p->host = host;
  p->port = port;
  if (p->active != NULL)
  {
    // Refuse a host a request cannot carry before connecting.
    uint8_t request[_AZ_MQTT_HTTP_CONNECT_REQUEST_MAX];
    int32_t size = 0;
    az_result const rc = _az_mqtt_http_connect_request(
        p->active, host, port, AZ_SPAN_FROM_BUFFER(request), &size);
    az_span_fill(AZ_SPAN_FROM_BUFFER(request), 0); // It holds the credentials.
    if (az_result_failed(rc))
    {
      az_mqtt_transport_close(_LOWER_T(p));
      return rc;
    }
    host = p->active->host;
    port = p->active->port;
  }
  // TLS options pass down: a layer below that cannot honour them refuses them.
  _az_RETURN_IF_FAILED(az_mqtt_transport_connect_start(_LOWER_T(p), host, port, tls_options));
  p->sent = 0;
  p->stage = _LOWER;
  return AZ_OK;
}

/** @brief _az_mqtt_http_connect_send_request()'s context: where, and until when. */
typedef struct
{
  _az_mqtt_io_layer* lower;
  int64_t deadline_ms;
} _request_sink;

/** @brief _az_mqtt_http_connect_send_fn over the layer below, within the deadline. */
static int32_t _send_request_part(void* context, uint8_t const* data, int32_t size)
{
  _request_sink const* const sink = (_request_sink const*)context;
  int32_t sent = 0;
  az_result const rc = _az_mqtt_io_layer_send_some(
      sink->lower,
      az_span_create((uint8_t*)(uintptr_t)data, size),
      _az_mqtt_io_layer_remaining(sink->deadline_ms),
      &sent);
  return az_result_failed(rc) ? -1 : sent;
}

/**
 * @brief Send the rest of the CONNECT request until @p deadline_ms; resumable.
 * @retval AZ_MQTT_ERROR_TIMEOUT Not all sent yet.
 * @retval AZ_MQTT_ERROR_PROXY Failed below.
 */
static az_result _send_request(_az_mqtt_proxy_transport* p, int64_t deadline_ms)
{
  _request_sink sink = { p->lower, deadline_ms };
  return _az_mqtt_http_connect_send_request(
      p->active, p->host, p->port, &p->sent, _send_request_part, &sink);
}

/** @brief Read the reply until @p deadline_ms; what follows it is stashed for receive(). */
static az_result _read_reply(_az_mqtt_proxy_transport* p, int64_t deadline_ms)
{
  for (;;)
  {
    int32_t const wait_ms = _az_mqtt_io_layer_remaining(deadline_ms);
    az_span received;
    az_result rc = az_mqtt_transport_receive(
        _LOWER_T(p), AZ_SPAN_FROM_BUFFER(p->stash), wait_ms, &received);
    if (az_result_failed(rc))
    {
      return AZ_MQTT_ERROR_PROXY; // Closed or failed before the reply.
    }
    if (az_span_size(received) == 0)
    {
      if (wait_ms == 0)
      {
        return AZ_MQTT_ERROR_TIMEOUT;
      }
      continue;
    }
    int32_t consumed = 0;
    rc = _az_mqtt_http_connect_reply_parse(&p->reply, received, &consumed);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      continue;
    }
    if (az_result_failed(rc))
    {
      _az_mqtt_io_layer_report(&p->errors, AZ_MQTT_NATIVE_ERROR_PROXY, p->reply.status, rc);
      return rc;
    }
    p->stash_start = (uint16_t)consumed;
    p->stash_end = (uint16_t)az_span_size(received);
    return AZ_OK;
  }
}

static az_result _connect_poll(az_mqtt_transport* t, int32_t timeout_ms)
{
  _az_mqtt_proxy_transport* const p = _P(t);
  int64_t const deadline = _az_mqtt_io_layer_deadline(timeout_ms);
  if (p->stage == _OPEN || p->stage == _IDLE)
  {
    return p->stage == _OPEN ? AZ_OK : AZ_MQTT_ERROR_INVALID_STATE;
  }
  az_result rc = AZ_OK;
  if (p->stage == _LOWER)
  {
    rc = az_mqtt_transport_connect_poll(_LOWER_T(p), timeout_ms);
    if (az_result_failed(rc))
    {
      return rc; // Not yet, or failed (and closed).
    }
    if (p->active == NULL)
    {
      p->stage = _OPEN;
      return AZ_OK;
    }
    _az_mqtt_http_reply_init(&p->reply);
    p->stage = _REQUEST;
  }
  p->errors.phase_result = AZ_MQTT_ERROR_PROXY; // The tunnel's socket errors are the proxy's.
  if (p->stage == _REQUEST)
  {
    rc = _send_request(p, deadline);
    if (az_result_succeeded(rc))
    {
      p->stage = _REPLY;
    }
  }
  if (az_result_succeeded(rc) && p->stage == _REPLY)
  {
    rc = _read_reply(p, deadline);
  }
  p->errors.phase_result = AZ_OK;
  if (rc == AZ_MQTT_ERROR_TIMEOUT)
  {
    return rc;
  }
  if (az_result_failed(rc))
  {
    p->stage = _IDLE;
    az_mqtt_transport_close(_LOWER_T(p));
    return rc;
  }
  p->stage = _OPEN;
  return AZ_OK;
}

static az_result _send(az_mqtt_transport* t, az_span data)
{
  return _P(t)->stage == _OPEN ? az_mqtt_transport_send(_LOWER_T(_P(t)), data)
                               : AZ_MQTT_ERROR_TRANSPORT;
}

static az_result _send_some(
    az_mqtt_transport* t,
    az_span data,
    int32_t timeout_ms,
    int32_t* out_sent)
{
  *out_sent = 0;
  return _P(t)->stage == _OPEN
      ? _az_mqtt_io_layer_send_some(_P(t)->lower, data, timeout_ms, out_sent)
      : AZ_MQTT_ERROR_TRANSPORT;
}

static az_result
_receive(az_mqtt_transport* t, az_span buffer, int32_t timeout_ms, az_span* out_received)
{
  _az_mqtt_proxy_transport* const p = _P(t);
  *out_received = AZ_SPAN_EMPTY;
  if (p->stage != _OPEN)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  if (p->stash_start < p->stash_end)
  {
    int32_t const stashed = p->stash_end - p->stash_start;
    int32_t const n = stashed < az_span_size(buffer) ? stashed : az_span_size(buffer);
    memcpy(az_span_ptr(buffer), p->stash + p->stash_start, (size_t)n);
    p->stash_start = (uint16_t)(p->stash_start + n);
    *out_received = az_span_slice(buffer, 0, n);
    return AZ_OK;
  }
  return az_mqtt_transport_receive(_LOWER_T(p), buffer, timeout_ms, out_received);
}

static void _shutdown(az_mqtt_transport* t) { az_mqtt_transport_shutdown(_LOWER_T(_P(t))); }

static void _close(az_mqtt_transport* t)
{
  _az_mqtt_proxy_transport* const p = _P(t);
  p->stage = _IDLE;
  p->stash_start = 0;
  p->stash_end = 0;
  az_mqtt_transport_close(_LOWER_T(p));
}

static az_result _set_proxy(az_mqtt_transport* t, az_mqtt_proxy_options const* proxy)
{
  _az_RETURN_IF_FAILED(_az_mqtt_http_connect_check(proxy));
  _P(t)->proxy = proxy != NULL && az_span_size(proxy->host) > 0 ? proxy : NULL;
  return AZ_OK;
}

static void
_set_error_callback(az_mqtt_transport* t, az_mqtt_transport_error_fn callback, void* context)
{
  _P(t)->errors.callback = callback;
  _P(t)->errors.context = context;
}

static az_mqtt_transport_vtable const _vtable = {
  _connect_start, _connect_poll, _send,       _receive,
  _shutdown,      _close,        _set_proxy, _set_error_callback,
};

static _az_mqtt_io_layer_ops const _ops = { _send_some };

az_result _az_mqtt_proxy_transport_init(_az_mqtt_proxy_transport* proxy, _az_mqtt_io_layer* lower)
{
  _az_PRECONDITION_NOT_NULL(proxy);
  _az_PRECONDITION_NOT_NULL(lower);
  memset(proxy, 0, sizeof(*proxy));
  proxy->layer.base.vtable = &_vtable;
  proxy->layer.ops = &_ops;
  proxy->lower = lower;
  _az_mqtt_io_layer_errors_attach(&proxy->errors, &lower->base);
  return AZ_OK;
}

#endif // AZ_MQTT_NO_PROXY
