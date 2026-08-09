// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Implementation of the for-testing TCP passthrough proxy.
 *
 * A single background thread runs an accept loop; for each accepted client it
 * opens an upstream connection and pumps bytes both ways with select(). Control
 * fields and counters are guarded by one mutex; the client->broker MQTT
 * fixed-header parser state is touched only by the pump thread.
 *
 * The Winsock/POSIX shim mirrors the proven idioms in
 * c/tests/deps/amqp/transport/az_amqp_sample_transport.c. POSIX feature-test
 * macros (_POSIX_C_SOURCE/_DEFAULT_SOURCE) are supplied by the build so
 * getaddrinfo/select/nanosleep are visible under -std=c99.
 */
#include "az_iot_test_proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET proxy_sock;
typedef int proxy_socklen;
#define PROXY_INVALID_SOCK INVALID_SOCKET
#define proxy_closesock(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int proxy_sock;
typedef socklen_t proxy_socklen;
#define PROXY_INVALID_SOCK (-1)
#define proxy_closesock(s) close(s)
#endif

#if defined(AZ_IOT_TEST_PROXY_TLS)
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

/* ------------------------------------------------------------------------- */
/* platform shims                                                            */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32)
typedef CRITICAL_SECTION proxy_mutex;
static void proxy_mutex_init(proxy_mutex* m) { InitializeCriticalSection(m); }
static void proxy_mutex_destroy(proxy_mutex* m) { DeleteCriticalSection(m); }
static void proxy_lock(proxy_mutex* m) { EnterCriticalSection(m); }
static void proxy_unlock(proxy_mutex* m) { LeaveCriticalSection(m); }
static void proxy_sleep_ms(unsigned ms) { Sleep(ms); }
#else
typedef pthread_mutex_t proxy_mutex;
static void proxy_mutex_init(proxy_mutex* m) { pthread_mutex_init(m, NULL); }
static void proxy_mutex_destroy(proxy_mutex* m) { pthread_mutex_destroy(m); }
static void proxy_lock(proxy_mutex* m) { pthread_mutex_lock(m); }
static void proxy_unlock(proxy_mutex* m) { pthread_mutex_unlock(m); }
static void proxy_sleep_ms(unsigned ms)
{
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)((ms % 1000u) * 1000000UL);
  nanosleep(&ts, NULL);
}
#endif

static void proxy_ensure_wsa(void)
{
#if defined(_WIN32)
  /* WSAStartup is internally synchronized and reference-counted. Call it once
   * per proxy start rather than gating on a non-atomic static flag (which would
   * race under concurrent az_iot_test_proxy_start); the refcount is released at
   * process exit. */
  WSADATA data;
  (void)WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

/* ------------------------------------------------------------------------- */
/* proxy state                                                               */
/* ------------------------------------------------------------------------- */

struct az_iot_test_proxy
{
  /* config, immutable after start */
  char upstream_host[256];
  char upstream_port[8];
  proxy_sock listen_sock;
  uint16_t listen_port;

  /* pump thread */
#if defined(_WIN32)
  HANDLE thread;
#else
  pthread_t thread;
  int thread_started;
#endif

  /* mutex-guarded control + counters */
  proxy_mutex lock;
  int should_stop;
  int drop_requested;
  uint64_t reset_after_bytes; /* 0 = disarmed; one-shot */
  uint32_t reset_after_packets; /* 0 = disarmed; one-shot */
  unsigned write_delay_ms;
  size_t fragment_max; /* 0 = no fragmentation */

  uint64_t conn_c2b_bytes; /* client->broker bytes, current connection */
  uint32_t conn_c2b_packets; /* client->broker MQTT packets, current connection */
  uint64_t total_bytes; /* both directions, cumulative */
  uint32_t total_packets; /* client->broker packets, cumulative */
  uint32_t connections; /* accepted client connections */

  /* client->broker fixed-header parser state (pump thread only) */
  int pkt_phase; /* 0=first byte, 1=remaining-length, 2=body */
  uint32_t pkt_rem;
  uint32_t pkt_mult;
  uint32_t pkt_body_left;

  /* synthetic-broker mode (C3): when connack_len > 0 the proxy answers the
   * client's CONNECT with these bytes instead of forwarding upstream, then
   * stays silent except for any injected DISCONNECT. */
  uint8_t connack[64];
  size_t connack_len;
  uint8_t disconnect[64];
  size_t disconnect_len;
  unsigned disconnect_delay_ms;

#if defined(AZ_IOT_TEST_PROXY_TLS)
  /* TLS termination (set by az_iot_test_proxy_enable_tls, read by the pump thread). */
  SSL_CTX* ssl_ctx; /* server context presenting the generated leaf; NULL = plaintext */
  char ca_pem[4096]; /* PEM of the CA the client should trust */
  size_t ca_pem_len;
#endif
};

/* ------------------------------------------------------------------------- */
/* socket helpers                                                            */
/* ------------------------------------------------------------------------- */

static long proxy_recv(proxy_sock s, char* buf, size_t cap)
{
#if defined(_WIN32)
  return (long)recv(s, buf, (int)cap, 0);
#else
  return (long)recv(s, buf, cap, 0);
#endif
}

static int proxy_send_all(proxy_sock s, const char* buf, size_t len)
{
  size_t off = 0;
  while (off < len)
  {
#if defined(_WIN32)
    int n = send(s, buf + off, (int)(len - off), 0);
#else
    long n = (long)send(s, buf + off, len - off, 0);
#endif
    if (n <= 0)
    {
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

/* Hard-reset a socket: SO_LINGER{on,0} makes close() emit a TCP RST so the peer
 * observes a connection error rather than a graceful shutdown. */
static void proxy_hard_close(proxy_sock s)
{
  if (s == PROXY_INVALID_SOCK)
  {
    return;
  }
  struct linger lg;
  lg.l_onoff = 1;
  lg.l_linger = 0;
  (void)setsockopt(s, SOL_SOCKET, SO_LINGER, (const char*)&lg, (proxy_socklen)sizeof(lg));
  proxy_closesock(s);
}

/* Wait until either socket is readable or the timeout elapses. Returns >0 if a
 * socket is ready, 0 on timeout, <0 on error; sets the *_ready flags. */
static int proxy_wait_readable(
    proxy_sock a,
    proxy_sock b,
    int* a_ready,
    int* b_ready,
    unsigned timeout_ms)
{
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(a, &rfds);
  FD_SET(b, &rfds);

  struct timeval tv;
  tv.tv_sec = (long)(timeout_ms / 1000u);
  tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

  int nfds = 0;
#if !defined(_WIN32)
  int fa = (int)a;
  int fb = (int)b;
  nfds = (fa > fb ? fa : fb) + 1;
#endif
  int r = select(nfds, &rfds, NULL, NULL, &tv);
  if (r < 0)
  {
    return -1;
  }
  *a_ready = FD_ISSET(a, &rfds) ? 1 : 0;
  *b_ready = FD_ISSET(b, &rfds) ? 1 : 0;
  return r;
}

/* Wait until a single socket is readable. Returns >0 if readable, 0 on timeout,
 * <0 on error. */
static int proxy_wait_readable_one(proxy_sock s, unsigned timeout_ms)
{
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(s, &rfds);

  struct timeval tv;
  tv.tv_sec = (long)(timeout_ms / 1000u);
  tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

  int nfds = 0;
#if !defined(_WIN32)
  nfds = (int)s + 1;
#endif
  return select(nfds, &rfds, NULL, NULL, &tv);
}

static proxy_sock proxy_accept_timeout(proxy_sock listen_sock, unsigned timeout_ms)
{
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(listen_sock, &rfds);

  struct timeval tv;
  tv.tv_sec = (long)(timeout_ms / 1000u);
  tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

  int nfds = 0;
#if !defined(_WIN32)
  nfds = (int)listen_sock + 1;
#endif
  int r = select(nfds, &rfds, NULL, NULL, &tv);
  if (r <= 0)
  {
    return PROXY_INVALID_SOCK;
  }
  return accept(listen_sock, NULL, NULL);
}

static proxy_sock proxy_connect_upstream(struct az_iot_test_proxy* m)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = NULL;
  if (getaddrinfo(m->upstream_host, m->upstream_port, &hints, &res) != 0 || res == NULL)
  {
    return PROXY_INVALID_SOCK;
  }

  proxy_sock s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == PROXY_INVALID_SOCK)
  {
    freeaddrinfo(res);
    return PROXY_INVALID_SOCK;
  }
  int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
  freeaddrinfo(res);
  if (rc != 0)
  {
    proxy_closesock(s);
    return PROXY_INVALID_SOCK;
  }
  return s;
}

/* Fixed on purpose: the proxy must never be reachable from another host. Read
 * the "Deliberate non-capabilities" note in az_iot_test_proxy.h before changing it. */
#define PROXY_LISTEN_ADDRESS "127.0.0.1"

static int proxy_open_listener(struct az_iot_test_proxy* m)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; /* loopback IPv4 for a stable, low-numbered fd */
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo* res = NULL;
  if (getaddrinfo(PROXY_LISTEN_ADDRESS, "0", &hints, &res) != 0 || res == NULL)
  {
    return -1;
  }

  proxy_sock s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == PROXY_INVALID_SOCK)
  {
    freeaddrinfo(res);
    return -1;
  }
  int yes = 1;
  (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, (proxy_socklen)sizeof(yes));
  if (bind(s, res->ai_addr, (int)res->ai_addrlen) != 0)
  {
    freeaddrinfo(res);
    proxy_closesock(s);
    return -1;
  }
  freeaddrinfo(res);
  if (listen(s, 1) != 0)
  {
    proxy_closesock(s);
    return -1;
  }

  struct sockaddr_in bound;
  memset(&bound, 0, sizeof(bound));
  proxy_socklen blen = (proxy_socklen)sizeof(bound);
  if (getsockname(s, (struct sockaddr*)&bound, &blen) != 0)
  {
    proxy_closesock(s);
    return -1;
  }
  m->listen_sock = s;
  m->listen_port = ntohs(bound.sin_port);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* forwarding + MQTT packet counting                                         */
/* ------------------------------------------------------------------------- */

/* Advance the client->broker MQTT fixed-header parser over `n` bytes and report
 * how many whole packets completed. Valid because C1 forwards plaintext. */
static void proxy_count_packets(
    struct az_iot_test_proxy* m,
    const char* buf,
    size_t n,
    uint32_t* out_new)
{
  uint32_t completed = 0;
  for (size_t i = 0; i < n; ++i)
  {
    unsigned char b = (unsigned char)buf[i];
    if (m->pkt_phase == 0)
    {
      m->pkt_phase = 1;
      m->pkt_rem = 0;
      m->pkt_mult = 1;
    }
    else if (m->pkt_phase == 1)
    {
      m->pkt_rem += (uint32_t)(b & 0x7Fu) * m->pkt_mult;
      m->pkt_mult *= 128u;
      if ((b & 0x80u) == 0u)
      {
        if (m->pkt_rem == 0u)
        {
          ++completed;
          m->pkt_phase = 0;
        }
        else
        {
          m->pkt_body_left = m->pkt_rem;
          m->pkt_phase = 2;
        }
      }
    }
    else /* body */
    {
      size_t avail = n - i;
      size_t skip = (avail < (size_t)m->pkt_body_left) ? avail : (size_t)m->pkt_body_left;
      m->pkt_body_left -= (uint32_t)skip;
      i += skip - 1; /* loop's ++i consumes the final body byte */
      if (m->pkt_body_left == 0u)
      {
        ++completed;
        m->pkt_phase = 0;
      }
    }
  }
  *out_new = completed;
}

static int proxy_forward(
    proxy_sock dst,
    const char* buf,
    size_t n,
    unsigned delay_ms,
    size_t fragment_max)
{
  size_t off = 0;
  do
  {
    size_t chunk = n - off;
    if (fragment_max > 0 && chunk > fragment_max)
    {
      chunk = fragment_max;
    }
    if (delay_ms > 0)
    {
      proxy_sleep_ms(delay_ms);
    }
    if (proxy_send_all(dst, buf + off, chunk) != 0)
    {
      return -1;
    }
    off += chunk;
  } while (off < n);
  return 0;
}

/* Client-side I/O abstracts plaintext vs. a terminated TLS session. `ssl` is an
 * SSL* when TLS termination is active, NULL otherwise. */
static void proxy_client_free(void* ssl)
{
#if defined(AZ_IOT_TEST_PROXY_TLS)
  if (ssl != NULL)
  {
    SSL_free((SSL*)ssl);
  }
#else
  (void)ssl;
#endif
}

static int proxy_ssl_pending(void* ssl)
{
#if defined(AZ_IOT_TEST_PROXY_TLS)
  return ssl != NULL && SSL_pending((SSL*)ssl) > 0;
#else
  (void)ssl;
  return 0;
#endif
}

static long proxy_client_recv(proxy_sock s, void* ssl, char* buf, size_t cap)
{
#if defined(AZ_IOT_TEST_PROXY_TLS)
  if (ssl != NULL)
  {
    int n = SSL_read((SSL*)ssl, buf, (int)cap);
    return (n > 0) ? (long)n : -1;
  }
#else
  (void)ssl;
#endif
  return proxy_recv(s, buf, cap);
}

static int proxy_client_send_all(proxy_sock s, void* ssl, const char* buf, size_t len)
{
#if defined(AZ_IOT_TEST_PROXY_TLS)
  if (ssl != NULL)
  {
    size_t off = 0;
    while (off < len)
    {
      int n = SSL_write((SSL*)ssl, buf + off, (int)(len - off));
      if (n <= 0)
      {
        return -1;
      }
      off += (size_t)n;
    }
    return 0;
  }
#else
  (void)ssl;
#endif
  return proxy_send_all(s, buf, len);
}

#if defined(AZ_IOT_TEST_PROXY_TLS)
/* Build an X.509 cert. issuer_cert==NULL => self-signed. `san` (e.g.
 * "IP:127.0.0.1") is added for leaf certs; CA certs get basicConstraints
 * CA:TRUE instead. Validity is [now+nb_off, now+na_off] seconds. */
static X509* proxy_make_cert(
    EVP_PKEY* subject_key,
    EVP_PKEY* issuer_key,
    X509* issuer_cert,
    const char* cn,
    const char* san,
    int is_ca,
    long serial,
    long nb_off,
    long na_off)
{
  X509* x = X509_new();
  if (x == NULL)
  {
    return NULL;
  }
  X509_NAME* name = NULL;
  int ok = 0;
  do
  {
    if (X509_set_version(x, 2) != 1)
    {
      break;
    }
    if (ASN1_INTEGER_set(X509_get_serialNumber(x), serial) != 1)
    {
      break;
    }
    if (X509_gmtime_adj(X509_getm_notBefore(x), nb_off) == NULL)
    {
      break;
    }
    if (X509_gmtime_adj(X509_getm_notAfter(x), na_off) == NULL)
    {
      break;
    }
    if (X509_set_pubkey(x, subject_key) != 1)
    {
      break;
    }

    name = X509_NAME_new();
    if (name == NULL)
    {
      break;
    }
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char*)cn, -1, -1, 0)
        != 1)
    {
      break;
    }
    if (X509_set_subject_name(x, name) != 1)
    {
      break;
    }
    if (X509_set_issuer_name(x, issuer_cert ? X509_get_subject_name(issuer_cert) : name) != 1)
    {
      break;
    }

    {
      X509V3_CTX ctx;
      X509V3_set_ctx_nodb(&ctx);
      X509V3_set_ctx(&ctx, issuer_cert ? issuer_cert : x, x, NULL, NULL, 0);
      X509_EXTENSION* ext = NULL;
      if (is_ca)
      {
        ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_basic_constraints, "critical,CA:TRUE");
      }
      else if (san != NULL)
      {
        ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_subject_alt_name, san);
      }
      if (is_ca || san != NULL)
      {
        if (ext == NULL)
        {
          break;
        }
        int added = X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
        if (added != 1)
        {
          break;
        }
      }
    }

    if (X509_sign(x, issuer_key, EVP_sha256()) == 0)
    {
      break;
    }
    ok = 1;
  } while (0);

  if (name != NULL)
  {
    X509_NAME_free(name);
  }
  if (!ok)
  {
    X509_free(x);
    return NULL;
  }
  return x;
}
#endif /* AZ_IOT_TEST_PROXY_TLS */

static void proxy_pump(
    struct az_iot_test_proxy* m,
    proxy_sock client,
    void* client_ssl,
    proxy_sock upstream)
{
  char buf[8192];
  for (;;)
  {
    proxy_lock(&m->lock);
    int stop = m->should_stop;
    int drop = m->drop_requested;
    m->drop_requested = 0;
    unsigned delay = m->write_delay_ms;
    size_t fragment_max = m->fragment_max;
    proxy_unlock(&m->lock);

    if (stop || drop)
    {
      goto done;
    }

    int client_ready = 0;
    int upstream_ready = 0;
    int sel = proxy_wait_readable(client, upstream, &client_ready, &upstream_ready, 200);
    if (sel < 0)
    {
      goto done;
    }

    if (client_ready)
    {
      /* Drain the client side, including any bytes buffered inside the TLS
       * session that select() cannot see. */
      do
      {
        long n = proxy_client_recv(client, client_ssl, buf, sizeof(buf));
        if (n <= 0)
        {
          goto done;
        }
        uint32_t new_packets = 0;
        proxy_count_packets(m, buf, (size_t)n, &new_packets);
        if (proxy_forward(upstream, buf, (size_t)n, delay, fragment_max) != 0)
        {
          goto done;
        }

        int fire = 0;
        proxy_lock(&m->lock);
        m->conn_c2b_bytes += (uint64_t)n;
        m->total_bytes += (uint64_t)n;
        m->conn_c2b_packets += new_packets;
        m->total_packets += new_packets;
        if (m->reset_after_bytes > 0 && m->conn_c2b_bytes >= m->reset_after_bytes)
        {
          fire = 1;
          m->reset_after_bytes = 0;
        }
        if (m->reset_after_packets > 0 && m->conn_c2b_packets >= m->reset_after_packets)
        {
          fire = 1;
          m->reset_after_packets = 0;
        }
        proxy_unlock(&m->lock);
        if (fire)
        {
          goto done;
        }
      } while (proxy_ssl_pending(client_ssl));
    }

    if (upstream_ready)
    {
      long n = proxy_recv(upstream, buf, sizeof(buf));
      if (n <= 0)
      {
        goto done;
      }
      if (proxy_client_send_all(client, client_ssl, buf, (size_t)n) != 0)
      {
        goto done;
      }
      proxy_lock(&m->lock);
      m->total_bytes += (uint64_t)n;
      proxy_unlock(&m->lock);
    }
  }

done:
  proxy_client_free(client_ssl);
  proxy_hard_close(client);
  proxy_hard_close(upstream);
}

static void proxy_send_synthetic(
    proxy_sock client,
    void* client_ssl,
    const uint8_t* data,
    size_t len,
    unsigned delay_ms)
{
  if (delay_ms > 0)
  {
    proxy_sleep_ms(delay_ms);
  }
  (void)proxy_client_send_all(client, client_ssl, (const char*)data, len);
}

/* Synthetic-broker pump (C3): answer the client's CONNECT with a canned CONNACK,
 * optionally inject a DISCONNECT, then stay silent (never emit PINGRESP) so an
 * idle client with a short keep-alive times out. No upstream is used. */
static void proxy_pump_synthetic(struct az_iot_test_proxy* m, proxy_sock client, void* client_ssl)
{
  char buf[8192];

  uint8_t connack[64];
  uint8_t disconnect[64];
  size_t connack_len;
  size_t disconnect_len;
  unsigned disconnect_delay;
  proxy_lock(&m->lock);
  connack_len = m->connack_len;
  memcpy(connack, m->connack, connack_len);
  disconnect_len = m->disconnect_len;
  memcpy(disconnect, m->disconnect, disconnect_len);
  disconnect_delay = m->disconnect_delay_ms;
  proxy_unlock(&m->lock);

  /* Wait for the client's first complete packet (its CONNECT). */
  int have_connect = 0;
  while (!have_connect)
  {
    proxy_lock(&m->lock);
    int stop = m->should_stop;
    proxy_unlock(&m->lock);
    if (stop)
    {
      goto done;
    }
    if (proxy_wait_readable_one(client, 200) <= 0)
    {
      continue;
    }
    long n = proxy_client_recv(client, client_ssl, buf, sizeof(buf));
    if (n <= 0)
    {
      goto done;
    }
    uint32_t new_packets = 0;
    proxy_count_packets(m, buf, (size_t)n, &new_packets);
    proxy_lock(&m->lock);
    m->total_bytes += (uint64_t)n;
    m->total_packets += new_packets;
    proxy_unlock(&m->lock);
    if (new_packets > 0)
    {
      have_connect = 1;
    }
  }

  /* Answer with the canned CONNACK, then optionally inject a DISCONNECT. */
  proxy_send_synthetic(client, client_ssl, connack, connack_len, 0);
  if (disconnect_len > 0)
  {
    proxy_send_synthetic(client, client_ssl, disconnect, disconnect_len, disconnect_delay);
    /* A real server closes the network connection after sending DISCONNECT. Give
     * the client a moment to read the packet, then close so it is surfaced. */
    proxy_sleep_ms(200);
    goto done;
  }

  /* Stay silent: drain and discard client packets (no PINGRESP) until the
   * client closes or we are asked to stop or drop. */
  for (;;)
  {
    proxy_lock(&m->lock);
    int stop = m->should_stop;
    int drop = m->drop_requested;
    m->drop_requested = 0;
    proxy_unlock(&m->lock);
    if (stop || drop)
    {
      goto done;
    }
    if (proxy_wait_readable_one(client, 200) <= 0)
    {
      continue;
    }
    long n = proxy_client_recv(client, client_ssl, buf, sizeof(buf));
    if (n <= 0)
    {
      goto done;
    }
    proxy_lock(&m->lock);
    m->total_bytes += (uint64_t)n;
    proxy_unlock(&m->lock);
  }

done:
  proxy_client_free(client_ssl);
  proxy_hard_close(client);
}

static void proxy_run(struct az_iot_test_proxy* m)
{
  for (;;)
  {
    proxy_lock(&m->lock);
    int stop = m->should_stop;
    proxy_unlock(&m->lock);
    if (stop)
    {
      break;
    }

    proxy_sock client = proxy_accept_timeout(m->listen_sock, 200);
    if (client == PROXY_INVALID_SOCK)
    {
      continue; /* timeout or transient accept error */
    }

    proxy_lock(&m->lock);
    ++m->connections;
    m->conn_c2b_bytes = 0;
    m->conn_c2b_packets = 0;
    m->pkt_phase = 0;
    m->pkt_rem = 0;
    m->pkt_mult = 1;
    m->pkt_body_left = 0;
    proxy_unlock(&m->lock);

    /* Terminate TLS on the client side when enabled. A handshake failure is the
     * expected outcome of the negative certificate tests: drop and move on. */
    void* client_ssl = NULL;
#if defined(AZ_IOT_TEST_PROXY_TLS)
    proxy_lock(&m->lock);
    SSL_CTX* ctx = m->ssl_ctx;
    proxy_unlock(&m->lock);
    if (ctx != NULL)
    {
      SSL* ssl = SSL_new(ctx);
      if (ssl == NULL)
      {
        proxy_hard_close(client);
        continue;
      }
      SSL_set_fd(ssl, (int)client);
      if (SSL_accept(ssl) != 1)
      {
        SSL_free(ssl);
        proxy_hard_close(client);
        continue;
      }
      client_ssl = ssl;
    }
#endif

    /* Synthetic-broker mode: when a CONNACK has been injected, answer the
     * client's CONNECT ourselves (no upstream) to drive CONNACK-code routing,
     * server DISCONNECT and keep-alive handling. */
    int synthetic;
    proxy_lock(&m->lock);
    synthetic = (m->connack_len > 0);
    proxy_unlock(&m->lock);
    if (synthetic)
    {
      proxy_pump_synthetic(m, client, client_ssl);
      continue;
    }

    proxy_sock upstream = proxy_connect_upstream(m);
    if (upstream == PROXY_INVALID_SOCK)
    {
      proxy_client_free(client_ssl);
      proxy_hard_close(client);
      continue;
    }

    proxy_pump(m, client, client_ssl, upstream);
  }
}

#if defined(_WIN32)
static DWORD WINAPI proxy_thread_entry(LPVOID arg)
{
  proxy_run((struct az_iot_test_proxy*)arg);
  return 0;
}
#else
static void* proxy_thread_entry(void* arg)
{
  proxy_run((struct az_iot_test_proxy*)arg);
  return NULL;
}
#endif

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

az_iot_test_proxy_options az_iot_test_proxy_options_default(void)
{
  az_iot_test_proxy_options o;
  o.upstream_host = NULL;
  o.upstream_port = 0;
  return o;
}

az_iot_test_proxy_tls_options az_iot_test_proxy_tls_options_default(void)
{
  az_iot_test_proxy_tls_options o;
  o.leaf_san = "IP:127.0.0.1";
  o.not_before_offset_sec = 0;
  o.not_after_offset_sec = 0;
  o.sign_with_untrusted_ca = 0;
  return o;
}

int az_iot_test_proxy_tls_supported(void)
{
#if defined(AZ_IOT_TEST_PROXY_TLS)
  return 1;
#else
  return 0;
#endif
}

int az_iot_test_proxy_enable_tls(az_iot_test_proxy* m, const az_iot_test_proxy_tls_options* tls_in)
{
#if !defined(AZ_IOT_TEST_PROXY_TLS)
  (void)m;
  (void)tls_in;
  return -1;
#else
  if (m == NULL)
  {
    return -1;
  }

  az_iot_test_proxy_tls_options tls
      = (tls_in != NULL) ? *tls_in : az_iot_test_proxy_tls_options_default();
  const char* san
      = (tls.leaf_san != NULL && tls.leaf_san[0] != '\0') ? tls.leaf_san : "IP:127.0.0.1";
  long nb = tls.not_before_offset_sec;
  long na = tls.not_after_offset_sec;
  if (nb == 0 && na == 0)
  {
    nb = -3600;
    na = 24L * 3600L;
  }

  EVP_PKEY* ca_key = NULL;
  X509* ca_cert = NULL;
  EVP_PKEY* bogus_key = NULL;
  X509* bogus_cert = NULL;
  EVP_PKEY* leaf_key = NULL;
  X509* leaf_cert = NULL;
  EVP_PKEY* signer_key = NULL;
  X509* signer_cert = NULL;
  SSL_CTX* ctx = NULL;
  char ca_local[sizeof(m->ca_pem)];
  size_t ca_local_len = 0;
  int rc = -1;

  ca_key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  if (ca_key == NULL)
  {
    goto done;
  }
  ca_cert = proxy_make_cert(
      ca_key,
      ca_key,
      NULL,
      "az-iot-test-proxy-trusted-ca",
      NULL,
      1,
      1,
      -3600,
      10L * 365 * 24 * 3600);
  if (ca_cert == NULL)
  {
    goto done;
  }
  signer_key = ca_key;
  signer_cert = ca_cert;

  if (tls.sign_with_untrusted_ca)
  {
    bogus_key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
    if (bogus_key == NULL)
    {
      goto done;
    }
    bogus_cert = proxy_make_cert(
        bogus_key,
        bogus_key,
        NULL,
        "az-iot-test-proxy-untrusted-ca",
        NULL,
        1,
        3,
        -3600,
        10L * 365 * 24 * 3600);
    if (bogus_cert == NULL)
    {
      goto done;
    }
    signer_key = bogus_key;
    signer_cert = bogus_cert;
  }

  leaf_key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  if (leaf_key == NULL)
  {
    goto done;
  }
  leaf_cert = proxy_make_cert(
      leaf_key, signer_key, signer_cert, "az-iot-test-proxy-leaf", san, 0, 2, nb, na);
  if (leaf_cert == NULL)
  {
    goto done;
  }

  /* Export the trusted CA (always the one we did NOT sign a bogus leaf with)
   * into a local buffer; it is published into the struct under the lock below,
   * atomically with ssl_ctx, so a concurrent az_iot_test_proxy_ca_pem() never sees a
   * half-written PEM. */
  {
    BIO* b = BIO_new(BIO_s_mem());
    if (b == NULL)
    {
      goto done;
    }
    if (PEM_write_bio_X509(b, ca_cert) != 1)
    {
      BIO_free(b);
      goto done;
    }
    char* data = NULL;
    long n = BIO_get_mem_data(b, &data);
    if (n <= 0 || (size_t)n >= sizeof(ca_local))
    {
      BIO_free(b);
      goto done;
    }
    memcpy(ca_local, data, (size_t)n);
    ca_local[n] = '\0';
    ca_local_len = (size_t)n;
    BIO_free(b);
  }

  ctx = SSL_CTX_new(TLS_server_method());
  if (ctx == NULL)
  {
    goto done;
  }
  if (SSL_CTX_use_certificate(ctx, leaf_cert) != 1)
  {
    goto done;
  }
  if (SSL_CTX_use_PrivateKey(ctx, leaf_key) != 1)
  {
    goto done;
  }
  if (tls.sign_with_untrusted_ca)
  {
    /* Present the full (untrusted) chain so the client fails on trust, not on a
     * missing intermediate. add_extra_chain_cert takes ownership on success. */
    if (SSL_CTX_add_extra_chain_cert(ctx, bogus_cert) != 1)
    {
      goto done;
    }
    bogus_cert = NULL;
  }

  proxy_lock(&m->lock);
  if (m->ssl_ctx != NULL)
  {
    SSL_CTX_free(m->ssl_ctx);
  }
  m->ssl_ctx = ctx;
  ctx = NULL;
  memcpy(m->ca_pem, ca_local, ca_local_len);
  m->ca_pem[ca_local_len] = '\0';
  m->ca_pem_len = ca_local_len;
  proxy_unlock(&m->lock);
  rc = 0;

done:
  if (ctx != NULL)
  {
    SSL_CTX_free(ctx);
  }
  if (leaf_cert != NULL)
  {
    X509_free(leaf_cert);
  }
  if (leaf_key != NULL)
  {
    EVP_PKEY_free(leaf_key);
  }
  if (bogus_cert != NULL)
  {
    X509_free(bogus_cert);
  }
  if (bogus_key != NULL)
  {
    EVP_PKEY_free(bogus_key);
  }
  if (ca_cert != NULL)
  {
    X509_free(ca_cert);
  }
  if (ca_key != NULL)
  {
    EVP_PKEY_free(ca_key);
  }
  return rc;
#endif
}

size_t az_iot_test_proxy_ca_pem(az_iot_test_proxy* m, char* out, size_t cap)
{
#if defined(AZ_IOT_TEST_PROXY_TLS)
  if (m == NULL || out == NULL || cap == 0)
  {
    return 0;
  }
  proxy_lock(&m->lock);
  size_t n = m->ca_pem_len;
  if (n == 0 || n + 1 > cap)
  {
    proxy_unlock(&m->lock);
    return 0;
  }
  memcpy(out, m->ca_pem, n);
  out[n] = '\0';
  proxy_unlock(&m->lock);
  return n;
#else
  (void)m;
  (void)out;
  (void)cap;
  return 0;
#endif
}

int az_iot_test_proxy_start(
    const az_iot_test_proxy_options* options,
    az_iot_test_proxy** out_proxy,
    uint16_t* out_port)
{
  if (options == NULL || out_proxy == NULL || options->upstream_host == NULL
      || options->upstream_port == 0)
  {
    return -1;
  }

  size_t host_len = strlen(options->upstream_host);
  if (host_len >= sizeof(((struct az_iot_test_proxy*)0)->upstream_host))
  {
    return -1;
  }

  proxy_ensure_wsa();

  struct az_iot_test_proxy* m = (struct az_iot_test_proxy*)calloc(1, sizeof(*m));
  if (m == NULL)
  {
    return -1;
  }
  m->listen_sock = PROXY_INVALID_SOCK;
  m->pkt_mult = 1;
  memcpy(m->upstream_host, options->upstream_host, host_len + 1);
  (void)snprintf(
      m->upstream_port, sizeof(m->upstream_port), "%u", (unsigned)options->upstream_port);
  proxy_mutex_init(&m->lock);

  if (proxy_open_listener(m) != 0)
  {
    proxy_mutex_destroy(&m->lock);
    free(m);
    return -1;
  }

#if defined(_WIN32)
  m->thread = CreateThread(NULL, 0, proxy_thread_entry, m, 0, NULL);
  if (m->thread == NULL)
#else
  if (pthread_create(&m->thread, NULL, proxy_thread_entry, m) != 0)
#endif
  {
    proxy_closesock(m->listen_sock);
    proxy_mutex_destroy(&m->lock);
    free(m);
    return -1;
  }
#if !defined(_WIN32)
  m->thread_started = 1;
#endif

  if (out_port != NULL)
  {
    *out_port = m->listen_port;
  }
  *out_proxy = m;
  return 0;
}

void az_iot_test_proxy_reset_after_bytes(az_iot_test_proxy* m, uint64_t bytes)
{
  proxy_lock(&m->lock);
  m->reset_after_bytes = bytes;
  proxy_unlock(&m->lock);
}

void az_iot_test_proxy_reset_after_packets(az_iot_test_proxy* m, uint32_t packets)
{
  proxy_lock(&m->lock);
  m->reset_after_packets = packets;
  proxy_unlock(&m->lock);
}

void az_iot_test_proxy_drop_now(az_iot_test_proxy* m)
{
  proxy_lock(&m->lock);
  m->drop_requested = 1;
  proxy_unlock(&m->lock);
}

void az_iot_test_proxy_set_write_delay_ms(az_iot_test_proxy* m, unsigned delay_ms)
{
  proxy_lock(&m->lock);
  m->write_delay_ms = delay_ms;
  proxy_unlock(&m->lock);
}

void az_iot_test_proxy_set_fragment(az_iot_test_proxy* m, size_t max_chunk)
{
  proxy_lock(&m->lock);
  m->fragment_max = max_chunk;
  proxy_unlock(&m->lock);
}

int az_iot_test_proxy_set_synthetic_connack(
    az_iot_test_proxy* m,
    const uint8_t* connack,
    size_t len)
{
  if (m == NULL || connack == NULL || len == 0 || len > sizeof(m->connack))
  {
    return -1;
  }
  proxy_lock(&m->lock);
  memcpy(m->connack, connack, len);
  m->connack_len = len;
  proxy_unlock(&m->lock);
  return 0;
}

int az_iot_test_proxy_set_synthetic_disconnect(
    az_iot_test_proxy* m,
    const uint8_t* disconnect,
    size_t len,
    unsigned delay_ms)
{
  if (m == NULL || disconnect == NULL || len == 0 || len > sizeof(m->disconnect))
  {
    return -1;
  }
  proxy_lock(&m->lock);
  memcpy(m->disconnect, disconnect, len);
  m->disconnect_len = len;
  m->disconnect_delay_ms = delay_ms;
  proxy_unlock(&m->lock);
  return 0;
}

uint64_t az_iot_test_proxy_bytes_forwarded(az_iot_test_proxy* m)
{
  proxy_lock(&m->lock);
  uint64_t v = m->total_bytes;
  proxy_unlock(&m->lock);
  return v;
}

uint32_t az_iot_test_proxy_packets_seen(az_iot_test_proxy* m)
{
  proxy_lock(&m->lock);
  uint32_t v = m->total_packets;
  proxy_unlock(&m->lock);
  return v;
}

uint32_t az_iot_test_proxy_connections(az_iot_test_proxy* m)
{
  proxy_lock(&m->lock);
  uint32_t v = m->connections;
  proxy_unlock(&m->lock);
  return v;
}

void az_iot_test_proxy_stop(az_iot_test_proxy* m)
{
  if (m == NULL)
  {
    return;
  }

  proxy_lock(&m->lock);
  m->should_stop = 1;
  proxy_unlock(&m->lock);

#if defined(_WIN32)
  if (m->thread != NULL)
  {
    WaitForSingleObject(m->thread, INFINITE);
    CloseHandle(m->thread);
  }
#else
  if (m->thread_started)
  {
    pthread_join(m->thread, NULL);
  }
#endif

  if (m->listen_sock != PROXY_INVALID_SOCK)
  {
    proxy_closesock(m->listen_sock);
  }
#if defined(AZ_IOT_TEST_PROXY_TLS)
  if (m->ssl_ctx != NULL)
  {
    SSL_CTX_free(m->ssl_ctx);
  }
#endif
  proxy_mutex_destroy(&m->lock);
  free(m);
}
