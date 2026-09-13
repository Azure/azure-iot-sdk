// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* See e2e_mqtt_transport.h. MQTT 3.1.1 over TLS, optionally through an HTTP
 * CONNECT proxy, framed by hand. */
#include "e2e_mqtt_transport.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "azure/iot/az_iot_log.h"

#define E2E_MQTT_RX_CAP (64 * 1024)

typedef struct
{
  az_iot_mqtt_client base;

  az_iot_e2e_mqtt_config cfg;

  SSL_CTX* ssl_ctx;
  SSL* ssl;
  int fd;

  az_iot_mqtt_event_callback cb;
  void* cb_ctx;

  uint16_t next_packet_id;

  uint8_t rx[E2E_MQTT_RX_CAP];
  size_t rx_len;

  bool connected;
  bool disconnect_reported;
} e2e_mqtt_client;

typedef struct
{
  az_iot_mqtt_factory base;
  az_iot_e2e_mqtt_config cfg;
} e2e_mqtt_factory;

/* --- small byte-buffer writer ------------------------------------------- */

typedef struct
{
  uint8_t* buf;
  size_t cap;
  size_t len;
  bool overflow;
} wbuf;

static void w_byte(wbuf* w, uint8_t b)
{
  if (w->len + 1 > w->cap)
  {
    w->overflow = true;
    return;
  }
  w->buf[w->len++] = b;
}

static void w_bytes(wbuf* w, const void* p, size_t n)
{
  if (w->len + n > w->cap)
  {
    w->overflow = true;
    return;
  }
  memcpy(w->buf + w->len, p, n);
  w->len += n;
}

static void w_u16(wbuf* w, uint16_t v)
{
  w_byte(w, (uint8_t)(v >> 8));
  w_byte(w, (uint8_t)(v & 0xff));
}

static void w_str(wbuf* w, const char* s)
{
  size_t n = strlen(s);
  w_u16(w, (uint16_t)n);
  w_bytes(w, s, n);
}

/* --- transport ----------------------------------------------------------- */

/* Bound on the blocking connect path, so a black-holed endpoint cannot hang the
 * caller past the timeout it asked for. */
static uint32_t g_connect_timeout_seconds = 30u;

static void set_socket_timeouts(int fd, uint32_t seconds)
{
  struct timeval tv;
  tv.tv_sec = (time_t)(seconds ? seconds : 30u);
  tv.tv_usec = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int tcp_connect(const char* host, const char* port)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = NULL;
  if (getaddrinfo(host, port, &hints, &res) != 0)
  {
    return -1;
  }
  int fd = -1;
  for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next)
  {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
    {
      continue;
    }
    /* Set before connect(), not after: the connect itself and the proxy
     * handshake are the operations most likely to block on a black-holed
     * endpoint, so bounding them afterwards would bound nothing. */
    set_socket_timeouts(fd, g_connect_timeout_seconds);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
    {
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

/* Open a tunnel to host:port and leave the socket ready for TLS. */
static int proxy_connect(const char* proxy, const char* host, uint16_t port)
{
  char proxy_host[256];
  const char* colon = strrchr(proxy, ':');
  if (colon == NULL || (size_t)(colon - proxy) >= sizeof(proxy_host))
  {
    return -1;
  }
  memcpy(proxy_host, proxy, (size_t)(colon - proxy));
  proxy_host[colon - proxy] = '\0';

  int fd = tcp_connect(proxy_host, colon + 1);
  if (fd < 0)
  {
    return -1;
  }

  char req[512];
  int n = snprintf(
      req,
      sizeof(req),
      "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n\r\n",
      host,
      (unsigned)port,
      host,
      (unsigned)port);
  if (n <= 0 || (size_t)n >= sizeof(req) || write(fd, req, (size_t)n) != n)
  {
    close(fd);
    return -1;
  }

  /* Read exactly to the end of the status/headers so no tunnelled byte is
   * consumed here: TLS starts at the very next byte. */
  char resp[1024];
  size_t got = 0;
  while (got < sizeof(resp) - 1)
  {
    ssize_t r = read(fd, resp + got, 1);
    if (r <= 0)
    {
      close(fd);
      return -1;
    }
    got += (size_t)r;
    resp[got] = '\0';
    if (got >= 4 && memcmp(resp + got - 4, "\r\n\r\n", 4) == 0)
    {
      break;
    }
  }
  if (strncmp(resp, "HTTP/1.1 200", 12) != 0 && strncmp(resp, "HTTP/1.0 200", 12) != 0)
  {
    AZ_IOT_LOG_ERRORF("e2e-mqtt: proxy refused the tunnel: %.*s", 32, resp);
    close(fd);
    return -1;
  }
  return fd;
}

static az_iot_result tls_start(
    e2e_mqtt_client* c,
    const char* host,
    const az_iot_mqtt_tls_options* tls)
{
  c->ssl_ctx = SSL_CTX_new(TLS_client_method());
  if (c->ssl_ctx == NULL)
  {
    return AZ_IOT_ERR_TLS;
  }
  SSL_CTX_set_min_proto_version(c->ssl_ctx, TLS1_2_VERSION);

  if (c->cfg.ca_file != NULL)
  {
    if (SSL_CTX_load_verify_locations(c->ssl_ctx, c->cfg.ca_file, NULL) != 1)
    {
      return AZ_IOT_ERR_TLS;
    }
  }
  else if (SSL_CTX_set_default_verify_paths(c->ssl_ctx) != 1)
  {
    return AZ_IOT_ERR_TLS;
  }
  SSL_CTX_set_verify(c->ssl_ctx, SSL_VERIFY_PEER, NULL);

  /* Client credentials. The connection client fills these for X.509, and
   * silently ignoring them would leave a caller believing it had mutual TLS
   * when the handshake was in fact anonymous. PEM files are supported; the
   * in-memory and non-extractable-key forms are not, and are refused rather
   * than dropped. */
  if (tls != NULL)
  {
    if (tls->client_cert_path != NULL && tls->client_key_path != NULL)
    {
      if (tls->client_key_password != NULL)
      {
        /* Without this OpenSSL prompts on the terminal for an encrypted key,
         * which would hang a test run rather than fail it. */
        SSL_CTX_set_default_passwd_cb_userdata(
            c->ssl_ctx, (void*)(uintptr_t)tls->client_key_password);
      }
      if (SSL_CTX_use_certificate_chain_file(c->ssl_ctx, tls->client_cert_path) != 1
          || SSL_CTX_use_PrivateKey_file(c->ssl_ctx, tls->client_key_path, SSL_FILETYPE_PEM) != 1)
      {
        AZ_IOT_LOG_ERROR("e2e-mqtt: could not load the client certificate or key");
        return AZ_IOT_ERR_TLS;
      }
    }
    else if (
        tls->client_cert_pem != NULL || tls->client_key_pem != NULL || tls->client_key_uri != NULL
        || tls->sign != NULL)
    {
      AZ_IOT_LOG_ERROR("e2e-mqtt: only PEM FILE credentials are supported by this test transport");
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    if (tls->trusted_ca_path != NULL
        && SSL_CTX_load_verify_locations(c->ssl_ctx, tls->trusted_ca_path, NULL) != 1)
    {
      return AZ_IOT_ERR_TLS;
    }
  }

  c->ssl = SSL_new(c->ssl_ctx);
  if (c->ssl == NULL)
  {
    return AZ_IOT_ERR_TLS;
  }
  /* SNI and hostname verification: without both, a proxied connection would
   * accept any certificate the tunnel endpoint happened to present. */
  if (SSL_set_tlsext_host_name(c->ssl, host) != 1 || SSL_set1_host(c->ssl, host) != 1
      || SSL_set_fd(c->ssl, c->fd) != 1)
  {
    return AZ_IOT_ERR_TLS;
  }
  if (SSL_connect(c->ssl) != 1)
  {
    AZ_IOT_LOG_ERRORF(
        "e2e-mqtt: TLS handshake failed: %s", ERR_error_string(ERR_get_error(), NULL));
    return AZ_IOT_ERR_TLS;
  }
  return AZ_IOT_OK;
}

static az_iot_result ssl_write_all(e2e_mqtt_client* c, const uint8_t* p, size_t n)
{
  size_t off = 0;
  while (off < n)
  {
    int w = SSL_write(c->ssl, p + off, (int)(n - off));
    if (w <= 0)
    {
      return AZ_IOT_ERR_MQTT;
    }
    off += (size_t)w;
  }
  return AZ_IOT_OK;
}

/* --- MQTT framing -------------------------------------------------------- */

static void w_remaining_length(wbuf* w, size_t n)
{
  do
  {
    uint8_t d = (uint8_t)(n % 128);
    n /= 128;
    if (n > 0)
    {
      d |= 0x80;
    }
    w_byte(w, d);
  } while (n > 0);
}

/* Build a packet: fixed header byte, then body, with the length between. */
static az_iot_result send_packet(e2e_mqtt_client* c, uint8_t header, const uint8_t* body, size_t n)
{
  uint8_t hdr[8];
  wbuf w = { hdr, sizeof(hdr), 0, false };
  w_byte(&w, header);
  w_remaining_length(&w, n);
  if (w.overflow)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  az_iot_result r = ssl_write_all(c, hdr, w.len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  return n > 0 ? ssl_write_all(c, body, n) : AZ_IOT_OK;
}

static uint16_t next_id(e2e_mqtt_client* c)
{
  if (++c->next_packet_id == 0)
  {
    c->next_packet_id = 1;
  }
  return c->next_packet_id;
}

static az_iot_result e2e_connect(az_iot_mqtt_client* self, const az_iot_mqtt_connect_options* opts)
{
  e2e_mqtt_client* c = (e2e_mqtt_client*)self;
  if (opts == NULL || opts->host == NULL || opts->client_id == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* connect() here is blocking, unlike the adapter contract, which expects I/O
   * in process_loop(). A deliberate simplification for a test transport, but a
   * bounded one: the socket timeouts below are installed before any blocking
   * call so a dead endpoint fails instead of hanging the pump. */
  g_connect_timeout_seconds = opts->connect_timeout_seconds ? opts->connect_timeout_seconds : 30u;

  uint16_t port = opts->port ? opts->port : 8883;
  if (c->cfg.proxy != NULL)
  {
    c->fd = proxy_connect(c->cfg.proxy, opts->host, port);
  }
  else
  {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    c->fd = tcp_connect(opts->host, port_str);
  }
  if (c->fd < 0)
  {
    AZ_IOT_LOG_ERRORF("e2e-mqtt: could not reach %s:%u", opts->host, (unsigned)port);
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  az_iot_result r = tls_start(c, opts->host, &opts->tls);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* The core leaves password NULL for X.509. Ask the host for one; a SAS
   * environment supplies it here. */
  char password[1024];
  password[0] = '\0';
  const char* pass = opts->password;
  if (pass == NULL && c->cfg.password_cb != NULL && opts->username != NULL)
  {
    c->cfg.password_cb(opts->username, password, sizeof(password), c->cfg.password_ctx);
    if (password[0] != '\0')
    {
      pass = password;
    }
  }

  uint8_t body[2048];
  wbuf w = { body, sizeof(body), 0, false };
  w_str(&w, "MQTT");
  w_byte(&w, 4); /* protocol level 3.1.1 */
  /* Bit 1 is Clean Session; the iface exposes it as clean_start, so honour it
   * rather than forcing a clean session and silently discarding v3.1.1 session
   * semantics the caller asked for. */
  uint8_t flags = (uint8_t)(opts->clean_start ? 0x02 : 0x00);
  if (opts->username != NULL)
  {
    flags |= 0x80;
  }
  if (pass != NULL)
  {
    flags |= 0x40;
  }
  w_byte(&w, flags);
  w_u16(&w, opts->keep_alive_seconds ? opts->keep_alive_seconds : 60);
  w_str(&w, opts->client_id);
  if (opts->username != NULL)
  {
    w_str(&w, opts->username);
  }
  if (pass != NULL)
  {
    w_str(&w, pass);
  }
  if (w.overflow)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  return send_packet(c, 0x10, body, w.len);
}

static az_iot_result e2e_disconnect(az_iot_mqtt_client* self)
{
  e2e_mqtt_client* c = (e2e_mqtt_client*)self;
  if (c->ssl != NULL)
  {
    (void)send_packet(c, 0xe0, NULL, 0);
  }
  c->connected = false;
  return AZ_IOT_OK;
}

static az_iot_result e2e_subscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    az_iot_mqtt_qos qos,
    uint16_t* out_packet_id)
{
  e2e_mqtt_client* c = (e2e_mqtt_client*)self;
  if (topic_filter == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Refused rather than quietly downgraded: a caller told its subscription was
   * established at QoS 2 would be wrong about the delivery guarantee. */
  if (qos > AZ_IOT_MQTT_QOS_1)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  uint16_t pid = next_id(c);
  uint8_t body[512];
  wbuf w = { body, sizeof(body), 0, false };
  w_u16(&w, pid);
  w_str(&w, topic_filter);
  w_byte(&w, (uint8_t)qos);
  if (w.overflow)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  az_iot_result r = send_packet(c, 0x82, body, w.len);
  if (r == AZ_IOT_OK && out_packet_id != NULL)
  {
    *out_packet_id = pid;
  }
  return r;
}

static az_iot_result e2e_unsubscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    uint16_t* out_packet_id)
{
  e2e_mqtt_client* c = (e2e_mqtt_client*)self;
  if (topic_filter == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  uint16_t pid = next_id(c);
  uint8_t body[512];
  wbuf w = { body, sizeof(body), 0, false };
  w_u16(&w, pid);
  w_str(&w, topic_filter);
  if (w.overflow)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  az_iot_result r = send_packet(c, 0xa2, body, w.len);
  if (r == AZ_IOT_OK && out_packet_id != NULL)
  {
    *out_packet_id = pid;
  }
  return r;
}

static az_iot_result e2e_publish(
    az_iot_mqtt_client* self,
    const az_iot_mqtt_message* msg,
    uint16_t* out_packet_id)
{
  e2e_mqtt_client* c = (e2e_mqtt_client*)self;
  if (msg == NULL || msg->topic == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (msg->qos > AZ_IOT_MQTT_QOS_1)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  bool qos1 = msg->qos >= AZ_IOT_MQTT_QOS_1;
  uint16_t pid = qos1 ? next_id(c) : 0;

  size_t need = 2 + strlen(msg->topic) + (qos1 ? 2u : 0u) + msg->payload_len;
  uint8_t* body = (uint8_t*)malloc(need > 0 ? need : 1);
  if (body == NULL)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  wbuf w = { body, need, 0, false };
  w_str(&w, msg->topic);
  if (qos1)
  {
    w_u16(&w, pid);
  }
  if (msg->payload != NULL && msg->payload_len > 0)
  {
    w_bytes(&w, msg->payload, msg->payload_len);
  }
  az_iot_result r = w.overflow
      ? AZ_IOT_ERR_NOT_ENOUGH_SPACE
      : send_packet(c, (uint8_t)(0x30 | (qos1 ? 0x02 : 0x00)), body, w.len);
  free(body);
  if (r == AZ_IOT_OK && out_packet_id != NULL)
  {
    *out_packet_id = pid;
  }
  return r;
}

static void emit(e2e_mqtt_client* c, const az_iot_mqtt_event* evt)
{
  if (c->cb != NULL)
  {
    c->cb(evt, c->cb_ctx);
  }
}

/* Decode whatever whole packets are buffered and raise them as events. */
static void dispatch_buffered(e2e_mqtt_client* c)
{
  size_t off = 0;
  while (off < c->rx_len)
  {
    uint8_t type = (uint8_t)(c->rx[off] >> 4);
    uint8_t flags = (uint8_t)(c->rx[off] & 0x0f);

    size_t i = off + 1;
    size_t len = 0;
    size_t mult = 1;
    uint8_t digit;
    do
    {
      if (i >= c->rx_len)
      {
        goto done; /* length not fully arrived */
      }
      digit = c->rx[i++];
      len += (size_t)(digit & 127) * mult;
      mult *= 128;
    } while ((digit & 0x80) != 0);

    if (i + len > c->rx_len)
    {
      goto done; /* body not fully arrived */
    }

    const uint8_t* p = c->rx + i;
    az_iot_mqtt_event evt;
    memset(&evt, 0, sizeof(evt));

    switch (type)
    {
      case 2: /* CONNACK */
      {
        int rc = (len >= 2) ? (int)p[1] : -1;
        evt.kind = AZ_IOT_MQTT_EVT_CONNECTED;
        evt.session_present = (len >= 1) && ((p[0] & 0x01) != 0);
        evt.protocol_code = rc > 0 ? rc : 0;
        /* Routed through the SDK's mapper rather than classified here: the
         * identity-versus-transport split is what decides whether the core
         * re-provisions, and it must read the same from every adapter. */
        evt.status = az_iot_mqtt_connack_result(AZ_IOT_MQTT_VERSION_3_1_1, rc);
        if (evt.status == AZ_IOT_OK)
        {
          c->connected = true;
        }
        else
        {
          AZ_IOT_LOG_ERRORF("e2e-mqtt: CONNACK refused, rc=%d", rc);
        }
        emit(c, &evt);
        break;
      }

      case 3: /* PUBLISH */
      {
        if (len < 2)
        {
          break;
        }
        size_t tlen = ((size_t)p[0] << 8) | p[1];
        if (2 + tlen > len)
        {
          break;
        }
        char topic[512];
        size_t copy = tlen < sizeof(topic) - 1 ? tlen : sizeof(topic) - 1;
        memcpy(topic, p + 2, copy);
        topic[copy] = '\0';

        size_t body_off = 2 + tlen;
        uint16_t pid = 0;
        bool qos1 = ((flags >> 1) & 3) > 0;
        if (qos1)
        {
          if (body_off + 2 > len)
          {
            break;
          }
          pid = (uint16_t)(((uint16_t)p[body_off] << 8) | p[body_off + 1]);
          body_off += 2;
        }

        az_iot_mqtt_message inbound;
        memset(&inbound, 0, sizeof(inbound));
        inbound.topic = topic;
        inbound.payload = p + body_off;
        inbound.payload_len = len - body_off;
        inbound.qos = qos1 ? AZ_IOT_MQTT_QOS_1 : AZ_IOT_MQTT_QOS_0;
        inbound.retain = (flags & 0x01) != 0;

        evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
        evt.status = AZ_IOT_OK;
        evt.message = &inbound;
        emit(c, &evt);

        if (qos1)
        {
          uint8_t ack[2] = { (uint8_t)(pid >> 8), (uint8_t)(pid & 0xff) };
          (void)send_packet(c, 0x40, ack, sizeof(ack));
        }
        break;
      }

      case 4: /* PUBACK */
        if (len >= 2)
        {
          evt.kind = AZ_IOT_MQTT_EVT_PUBLISH_ACK;
          evt.status = AZ_IOT_OK;
          evt.packet_id = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
          emit(c, &evt);
        }
        break;

      case 9: /* SUBACK */
        if (len >= 3)
        {
          evt.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
          evt.packet_id = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
          evt.protocol_code = (int32_t)p[2];
          evt.status = az_iot_mqtt_suback_result(AZ_IOT_MQTT_VERSION_3_1_1, (int)p[2]);
          emit(c, &evt);
        }
        break;

      case 11: /* UNSUBACK */
        if (len >= 2)
        {
          evt.kind = AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK;
          evt.status = AZ_IOT_OK;
          evt.packet_id = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
          emit(c, &evt);
        }
        break;

      default: /* PINGRESP and anything else: nothing to surface. */
        break;
    }

    off = i + len;
  }

done:
  if (off > 0)
  {
    memmove(c->rx, c->rx + off, c->rx_len - off);
    c->rx_len -= off;
  }
}

static az_iot_result e2e_process_loop(az_iot_mqtt_client* self, uint32_t timeout_ms)
{
  e2e_mqtt_client* c = (e2e_mqtt_client*)self;
  if (c->ssl == NULL)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  /* Wait for readability rather than blocking in SSL_read: do_work() is called
   * in a loop and must return even when the service has nothing to say. */
  struct timeval tv;
  tv.tv_sec = (time_t)(timeout_ms / 1000u);
  tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);

  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(c->fd, &rd);

  /* Bytes already decrypted inside OpenSSL would not make the fd readable. */
  if (SSL_pending(c->ssl) == 0)
  {
    int s = select(c->fd + 1, &rd, NULL, NULL, &tv);
    if (s < 0)
    {
      return errno == EINTR ? AZ_IOT_OK : AZ_IOT_ERR_MQTT;
    }
    if (s == 0)
    {
      return AZ_IOT_OK;
    }
  }

  if (c->rx_len >= sizeof(c->rx))
  {
    AZ_IOT_LOG_ERROR("e2e-mqtt: receive buffer full");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  int n = SSL_read(c->ssl, c->rx + c->rx_len, (int)(sizeof(c->rx) - c->rx_len));
  if (n <= 0)
  {
    int err = SSL_get_error(c->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
    {
      return AZ_IOT_OK;
    }
    /* Reported whether or not CONNACK ever arrived. A peer that closes during
     * the handshake would otherwise leave the connection client waiting in
     * CONNECTING forever, and a close would never settle to IDLE. `reported`
     * keeps it to one event. */
    if (!c->disconnect_reported)
    {
      az_iot_mqtt_event evt;
      memset(&evt, 0, sizeof(evt));
      evt.kind = AZ_IOT_MQTT_EVT_DISCONNECTED;
      evt.status = AZ_IOT_ERR_NOT_CONNECTED;
      c->disconnect_reported = true;
      c->connected = false;
      emit(c, &evt);
    }
    return AZ_IOT_OK;
  }

  c->rx_len += (size_t)n;
  dispatch_buffered(c);
  return AZ_IOT_OK;
}

static void e2e_set_inbound_cb(
    az_iot_mqtt_client* self,
    az_iot_mqtt_event_callback cb,
    void* user_ctx)
{
  e2e_mqtt_client* c = (e2e_mqtt_client*)self;
  c->cb = cb;
  c->cb_ctx = user_ctx;
}

static void e2e_destroy(az_iot_mqtt_client* self)
{
  e2e_mqtt_client* c = (e2e_mqtt_client*)self;
  if (c == NULL)
  {
    return;
  }
  if (c->ssl != NULL)
  {
    SSL_free(c->ssl);
  }
  if (c->ssl_ctx != NULL)
  {
    SSL_CTX_free(c->ssl_ctx);
  }
  if (c->fd >= 0)
  {
    close(c->fd);
  }
  free(c);
}

static const az_iot_mqtt_iface k_iface = {
  .version = AZ_IOT_MQTT_VERSION_3_1_1,
  .connect = e2e_connect,
  .disconnect = e2e_disconnect,
  .subscribe = e2e_subscribe,
  .unsubscribe = e2e_unsubscribe,
  .publish = e2e_publish,
  .process_loop = e2e_process_loop,
  .set_inbound_cb = e2e_set_inbound_cb,
  .destroy = e2e_destroy,
};

static az_iot_mqtt_client* e2e_factory_create(void* factory_ctx)
{
  e2e_mqtt_factory* f = (e2e_mqtt_factory*)factory_ctx;
  e2e_mqtt_client* c = (e2e_mqtt_client*)calloc(1, sizeof(*c));
  if (c == NULL)
  {
    return NULL;
  }
  c->base.iface = &k_iface;
  c->cfg = f->cfg;
  c->fd = -1;
  c->next_packet_id = 0;
  return (az_iot_mqtt_client*)c;
}

static void e2e_factory_destroy(void* factory_ctx) { free(factory_ctx); }

az_iot_mqtt_factory* az_iot_e2e_mqtt_factory_create(const az_iot_e2e_mqtt_config* config)
{
  if (config == NULL)
  {
    return NULL;
  }
  e2e_mqtt_factory* f = (e2e_mqtt_factory*)calloc(1, sizeof(*f));
  if (f == NULL)
  {
    return NULL;
  }
  f->base.version = AZ_IOT_MQTT_VERSION_3_1_1;
  f->base.create = e2e_factory_create;
  f->base.factory_ctx = f;
  f->base.destroy = e2e_factory_destroy;
  f->cfg = *config;
  return (az_iot_mqtt_factory*)f;
}

void az_iot_e2e_mqtt_factory_destroy(az_iot_mqtt_factory* factory) { free(factory); }
