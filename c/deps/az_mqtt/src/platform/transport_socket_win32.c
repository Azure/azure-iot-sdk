// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file transport_socket_win32.c
 * @brief Internal: Winsock TCP transport, the bottom of the platform transport; also the
 * platform clock and random source.
 */

#include "az_mqtt_io_layers_internal.h"

#include <azure/core/internal/az_precondition_internal.h>

#include <stdio.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <bcrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>

typedef struct
{
  _az_mqtt_io_layer layer; ///< Must be first.
  SOCKET fd;
  _az_mqtt_io_layer_errors errors;
  /** @brief While connecting: resolved addresses, the next to try, when the current started. */
  struct addrinfo* addresses;
  struct addrinfo* next_address;
  int64_t attempt_start_ms;
  /** @brief WSA error of the last address that failed (each is reported as it fails). */
  int last_error;
  bool connecting;
  bool connected;
} _socket_transport;

#define _S(t) ((_socket_transport*)(t))

AZ_NODISCARD int64_t az_mqtt_transport_clock_ms(void) { return (int64_t)GetTickCount64(); }

AZ_NODISCARD az_result az_mqtt_transport_random(az_span buffer)
{
  return BCryptGenRandom(
             NULL,
             az_span_ptr(buffer),
             (ULONG)az_span_size(buffer),
             BCRYPT_USE_SYSTEM_PREFERRED_RNG)
          == 0
      ? AZ_OK
      : AZ_MQTT_ERROR_TRANSPORT;
}

/**
 * @brief The result for WSA error @p err (0: orderly close): AZ_MQTT_ERROR_CONNECTION_CLOSED,
 * AZ_MQTT_ERROR_CONNECTION_REFUSED or AZ_MQTT_ERROR_TRANSPORT.
 */
static az_result _wsa_result(int err)
{
  switch (err)
  {
    case 0:
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENOTCONN:
    case WSAESHUTDOWN:
      return AZ_MQTT_ERROR_CONNECTION_CLOSED;
    case WSAECONNREFUSED:
      return AZ_MQTT_ERROR_CONNECTION_REFUSED;
    default:
      return AZ_MQTT_ERROR_TRANSPORT;
  }
}

/** @brief _wsa_result() of @p err, reported unless @p err is 0. */
static az_result _socket_error(_socket_transport* s, int err)
{
  az_result const rc = _wsa_result(err);
  if (err != 0)
  {
    _az_mqtt_io_layer_report(&s->errors, AZ_MQTT_NATIVE_ERROR_SOCKET, err, rc);
  }
  return rc;
}

/** @brief The connection failed with WSA error @p err (0: closed by the peer): unusable. */
static az_result _fail(_socket_transport* s, int err)
{
  s->connected = false;
  return _socket_error(s, err);
}

/**
 * @brief Wait up to @p timeout_ms (-1: no limit) for @p fd to be readable, or writable (or
 * failed) if @p write.
 * @return 1 ready, 0 timed out, -1 failed (WSAGetLastError()).
 */
static int _wait(SOCKET fd, bool write, int32_t timeout_ms)
{
  fd_set fds;
  fd_set error_fds;
  FD_ZERO(&fds);
  FD_ZERO(&error_fds);
  FD_SET(fd, &fds);
  FD_SET(fd, &error_fds);
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  int const sel = select(
      0, // Ignored on Windows.
      write ? NULL : &fds,
      write ? &fds : NULL,
      write ? &error_fds : NULL,
      timeout_ms < 0 ? NULL : &tv);
  return sel < 0 ? -1 : (sel > 0 ? 1 : 0);
}

static void _release_addresses(_socket_transport* s)
{
  if (s->addresses != NULL)
  {
    freeaddrinfo(s->addresses);
  }
  s->addresses = NULL;
  s->next_address = NULL;
}

static void _close(az_mqtt_transport* t)
{
  _socket_transport* const s = _S(t);
  if (s->fd != INVALID_SOCKET)
  {
    closesocket(s->fd);
    s->fd = INVALID_SOCKET;
  }
  _release_addresses(s);
  s->connecting = false;
  s->connected = false;
}

/** @brief Record and report the failure of the current address (WSA error @p err). */
static void _fail_address(_socket_transport* s, int err)
{
  s->last_error = err;
  _socket_error(s, err);
}

/** @brief Start a non-blocking connect to the next address that accepts one. */
static az_result _connect_next(_socket_transport* s)
{
  while (s->next_address != NULL)
  {
    struct addrinfo* const a = s->next_address;
    s->next_address = a->ai_next;
    SOCKET const fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd == INVALID_SOCKET)
    {
      _fail_address(s, WSAGetLastError());
      continue;
    }
    u_long non_blocking = 1;
    if (ioctlsocket(fd, FIONBIO, &non_blocking) == 0
        && (connect(fd, a->ai_addr, (int)a->ai_addrlen) == 0
            || WSAGetLastError() == WSAEWOULDBLOCK))
    {
      s->fd = fd;
      s->attempt_start_ms = az_mqtt_transport_clock_ms();
      return AZ_OK;
    }
    _fail_address(s, WSAGetLastError());
    closesocket(fd);
  }
  return AZ_MQTT_ERROR_TRANSPORT;
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

  char host_str[256];
  if (az_span_size(host) >= (int32_t)sizeof(host_str))
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  memcpy(host_str, az_span_ptr(host), (size_t)az_span_size(host));
  host_str[az_span_size(host)] = '\0';
  char port_str[6];
  (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int const resolved = getaddrinfo(host_str, port_str, &hints, &s->addresses);
  if (resolved != 0 || s->addresses == NULL)
  {
    _release_addresses(s);
    _az_mqtt_io_layer_report(
        &s->errors, AZ_MQTT_NATIVE_ERROR_NAME_RESOLUTION, resolved, AZ_MQTT_ERROR_NAME_RESOLUTION);
    return AZ_MQTT_ERROR_NAME_RESOLUTION;
  }
  s->next_address = s->addresses;
  if (az_result_failed(_connect_next(s)))
  {
    _close(t);
    return _wsa_result(s->last_error);
  }
  s->connecting = true;
  return AZ_OK;
}

/**
 * @brief Wait up to @p timeout_ms for the connect. An address that neither connects nor fails
 * within AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS is abandoned while others remain.
 */
static az_result _connect_poll(az_mqtt_transport* t, int32_t timeout_ms)
{
  _socket_transport* const s = _S(t);
  if (!s->connecting)
  {
    return s->connected ? AZ_OK : AZ_MQTT_ERROR_INVALID_STATE;
  }
  int64_t const deadline = _az_mqtt_io_layer_deadline(timeout_ms);
  while (s->fd != INVALID_SOCKET)
  {
    int32_t wait_ms = _az_mqtt_io_layer_remaining(deadline);
    bool const bounded_attempt = s->next_address != NULL;
    if (bounded_attempt)
    {
      int32_t const attempt_left
          = _az_mqtt_io_layer_remaining(s->attempt_start_ms + AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS);
      if (wait_ms < 0 || attempt_left < wait_ms)
      {
        wait_ms = attempt_left;
      }
    }
    int const w = _wait(s->fd, true, wait_ms);
    if (w == 0
        && !(
            bounded_attempt
            && az_mqtt_transport_clock_ms() - s->attempt_start_ms
                >= AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS))
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }
    int err = 0;
    int len = (int)sizeof(err);
    if (w > 0 && getsockopt(s->fd, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0)
    {
      _release_addresses(s); // The socket stays non-blocking.
      s->connecting = false;
      s->connected = true;
      return AZ_OK;
    }
    // Failed, or its attempt budget ran out: move on to the next address.
    _fail_address(
        s,
        w < 0 ? WSAGetLastError() : (w == 0 ? WSAETIMEDOUT : (err != 0 ? err : WSAGetLastError())));
    closesocket(s->fd);
    s->fd = INVALID_SOCKET;
    if (az_result_failed(_connect_next(s)))
    {
      break;
    }
  }
  az_result const rc = _wsa_result(s->last_error);
  _close(t);
  return rc;
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
  int64_t const deadline = _az_mqtt_io_layer_deadline(timeout_ms);
  for (;;)
  {
    int const n = send(s->fd, (char const*)az_span_ptr(data), (int)az_span_size(data), 0);
    if (n > 0)
    {
      *out_sent = n;
      return AZ_OK;
    }
    int const err = WSAGetLastError();
    if (n < 0 && err != WSAEWOULDBLOCK)
    {
      return _fail(s, err);
    }
    int const w = _wait(s->fd, true, _az_mqtt_io_layer_remaining(deadline));
    if (w == 0)
    {
      return AZ_OK; // Nothing could be sent in time.
    }
    if (w < 0)
    {
      return _fail(s, WSAGetLastError());
    }
  }
}

static az_result _send(az_mqtt_transport* t, az_span data)
{
  int64_t const deadline = _az_mqtt_io_layer_deadline(AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS);
  while (az_span_size(data) > 0)
  {
    int32_t sent = 0;
    az_result const rc = _send_some(t, data, _az_mqtt_io_layer_remaining(deadline), &sent);
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

static az_result _receive(
    az_mqtt_transport* t,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received)
{
  _socket_transport* const s = _S(t);
  *out_received = AZ_SPAN_EMPTY;
  if (!s->connected)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  int64_t const deadline = _az_mqtt_io_layer_deadline(timeout_ms);
  for (;;)
  {
    int const w = _wait(s->fd, false, _az_mqtt_io_layer_remaining(deadline));
    if (w == 0)
    {
      return AZ_OK; // Timed out: out_received stays empty.
    }
    if (w < 0)
    {
      return _fail(s, WSAGetLastError());
    }
    int const n = recv(s->fd, (char*)az_span_ptr(buffer), (int)az_span_size(buffer), 0);
    if (n > 0)
    {
      *out_received = az_span_slice(buffer, 0, n);
      return AZ_OK;
    }
    int const err = n == 0 ? 0 : WSAGetLastError(); // 0: orderly close.
    if (err != WSAEWOULDBLOCK)
    {
      return _fail(s, err);
    }
  }
}

static void _set_error_callback(
    az_mqtt_transport* t,
    az_mqtt_transport_error_fn callback,
    void* context)
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
  static bool wsa_started = false;
  if (!wsa_started)
  {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
      return AZ_MQTT_ERROR_TRANSPORT;
    }
    wsa_started = true;
  }
  _socket_transport* const s = (_socket_transport*)storage;
  memset(s, 0, sizeof(*s));
  s->layer.base.vtable = &_vtable;
  s->layer.ops = &_ops;
  s->fd = INVALID_SOCKET;
  return AZ_OK;
}
