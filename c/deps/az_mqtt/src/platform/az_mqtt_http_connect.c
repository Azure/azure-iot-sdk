// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "az_mqtt_http_connect.h"

#include <azure/core/az_base64.h>

#include <stdbool.h>

#ifndef AZ_MQTT_NO_PROXY
/** @brief Whether @p text has a byte that would end or split an HTTP header line. */
static bool _has_line_break(az_span text)
{
  uint8_t const* p = az_span_ptr(text);
  for (int32_t i = 0; i < az_span_size(text); i++)
  {
    if (p[i] == '\r' || p[i] == '\n' || p[i] == '\0')
    {
      return true;
    }
  }
  return false;
}

/** @brief Whether @p host can go in a request line: non-empty, bounded, no control or space. */
static bool _is_valid_host(az_span host)
{
  uint8_t const* const p = az_span_ptr(host);
  int32_t const size = az_span_size(host);
  if (size == 0 || size > AZ_MQTT_PROXY_HOST_MAX)
  {
    return false;
  }
  for (int32_t i = 0; i < size; i++)
  {
    if (p[i] <= ' ' || p[i] == 0x7F)
    {
      return false;
    }
  }
  return true;
}

az_result _az_mqtt_http_connect_check(az_mqtt_proxy_options const* proxy)
{
  if (proxy == NULL || az_span_size(proxy->host) == 0)
  {
    return AZ_OK;
  }
  int32_t const user_size = az_span_size(proxy->username);
  int32_t const password_size = az_span_size(proxy->password);
  if (proxy->port == 0 || !_is_valid_host(proxy->host)
      || user_size > AZ_MQTT_PROXY_CREDENTIALS_MAX // Separate checks: the sum may overflow.
      || password_size > AZ_MQTT_PROXY_CREDENTIALS_MAX - user_size
      || _has_line_break(proxy->username) || _has_line_break(proxy->password)
      || az_span_find(proxy->username, AZ_SPAN_FROM_STR(":")) >= 0 // RFC 7617 §2
      || (user_size == 0 && password_size > 0))
  {
    return AZ_MQTT_ERROR_INVALID_CONFIG;
  }
  return AZ_OK;
}

/** @brief Copy @p text to @p *rest; false if it does not fit. */
static bool _append(az_span* rest, az_span text)
{
  if (az_span_size(*rest) < az_span_size(text))
  {
    return false;
  }
  *rest = az_span_copy(*rest, text);
  return true;
}

/** @brief "host:port", bracketing an IPv6 literal. */
static bool _append_authority(az_span* rest, az_span host, uint16_t port)
{
  bool const ipv6 = az_span_find(host, AZ_SPAN_FROM_STR(":")) >= 0;
  bool ok = (!ipv6 || _append(rest, AZ_SPAN_FROM_STR("["))) && _append(rest, host)
      && (!ipv6 || _append(rest, AZ_SPAN_FROM_STR("]"))) && _append(rest, AZ_SPAN_FROM_STR(":"));
  return ok && az_result_succeeded(az_span_u32toa(*rest, port, rest));
}

/** @brief "Proxy-Authorization: Basic <base64(user:password)>\r\n". */
static bool _append_credentials(az_span* rest, az_mqtt_proxy_options const* proxy)
{
  uint8_t plain[AZ_MQTT_PROXY_CREDENTIALS_MAX + 1];
  az_span joined = AZ_SPAN_FROM_BUFFER(plain);
  joined = az_span_copy(joined, proxy->username);
  joined = az_span_copy_u8(joined, ':');
  joined = az_span_copy(joined, proxy->password);
  az_span const user_pass
      = az_span_slice(AZ_SPAN_FROM_BUFFER(plain), 0, (int32_t)sizeof(plain) - az_span_size(joined));

  bool ok = _append(rest, AZ_SPAN_FROM_STR("Proxy-Authorization: Basic "));
  int32_t written = 0;
  ok = ok && az_span_size(*rest) >= az_base64_get_max_encoded_size(az_span_size(user_pass))
      && az_result_succeeded(az_base64_encode(*rest, user_pass, &written));
  if (ok)
  {
    *rest = az_span_slice_to_end(*rest, written);
  }
  az_span_fill(AZ_SPAN_FROM_BUFFER(plain), 0);
  return ok && _append(rest, AZ_SPAN_FROM_STR("\r\n"));
}

az_result _az_mqtt_http_connect_request(
    az_mqtt_proxy_options const* proxy,
    az_span host,
    uint16_t port,
    az_span buffer,
    int32_t* out_size)
{
  if (!_is_valid_host(host))
  {
    return AZ_MQTT_ERROR_INVALID_CONFIG;
  }
  az_span rest = buffer;
  bool ok = _append(&rest, AZ_SPAN_FROM_STR("CONNECT ")) && _append_authority(&rest, host, port)
      && _append(&rest, AZ_SPAN_FROM_STR(" HTTP/1.1\r\nHost: "))
      && _append_authority(&rest, host, port) && _append(&rest, AZ_SPAN_FROM_STR("\r\n"));
  if (ok && az_span_size(proxy->username) > 0)
  {
    ok = _append_credentials(&rest, proxy);
  }
  ok = ok && _append(&rest, AZ_SPAN_FROM_STR("\r\n"));
  if (!ok)
  {
    az_span_fill(buffer, 0);
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  *out_size = az_span_size(buffer) - az_span_size(rest);
  return AZ_OK;
}

az_result _az_mqtt_http_connect_send_request(
    az_mqtt_proxy_options const* proxy,
    az_span host,
    uint16_t port,
    int32_t* in_out_sent,
    _az_mqtt_http_connect_send_fn send,
    void* context)
{
  uint8_t request[_AZ_MQTT_HTTP_CONNECT_REQUEST_MAX];
  int32_t size = 0;
  az_result rc
      = _az_mqtt_http_connect_request(proxy, host, port, AZ_SPAN_FROM_BUFFER(request), &size);
  while (az_result_succeeded(rc) && *in_out_sent < size)
  {
    int32_t const n = send(context, request + *in_out_sent, size - *in_out_sent);
    if (n > 0)
    {
      *in_out_sent += n < size - *in_out_sent ? n : size - *in_out_sent;
    }
    else
    {
      rc = n == 0 ? AZ_MQTT_ERROR_TIMEOUT : AZ_MQTT_ERROR_PROXY;
    }
  }
  az_span_fill(AZ_SPAN_FROM_BUFFER(request), 0); // It holds the credentials.
  return rc;
}

#endif // AZ_MQTT_NO_PROXY

/** @brief Where the parser is in the reply. */
enum
{
  _REPLY_VERSION, ///< "HTTP/1.x " (position counts its bytes)
  _REPLY_STATUS, ///< Three digits (position counts them), then SP or the line end
  _REPLY_REASON, ///< Rest of the status line
  _REPLY_LINE_START, ///< A header line, or the empty line ending the headers
  _REPLY_HEADER, ///< Rest of a header line
  _REPLY_END_CR, ///< CR of the empty line seen
  _REPLY_LINE_CR, ///< CR ending the status line seen
  _REPLY_HEADER_CR, ///< CR ending a header line seen
};

/** @brief Whether @p c may appear inside a reason phrase or header line (RFC 9110 §5.5). */
static bool _is_field_byte(uint8_t c) { return c == '\t' || c >= 0x20 ? c != 0x7F : false; }

void _az_mqtt_http_reply_init(_az_mqtt_http_reply* reply)
{
  reply->received = 0;
  reply->status = 0;
  reply->state = _REPLY_VERSION;
  reply->position = 0;
}

#ifdef AZ_MQTT_NO_WEBSOCKETS
static // Only the CONNECT reply parser uses it.
#endif
    _az_mqtt_http_event
    _az_mqtt_http_reply_step(_az_mqtt_http_reply* r, uint8_t c)
{
  static char const version[] = "HTTP/1.";
  if (++r->received > _AZ_MQTT_HTTP_REPLY_MAX)
  {
    r->status = 0;
    return _AZ_MQTT_HTTP_MALFORMED;
  }
  switch (r->state)
  {
    case _REPLY_VERSION:
      if (r->position < sizeof(version) - 1 ? c == (uint8_t)version[r->position]
                                            : (r->position == sizeof(version) - 1
                                                   ? (c >= '0' && c <= '9')
                                                   : c == ' '))
      {
        if (++r->position == sizeof(version) + 1)
        {
          r->state = _REPLY_STATUS;
          r->position = 0;
        }
        return _AZ_MQTT_HTTP_MORE;
      }
      break;
    case _REPLY_STATUS:
      if (r->position < 3 && c >= '0' && c <= '9')
      {
        r->status = (uint16_t)(r->status * 10 + (c - '0'));
        r->position++;
        return _AZ_MQTT_HTTP_MORE;
      }
      if (r->position == 3 && (c == ' ' || c == '\r' || c == '\n'))
      {
        r->state = c == '\n' ? _REPLY_LINE_START : (c == '\r' ? _REPLY_LINE_CR : _REPLY_REASON);
        return _AZ_MQTT_HTTP_MORE;
      }
      break;
    case _REPLY_REASON:
      if (c == '\r' || c == '\n' || _is_field_byte(c))
      {
        r->state = c == '\n' ? _REPLY_LINE_START : (c == '\r' ? _REPLY_LINE_CR : r->state);
        return _AZ_MQTT_HTTP_MORE;
      }
      break;
    case _REPLY_HEADER:
      if (c == '\r')
      {
        r->state = _REPLY_HEADER_CR;
        return _AZ_MQTT_HTTP_MORE;
      }
      if (c == '\n')
      {
        r->state = _REPLY_LINE_START;
        return _AZ_MQTT_HTTP_HEADER_END;
      }
      if (_is_field_byte(c))
      {
        return _AZ_MQTT_HTTP_HEADER_BYTE;
      }
      break;
    case _REPLY_LINE_CR:
    case _REPLY_HEADER_CR:
      if (c == '\n')
      {
        bool const header = r->state == _REPLY_HEADER_CR;
        r->state = _REPLY_LINE_START;
        return header ? _AZ_MQTT_HTTP_HEADER_END : _AZ_MQTT_HTTP_MORE;
      }
      break;
    case _REPLY_LINE_START:
      if (c == '\n')
      {
        return _AZ_MQTT_HTTP_END;
      }
      if (c == '\r')
      {
        r->state = _REPLY_END_CR;
        return _AZ_MQTT_HTTP_MORE;
      }
      if (_is_field_byte(c))
      {
        r->state = _REPLY_HEADER;
        return _AZ_MQTT_HTTP_HEADER_BYTE;
      }
      break;
    case _REPLY_END_CR:
      if (c == '\n')
      {
        return _AZ_MQTT_HTTP_END;
      }
      break;
    default:
      break;
  }
  r->status = 0;
  return _AZ_MQTT_HTTP_MALFORMED;
}

#ifndef AZ_MQTT_NO_PROXY
az_result _az_mqtt_http_connect_reply_parse(
    _az_mqtt_http_reply* reply,
    az_span data,
    int32_t* out_consumed)
{
  uint8_t const* p = az_span_ptr(data);
  for (int32_t i = 0; i < az_span_size(data); i++)
  {
    _az_mqtt_http_event const event = _az_mqtt_http_reply_step(reply, p[i]);
    if (event == _AZ_MQTT_HTTP_END)
    {
      if (reply->status >= 100 && reply->status < 200)
      {
        uint32_t const received = reply->received; // Interim: the real reply follows.
        _az_mqtt_http_reply_init(reply);
        reply->received = received;
        continue;
      }
      *out_consumed = i + 1;
      if (reply->status >= 200 && reply->status < 300)
      {
        return AZ_OK;
      }
      return reply->status == 407 ? AZ_MQTT_ERROR_PROXY_AUTH : AZ_MQTT_ERROR_PROXY;
    }
    if (event == _AZ_MQTT_HTTP_MALFORMED)
    {
      *out_consumed = i + 1;
      return AZ_MQTT_ERROR_PROXY;
    }
  }
  *out_consumed = az_span_size(data);
  return AZ_MQTT_ERROR_TIMEOUT;
}
#endif // AZ_MQTT_NO_PROXY
