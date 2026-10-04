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
#include <signal.h>
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
/* monotonic clock and deterministic RNG                                     */
/* ------------------------------------------------------------------------- */

/* Milliseconds from an unspecified origin. Monotonic, so it is unaffected by a
 * wall-clock step; scheduling must never go backwards mid-test. */
static uint64_t proxy_now_ms(void)
{
#if defined(_WIN32)
  return (uint64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

/* xorshift32. Seeded per direction so a jitter sequence replays exactly; an
 * impairment failure that cannot be reproduced is worse than no test. */
static uint32_t proxy_rand(uint32_t* state)
{
  uint32_t x = (*state != 0u) ? *state : 0x9E3779B9u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

/* ------------------------------------------------------------------------- */
/* egress scheduling                                                         */
/* ------------------------------------------------------------------------- */

/* Buffered bytes waiting for their release time. Shaping cannot be done by
 * sleeping inline in the pump: that blocks the opposite direction too, turning
 * "200 ms of latency" into "a 200 ms freeze of the whole session". Each chunk
 * instead carries a release deadline and the pump sends whatever is due. */
typedef struct proxy_chunk
{
  struct proxy_chunk* next;
  uint64_t release_at_ms;
  size_t len;
  size_t sent; /* partial writes resume here */
  char* data; /* points just past this header, in the same allocation */
} proxy_chunk;

/* Backpressure bound. Reached only when a bandwidth ceiling is far below the
 * offered load; the pump then stops reading that source, which is what a real
 * congested link does. Dropping buffered bytes instead would corrupt the
 * stream. */
#define PROXY_EGRESS_MAX_QUEUED (256u * 1024u)

typedef struct proxy_egress
{
  proxy_chunk* head;
  proxy_chunk* tail;
  uint64_t queued;
  double tokens; /* token bucket, bytes */
  uint64_t tokens_at_ms;
  uint32_t rng;
  uint32_t seed_applied; /* the seed `rng` was derived from */
  int seeded;
  uint64_t writes; /* drained into the proxy's cumulative counter */
} proxy_egress;

/* A rule plus the bookkeeping the caller does not supply: `bytes` is copied so
 * the caller's buffer need not outlive the call, `seen` counts matches of the
 * trigger including those skipped, and `hits` counts the times it fired. */
typedef struct proxy_rule
{
  az_iot_test_proxy_rule spec;
  uint8_t bytes[AZ_IOT_TEST_PROXY_RULE_BYTES_MAX];
  size_t bytes_len;
  uint32_t seen;
  uint32_t hits;
} proxy_rule;

/* ------------------------------------------------------------------------- */
/* proxy state                                                               */
/* ------------------------------------------------------------------------- */

struct az_iot_test_proxy
{
  /* config, immutable after start */
  char upstream_host[256];
  char upstream_port[8];
  int http_connect;
  int opaque_stream;
  char required_username[128];
  char required_password[128];
  int require_auth;
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
  /* Per-direction impairment, indexed by az_iot_test_proxy_direction. */
  az_iot_test_proxy_impairment imp[2];
  uint64_t stall_until_ms[2];
  uint64_t queued_snapshot[2]; /* published for az_iot_test_proxy_queued_bytes */
  uint64_t writes[2];

  proxy_rule rules[AZ_IOT_TEST_PROXY_RULES_MAX];
  size_t rule_count;

  uint64_t conn_c2b_bytes; /* client->broker bytes, current connection */
  uint32_t conn_c2b_packets; /* client->broker MQTT packets, current connection */
  uint64_t total_bytes; /* both directions, cumulative */
  uint32_t total_packets; /* client->broker packets, cumulative */
  uint32_t connections; /* accepted client connections */
  uint32_t tunnels_opened; /* CONNECT requests answered 2xx (http_connect only) */
  uint32_t auth_failures; /* CONNECT requests refused 407 (http_connect only) */
  char last_connect_target[300]; /* authority from the last CONNECT request line */
  int have_connect_target;

  /* Decoded terms of the current session: the client's CONNECT and the reason
   * code of its DISCONNECT. Reset when a client connects. */
  az_iot_test_proxy_connect_fields connect_fields;
  int have_client_disconnect;
  uint8_t client_disconnect_reason;

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
  /* Retained so client certificates can be issued after the handshake material
   * is built: a CA that has already been freed cannot sign. */
  EVP_PKEY* ca_key;
  X509* ca_cert;
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

/* Dial host:port (both as strings), returning a connected socket or
 * PROXY_INVALID_SOCK. Shared by the passthrough upstream and by CONNECT mode,
 * which dials whatever authority the client named. */
static proxy_sock proxy_dial(const char* host, const char* port)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = NULL;
  if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL)
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

static proxy_sock proxy_connect_upstream(struct az_iot_test_proxy* m)
{
  return proxy_dial(m->upstream_host, m->upstream_port);
}

/* ------------------------------------------------------------------------- */
/* HTTP CONNECT mode                                                          */
/*                                                                            */
/* Enough of an HTTP proxy to tunnel a real MQTT session and to refuse one:    */
/* read the request line and headers, optionally check Basic credentials,      */
/* answer 200 or 407, then hand the socket to the ordinary pump. Deliberately  */
/* not a general HTTP implementation -- only the CONNECT method exists, and    */
/* only the headers this needs are parsed.                                     */
/* ------------------------------------------------------------------------- */

static const char k_b64_alphabet[]
    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64-encode `len` bytes. Returns the length written, or 0 if it would not
 * fit. Used to build the credential this proxy EXPECTS, so the comparison is
 * against the same encoding a client must produce. */
static size_t proxy_b64_encode(const unsigned char* in, size_t len, char* out, size_t cap)
{
  size_t need = ((len + 2u) / 3u) * 4u;
  if (need + 1u > cap)
  {
    return 0;
  }
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3)
  {
    unsigned v = (unsigned)in[i] << 16;
    v |= (i + 1 < len) ? ((unsigned)in[i + 1] << 8) : 0u;
    v |= (i + 2 < len) ? (unsigned)in[i + 2] : 0u;
    out[o++] = k_b64_alphabet[(v >> 18) & 0x3Fu];
    out[o++] = k_b64_alphabet[(v >> 12) & 0x3Fu];
    out[o++] = (i + 1 < len) ? k_b64_alphabet[(v >> 6) & 0x3Fu] : '=';
    out[o++] = (i + 2 < len) ? k_b64_alphabet[v & 0x3Fu] : '=';
  }
  out[o] = '\0';
  return o;
}

/* Read the request head, up to and including the blank line that ends it.
 *
 * One byte at a time, so the tunnelled payload that may arrive in the same
 * segment is left in the socket: those bytes belong to the client's TLS
 * handshake with the real broker and must not be consumed here.
 *
 * Bounded, and it watches should_stop. An unbounded blocking recv would hang
 * the pump thread on a client that connects and then says nothing -- and
 * az_iot_test_proxy_stop() joins that thread, so the hang would take down the
 * whole test process at teardown instead of failing inside the test's own
 * timeout. `deadline_ms` bounds the head as a whole, not each byte, so a
 * dribbling client cannot extend it indefinitely. */
static int proxy_read_http_head(
    struct az_iot_test_proxy* m,
    proxy_sock s,
    char* buf,
    size_t cap,
    size_t* out_len,
    unsigned deadline_ms)
{
  size_t n = 0;
  /* One fixed end time, not an accumulator of idle waits: counting only the
   * timed-out selects would leave a peer that dribbles a byte every 199ms
   * inside its budget forever, bounded in practice only by the header cap. */
  const uint64_t end_ms = proxy_now_ms() + (uint64_t)deadline_ms;
  while (n + 1 < cap)
  {
    proxy_lock(&m->lock);
    int stop = m->should_stop;
    proxy_unlock(&m->lock);
    if (stop)
    {
      return -1;
    }
    if (proxy_now_ms() >= end_ms)
    {
      return -1;
    }

    int ready = proxy_wait_readable_one(s, 200);
    if (ready < 0)
    {
      return -1;
    }
    if (ready == 0)
    {
      continue;
    }

    char c;
    long r = (long)recv(s, &c, 1, 0);
    if (r != 1)
    {
      return -1;
    }
    buf[n++] = c;
    if (n >= 4 && memcmp(buf + n - 4, "\r\n\r\n", 4) == 0)
    {
      buf[n] = '\0';
      *out_len = n;
      return 0;
    }
  }
  return -1;
}

/* Case-insensitive search for a header line, returning its value (trimmed of
 * leading spaces) or NULL. `head` is NUL-terminated. */
static const char* proxy_find_header(const char* head, const char* name)
{
  size_t name_len = strlen(name);
  const char* p = head;
  while (*p != '\0')
  {
    const char* line_end = strstr(p, "\r\n");
    if (line_end == NULL || line_end == p)
    {
      break;
    }
    size_t line_len = (size_t)(line_end - p);
    if (line_len > name_len)
    {
      size_t i = 0;
      for (; i < name_len; ++i)
      {
        char a = p[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z')
        {
          a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z')
        {
          b = (char)(b - 'A' + 'a');
        }
        if (a != b)
        {
          break;
        }
      }
      if (i == name_len && p[name_len] == ':')
      {
        const char* v = p + name_len + 1;
        while (*v == ' ' || *v == '\t')
        {
          ++v;
        }
        return v;
      }
    }
    p = line_end + 2;
  }
  return NULL;
}

/* Perform the CONNECT exchange. On success returns a connected upstream socket
 * and the tunnel is open; on any refusal the client has been answered and the
 * caller closes it. */
static proxy_sock proxy_http_connect_accept(struct az_iot_test_proxy* m, proxy_sock client)
{
  char head[2048];
  size_t head_len = 0;
  /* 10s is generous for a request a real client sends in one segment, and short
   * enough that a stuck client fails well inside a test's own timeout. */
  if (proxy_read_http_head(m, client, head, sizeof(head), &head_len, 10000u) != 0)
  {
    return PROXY_INVALID_SOCK;
  }

  /* Request line: "CONNECT host:port HTTP/1.1". Only CONNECT is implemented;
   * anything else is answered 405 so a misdirected client fails loudly rather
   * than hanging. */
  if (strncmp(head, "CONNECT ", 8) != 0)
  {
    static const char k_405[] = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
    (void)proxy_send_all(client, k_405, sizeof(k_405) - 1);
    return PROXY_INVALID_SOCK;
  }

  const char* authority = head + 8;
  const char* sp = strchr(authority, ' ');
  if (sp == NULL || (size_t)(sp - authority) >= sizeof(m->last_connect_target))
  {
    return PROXY_INVALID_SOCK;
  }
  size_t auth_len = (size_t)(sp - authority);

  char target[300];
  memcpy(target, authority, auth_len);
  target[auth_len] = '\0';

  proxy_lock(&m->lock);
  memcpy(m->last_connect_target, target, auth_len + 1);
  m->have_connect_target = 1;
  int require_auth = m->require_auth;
  char want_user[128];
  char want_pass[128];
  memcpy(want_user, m->required_username, sizeof(want_user));
  memcpy(want_pass, m->required_password, sizeof(want_pass));
  proxy_unlock(&m->lock);

  if (require_auth)
  {
    /* Build the credential this proxy expects and compare against what arrived.
     * Comparing the ENCODED forms keeps the check in one place and means a
     * client that percent-mangles a delimiter mismatches here. */
    char plain[264];
    int plain_len = snprintf(plain, sizeof(plain), "%s:%s", want_user, want_pass);
    char expected[360];
    size_t expected_len = 0;
    if (plain_len > 0 && (size_t)plain_len < sizeof(plain))
    {
      expected_len = proxy_b64_encode(
          (const unsigned char*)plain, (size_t)plain_len, expected, sizeof(expected));
    }

    const char* got = proxy_find_header(head, "proxy-authorization");
    int ok = 0;
    if (got != NULL && expected_len > 0 && strncmp(got, "Basic ", 6) == 0)
    {
      const char* got_b64 = got + 6;
      size_t got_len = 0;
      while (got_b64[got_len] != '\0' && got_b64[got_len] != '\r' && got_b64[got_len] != ' ')
      {
        ++got_len;
      }
      ok = (got_len == expected_len) && (memcmp(got_b64, expected, expected_len) == 0);
    }

    if (!ok)
    {
      proxy_lock(&m->lock);
      m->auth_failures++;
      proxy_unlock(&m->lock);
      static const char k_407[] = "HTTP/1.1 407 Proxy Authentication Required\r\n"
                                  "Proxy-Authenticate: Basic realm=\"az-iot-test-proxy\"\r\n"
                                  "Content-Length: 0\r\n\r\n";
      (void)proxy_send_all(client, k_407, sizeof(k_407) - 1);
      return PROXY_INVALID_SOCK;
    }
  }

  /* Split "host:port". A missing port is a malformed CONNECT here: the client
   * always knows the port it wants. */
  char host[256];
  const char* colon = strrchr(target, ':');
  if (colon == NULL || colon == target || (size_t)(colon - target) >= sizeof(host))
  {
    static const char k_400[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
    (void)proxy_send_all(client, k_400, sizeof(k_400) - 1);
    return PROXY_INVALID_SOCK;
  }
  size_t host_len = (size_t)(colon - target);
  memcpy(host, target, host_len);
  host[host_len] = '\0';

  proxy_sock upstream = proxy_dial(host, colon + 1);
  if (upstream == PROXY_INVALID_SOCK)
  {
    static const char k_502[] = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
    (void)proxy_send_all(client, k_502, sizeof(k_502) - 1);
    return PROXY_INVALID_SOCK;
  }

  static const char k_200[] = "HTTP/1.1 200 Connection established\r\n\r\n";
  if (proxy_send_all(client, k_200, sizeof(k_200) - 1) != 0)
  {
    proxy_closesock(upstream);
    return PROXY_INVALID_SOCK;
  }

  proxy_lock(&m->lock);
  m->tunnels_opened++;
  proxy_unlock(&m->lock);
  return upstream;
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

/* ------------------------------------------------------------------------- */
/* egress queue (pump thread only, except the config it copies under lock)    */
/* ------------------------------------------------------------------------- */

static void eg_clear(proxy_egress* e)
{
  proxy_chunk* c = e->head;
  while (c != NULL)
  {
    proxy_chunk* next = c->next;
    free(c);
    c = next;
  }
  e->head = NULL;
  e->tail = NULL;
  e->queued = 0;
}

/* Split `buf` into release-scheduled chunks. Deadlines never decrease along the
 * queue: jitter may make a chunk's own deadline earlier than its predecessor's,
 * and letting it overtake would reorder a TCP byte stream rather than model a
 * network. Returns -1 only on allocation failure. */
static int eg_push(
    proxy_egress* e,
    const az_iot_test_proxy_impairment* imp,
    const char* buf,
    size_t n)
{
  size_t frag = (imp->fragment_max > 0 && imp->fragment_max < n) ? imp->fragment_max : n;
  if (frag == 0)
  {
    frag = n;
  }
  const uint64_t now = proxy_now_ms();

  size_t off = 0;
  while (off < n)
  {
    size_t chunk = n - off;
    if (chunk > frag)
    {
      chunk = frag;
    }

    proxy_chunk* q = (proxy_chunk*)malloc(sizeof(*q) + chunk);
    if (q == NULL)
    {
      return -1;
    }

    uint64_t delay = (uint64_t)imp->base_delay_ms;
    if (imp->jitter_ms > 0)
    {
      delay += (uint64_t)(proxy_rand(&e->rng) % ((uint32_t)imp->jitter_ms + 1u));
    }
    uint64_t release = now + delay;
    if (e->tail != NULL && release < e->tail->release_at_ms)
    {
      release = e->tail->release_at_ms;
    }

    q->next = NULL;
    q->release_at_ms = release;
    q->len = chunk;
    q->sent = 0;
    q->data = (char*)(q + 1);
    memcpy(q->data, buf + off, chunk);

    if (e->tail == NULL)
    {
      e->head = q;
      e->tail = q;
    }
    else
    {
      e->tail->next = q;
      e->tail = q;
    }
    e->queued += (uint64_t)chunk;
    off += chunk;
  }
  return 0;
}

/* Absolute time the pump next has useful work for this queue, or 0 when the
 * queue is empty. Returning the head's deadline alone would spin once the
 * deadline has passed but the token bucket is empty, so a rate-limited queue
 * reports when it can next afford a byte. */
static uint64_t eg_next_wakeup_ms(
    const proxy_egress* e,
    const az_iot_test_proxy_impairment* imp,
    uint64_t now)
{
  if (e->head == NULL)
  {
    return 0;
  }
  if (e->head->release_at_ms > now)
  {
    return e->head->release_at_ms;
  }
  if (imp->bytes_per_sec == 0 || e->tokens >= 1.0)
  {
    return now;
  }
  double ms = (1.0 - e->tokens) * 1000.0 / (double)imp->bytes_per_sec;
  return now + (uint64_t)ms + 1u;
}

/* Send everything that is due, honouring the bandwidth ceiling and any stall.
 * `ssl` is non-NULL only for the broker->client direction under TLS. Returns -1
 * if the destination died. */
static int eg_flush(
    proxy_egress* e,
    const az_iot_test_proxy_impairment* imp,
    uint64_t stall_until_ms,
    proxy_sock dst,
    void* ssl)
{
  const uint64_t now = proxy_now_ms();
  if (now < stall_until_ms)
  {
    return 0;
  }

  /* Refill the bucket for the elapsed interval before spending from it. The
   * bucket starts empty and holds at most 100 ms of credit: a full second of
   * burst would let every payload a test can hold in memory through instantly,
   * which is indistinguishable from having no ceiling at all. */
  if (imp->bytes_per_sec > 0)
  {
    if (e->tokens_at_ms == 0)
    {
      e->tokens_at_ms = now;
    }
    else if (now > e->tokens_at_ms)
    {
      e->tokens += (double)(now - e->tokens_at_ms) * (double)imp->bytes_per_sec / 1000.0;
      e->tokens_at_ms = now;
    }
    double burst = (double)imp->bytes_per_sec / 10.0;
    if (burst < 1.0)
    {
      burst = 1.0;
    }
    if (e->tokens > burst)
    {
      e->tokens = burst;
    }
  }

  while (e->head != NULL && e->head->release_at_ms <= now)
  {
    proxy_chunk* c = e->head;
    size_t remaining = c->len - c->sent;
    size_t allowed = remaining;

    if (imp->bytes_per_sec > 0)
    {
      if (e->tokens < 1.0)
      {
        break; /* no budget this pass; the pump will come back */
      }
      if ((double)allowed > e->tokens)
      {
        allowed = (size_t)e->tokens;
      }
    }

    int rc;
    if (ssl != NULL)
    {
      rc = proxy_client_send_all(dst, ssl, c->data + c->sent, allowed);
    }
    else
    {
      rc = proxy_send_all(dst, c->data + c->sent, allowed);
    }
    if (rc != 0)
    {
      return -1;
    }
    ++e->writes;

    if (imp->bytes_per_sec > 0)
    {
      e->tokens -= (double)allowed;
    }
    c->sent += allowed;
    e->queued -= (uint64_t)allowed;

    if (c->sent >= c->len)
    {
      e->head = c->next;
      if (e->head == NULL)
      {
        e->tail = NULL;
      }
      free(c);
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* MQTT packet framing and the rule engine                                   */
/* ------------------------------------------------------------------------- */

/* Rules act on whole packets, so one is reassembled before the proxy decides
 * what to do with it. Anything larger is streamed through unmatched: the cap
 * stops a fault injector from turning into a store-and-forward broker, and no
 * conformance payload comes near it. */
#define PROXY_PKT_MAX 16384

typedef struct proxy_framer
{
  uint8_t buf[PROXY_PKT_MAX];
  size_t have;
  size_t need; /* total packet size, once the fixed header decodes */
  size_t oversized_left; /* bytes of an unbuffered packet still to stream */
} proxy_framer;

/* Decode the fixed header. Returns 1 with `*hdr_len` and `*total` filled, 0 if
 * more bytes are needed, or -1 if the remaining-length varint is malformed. */
static int proxy_frame_size(const uint8_t* buf, size_t have, size_t* hdr_len, size_t* total)
{
  if (have < 2)
  {
    return 0;
  }
  size_t rem = 0;
  size_t mult = 1;
  size_t i = 1;
  for (;;)
  {
    if (i > 4)
    {
      return -1; /* the varint is four bytes at most */
    }
    if (i >= have)
    {
      return 0;
    }
    uint8_t byte = buf[i];
    rem += (size_t)(byte & 0x7Fu) * mult;
    mult *= 128u;
    ++i;
    if ((byte & 0x80u) == 0u)
    {
      break;
    }
  }
  *hdr_len = i;
  *total = i + rem;
  return 1;
}

/* Count whole packets without buffering or forwarding them. The synthetic
 * broker never relays anything, so it needs the framing but not the bytes; a
 * fixed header is five bytes at most, hence no payload buffer here. */
typedef struct proxy_counter
{
  uint8_t hdr[5];
  size_t have;
  size_t body_left;
} proxy_counter;

static void proxy_frame_count(proxy_counter* pc, const char* data, size_t n, uint32_t* out_packets)
{
  const uint8_t* p = (const uint8_t*)data;
  size_t left = n;
  while (left > 0)
  {
    if (pc->body_left > 0)
    {
      size_t take = (left < pc->body_left) ? left : pc->body_left;
      pc->body_left -= take;
      p += take;
      left -= take;
      if (pc->body_left == 0)
      {
        ++(*out_packets);
        pc->have = 0;
      }
      continue;
    }

    pc->hdr[pc->have++] = *p++;
    --left;
    size_t hdr_len = 0;
    size_t total = 0;
    int r = proxy_frame_size(pc->hdr, pc->have, &hdr_len, &total);
    if (r < 0)
    {
      pc->have = 0;
    }
    else if (r == 1)
    {
      if (total == pc->have)
      {
        ++(*out_packets); /* nothing after the header, e.g. PINGREQ */
        pc->have = 0;
      }
      else
      {
        pc->body_left = total - pc->have;
      }
    }
  }
}

/* Extract the packet id an acknowledgement would have to echo. PUBLISH carries
 * it after the topic and only above QoS 0; the ack and subscribe families carry
 * it first. Returns 0 when the packet has no id. */
static int proxy_packet_id(const uint8_t* pkt, size_t len, uint8_t out[2])
{
  size_t hdr = 0;
  size_t total = 0;
  if (proxy_frame_size(pkt, len, &hdr, &total) != 1)
  {
    return 0;
  }
  uint8_t type = (uint8_t)(pkt[0] >> 4);
  size_t at;
  if (type == AZ_IOT_TEST_PROXY_PKT_PUBLISH)
  {
    if (((pkt[0] >> 1) & 0x03u) == 0u || hdr + 2 > len)
    {
      return 0;
    }
    at = hdr + 2 + (((size_t)pkt[hdr] << 8) | (size_t)pkt[hdr + 1]);
  }
  else if (
      type == AZ_IOT_TEST_PROXY_PKT_PUBACK || type == AZ_IOT_TEST_PROXY_PKT_PUBREC
      || type == AZ_IOT_TEST_PROXY_PKT_PUBREL || type == AZ_IOT_TEST_PROXY_PKT_PUBCOMP
      || type == AZ_IOT_TEST_PROXY_PKT_SUBSCRIBE || type == AZ_IOT_TEST_PROXY_PKT_SUBACK
      || type == AZ_IOT_TEST_PROXY_PKT_UNSUBSCRIBE || type == AZ_IOT_TEST_PROXY_PKT_UNSUBACK)
  {
    at = hdr;
  }
  else
  {
    return 0;
  }
  if (at + 2 > len)
  {
    return 0;
  }
  out[0] = pkt[at];
  out[1] = pkt[at + 1];
  return 1;
}

/* --- session-setup inspection: decode the FIELDS of the client's CONNECT and
 * DISCONNECT. Nothing is retained but the scalars in
 * az_iot_test_proxy_connect_fields; no PUBLISH is examined and no payload is
 * copied (a Will is reported by topic and length only; credentials by length
 * and digest only). --- */

/* Read an MQTT variable byte integer at *at. Returns 0 on a truncated or
 * over-long encoding, which ends decoding of the packet. */
static int proxy_mqtt_varint(const uint8_t* p, size_t len, size_t* at, uint32_t* out)
{
  uint32_t value = 0;
  uint32_t multiplier = 1;
  for (int i = 0; i < 4; ++i)
  {
    if (*at >= len)
    {
      return 0;
    }
    uint8_t b = p[(*at)++];
    value += (uint32_t)(b & 0x7Fu) * multiplier;
    if ((b & 0x80u) == 0)
    {
      *out = value;
      return 1;
    }
    multiplier *= 128u;
  }
  return 0;
}

/* Length in bytes of the value of MQTT 5 property `id` starting at *at, so an
 * unrecognised property between two that matter does not end the scan. Returns
 * 0 when the property cannot be spanned -- an id this table does not know, or a
 * length that runs past the packet -- and the caller stops there rather than
 * reading whatever follows as a property id. */
static int proxy_mqtt_skip_property(const uint8_t* p, size_t len, size_t* at, uint8_t id)
{
  size_t fixed = 0;
  switch (id)
  {
    case 0x01u: /* payload format indicator */
    case 0x17u: /* request problem information */
    case 0x19u: /* request response information */
    case 0x24u: /* maximum QoS */
    case 0x25u: /* retain available */
    case 0x28u: /* wildcard subscription available */
    case 0x29u: /* subscription identifier available */
    case 0x2Au: /* shared subscription available */
      fixed = 1;
      break;
    case 0x13u: /* server keep alive */
    case 0x21u: /* receive maximum */
    case 0x22u: /* topic alias maximum */
    case 0x23u: /* topic alias */
      fixed = 2;
      break;
    case 0x02u: /* message expiry interval */
    case 0x11u: /* session expiry interval */
    case 0x18u: /* will delay interval */
    case 0x27u: /* maximum packet size */
      fixed = 4;
      break;
    case 0x0Bu: /* subscription identifier (variable byte integer) */
    {
      uint32_t ignored = 0;
      return proxy_mqtt_varint(p, len, at, &ignored);
    }
    case 0x26u: /* user property: two length-prefixed strings */
    {
      for (int i = 0; i < 2; ++i)
      {
        if (*at + 2 > len)
        {
          return 0;
        }
        size_t n = ((size_t)p[*at] << 8) | (size_t)p[*at + 1];
        *at += 2;
        if (*at + n > len)
        {
          return 0;
        }
        *at += n;
      }
      return 1;
    }
    case 0x03u: /* content type */
    case 0x08u: /* response topic */
    case 0x09u: /* correlation data (binary, same framing) */
    case 0x12u: /* assigned client identifier */
    case 0x15u: /* authentication method */
    case 0x16u: /* authentication data (binary) */
    case 0x1Au: /* response information */
    case 0x1Cu: /* server reference */
    case 0x1Fu: /* reason string */
    {
      if (*at + 2 > len)
      {
        return 0;
      }
      size_t n = ((size_t)p[*at] << 8) | (size_t)p[*at + 1];
      *at += 2;
      if (*at + n > len)
      {
        return 0;
      }
      *at += n;
      return 1;
    }
    default:
      return 0;
  }
  if (*at + fixed > len)
  {
    return 0;
  }
  *at += fixed;
  return 1;
}

static uint32_t proxy_be32(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Walk one MQTT 5 property block, recording the one property the caller cares
 * about (`want`) and stepping over everything else. `*at` is left just past the
 * block when the whole block was spanned. */
static void proxy_mqtt_scan_properties(
    const uint8_t* p,
    size_t len,
    size_t* at,
    uint8_t want,
    int* out_present,
    uint32_t* out_value)
{
  uint32_t props_len = 0;
  if (!proxy_mqtt_varint(p, len, at, &props_len))
  {
    *at = len; /* undecodable: nothing after this can be trusted either */
    return;
  }
  size_t end = *at + (size_t)props_len;
  if (end > len)
  {
    *at = len;
    return;
  }
  size_t cur = *at;
  while (cur < end)
  {
    uint8_t id = p[cur++];
    size_t value_at = cur;
    if (!proxy_mqtt_skip_property(p, end, &cur, id))
    {
      break;
    }
    if (id == want && value_at + 4 <= end)
    {
      *out_present = 1;
      *out_value = proxy_be32(p + value_at);
    }
  }
  *at = end;
}

/* Decode the client's CONNECT into `out`. Silently leaves `out->seen` at 0 for
 * anything that does not decode: this is observation, not validation. */
uint64_t az_iot_test_proxy_digest(const void* bytes, size_t len)
{
  const uint8_t* b = (const uint8_t*)bytes;
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < len; ++i)
  {
    h ^= b[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

/* Reads a length-prefixed CONNECT string field at `*at` into presence, length
 * and digest. Returns 0 when it runs past the packet. */
static int proxy_connect_string_digest(
    const uint8_t* pkt,
    size_t len,
    size_t* at,
    int* has,
    size_t* out_len,
    uint64_t* out_digest)
{
  if (*at + 2 > len)
  {
    return 0;
  }
  size_t n = ((size_t)pkt[*at] << 8) | (size_t)pkt[*at + 1];
  *at += 2;
  if (*at + n > len)
  {
    return 0;
  }
  *has = 1;
  *out_len = n;
  *out_digest = az_iot_test_proxy_digest(pkt + *at, n);
  *at += n;
  return 1;
}

static void proxy_decode_connect(
    const uint8_t* pkt,
    size_t len,
    az_iot_test_proxy_connect_fields* out)
{
  size_t hdr = 0;
  size_t total = 0;
  if (proxy_frame_size(pkt, len, &hdr, &total) != 1 || total > len)
  {
    return;
  }
  size_t at = hdr;
  /* protocol name */
  if (at + 2 > len)
  {
    return;
  }
  size_t name_len = ((size_t)pkt[at] << 8) | (size_t)pkt[at + 1];
  at += 2 + name_len;
  if (at + 4 > len)
  {
    return;
  }
  uint8_t level = pkt[at++];
  uint8_t flags = pkt[at++];
  at += 2; /* keep alive */

  az_iot_test_proxy_connect_fields f;
  memset(&f, 0, sizeof(f));
  f.protocol_level = level;
  f.clean_flag = (flags & 0x02u) ? 1 : 0;
  f.will_flag = (flags & 0x04u) ? 1 : 0;
  f.will_qos = (uint8_t)((flags >> 3) & 0x03u);
  f.will_retain = (flags & 0x20u) ? 1 : 0;

  if (level >= 5)
  {
    proxy_mqtt_scan_properties(
        pkt, len, &at, 0x11u, &f.has_session_expiry, &f.session_expiry_seconds);
  }

  /* client identifier */
  if (at + 2 > len)
  {
    return;
  }
  size_t id_len = ((size_t)pkt[at] << 8) | (size_t)pkt[at + 1];
  at += 2 + id_len;

  if (f.will_flag)
  {
    if (level >= 5)
    {
      proxy_mqtt_scan_properties(pkt, len, &at, 0x18u, &f.has_will_delay, &f.will_delay_seconds);
    }
    if (at + 2 > len)
    {
      return;
    }
    size_t topic_len = ((size_t)pkt[at] << 8) | (size_t)pkt[at + 1];
    at += 2;
    if (at + topic_len > len)
    {
      return;
    }
    size_t copy = topic_len;
    if (copy >= sizeof(f.will_topic))
    {
      copy = sizeof(f.will_topic) - 1;
    }
    memcpy(f.will_topic, pkt + at, copy);
    f.will_topic[copy] = '\0';
    at += topic_len;
    if (at + 2 > len)
    {
      return;
    }
    f.will_payload_len = ((size_t)pkt[at] << 8) | (size_t)pkt[at + 1];
    at += 2 + f.will_payload_len;
  }

  if ((flags & 0x80u)
      && !proxy_connect_string_digest(
          pkt, len, &at, &f.has_username, &f.username_len, &f.username_digest))
  {
    return;
  }
  if ((flags & 0x40u)
      && !proxy_connect_string_digest(
          pkt, len, &at, &f.has_password, &f.password_len, &f.password_digest))
  {
    return;
  }

  f.seen = 1;
  *out = f;
}

/* Record what one client->broker packet says about the session. Called for
 * every framed packet in that direction; only CONNECT and DISCONNECT are
 * looked at. */
static void proxy_observe_c2b(struct az_iot_test_proxy* m, const uint8_t* pkt, size_t len)
{
  if (len == 0)
  {
    return;
  }
  uint8_t type = (uint8_t)(pkt[0] >> 4);
  if (type == AZ_IOT_TEST_PROXY_PKT_CONNECT)
  {
    az_iot_test_proxy_connect_fields f;
    memset(&f, 0, sizeof(f));
    proxy_decode_connect(pkt, len, &f);
    if (!f.seen)
    {
      return;
    }
    proxy_lock(&m->lock);
    m->connect_fields = f;
    proxy_unlock(&m->lock);
    return;
  }
  if (type != AZ_IOT_TEST_PROXY_PKT_DISCONNECT)
  {
    return;
  }
  size_t hdr = 0;
  size_t total = 0;
  if (proxy_frame_size(pkt, len, &hdr, &total) != 1)
  {
    return;
  }
  /* An empty body is a DISCONNECT with no reason code, which MQTT 5 defines as
   * Normal Disconnection and 3.1.1 has no way to qualify at all. */
  uint8_t reason = (total > hdr) ? pkt[hdr] : 0u;
  proxy_lock(&m->lock);
  m->have_client_disconnect = 1;
  m->client_disconnect_reason = reason;
  proxy_unlock(&m->lock);
}

/* Run the rule table over one complete packet. May shorten or edit `pkt` in
 * place. Returns 0 if the packet must not be forwarded.
 * Injections are collected under the lock and pushed after releasing it, so
 * that queueing never happens with the lock held. */
static int proxy_apply_rules(
    struct az_iot_test_proxy* m,
    int dir,
    proxy_egress* eg,
    const az_iot_test_proxy_impairment* imp,
    uint8_t* pkt,
    size_t* len,
    int* out_drop)
{
  struct
  {
    int dir;
    size_t len;
    uint8_t bytes[AZ_IOT_TEST_PROXY_RULE_BYTES_MAX];
  } pending[AZ_IOT_TEST_PROXY_RULES_MAX];
  size_t pending_count = 0;

  uint8_t pid[2] = { 0, 0 };
  int have_pid = proxy_packet_id(pkt, *len, pid);
  uint8_t type = (uint8_t)(pkt[0] >> 4);
  int forward = 1;

  proxy_lock(&m->lock);
  for (size_t i = 0; i < m->rule_count; ++i)
  {
    proxy_rule* r = &m->rules[i];
    if ((int)r->spec.dir != dir)
    {
      continue;
    }
    if (r->spec.on_packet != AZ_IOT_TEST_PROXY_PKT_ANY && r->spec.on_packet != type)
    {
      continue;
    }
    if (r->seen++ < r->spec.skip)
    {
      continue;
    }
    if (!r->spec.repeat && r->hits > 0)
    {
      continue;
    }
    ++r->hits;

    az_iot_test_proxy_action action = r->spec.action;
    if (action == AZ_IOT_TEST_PROXY_ACTION_SUPPRESS)
    {
      forward = 0;
    }
    else if (action == AZ_IOT_TEST_PROXY_ACTION_TRUNCATE)
    {
      if (r->spec.truncate_to < *len)
      {
        *len = r->spec.truncate_to;
      }
    }
    else if (action == AZ_IOT_TEST_PROXY_ACTION_CORRUPT)
    {
      if (r->spec.corrupt_offset < *len)
      {
        pkt[r->spec.corrupt_offset] ^= r->spec.corrupt_mask;
      }
    }
    else if (action == AZ_IOT_TEST_PROXY_ACTION_DROP)
    {
      *out_drop = 1;
    }
    else if (action == AZ_IOT_TEST_PROXY_ACTION_STALL)
    {
      /* Set directly: az_iot_test_proxy_stall() would take the lock we hold. */
      m->stall_until_ms[(int)r->spec.stall_dir] = proxy_now_ms() + (uint64_t)r->spec.stall_ms;
    }
    else if (
        action == AZ_IOT_TEST_PROXY_ACTION_INJECT && pending_count < AZ_IOT_TEST_PROXY_RULES_MAX)
    {
      memcpy(pending[pending_count].bytes, r->bytes, r->bytes_len);
      pending[pending_count].len = r->bytes_len;
      pending[pending_count].dir = (int)r->spec.inject_dir;
      if (r->spec.echo_packet_id && have_pid && r->spec.packet_id_offset + 2 <= r->bytes_len)
      {
        pending[pending_count].bytes[r->spec.packet_id_offset] = pid[0];
        pending[pending_count].bytes[r->spec.packet_id_offset + 1] = pid[1];
      }
      ++pending_count;
    }
  }
  proxy_unlock(&m->lock);

  for (size_t i = 0; i < pending_count; ++i)
  {
    int d = pending[i].dir;
    (void)eg_push(&eg[d], &imp[d], (const char*)pending[i].bytes, pending[i].len);
  }
  return forward;
}

/* Frame `n` bytes read from `dir`, run the rules over each complete packet and
 * queue whatever survives. Reports how many packets completed so the existing
 * reset-after-N-packets control keeps its meaning. */
static void proxy_ingest(
    struct az_iot_test_proxy* m,
    int dir,
    proxy_framer* fr,
    proxy_egress* eg,
    const az_iot_test_proxy_impairment* imp,
    const char* data,
    size_t n,
    uint32_t* out_packets,
    int* out_drop)
{
  const uint8_t* p = (const uint8_t*)data;
  size_t left = n;

  /* Opaque mode: shape and forward, with no framing at all. The stream is not
   * MQTT here (a WebSocket session, say), so anything the framer decided about
   * packet boundaries would be read out of bytes that are not a fixed header --
   * and it would stall waiting for a length that never arrives. */
  if (m->opaque_stream)
  {
    (void)eg_push(&eg[dir], &imp[dir], data, n);
    return;
  }

  while (left > 0)
  {
    if (fr->oversized_left > 0)
    {
      size_t take = (left < fr->oversized_left) ? left : fr->oversized_left;
      (void)eg_push(&eg[dir], &imp[dir], (const char*)p, take);
      fr->oversized_left -= take;
      if (fr->oversized_left == 0)
      {
        ++(*out_packets);
      }
      p += take;
      left -= take;
      continue;
    }

    if (fr->need == 0)
    {
      /* One byte at a time until the fixed header decodes; it is five bytes at
       * most, so this is not the hot path. */
      fr->buf[fr->have++] = *p++;
      --left;
      size_t hdr = 0;
      size_t total = 0;
      int r = proxy_frame_size(fr->buf, fr->have, &hdr, &total);
      if (r < 0)
      {
        /* A malformed length cannot be framed. Pass what was buffered through
         * and resynchronise on the next byte rather than stalling the stream. */
        (void)eg_push(&eg[dir], &imp[dir], (const char*)fr->buf, fr->have);
        fr->have = 0;
      }
      else if (r == 1)
      {
        if (total > PROXY_PKT_MAX)
        {
          (void)eg_push(&eg[dir], &imp[dir], (const char*)fr->buf, fr->have);
          fr->oversized_left = total - fr->have;
          fr->have = 0;
        }
        else
        {
          fr->need = total;
        }
      }
      if (fr->need == 0)
      {
        continue; /* still short of a fixed header */
      }
      /* Otherwise fall through: a packet with an empty body is already
       * complete, and waiting for more bytes would strand it. */
    }

    size_t want = fr->need - fr->have;
    size_t take = (left < want) ? left : want;
    if (take > 0)
    {
      memcpy(fr->buf + fr->have, p, take);
      fr->have += take;
      p += take;
      left -= take;
    }

    if (fr->have == fr->need)
    {
      size_t len = fr->need;
      if (dir == AZ_IOT_TEST_PROXY_C2B)
      {
        /* Before the rules, which may edit or suppress the packet: what is
         * asserted on has to be what the client actually sent. */
        proxy_observe_c2b(m, fr->buf, len);
      }
      if (proxy_apply_rules(m, dir, eg, imp, fr->buf, &len, out_drop) && len > 0)
      {
        (void)eg_push(&eg[dir], &imp[dir], (const char*)fr->buf, len);
      }
      ++(*out_packets);
      fr->have = 0;
      fr->need = 0;
    }
  }
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
  proxy_egress eg[2];
  memset(eg, 0, sizeof(eg));
  /* Framing state is per connection, so it lives here rather than in the proxy
   * and cannot leak a half-read packet into the next session. */
  proxy_framer* fr = (proxy_framer*)calloc(2, sizeof(*fr));
  if (fr == NULL)
  {
    proxy_client_free(client_ssl);
    proxy_hard_close(client);
    proxy_hard_close(upstream);
    return;
  }
  int rule_drop = 0;
  unsigned wait_ms = 200;

  for (;;)
  {
    az_iot_test_proxy_impairment imp[2];
    uint64_t stall[2];

    proxy_lock(&m->lock);
    int stop = m->should_stop;
    int drop = m->drop_requested;
    m->drop_requested = 0;
    imp[0] = m->imp[0];
    imp[1] = m->imp[1];
    stall[0] = m->stall_until_ms[0];
    stall[1] = m->stall_until_ms[1];
    proxy_unlock(&m->lock);

    /* Re-seed whenever the configured seed changes, not just on the first
     * pass: impairment is documented as taking effect on the next chunk, and a
     * seed that could only be set before the connection opened would be
     * useless for reproducing a failure seen mid-run. */
    for (int d = 0; d < 2; ++d)
    {
      if (!eg[d].seeded || eg[d].seed_applied != imp[d].seed)
      {
        eg[d].seed_applied = imp[d].seed;
        eg[d].seeded = 1;
        eg[d].rng = (imp[d].seed != 0u) ? imp[d].seed : (uint32_t)(0x5EED0001u + (uint32_t)d);
      }
    }

    if (stop || drop)
    {
      goto done;
    }

    int client_ready = 0;
    int upstream_ready = 0;
    int sel = proxy_wait_readable(client, upstream, &client_ready, &upstream_ready, wait_ms);
    if (sel < 0)
    {
      goto done;
    }

    /* Read only while the destination queue has room. Past the bound the pump
     * stops draining the source, which is the backpressure a congested link
     * applies; discarding queued bytes would corrupt the stream. */
    if (client_ready && eg[AZ_IOT_TEST_PROXY_C2B].queued < PROXY_EGRESS_MAX_QUEUED)
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
        proxy_ingest(
            m,
            AZ_IOT_TEST_PROXY_C2B,
            &fr[AZ_IOT_TEST_PROXY_C2B],
            eg,
            imp,
            buf,
            (size_t)n,
            &new_packets,
            &rule_drop);

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
      } while (proxy_ssl_pending(client_ssl)
               && eg[AZ_IOT_TEST_PROXY_C2B].queued < PROXY_EGRESS_MAX_QUEUED);
    }

    if (upstream_ready && eg[AZ_IOT_TEST_PROXY_B2C].queued < PROXY_EGRESS_MAX_QUEUED)
    {
      long n = proxy_recv(upstream, buf, sizeof(buf));
      if (n <= 0)
      {
        goto done;
      }
      uint32_t b2c_packets = 0;
      proxy_ingest(
          m,
          AZ_IOT_TEST_PROXY_B2C,
          &fr[AZ_IOT_TEST_PROXY_B2C],
          eg,
          imp,
          buf,
          (size_t)n,
          &b2c_packets,
          &rule_drop);
      proxy_lock(&m->lock);
      m->total_bytes += (uint64_t)n;
      proxy_unlock(&m->lock);
    }

    if (eg_flush(
            &eg[AZ_IOT_TEST_PROXY_C2B],
            &imp[AZ_IOT_TEST_PROXY_C2B],
            stall[AZ_IOT_TEST_PROXY_C2B],
            upstream,
            NULL)
        != 0)
    {
      goto done;
    }
    if (eg_flush(
            &eg[AZ_IOT_TEST_PROXY_B2C],
            &imp[AZ_IOT_TEST_PROXY_B2C],
            stall[AZ_IOT_TEST_PROXY_B2C],
            client,
            client_ssl)
        != 0)
    {
      goto done;
    }

    /* A rule asked for a reset. Both queues have just been flushed, so an
     * "inject, then drop" pair gets its injected packet onto the wire before
     * the connection goes away -- whichever direction triggered it. */
    if (rule_drop)
    {
      goto done;
    }

    /* Sleep no longer than the next scheduled release, or a shaped chunk would
     * be quantised to the poll interval. Computed after the flush so it sees
     * the token bucket this iteration just spent from. */
    wait_ms = 200;
    const uint64_t after = proxy_now_ms();
    for (int d = 0; d < 2; ++d)
    {
      uint64_t due = eg_next_wakeup_ms(&eg[d], &imp[d], after);
      if (due != 0)
      {
        uint64_t in_ms = (due > after) ? (due - after) : 0;
        if (in_ms < (uint64_t)wait_ms)
        {
          wait_ms = (unsigned)in_ms;
        }
      }
      if (stall[d] > after && (stall[d] - after) < (uint64_t)wait_ms)
      {
        wait_ms = (unsigned)(stall[d] - after);
      }
      /* A source held off by backpressure stays readable, so select() would
       * return at once; a small floor keeps that from becoming a spin. */
      if (eg[d].queued >= PROXY_EGRESS_MAX_QUEUED && wait_ms < 5u)
      {
        wait_ms = 5u;
      }
    }

    /* Republish the queue depth so a test can wait for a shaped transfer to
     * drain rather than sleeping for a guessed interval. */
    proxy_lock(&m->lock);
    for (int d = 0; d < 2; ++d)
    {
      m->queued_snapshot[d] = eg[d].queued;
      m->writes[d] += eg[d].writes; /* cumulative across connections */
      eg[d].writes = 0;
    }
    proxy_unlock(&m->lock);
  }

done:
  free(fr);
  eg_clear(&eg[0]);
  eg_clear(&eg[1]);
  proxy_lock(&m->lock);
  m->queued_snapshot[0] = 0;
  m->queued_snapshot[1] = 0;
  proxy_unlock(&m->lock);
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
  proxy_counter counter;
  memset(&counter, 0, sizeof(counter));
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
    proxy_frame_count(&counter, buf, (size_t)n, &new_packets);
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
    memset(&m->connect_fields, 0, sizeof(m->connect_fields));
    m->have_client_disconnect = 0;
    m->client_disconnect_reason = 0;
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

    proxy_sock upstream;
    if (m->http_connect)
    {
      /* The client names its own upstream, so the CONNECT exchange happens
       * before any pumping. TLS is deliberately NOT terminated on this path:
       * the bytes that follow the 200 are the client's end-to-end handshake
       * with the real broker, and this proxy must stay ignorant of them. */
      upstream = proxy_http_connect_accept(m, client);
    }
    else
    {
      upstream = proxy_connect_upstream(m);
    }
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
  /* A peer that closes mid-write makes send()/SSL_write raise SIGPIPE in this
   * thread. Block it here so EPIPE is returned instead and the harness does not
   * depend on the adapter under test ignoring SIGPIPE process-wide. */
  sigset_t pipe_set;
  sigemptyset(&pipe_set);
  sigaddset(&pipe_set, SIGPIPE);
  (void)pthread_sigmask(SIG_BLOCK, &pipe_set, NULL);
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
  memset(&o, 0, sizeof(o));
  return o;
}

az_iot_test_proxy_tls_options az_iot_test_proxy_tls_options_default(void)
{
  /* Zero first: naming each field means every field added later starts life
   * uninitialised, and the only reason that was noticed here is that valgrind
   * runs over these tests. */
  az_iot_test_proxy_tls_options o;
  memset(&o, 0, sizeof(o));
  o.leaf_san = "IP:127.0.0.1";
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

#if defined(AZ_IOT_TEST_PROXY_TLS)
/* Verify callback for accept_any_client_cert: report success regardless of what
 * OpenSSL made of the chain. See the option's note in the header. */
static int proxy_accept_any_cert_cb(int preverify_ok, X509_STORE_CTX* ctx)
{
  (void)preverify_ok;
  (void)ctx;
  return 1;
}
#endif

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
  if (tls.require_client_cert)
  {
    /* Ask for a certificate and refuse the handshake without an acceptable
     * one. The CA also goes into the verification store, or every client
     * certificate would be rejected as unknown rather than on its own merits. */
    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (store == NULL || X509_STORE_add_cert(store, ca_cert) != 1)
    {
      goto done;
    }
    if (SSL_CTX_add_client_CA(ctx, ca_cert) != 1)
    {
      goto done;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  }
  else if (tls.accept_any_client_cert)
  {
    /* Request a certificate and let every one through. The point is to make the
     * client sign the CertificateVerify with its private key; who issued the
     * certificate is deliberately not examined. See the header. */
    SSL_CTX_set_verify(
        ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, proxy_accept_any_cert_cb);
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
  /* The proxy takes over the CA so client certificates can still be signed
   * after this call returns. */
  if (m->ca_cert != NULL)
  {
    X509_free(m->ca_cert);
  }
  if (m->ca_key != NULL)
  {
    EVP_PKEY_free(m->ca_key);
  }
  m->ca_cert = ca_cert;
  m->ca_key = ca_key;
  ca_cert = NULL;
  ca_key = NULL;
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

az_iot_test_proxy_client_cert_options az_iot_test_proxy_client_cert_options_default(void)
{
  az_iot_test_proxy_client_cert_options o;
  memset(&o, 0, sizeof(o));
  return o;
}

int az_iot_test_proxy_issue_client_cert(
    az_iot_test_proxy* m,
    const az_iot_test_proxy_client_cert_options* options,
    char* cert_pem,
    size_t cert_cap,
    char* key_pem,
    size_t key_cap)
{
#if !defined(AZ_IOT_TEST_PROXY_TLS)
  (void)m;
  (void)options;
  (void)cert_pem;
  (void)cert_cap;
  (void)key_pem;
  (void)key_cap;
  return -1;
#else
  if (m == NULL || cert_pem == NULL || cert_cap == 0 || key_pem == NULL || key_cap == 0)
  {
    return -1;
  }

  az_iot_test_proxy_client_cert_options opt
      = (options != NULL) ? *options : az_iot_test_proxy_client_cert_options_default();
  const char* cn = (opt.common_name != NULL && opt.common_name[0] != '\0') ? opt.common_name
                                                                           : "az-iot-test-client";
  long nb = opt.not_before_offset_sec;
  long na = opt.not_after_offset_sec;
  if (nb == 0 && na == 0)
  {
    nb = -3600;
    na = 24L * 3600L;
  }

  /* Take a reference while the lock is held: enable_tls() may replace and free
   * the CA, and signing with a raw pointer read under the lock but used after
   * releasing it is a use-after-free waiting for the first test that
   * reconfigures TLS from another thread. */
  proxy_lock(&m->lock);
  EVP_PKEY* ca_key = m->ca_key;
  X509* ca_cert = m->ca_cert;
  if (ca_key != NULL && ca_cert != NULL && EVP_PKEY_up_ref(ca_key) == 1)
  {
    if (X509_up_ref(ca_cert) != 1)
    {
      EVP_PKEY_free(ca_key);
      ca_key = NULL;
      ca_cert = NULL;
    }
  }
  else
  {
    ca_key = NULL;
    ca_cert = NULL;
  }
  proxy_unlock(&m->lock);
  if (ca_key == NULL || ca_cert == NULL)
  {
    return -1; /* enable_tls has not run, so there is nothing to sign with */
  }

  EVP_PKEY* key = NULL;
  X509* cert = NULL;
  EVP_PKEY* rogue_key = NULL;
  X509* rogue_cert = NULL;
  BIO* bio = NULL;
  int rc = -1;

  key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  if (key == NULL)
  {
    goto done;
  }

  EVP_PKEY* signer_key = ca_key;
  X509* signer_cert = ca_cert;
  if (opt.sign_with_untrusted_ca)
  {
    rogue_key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
    if (rogue_key == NULL)
    {
      goto done;
    }
    rogue_cert = proxy_make_cert(
        rogue_key,
        rogue_key,
        NULL,
        "az-iot-test-rogue-client-ca",
        NULL,
        1,
        4,
        -3600,
        10L * 365 * 24 * 3600);
    if (rogue_cert == NULL)
    {
      goto done;
    }
    signer_key = rogue_key;
    signer_cert = rogue_cert;
  }

  /* No SAN: a client certificate is matched on its issuer and validity, not on
   * a hostname. */
  cert = proxy_make_cert(key, signer_key, signer_cert, cn, NULL, 0, 5, nb, na);
  if (cert == NULL)
  {
    goto done;
  }

  bio = BIO_new(BIO_s_mem());
  if (bio == NULL || PEM_write_bio_X509(bio, cert) != 1)
  {
    goto done;
  }
  {
    char* data = NULL;
    long n = BIO_get_mem_data(bio, &data);
    if (n <= 0 || (size_t)n >= cert_cap)
    {
      goto done;
    }
    memcpy(cert_pem, data, (size_t)n);
    cert_pem[n] = '\0';
  }
  BIO_free(bio);

  bio = BIO_new(BIO_s_mem());
  if (bio == NULL || PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL) != 1)
  {
    goto done;
  }
  {
    char* data = NULL;
    long n = BIO_get_mem_data(bio, &data);
    if (n <= 0 || (size_t)n >= key_cap)
    {
      goto done;
    }
    memcpy(key_pem, data, (size_t)n);
    key_pem[n] = '\0';
  }
  rc = 0;

done:
  if (bio != NULL)
  {
    BIO_free(bio);
  }
  if (cert != NULL)
  {
    X509_free(cert);
  }
  if (rogue_cert != NULL)
  {
    X509_free(rogue_cert);
  }
  if (rogue_key != NULL)
  {
    EVP_PKEY_free(rogue_key);
  }
  if (key != NULL)
  {
    EVP_PKEY_free(key);
  }
  X509_free(ca_cert);
  EVP_PKEY_free(ca_key);
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
  /* In CONNECT mode the client names the upstream, so upstream_host/port are
   * not required -- and supplying them would be misleading, since they are
   * ignored. In passthrough mode they remain mandatory. */
  if (options == NULL || out_proxy == NULL)
  {
    return -1;
  }
  if (!options->http_connect && (options->upstream_host == NULL || options->upstream_port == 0))
  {
    return -1;
  }

  size_t host_len = (options->upstream_host != NULL) ? strlen(options->upstream_host) : 0;
  if (host_len >= sizeof(((struct az_iot_test_proxy*)0)->upstream_host))
  {
    return -1;
  }

  /* Credential validation applies only to CONNECT mode, where these fields
   * mean something. Rejecting them in passthrough mode would change that
   * mode's existing contract, having documented them as ignored there. */
  if (options->http_connect)
  {
    /* A password without a username is not a credential Basic can express. */
    if (options->required_password != NULL && options->required_username == NULL)
    {
      return -1;
    }
    /* Nor can a user-id contain a colon: Basic splits the decoded credential at
     * the first one, so "dev:ice" would authenticate as user "dev" with a
     * different password than the caller configured -- and the fixture would
     * quietly be testing something else. The Paho adapter refuses the same
     * input; refuse it here too rather than accepting a credential neither
     * side can represent. */
    if (options->required_username != NULL && strchr(options->required_username, ':') != NULL)
    {
      return -1;
    }
    if (options->required_username != NULL
        && (strlen(options->required_username)
                >= sizeof(((struct az_iot_test_proxy*)0)->required_username)
            || (options->required_password != NULL
                && strlen(options->required_password)
                    >= sizeof(((struct az_iot_test_proxy*)0)->required_password))))
    {
      return -1;
    }
  }

  proxy_ensure_wsa();

  struct az_iot_test_proxy* m = (struct az_iot_test_proxy*)calloc(1, sizeof(*m));
  if (m == NULL)
  {
    return -1;
  }
  m->listen_sock = PROXY_INVALID_SOCK;
  m->pkt_mult = 1;
  if (options->upstream_host != NULL)
  {
    memcpy(m->upstream_host, options->upstream_host, host_len + 1);
  }
  (void)snprintf(
      m->upstream_port, sizeof(m->upstream_port), "%u", (unsigned)options->upstream_port);
  m->http_connect = options->http_connect ? 1 : 0;
  m->opaque_stream = options->opaque_stream ? 1 : 0;
  if (m->http_connect && options->required_username != NULL)
  {
    m->require_auth = 1;
    (void)snprintf(
        m->required_username, sizeof(m->required_username), "%s", options->required_username);
    (void)snprintf(
        m->required_password,
        sizeof(m->required_password),
        "%s",
        (options->required_password != NULL) ? options->required_password : "");
  }
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

az_iot_test_proxy_impairment az_iot_test_proxy_impairment_default(void)
{
  az_iot_test_proxy_impairment imp;
  memset(&imp, 0, sizeof(imp));
  return imp;
}

void az_iot_test_proxy_set_impairment(
    az_iot_test_proxy* m,
    az_iot_test_proxy_direction dir,
    const az_iot_test_proxy_impairment* impairment)
{
  if (m == NULL || (int)dir < 0 || (int)dir > 1)
  {
    return;
  }
  proxy_lock(&m->lock);
  if (impairment != NULL)
  {
    m->imp[(int)dir] = *impairment;
  }
  else
  {
    memset(&m->imp[(int)dir], 0, sizeof(m->imp[(int)dir]));
  }
  proxy_unlock(&m->lock);
}

void az_iot_test_proxy_stall(az_iot_test_proxy* m, az_iot_test_proxy_direction dir, unsigned ms)
{
  if (m == NULL || (int)dir < 0 || (int)dir > 1)
  {
    return;
  }
  proxy_lock(&m->lock);
  m->stall_until_ms[(int)dir] = proxy_now_ms() + (uint64_t)ms;
  proxy_unlock(&m->lock);
}

uint64_t az_iot_test_proxy_queued_bytes(az_iot_test_proxy* m, az_iot_test_proxy_direction dir)
{
  if (m == NULL || (int)dir < 0 || (int)dir > 1)
  {
    return 0;
  }
  proxy_lock(&m->lock);
  uint64_t q = m->queued_snapshot[(int)dir];
  proxy_unlock(&m->lock);
  return q;
}

int az_iot_test_proxy_add_rule(az_iot_test_proxy* m, const az_iot_test_proxy_rule* rule)
{
  if (m == NULL || rule == NULL || (int)rule->dir < 0 || (int)rule->dir > 1)
  {
    return -1;
  }
  if (rule->action != AZ_IOT_TEST_PROXY_ACTION_INJECT
      && rule->action != AZ_IOT_TEST_PROXY_ACTION_SUPPRESS
      && rule->action != AZ_IOT_TEST_PROXY_ACTION_TRUNCATE
      && rule->action != AZ_IOT_TEST_PROXY_ACTION_CORRUPT
      && rule->action != AZ_IOT_TEST_PROXY_ACTION_DROP
      && rule->action != AZ_IOT_TEST_PROXY_ACTION_STALL)
  {
    return -1;
  }
  if (rule->on_packet > AZ_IOT_TEST_PROXY_PKT_AUTH)
  {
    return -1;
  }
  if (rule->action == AZ_IOT_TEST_PROXY_ACTION_INJECT
      && (rule->bytes == NULL || rule->bytes_len == 0
          || rule->bytes_len > AZ_IOT_TEST_PROXY_RULE_BYTES_MAX || (int)rule->inject_dir < 0
          || (int)rule->inject_dir > 1))
  {
    return -1;
  }

  proxy_lock(&m->lock);
  if (m->rule_count >= AZ_IOT_TEST_PROXY_RULES_MAX)
  {
    proxy_unlock(&m->lock);
    return -1;
  }
  size_t index = m->rule_count++;
  proxy_rule* r = &m->rules[index];
  memset(r, 0, sizeof(*r));
  r->spec = *rule;
  if (rule->action == AZ_IOT_TEST_PROXY_ACTION_INJECT)
  {
    memcpy(r->bytes, rule->bytes, rule->bytes_len);
    r->bytes_len = rule->bytes_len;
  }
  /* The copy is what gets used; forget the caller's pointer so a dangling one
   * cannot be dereferenced later. */
  r->spec.bytes = NULL;
  proxy_unlock(&m->lock);
  return (int)index;
}

void az_iot_test_proxy_clear_rules(az_iot_test_proxy* m)
{
  if (m == NULL)
  {
    return;
  }
  proxy_lock(&m->lock);
  memset(m->rules, 0, sizeof(m->rules));
  m->rule_count = 0;
  proxy_unlock(&m->lock);
}

uint32_t az_iot_test_proxy_rule_hits(az_iot_test_proxy* m, size_t index)
{
  if (m == NULL)
  {
    return 0;
  }
  proxy_lock(&m->lock);
  uint32_t hits = (index < m->rule_count) ? m->rules[index].hits : 0u;
  proxy_unlock(&m->lock);
  return hits;
}

uint64_t az_iot_test_proxy_writes(az_iot_test_proxy* m, az_iot_test_proxy_direction dir)
{
  if (m == NULL || (int)dir < 0 || (int)dir > 1)
  {
    return 0;
  }
  proxy_lock(&m->lock);
  uint64_t w = m->writes[(int)dir];
  proxy_unlock(&m->lock);
  return w;
}

/* Shorthand that shapes BOTH directions. The previous implementation shaped
 * only client->broker despite the documented contract. */
void az_iot_test_proxy_set_write_delay_ms(az_iot_test_proxy* m, unsigned delay_ms)
{
  if (m == NULL)
  {
    return;
  }
  proxy_lock(&m->lock);
  m->imp[0].base_delay_ms = delay_ms;
  m->imp[1].base_delay_ms = delay_ms;
  proxy_unlock(&m->lock);
}

void az_iot_test_proxy_set_fragment(az_iot_test_proxy* m, size_t max_chunk)
{
  if (m == NULL)
  {
    return;
  }
  proxy_lock(&m->lock);
  m->imp[0].fragment_max = max_chunk;
  m->imp[1].fragment_max = max_chunk;
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

int az_iot_test_proxy_last_connect_fields(
    az_iot_test_proxy* m,
    az_iot_test_proxy_connect_fields* out)
{
  if (!out)
  {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  if (!m)
  {
    return 0;
  }
  proxy_lock(&m->lock);
  az_iot_test_proxy_connect_fields f = m->connect_fields;
  proxy_unlock(&m->lock);
  *out = f;
  return f.seen ? 1 : 0;
}

int az_iot_test_proxy_last_client_disconnect_reason(az_iot_test_proxy* m, uint8_t* out_code)
{
  if (!m || !out_code)
  {
    return 0;
  }
  proxy_lock(&m->lock);
  int have = m->have_client_disconnect;
  uint8_t code = m->client_disconnect_reason;
  proxy_unlock(&m->lock);
  if (!have)
  {
    return 0;
  }
  *out_code = code;
  return 1;
}

uint32_t az_iot_test_proxy_tunnels_opened(az_iot_test_proxy* m)
{
  proxy_lock(&m->lock);
  uint32_t v = m->tunnels_opened;
  proxy_unlock(&m->lock);
  return v;
}

uint32_t az_iot_test_proxy_auth_failures(az_iot_test_proxy* m)
{
  proxy_lock(&m->lock);
  uint32_t v = m->auth_failures;
  proxy_unlock(&m->lock);
  return v;
}

const char* az_iot_test_proxy_last_connect_target(az_iot_test_proxy* m)
{
  proxy_lock(&m->lock);
  const char* v = m->have_connect_target ? m->last_connect_target : NULL;
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
  if (m->ca_cert != NULL)
  {
    X509_free(m->ca_cert);
  }
  if (m->ca_key != NULL)
  {
    EVP_PKEY_free(m->ca_key);
  }
#endif
  proxy_mutex_destroy(&m->lock);
  free(m);
}
