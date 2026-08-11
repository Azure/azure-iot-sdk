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
  /* Per-direction impairment, indexed by az_iot_test_proxy_direction. */
  az_iot_test_proxy_impairment imp[2];
  uint64_t stall_until_ms[2];
  uint64_t queued_snapshot[2]; /* published for az_iot_test_proxy_queued_bytes */
  uint64_t writes[2];

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
        proxy_count_packets(m, buf, (size_t)n, &new_packets);
        if (eg_push(&eg[AZ_IOT_TEST_PROXY_C2B], &imp[AZ_IOT_TEST_PROXY_C2B], buf, (size_t)n) != 0)
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
      if (eg_push(&eg[AZ_IOT_TEST_PROXY_B2C], &imp[AZ_IOT_TEST_PROXY_B2C], buf, (size_t)n) != 0)
      {
        goto done;
      }
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
