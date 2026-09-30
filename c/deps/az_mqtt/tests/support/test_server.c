// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_server.c
 * @brief In-process TCP/TLS peer for transport tests. See test_server.h.
 */

#include "test_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#define MAX_SILENT 8

struct test_server
{
  test_server_options options;
  int listen_fd;
  uint16_t port;
  pthread_t thread;
  // Shared with the server thread: guarded by lock.
  pthread_mutex_t lock;
  int stop;
  int accepted;
  int handshakes;
  bool saw_client_cert;
  int pingreqs;
  bool client_closed;
  int silent_fds[MAX_SILENT];
  int silent_count;
  SSL_CTX* ctx;
  char ca_path[64];
  char client_cert_path[64];
  char client_key_path[64];
};

/** @brief Read a shared int under the lock. */
static int _get(test_server* s, int const* field)
{
  pthread_mutex_lock(&s->lock);
  int v = *field;
  pthread_mutex_unlock(&s->lock);
  return v;
}

/** @brief Add to a shared int under the lock. */
static void _add(test_server* s, int* field, int delta)
{
  pthread_mutex_lock(&s->lock);
  *field += delta;
  pthread_mutex_unlock(&s->lock);
}

static bool _stopping(test_server* s) { return _get(s, &s->stop) != 0; }

test_server_options test_server_options_default(void)
{
  test_server_options o;
  memset(&o, 0, sizeof(o));
  o.tls = true;
  o.san = "DNS:localhost,IP:127.0.0.1";
  o.behavior = TEST_SERVER_MQTT;
  return o;
}

// ──────────────────────── Certificates ───────────────────────

static bool _add_ext(X509* cert, X509V3_CTX* ctx, int nid, char const* value)
{
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, ctx, nid, value);
  if (ext == NULL)
  {
    return false;
  }
  bool ok = X509_add_ext(cert, ext, -1) == 1;
  X509_EXTENSION_free(ext);
  return ok;
}

static X509* _make_cert(
    EVP_PKEY* key,
    char const* cn,
    X509* issuer,
    EVP_PKEY* issuer_key,
    bool is_ca,
    char const* san,
    long not_before_s,
    long not_after_s)
{
  static long serial = 1;
  X509* cert = X509_new();
  if (cert == NULL)
  {
    return NULL;
  }
  X509_set_version(cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(cert), serial++);
  X509_gmtime_adj(X509_getm_notBefore(cert), not_before_s);
  X509_gmtime_adj(X509_getm_notAfter(cert), not_after_s);
  X509_set_pubkey(cert, key);
  X509_NAME* name = X509_get_subject_name(cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char const*)cn, -1, -1, 0);
  X509_set_issuer_name(cert, issuer != NULL ? X509_get_subject_name(issuer) : name);

  X509V3_CTX v3;
  X509V3_set_ctx_nodb(&v3);
  X509V3_set_ctx(&v3, issuer != NULL ? issuer : cert, cert, NULL, NULL, 0);
  bool ok = _add_ext(cert, &v3, NID_basic_constraints, is_ca ? "critical,CA:TRUE" : "CA:FALSE")
      && _add_ext(
             cert,
             &v3,
             NID_key_usage,
             is_ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature")
      && _add_ext(cert, &v3, NID_subject_key_identifier, "hash")
      && (is_ca || _add_ext(cert, &v3, NID_ext_key_usage, "serverAuth,clientAuth"))
      && (san == NULL || _add_ext(cert, &v3, NID_subject_alt_name, san))
      && X509_sign(cert, issuer_key != NULL ? issuer_key : key, EVP_sha256()) > 0;
  if (!ok)
  {
    X509_free(cert);
    return NULL;
  }
  return cert;
}

static bool _write_pem(char* path, size_t cap, X509* cert, EVP_PKEY* key)
{
  snprintf(path, cap, "/tmp/azmqtt_test_XXXXXX");
  int fd = mkstemp(path);
  if (fd < 0)
  {
    return false;
  }
  FILE* f = fdopen(fd, "w");
  if (f == NULL)
  {
    close(fd);
    return false;
  }
  bool ok = cert != NULL ? PEM_write_X509(f, cert) == 1
                         : PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL) == 1;
  fclose(f);
  return ok;
}

static bool _setup_tls(test_server* s)
{
  const long day = 24L * 3600L;
  EVP_PKEY* ca_key = EVP_EC_gen("P-256");
  EVP_PKEY* rogue_key = EVP_EC_gen("P-256");
  EVP_PKEY* srv_key = EVP_EC_gen("P-256");
  EVP_PKEY* cli_key = EVP_EC_gen("P-256");
  X509* ca = _make_cert(ca_key, "az-mqtt-test-ca", NULL, NULL, true, NULL, -day, 30 * day);
  X509* rogue = _make_cert(rogue_key, "az-mqtt-rogue-ca", NULL, NULL, true, NULL, -day, 30 * day);
  X509* signer = s->options.untrusted_ca ? rogue : ca;
  EVP_PKEY* signer_key = s->options.untrusted_ca ? rogue_key : ca_key;
  X509* srv = _make_cert(
      srv_key,
      "az-mqtt-test-server",
      signer,
      signer_key,
      false,
      s->options.san,
      s->options.expired ? -2 * day : -day,
      s->options.expired ? -day : 30 * day);
  X509* cli = _make_cert(cli_key, "az-mqtt-test-client", ca, ca_key, false, NULL, -day, 30 * day);

  bool ok = ca_key && rogue_key && srv_key && cli_key && ca && rogue && srv && cli
      && _write_pem(s->ca_path, sizeof(s->ca_path), ca, NULL)
      && _write_pem(s->client_cert_path, sizeof(s->client_cert_path), cli, NULL)
      && _write_pem(s->client_key_path, sizeof(s->client_key_path), NULL, cli_key);

  if (ok)
  {
    s->ctx = SSL_CTX_new(TLS_server_method());
    ok = s->ctx != NULL && SSL_CTX_use_certificate(s->ctx, srv) == 1
        && SSL_CTX_add1_chain_cert(s->ctx, signer) == 1
        && SSL_CTX_use_PrivateKey(s->ctx, srv_key) == 1;
  }
  if (ok && s->options.require_client_cert)
  {
    X509_STORE* store = SSL_CTX_get_cert_store(s->ctx);
    ok = X509_STORE_add_cert(store, ca) == 1;
    SSL_CTX_set_verify(s->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
  }

  X509_free(ca);
  X509_free(rogue);
  X509_free(srv);
  X509_free(cli);
  EVP_PKEY_free(ca_key);
  EVP_PKEY_free(rogue_key);
  EVP_PKEY_free(srv_key);
  EVP_PKEY_free(cli_key);
  return ok;
}

// ──────────────────────── Connection handling ────────────────

typedef struct
{
  int fd;
  SSL* ssl;
} conn;

static int _read(conn* c, uint8_t* buf, int len)
{
  return c->ssl != NULL ? SSL_read(c->ssl, buf, len) : (int)recv(c->fd, buf, (size_t)len, 0);
}

static bool _write(conn* c, uint8_t const* buf, int len)
{
  int n = c->ssl != NULL ? SSL_write(c->ssl, buf, len)
                         : (int)send(c->fd, buf, (size_t)len, MSG_NOSIGNAL);
  return n == len;
}

static bool _wait_readable(test_server* s, conn* c)
{
  while (!_stopping(s))
  {
    if (c->ssl != NULL && SSL_pending(c->ssl) > 0)
    {
      return true;
    }
    struct pollfd p = { c->fd, POLLIN, 0 };
    int r = poll(&p, 1, 50);
    if (r > 0)
    {
      return true;
    }
    if (r < 0)
    {
      return false;
    }
  }
  return false;
}

static bool _read_exact(test_server* s, conn* c, uint8_t* buf, int len)
{
  int got = 0;
  while (got < len)
  {
    if (!_wait_readable(s, c))
    {
      return false;
    }
    int n = _read(c, buf + got, len - got);
    if (n <= 0)
    {
      return false;
    }
    got += n;
  }
  return true;
}

/** @brief Read one MQTT packet; returns its type nibble or -1. Body truncated to cap. */
static int _read_packet(test_server* s, conn* c, uint8_t* body, int cap, int* out_len)
{
  uint8_t b;
  if (!_read_exact(s, c, &b, 1))
  {
    return -1;
  }
  int type = b >> 4;
  int len = 0;
  int shift = 0;
  do
  {
    if (!_read_exact(s, c, &b, 1) || shift > 21)
    {
      return -1;
    }
    len |= (b & 0x7F) << shift;
    shift += 7;
  } while (b & 0x80);
  int keep = len < cap ? len : cap;
  if (!_read_exact(s, c, body, keep))
  {
    return -1;
  }
  for (int i = keep; i < len; i++)
  {
    if (!_read_exact(s, c, &b, 1))
    {
      return -1;
    }
  }
  *out_len = keep;
  return type;
}

static void _serve(test_server* s, conn* c)
{
  uint8_t body[512];
  int len = 0;
  if (_read_packet(s, c, body, (int)sizeof(body), &len) != 1 || len < 7)
  {
    return;
  }
  // CONNECT variable header: 00 04 'M' 'Q' 'T' 'T' <level>
  bool const v5 = body[6] == 5;
  uint8_t connack_v5[] = { 0x20, 0x03, 0x00, s->options.connack_code, 0x00, 0x00, 0x00, 0x00 };
  uint8_t const connack_v3[] = { 0x20, 0x02, 0x00, s->options.connack_code };
  int connack_v5_len = 5;
  if (s->options.server_keep_alive != 0 || s->options.server_keep_alive_present)
  {
    // Properties: Server Keep Alive (0x13).
    connack_v5[1] = 0x06;
    connack_v5[4] = 0x03;
    connack_v5[5] = 0x13;
    connack_v5[6] = (uint8_t)(s->options.server_keep_alive >> 8);
    connack_v5[7] = (uint8_t)(s->options.server_keep_alive & 0xFF);
    connack_v5_len = 8;
  }
  bool ok = v5 ? _write(c, connack_v5, connack_v5_len)
               : _write(c, connack_v3, (int)sizeof(connack_v3));
  if (!ok || s->options.connack_code != 0)
  {
    return;
  }
  if (s->options.burst_publishes > 0)
  {
    // QoS 0 PUBLISH "t" / "p": 30 len 00 01 't' [00 props] 'p'
    uint8_t burst[64 * 8];
    int n = 0;
    for (int i = 0; i < s->options.burst_publishes && n + 8 <= (int)sizeof(burst); i++)
    {
      burst[n++] = 0x30;
      burst[n++] = v5 ? 0x05 : 0x04;
      burst[n++] = 0x00;
      burst[n++] = 0x01;
      burst[n++] = 't';
      if (v5)
      {
        burst[n++] = 0x00;
      }
      burst[n++] = 'p';
    }
    if (!_write(c, burst, n))
    {
      return;
    }
  }
  if (v5 && s->options.publish_properties > 0 && s->options.publish_properties <= 12)
  {
    // QoS 0 PUBLISH "t" / "p" with N x (user property "k"="v", subscription identifier i+1).
    int const count = s->options.publish_properties;
    uint8_t pub[128];
    int n = 0;
    pub[n++] = 0x30;
    pub[n++] = (uint8_t)(3 + 1 + count * 9 + 1);
    pub[n++] = 0x00;
    pub[n++] = 0x01;
    pub[n++] = 't';
    pub[n++] = (uint8_t)(count * 9);
    for (int i = 0; i < count; i++)
    {
      static const uint8_t user_property[] = { 0x26, 0x00, 0x01, 'k', 0x00, 0x01, 'v' };
      memcpy(&pub[n], user_property, sizeof(user_property));
      n += (int)sizeof(user_property);
      pub[n++] = 0x0B;
      pub[n++] = (uint8_t)(i + 1);
    }
    pub[n++] = 'p';
    if (!_write(c, pub, n))
    {
      return;
    }
  }
  if (s->options.send_empty_disconnect)
  {
    static const uint8_t disconnect[] = { 0xE0, 0x00 };
    if (!_write(c, disconnect, (int)sizeof(disconnect)))
    {
      return;
    }
  }
  if (s->options.send_auth)
  {
    static const uint8_t auth[] = { 0xF0, 0x00 };
    if (!_write(c, auth, (int)sizeof(auth)))
    {
      return;
    }
  }
  if (s->options.behavior == TEST_SERVER_DISCONNECT_AFTER_CONNACK)
  {
    static const uint8_t disconnect_v5[] = { 0xE0, 0x01, 0x8B };
    if (v5)
    {
      (void)_write(c, disconnect_v5, (int)sizeof(disconnect_v5));
    }
    usleep(100 * 1000);
    return;
  }
  if (s->options.behavior == TEST_SERVER_PARTIAL_TLS_RECORD)
  {
    // Application-data record header announcing 64 bytes, then only 5 of them,
    // written under OpenSSL so the client sees a record that never completes.
    static const uint8_t partial[] = { 0x17, 0x03, 0x03, 0x00, 0x40, 1, 2, 3, 4, 5 };
    (void)send(c->fd, partial, sizeof(partial), MSG_NOSIGNAL);
    while (!_stopping(s))
    {
      usleep(20 * 1000);
    }
    return;
  }
  if (s->options.behavior == TEST_SERVER_STOP_READING)
  {
    while (!_stopping(s))
    {
      usleep(20 * 1000);
    }
    return;
  }
  if (s->options.behavior == TEST_SERVER_CLOSE_AFTER_CONNACK)
  {
    // Plain close (FIN). The client's next write draws an RST and the one after
    // that fails with EPIPE, which is what raises SIGPIPE.
    usleep(200 * 1000);
    return;
  }
  for (;;)
  {
    int type = _read_packet(s, c, body, (int)sizeof(body), &len);
    if (type < 0 || type == 14)
    {
      pthread_mutex_lock(&s->lock);
      s->client_closed = !s->stop;
      pthread_mutex_unlock(&s->lock);
      return;
    }
    if (type == 8 && len >= 2)
    {
      // SUBACK: same packet id, then reason codes (MQTT 5 adds an empty property length).
      int codes = s->options.suback_codes > 0 ? s->options.suback_codes : 1;
      if (codes > 64)
      {
        codes = 64; // Bounded before it sizes both the length byte and the payload.
      }
      uint8_t suback[4 + 1 + 64];
      int n = 0;
      suback[n++] = 0x90;
      suback[n++] = (uint8_t)(2 + (v5 ? 1 : 0) + codes);
      suback[n++] = body[0];
      suback[n++] = body[1];
      if (v5)
      {
        suback[n++] = 0x00;
      }
      for (int i = 0; i < codes; i++)
      {
        suback[n++] = 0x00;
      }
      (void)_write(c, suback, n);
    }
    if (type == 12)
    {
      _add(s, &s->pingreqs, 1);
      static const uint8_t pingresp[] = { 0xD0, 0x00 };
      if (!s->options.no_pingresp)
      {
        (void)_write(c, pingresp, (int)sizeof(pingresp));
      }
    }
  }
}

static void* _run(void* arg)
{
  test_server* s = (test_server*)arg;
  // SIGPIPE from write() goes to the writing thread: blocking it here keeps the
  // server's own writes from masking or causing one in the client under test.
  sigset_t pipe_set;
  sigemptyset(&pipe_set);
  sigaddset(&pipe_set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &pipe_set, NULL);
  while (!_stopping(s))
  {
    struct pollfd p = { s->listen_fd, POLLIN, 0 };
    if (poll(&p, 1, 50) <= 0)
    {
      continue;
    }
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd < 0)
    {
      continue;
    }
    _add(s, &s->accepted, 1);
    if (s->options.behavior == TEST_SERVER_SILENT)
    {
      if (s->silent_count < MAX_SILENT)
      {
        s->silent_fds[s->silent_count++] = fd;
      }
      else
      {
        close(fd);
      }
      continue;
    }
    conn c = { fd, NULL };
    if (s->options.tls)
    {
      struct timeval tv = { 5, 0 };
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      c.ssl = SSL_new(s->ctx);
      if (c.ssl != NULL && SSL_set_fd(c.ssl, fd) == 1 && SSL_accept(c.ssl) == 1)
      {
        X509* peer = SSL_get1_peer_certificate(c.ssl);
        pthread_mutex_lock(&s->lock);
        s->saw_client_cert = peer != NULL;
        s->handshakes++;
        pthread_mutex_unlock(&s->lock);
        X509_free(peer);
        _serve(s, &c);
      }
      SSL_free(c.ssl);
    }
    else
    {
      _serve(s, &c);
    }
    close(fd);
  }
  return NULL;
}

// ──────────────────────── Public API ─────────────────────────

test_server* test_server_start(test_server_options const* options)
{
  test_server* s = (test_server*)calloc(1, sizeof(*s));
  if (s == NULL)
  {
    return NULL;
  }
  s->options = *options;
  s->listen_fd = -1;
  pthread_mutex_init(&s->lock, NULL);
  if (s->options.tls && !_setup_tls(s))
  {
    test_server_stop(s);
    return NULL;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(addr);
  s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->listen_fd >= 0 && s->options.behavior == TEST_SERVER_STOP_READING)
  {
    int small = 4096; // Inherited by accepted sockets: backs the client up quickly.
    setsockopt(s->listen_fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  }
  if (s->listen_fd < 0 || bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0
      || listen(s->listen_fd, 8) != 0
      || getsockname(s->listen_fd, (struct sockaddr*)&addr, &alen) != 0)
  {
    test_server_stop(s);
    return NULL;
  }
  s->port = ntohs(addr.sin_port);
  if (pthread_create(&s->thread, NULL, _run, s) != 0)
  {
    s->thread = 0;
    test_server_stop(s);
    return NULL;
  }
  return s;
}

void test_server_stop(test_server* s)
{
  if (s == NULL)
  {
    return;
  }
  pthread_mutex_lock(&s->lock);
  s->stop = 1;
  pthread_mutex_unlock(&s->lock);
  if (s->thread != 0)
  {
    pthread_join(s->thread, NULL);
  }
  for (int i = 0; i < s->silent_count; i++)
  {
    close(s->silent_fds[i]);
  }
  if (s->listen_fd >= 0)
  {
    close(s->listen_fd);
  }
  SSL_CTX_free(s->ctx);
  pthread_mutex_destroy(&s->lock);
  if (s->ca_path[0] != '\0')
  {
    remove(s->ca_path);
    remove(s->client_cert_path);
    remove(s->client_key_path);
  }
  free(s);
}

uint16_t test_server_port(test_server const* s) { return s->port; }
char const* test_server_ca_path(test_server const* s) { return s->ca_path; }
char const* test_server_client_cert_path(test_server const* s) { return s->client_cert_path; }
char const* test_server_client_key_path(test_server const* s) { return s->client_key_path; }
int test_server_accepted(test_server* s) { return _get(s, &s->accepted); }
int test_server_handshakes(test_server* s) { return _get(s, &s->handshakes); }
bool test_server_saw_client_cert(test_server* s)
{
  pthread_mutex_lock(&s->lock);
  bool v = s->saw_client_cert;
  pthread_mutex_unlock(&s->lock);
  return v;
}
int test_server_pingreqs(test_server* s) { return _get(s, &s->pingreqs); }
bool test_server_client_closed(test_server* s)
{
  pthread_mutex_lock(&s->lock);
  bool v = s->client_closed;
  pthread_mutex_unlock(&s->lock);
  return v;
}
