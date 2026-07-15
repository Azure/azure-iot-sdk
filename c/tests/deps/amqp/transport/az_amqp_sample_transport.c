// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/*
 * Reference non-blocking transport for the AMQP samples.
 *
 *   - Plaintext TCP on every platform (BSD sockets / Winsock) for local brokers on port 5672.
 *   - TLS via OpenSSL on Linux/macOS when built with AZ_AMQP_SAMPLE_USE_OPENSSL (and linked against
 *     -lssl -lcrypto). This is the path the Azure Service Bus sample uses (AMQPS, port 5671).
 *   - On Windows, TLS is left as an integration point (wire Schannel here); plaintext works as-is.
 *
 * The whole adapter is socket code plus the OpenSSL handshake/encrypt/decrypt; the AMQP client above
 * it never sees any of it.
 */

#include "az_amqp_sample_transport.h"

#include <azure/core/az_span.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET _sock;
#define _SOCK_INVALID INVALID_SOCKET
#define _sock_would_block() (WSAGetLastError() == WSAEWOULDBLOCK)
#define _sock_in_progress() (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS)
#define _sock_last_error() (WSAGetLastError())
#define _sock_close(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int _sock;
#define _SOCK_INVALID (-1)
#define _sock_would_block() (errno == EWOULDBLOCK || errno == EAGAIN)
#define _sock_in_progress() (errno == EINPROGRESS || errno == EWOULDBLOCK)
#define _sock_last_error() (errno)
#define _sock_close(s) close(s)
#endif

#if defined(AZ_AMQP_SAMPLE_USE_OPENSSL)
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

enum
{
  _PHASE_NEED_CONNECT = 0,
  _PHASE_CONNECTING,
  _PHASE_TLS_HANDSHAKE,
  _PHASE_READY,
  _PHASE_CLOSED,
};

static az_amqp_sample_transport* _impl(az_amqp_transport* t)
{
  return (az_amqp_sample_transport*)az_amqp_transport_get_impl(t);
}

static void _set_error(az_amqp_sample_transport* s, int32_t code, char const* message)
{
  s->last_error_status = code;
  size_t n = strlen(message);
  if (n >= sizeof(s->last_error_message))
  {
    n = sizeof(s->last_error_message) - 1;
  }
  memcpy(s->last_error_message, message, n);
  s->last_error_message[n] = 0;
}

static int _host_cstr(az_amqp_sample_transport* s, char* out, size_t cap)
{
  int32_t hn = az_span_size(s->options.host_name);
  if (hn <= 0 || (size_t)hn >= cap)
  {
    return -1;
  }
  memcpy(out, az_span_ptr(s->options.host_name), (size_t)hn);
  out[hn] = 0;
  return 0;
}

static void _ensure_wsa(void)
{
#ifdef _WIN32
  static int started = 0;
  if (!started)
  {
    WSADATA data;
    (void)WSAStartup(MAKEWORD(2, 2), &data);
    started = 1;
  }
#endif
}

static void _set_nonblocking(_sock sock)
{
#ifdef _WIN32
  u_long mode = 1;
  (void)ioctlsocket(sock, FIONBIO, &mode);
#else
  int flags = fcntl(sock, F_GETFL, 0);
  (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

static bool _tls_active(az_amqp_sample_transport* s)
{
#if defined(AZ_AMQP_SAMPLE_USE_OPENSSL)
  return s->options.tls_enabled;
#elif defined(_WIN32)
  return s->options.tls_enabled;
#else
  (void)s;
  return false;
#endif
}

#if defined(AZ_AMQP_SAMPLE_USE_OPENSSL)
static void _set_ssl_error(az_amqp_sample_transport* s, char const* where)
{
  unsigned long e = ERR_get_error();
  char buf[160];
  if (e != 0)
  {
    char reason[120];
    ERR_error_string_n(e, reason, sizeof(reason));
    (void)snprintf(buf, sizeof(buf), "%s: %s", where, reason);
  }
  else
  {
    (void)snprintf(buf, sizeof(buf), "%s", where);
  }
  _set_error(s, (int32_t)e, buf);
}

static az_amqp_transport_status _tls_handshake(az_amqp_sample_transport* s)
{
  if (s->tls_session == NULL)
  {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL)
    {
      _set_ssl_error(s, "SSL_CTX_new failed");
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    (void)SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    (void)SSL_CTX_set_default_verify_paths(ctx); // system CA store validates Azure's chain
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    SSL* ssl = SSL_new(ctx);
    if (ssl == NULL)
    {
      SSL_CTX_free(ctx);
      _set_ssl_error(s, "SSL_new failed");
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    SSL_set_fd(ssl, (int)s->file_descriptor);

    char host[256];
    az_span sni = az_span_size(s->options.tls.server_name_indication) > 0
        ? s->options.tls.server_name_indication
        : s->options.host_name;
    int32_t hn = az_span_size(sni);
    if (hn > 0 && (size_t)hn < sizeof(host))
    {
      memcpy(host, az_span_ptr(sni), (size_t)hn);
      host[hn] = 0;
      (void)SSL_set_tlsext_host_name(ssl, host);
      (void)SSL_set1_host(ssl, host);
    }
    s->tls_context = ctx;
    s->tls_session = ssl;
  }

  SSL* ssl = (SSL*)s->tls_session;
  int r = SSL_connect(ssl);
  if (r == 1)
  {
    return AZ_AMQP_TRANSPORT_STATUS_OK;
  }
  int err = SSL_get_error(ssl, r);
  if (err == SSL_ERROR_WANT_READ)
  {
    return AZ_AMQP_TRANSPORT_STATUS_WANT_READ;
  }
  if (err == SSL_ERROR_WANT_WRITE)
  {
    return AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE;
  }
  _set_ssl_error(s, "TLS handshake failed");
  return AZ_AMQP_TRANSPORT_STATUS_ERROR;
}
#endif // AZ_AMQP_SAMPLE_USE_OPENSSL

#if defined(_WIN32) && !defined(AZ_AMQP_SAMPLE_USE_OPENSSL)
// ---- Native Windows TLS via Schannel (SSPI). Single connection per process (sample scope). ----
#define SECURITY_WIN32
// Expose the modern SCH_CREDENTIALS / TLS_PARAMETERS structures. Without this
// macro, schannel.h only defines the legacy SCHANNEL_CRED, which caps Schannel
// negotiation at TLS 1.2; SCH_CREDENTIALS is required to negotiate TLS 1.3.
// TLS_PARAMETERS references UNICODE_STRING, so winternl.h must precede schannel.h.
#define SCHANNEL_USE_BLACKLISTS
#include <winternl.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>
#pragma comment(lib, "secur32.lib")

#define _SCH_BUF 32768

static struct
{
  CredHandle cred;
  CtxtHandle ctxt;
  bool cred_ok;
  bool ctxt_ok;
  bool started;
  bool done;
  bool reneg_active; // driving a TLS post-handshake / renegotiation flight
  bool reneg_isc_done; // reneg ISC returned SEC_E_OK; only the final flush remains
  SecPkgContext_StreamSizes sizes;
  uint8_t in[_SCH_BUF]; // accumulated inbound ciphertext
  int in_len;
  uint8_t plain[_SCH_BUF]; // leftover decrypted plaintext
  int plain_off;
  int plain_len;
  uint8_t out[_SCH_BUF]; // pending outbound ciphertext to flush
  int out_off;
  int out_len;
} g_sch;

// Optional Schannel diagnostics, enabled by setting AZ_AMQP_TLS_DEBUG in the
// environment (a no-op otherwise). Lines are phase-labeled and written to stderr
// so a CI run can show WHERE a connection was closed -- during the TLS handshake
// (server rejected our ClientHello) vs. after it (at the AMQP/app layer) -- and
// which protocol/cipher was negotiated.
static bool _sch_debug_enabled(void)
{
  static int enabled = -1;
  if (enabled < 0)
  {
    enabled = (getenv("AZ_AMQP_TLS_DEBUG") != NULL) ? 1 : 0;
  }
  return enabled != 0;
}

static void _sch_dbg(char const* format, ...)
{
  if (!_sch_debug_enabled())
  {
    return;
  }
  va_list args;
  va_start(args, format);
  (void)fputs("[az-amqp-tls] ", stderr);
  (void)vfprintf(stderr, format, args);
  (void)fputc('\n', stderr);
  va_end(args);
}

static void _sch_fail(az_amqp_sample_transport* s, char const* where, long status)
{
  char buf[128];
  (void)snprintf(buf, sizeof(buf), "%s (0x%08lx)", where, (unsigned long)status);
  _set_error(s, (int32_t)status, buf);
}

// Sends as much pending ciphertext as the socket accepts. OK when drained; WANT_WRITE if more remains.
static az_amqp_transport_status _sch_flush(az_amqp_sample_transport* s)
{
  _sock sock = (_sock)s->file_descriptor;
  while (g_sch.out_off < g_sch.out_len)
  {
    int n = send(sock, (char const*)(g_sch.out + g_sch.out_off), g_sch.out_len - g_sch.out_off, 0);
    if (n > 0)
    {
      g_sch.out_off += n;
    }
    else if (_sock_would_block())
    {
      return AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE;
    }
    else
    {
      _set_error(s, _sock_last_error(), "send() failed");
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
  }
  g_sch.out_off = g_sch.out_len = 0;
  return AZ_AMQP_TRANSPORT_STATUS_OK;
}

// Appends token bytes to the pending outbound buffer.
static bool _sch_queue(uint8_t const* p, int n)
{
  if (g_sch.out_len + n > _SCH_BUF)
  {
    return false;
  }
  memcpy(g_sch.out + g_sch.out_len, p, (size_t)n);
  g_sch.out_len += n;
  return true;
}

static az_amqp_transport_status _sch_handshake(az_amqp_sample_transport* s)
{
  char host[256];
  az_span sni = az_span_size(s->options.tls.server_name_indication) > 0
      ? s->options.tls.server_name_indication
      : s->options.host_name;
  int32_t hn = az_span_size(sni);
  if (hn <= 0 || (size_t)hn >= sizeof(host))
  {
    _set_error(s, 0, "invalid TLS server name");
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
  memcpy(host, az_span_ptr(sni), (size_t)hn);
  host[hn] = 0;

  DWORD const isc_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
      | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

  if (g_sch.done)
  {
    return _sch_flush(s); // handshake complete; only the final token flush may remain
  }

  if (!g_sch.started)
  {
    memset(&g_sch, 0, sizeof(g_sch));
    _sch_dbg("starting TLS handshake to %s:%u", host, (unsigned)s->options.port);
    // Use the modern SCH_CREDENTIALS structure. The legacy SCHANNEL_CRED caps
    // negotiation at TLS 1.2; SCH_CREDENTIALS lets Schannel negotiate TLS 1.3
    // where available (falling back to 1.2). We disable only TLS 1.0/1.1 so the
    // highest modern protocol the server and OS share is chosen. TLS 1.3's
    // post-handshake messages (NewSessionTicket / KeyUpdate) surface from
    // DecryptMessage as SEC_I_RENEGOTIATE and are handled in _sch_read /
    // _sch_drive_reneg.
    TLS_PARAMETERS tls_params;
    memset(&tls_params, 0, sizeof(tls_params));
    tls_params.grbitDisabledProtocols = (DWORD)(SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_1_CLIENT);
    SCH_CREDENTIALS cred;
    memset(&cred, 0, sizeof(cred));
    cred.dwVersion = SCH_CREDENTIALS_VERSION;
    cred.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    cred.cTlsParameters = 1;
    cred.pTlsParameters = &tls_params;
    SECURITY_STATUS st = AcquireCredentialsHandleA(
        NULL, (SEC_CHAR*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL, &g_sch.cred, NULL);
    if (st != SEC_E_OK)
    {
      _sch_fail(s, "AcquireCredentialsHandle failed", (long)st);
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    g_sch.cred_ok = true;

    SecBuffer outbuf = { 0, SECBUFFER_TOKEN, NULL };
    SecBufferDesc out = { SECBUFFER_VERSION, 1, &outbuf };
    DWORD attrs = 0;
    st = InitializeSecurityContextA(
        &g_sch.cred, NULL, (SEC_CHAR*)host, isc_flags, 0, 0, NULL, 0, &g_sch.ctxt, &out, &attrs, NULL);
    g_sch.ctxt_ok = true;
    g_sch.started = true;
    if (st != SEC_I_CONTINUE_NEEDED)
    {
      _sch_fail(s, "InitializeSecurityContext (initial) failed", (long)st);
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    if (outbuf.cbBuffer > 0 && outbuf.pvBuffer != NULL)
    {
      (void)_sch_queue((uint8_t const*)outbuf.pvBuffer, (int)outbuf.cbBuffer);
      FreeContextBuffer(outbuf.pvBuffer);
    }
    az_amqp_transport_status f = _sch_flush(s);
    if (f == AZ_AMQP_TRANSPORT_STATUS_ERROR)
    {
      return f;
    }
    return AZ_AMQP_TRANSPORT_STATUS_WANT_READ; // sent ClientHello; await the server's reply
  }

  // Flush any leftover token before continuing.
  az_amqp_transport_status fs = _sch_flush(s);
  if (fs != AZ_AMQP_TRANSPORT_STATUS_OK)
  {
    return fs;
  }

  // Read more handshake ciphertext.
  _sock sock = (_sock)s->file_descriptor;
  int n = recv(sock, (char*)(g_sch.in + g_sch.in_len), _SCH_BUF - g_sch.in_len, 0);
  if (n == 0)
  {
    _sch_dbg("peer closed DURING the TLS handshake (server rejected our ClientHello)");
    return AZ_AMQP_TRANSPORT_STATUS_CLOSED;
  }
  if (n < 0)
  {
    return _sock_would_block() ? AZ_AMQP_TRANSPORT_STATUS_WANT_READ
                               : AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
  g_sch.in_len += n;

  int incomplete_creds_seen = 0;
  for (;;)
  {
    SecBuffer inbufs[2]
        = { { (unsigned long)g_sch.in_len, SECBUFFER_TOKEN, g_sch.in }, { 0, SECBUFFER_EMPTY, NULL } };
    SecBufferDesc in = { SECBUFFER_VERSION, 2, inbufs };
    SecBuffer outbuf = { 0, SECBUFFER_TOKEN, NULL };
    SecBufferDesc out = { SECBUFFER_VERSION, 1, &outbuf };
    DWORD attrs = 0;
    SECURITY_STATUS st = InitializeSecurityContextA(
        &g_sch.cred, &g_sch.ctxt, (SEC_CHAR*)host, isc_flags, 0, 0, &in, 0, NULL, &out, &attrs, NULL);

    if (outbuf.cbBuffer > 0 && outbuf.pvBuffer != NULL)
    {
      (void)_sch_queue((uint8_t const*)outbuf.pvBuffer, (int)outbuf.cbBuffer);
      FreeContextBuffer(outbuf.pvBuffer);
    }

    if (st == SEC_E_INCOMPLETE_MESSAGE)
    {
      return _sch_flush(s) == AZ_AMQP_TRANSPORT_STATUS_ERROR ? AZ_AMQP_TRANSPORT_STATUS_ERROR
                                                             : AZ_AMQP_TRANSPORT_STATUS_WANT_READ;
    }

    if (st == SEC_I_INCOMPLETE_CREDENTIALS)
    {
      // The server requested (optional) TLS client authentication. This transport
      // authenticates via SAS/CBS and never presents a client certificate, so continue
      // the handshake anonymously by re-invoking InitializeSecurityContext with the same
      // server flight. Azure IoT Hub's AMQP endpoint issues this request; Event Hubs does
      // not, which is why only the c2d (IoT Hub) connection was affected.
      if (++incomplete_creds_seen > 2)
      {
        _sch_fail(s, "InitializeSecurityContext: incomplete credentials", (long)st);
        return AZ_AMQP_TRANSPORT_STATUS_ERROR;
      }
      continue;
    }

    // Move any leftover ("extra") ciphertext to the front of the input buffer.
    int consumed = g_sch.in_len;
    if (inbufs[1].BufferType == SECBUFFER_EXTRA)
    {
      consumed = g_sch.in_len - (int)inbufs[1].cbBuffer;
    }
    if (consumed > 0 && consumed <= g_sch.in_len)
    {
      memmove(g_sch.in, g_sch.in + consumed, (size_t)(g_sch.in_len - consumed));
      g_sch.in_len -= consumed;
    }

    if (st == SEC_I_CONTINUE_NEEDED)
    {
      az_amqp_transport_status f = _sch_flush(s);
      if (f == AZ_AMQP_TRANSPORT_STATUS_ERROR)
      {
        return f;
      }
      if (g_sch.in_len > 0)
      {
        continue; // more handshake records already buffered
      }
      return AZ_AMQP_TRANSPORT_STATUS_WANT_READ;
    }
    if (st == SEC_E_OK)
    {
      if (QueryContextAttributes(&g_sch.ctxt, SECPKG_ATTR_STREAM_SIZES, &g_sch.sizes) != SEC_E_OK)
      {
        _set_error(s, 0, "QueryContextAttributes(STREAM_SIZES) failed");
        return AZ_AMQP_TRANSPORT_STATUS_ERROR;
      }
      if (_sch_debug_enabled())
      {
        SecPkgContext_ConnectionInfo ci;
        if (QueryContextAttributes(&g_sch.ctxt, SECPKG_ATTR_CONNECTION_INFO, &ci) == SEC_E_OK)
        {
          _sch_dbg(
              "TLS handshake OK: protocol=0x%04x cipher=0x%04x strength=%d",
              (unsigned)ci.dwProtocol,
              (unsigned)ci.aiCipher,
              (int)ci.dwCipherStrength);
        }
      }
      g_sch.done = true;
      return _sch_flush(s);
    }
    _sch_fail(s, "InitializeSecurityContext failed", (long)st);
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
}

// Advances a TLS post-handshake flight (TLS 1.3 NewSessionTicket / KeyUpdate, or
// a TLS 1.2 server-initiated renegotiation) that DecryptMessage surfaced via
// SEC_I_RENEGOTIATE. The handshake token to process must already be at the front
// of g_sch.in. Non-blocking and resumable across calls (state lives in
// g_sch.reneg_active / g_sch.reneg_isc_done): returns OK once the flight is fully
// processed (and reneg_active is cleared), WANT_READ when more socket data is
// needed, WANT_WRITE while an output token (e.g. a KeyUpdate response) is
// mid-flush, CLOSED/ERROR otherwise.
static az_amqp_transport_status _sch_drive_reneg(az_amqp_sample_transport* s)
{
  char host[256];
  az_span sni = az_span_size(s->options.tls.server_name_indication) > 0
      ? s->options.tls.server_name_indication
      : s->options.host_name;
  int32_t hn = az_span_size(sni);
  if (hn <= 0 || (size_t)hn >= sizeof(host))
  {
    _set_error(s, 0, "invalid TLS server name");
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
  memcpy(host, az_span_ptr(sni), (size_t)hn);
  host[hn] = 0;

  DWORD const isc_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
      | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
  _sock sock = (_sock)s->file_descriptor;

  for (;;)
  {
    // Flush any queued output token (e.g. a KeyUpdate response) before proceeding.
    az_amqp_transport_status fs = _sch_flush(s);
    if (fs != AZ_AMQP_TRANSPORT_STATUS_OK)
    {
      return fs; // WANT_WRITE (resume later) or ERROR
    }
    if (g_sch.reneg_isc_done)
    {
      // ISC already reported SEC_E_OK; its final token has now been flushed.
      g_sch.reneg_active = false;
      g_sch.reneg_isc_done = false;
      return AZ_AMQP_TRANSPORT_STATUS_OK;
    }

    if (g_sch.in_len > 0)
    {
      SecBuffer inbufs[2]
          = { { (unsigned long)g_sch.in_len, SECBUFFER_TOKEN, g_sch.in }, { 0, SECBUFFER_EMPTY, NULL } };
      SecBufferDesc in = { SECBUFFER_VERSION, 2, inbufs };
      SecBuffer outbuf = { 0, SECBUFFER_TOKEN, NULL };
      SecBufferDesc out = { SECBUFFER_VERSION, 1, &outbuf };
      DWORD attrs = 0;
      SECURITY_STATUS st = InitializeSecurityContextA(
          &g_sch.cred, &g_sch.ctxt, (SEC_CHAR*)host, isc_flags, 0, 0, &in, 0, NULL, &out, &attrs, NULL);

      if (outbuf.cbBuffer > 0 && outbuf.pvBuffer != NULL)
      {
        (void)_sch_queue((uint8_t const*)outbuf.pvBuffer, (int)outbuf.cbBuffer);
        FreeContextBuffer(outbuf.pvBuffer);
      }

      if (st != SEC_E_INCOMPLETE_MESSAGE)
      {
        // Consume what ISC processed; keep any trailing extra (following records).
        int consumed = g_sch.in_len;
        if (inbufs[1].BufferType == SECBUFFER_EXTRA)
        {
          consumed = g_sch.in_len - (int)inbufs[1].cbBuffer;
        }
        if (consumed > 0 && consumed <= g_sch.in_len)
        {
          memmove(g_sch.in, g_sch.in + consumed, (size_t)(g_sch.in_len - consumed));
          g_sch.in_len -= consumed;
        }

        if (st == SEC_E_OK)
        {
          g_sch.reneg_isc_done = true;
          continue; // loop to flush the final token, then finish
        }
        if (st == SEC_I_CONTINUE_NEEDED)
        {
          if (g_sch.in_len > 0)
          {
            continue; // more handshake data already buffered
          }
          // otherwise fall through to read more
        }
        else if (st == SEC_I_INCOMPLETE_CREDENTIALS)
        {
          continue; // server requested a client cert; continue anonymously
        }
        else
        {
          _sch_fail(s, "InitializeSecurityContext (post-handshake) failed", (long)st);
          return AZ_AMQP_TRANSPORT_STATUS_ERROR;
        }
      }
      // SEC_E_INCOMPLETE_MESSAGE: fall through to read more ciphertext.
    }

    if (g_sch.in_len >= _SCH_BUF)
    {
      _set_error(s, 0, "TLS record exceeds buffer");
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    int n = recv(sock, (char*)(g_sch.in + g_sch.in_len), _SCH_BUF - g_sch.in_len, 0);
    if (n == 0)
    {
      return AZ_AMQP_TRANSPORT_STATUS_CLOSED;
    }
    if (n < 0)
    {
      return _sock_would_block() ? AZ_AMQP_TRANSPORT_STATUS_WANT_READ
                                 : AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    g_sch.in_len += n;
  }
}

static az_amqp_transport_status
_sch_read(az_amqp_sample_transport* s, az_span destination, size_t* out_bytes)
{
  // Finish any in-progress TLS post-handshake / renegotiation flight before
  // serving application data.
  if (g_sch.reneg_active)
  {
    az_amqp_transport_status rs = _sch_drive_reneg(s);
    if (rs != AZ_AMQP_TRANSPORT_STATUS_OK)
    {
      return rs;
    }
  }

  // Serve any leftover decrypted plaintext first.
  if (g_sch.plain_off < g_sch.plain_len)
  {
    int avail = g_sch.plain_len - g_sch.plain_off;
    int want = az_span_size(destination);
    int take = (avail < want) ? avail : want;
    memcpy(az_span_ptr(destination), g_sch.plain + g_sch.plain_off, (size_t)take);
    g_sch.plain_off += take;
    *out_bytes = (size_t)take;
    return AZ_AMQP_TRANSPORT_STATUS_OK;
  }

  _sock sock = (_sock)s->file_descriptor;
  for (;;)
  {
    if (g_sch.in_len > 0)
    {
      SecBuffer bufs[4] = { { (unsigned long)g_sch.in_len, SECBUFFER_DATA, g_sch.in },
                            { 0, SECBUFFER_EMPTY, NULL },
                            { 0, SECBUFFER_EMPTY, NULL },
                            { 0, SECBUFFER_EMPTY, NULL } };
      SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
      SECURITY_STATUS st = DecryptMessage(&g_sch.ctxt, &desc, 0, NULL);
      if (st == SEC_E_OK)
      {
        SecBuffer* data = NULL;
        SecBuffer* extra = NULL;
        for (int i = 0; i < 4; i++)
        {
          if (bufs[i].BufferType == SECBUFFER_DATA && data == NULL)
          {
            data = &bufs[i];
          }
          else if (bufs[i].BufferType == SECBUFFER_EXTRA)
          {
            extra = &bufs[i];
          }
        }
        int plen = (data != NULL) ? (int)data->cbBuffer : 0;
        if (plen > 0)
        {
          memcpy(g_sch.plain, data->pvBuffer, (size_t)plen);
        }
        g_sch.plain_off = 0;
        g_sch.plain_len = plen;

        int extra_len = (extra != NULL) ? (int)extra->cbBuffer : 0;
        if (extra_len > 0)
        {
          memmove(g_sch.in, g_sch.in + (g_sch.in_len - extra_len), (size_t)extra_len);
        }
        g_sch.in_len = extra_len;

        if (g_sch.plain_len > 0)
        {
          int want = az_span_size(destination);
          int take = (g_sch.plain_len < want) ? g_sch.plain_len : want;
          memcpy(az_span_ptr(destination), g_sch.plain, (size_t)take);
          g_sch.plain_off = take;
          *out_bytes = (size_t)take;
          return AZ_AMQP_TRANSPORT_STATUS_OK;
        }
        // Decrypted zero bytes (e.g. a handshake record); loop to read more.
      }
      else if (st == SEC_I_CONTEXT_EXPIRED)
      {
        return AZ_AMQP_TRANSPORT_STATUS_CLOSED;
      }
      else if (st == SEC_I_RENEGOTIATE)
      {
        // A TLS post-handshake message (TLS 1.3 NewSessionTicket / KeyUpdate) or
        // a TLS 1.2 renegotiation request. DecryptMessage hands the handshake
        // bytes back in a SECBUFFER_EXTRA; move them to the front of g_sch.in and
        // drive the flight through InitializeSecurityContext, then resume
        // decrypting application data.
        SecBuffer* extra = NULL;
        for (int i = 0; i < 4; i++)
        {
          if (bufs[i].BufferType == SECBUFFER_EXTRA)
          {
            extra = &bufs[i];
            break;
          }
        }
        int extra_len = (extra != NULL) ? (int)extra->cbBuffer : 0;
        if (extra_len > 0 && extra->pvBuffer != NULL)
        {
          memmove(g_sch.in, extra->pvBuffer, (size_t)extra_len);
        }
        g_sch.in_len = extra_len;
        g_sch.reneg_active = true;
        az_amqp_transport_status rs = _sch_drive_reneg(s);
        if (rs != AZ_AMQP_TRANSPORT_STATUS_OK)
        {
          return rs;
        }
        continue; // renegotiation complete; try decrypting application data again
      }
      else if (st != SEC_E_INCOMPLETE_MESSAGE)
      {
        _sch_fail(s, "DecryptMessage failed", (long)st);
        return AZ_AMQP_TRANSPORT_STATUS_ERROR;
      }
      // SEC_E_INCOMPLETE_MESSAGE: fall through to read more ciphertext.
    }

    if (g_sch.in_len >= _SCH_BUF)
    {
      _set_error(s, 0, "TLS record exceeds buffer");
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    int n = recv(sock, (char*)(g_sch.in + g_sch.in_len), _SCH_BUF - g_sch.in_len, 0);
    if (n == 0)
    {
      return AZ_AMQP_TRANSPORT_STATUS_CLOSED;
    }
    if (n < 0)
    {
      return _sock_would_block() ? AZ_AMQP_TRANSPORT_STATUS_WANT_READ
                                 : AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    g_sch.in_len += n;
  }
}

static az_amqp_transport_status
_sch_write(az_amqp_sample_transport* s, az_span source, size_t* out_bytes)
{
  *out_bytes = 0;
  // Flush pending ciphertext from a previous partial write before encrypting more.
  az_amqp_transport_status fs = _sch_flush(s);
  if (fs != AZ_AMQP_TRANSPORT_STATUS_OK)
  {
    return fs;
  }

  int chunk = az_span_size(source);
  if (chunk > (int)g_sch.sizes.cbMaximumMessage)
  {
    chunk = (int)g_sch.sizes.cbMaximumMessage;
  }
  int total = (int)g_sch.sizes.cbHeader + chunk + (int)g_sch.sizes.cbTrailer;
  if (total > _SCH_BUF)
  {
    _set_error(s, 0, "TLS write chunk exceeds buffer");
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
  memcpy(g_sch.out + g_sch.sizes.cbHeader, az_span_ptr(source), (size_t)chunk);

  SecBuffer bufs[4]
      = { { g_sch.sizes.cbHeader, SECBUFFER_STREAM_HEADER, g_sch.out },
          { (unsigned long)chunk, SECBUFFER_DATA, g_sch.out + g_sch.sizes.cbHeader },
          { g_sch.sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, g_sch.out + g_sch.sizes.cbHeader + chunk },
          { 0, SECBUFFER_EMPTY, NULL } };
  SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
  SECURITY_STATUS st = EncryptMessage(&g_sch.ctxt, 0, &desc, 0);
  if (st != SEC_E_OK)
  {
    _sch_fail(s, "EncryptMessage failed", (long)st);
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
  g_sch.out_off = 0;
  g_sch.out_len = (int)(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
  *out_bytes = (size_t)chunk; // the plaintext is accepted (buffered as ciphertext)
  (void)_sch_flush(s); // best-effort; any remainder flushes on the next write
  return AZ_AMQP_TRANSPORT_STATUS_OK;
}

static void _sch_close(void)
{
  if (g_sch.ctxt_ok)
  {
    DeleteSecurityContext(&g_sch.ctxt);
    g_sch.ctxt_ok = false;
  }
  if (g_sch.cred_ok)
  {
    FreeCredentialsHandle(&g_sch.cred);
    g_sch.cred_ok = false;
  }
  // Fully reset the single global slot so the next connection performs a fresh
  // handshake. Clearing `started` alone is not enough: `_sch_handshake` tests
  // `g_sch.done` first, so a leftover `done` from the previous connection would
  // make the next one skip its handshake and then fail at the first encrypt.
  memset(&g_sch, 0, sizeof(g_sch));
}
#endif // _WIN32 && !AZ_AMQP_SAMPLE_USE_OPENSSL

// ---- vtable ----

static az_amqp_transport_status _open(az_amqp_transport* transport)
{
  az_amqp_sample_transport* s = _impl(transport);
  _ensure_wsa();

  char host[256];
  if (_host_cstr(s, host, sizeof(host)) != 0)
  {
    _set_error(s, 0, "invalid host name");
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
  char port[8];
  (void)snprintf(port, sizeof(port), "%u", (unsigned)s->options.port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result = NULL;
  if (getaddrinfo(host, port, &hints, &result) != 0 || result == NULL)
  {
    _set_error(s, _sock_last_error(), "DNS resolution failed");
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }

  _sock sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (sock == _SOCK_INVALID)
  {
    freeaddrinfo(result);
    _set_error(s, _sock_last_error(), "socket() failed");
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
  _set_nonblocking(sock);

  int rc = connect(sock, result->ai_addr, (int)result->ai_addrlen);
  freeaddrinfo(result);
  s->file_descriptor = (int64_t)sock;
  s->phase = _PHASE_CONNECTING;

  if (rc == 0 || _sock_in_progress())
  {
    return (rc == 0) ? AZ_AMQP_TRANSPORT_STATUS_OK : AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE;
  }
  _sock_close(sock);
  s->file_descriptor = -1;
  _set_error(s, _sock_last_error(), "connect() failed");
  return AZ_AMQP_TRANSPORT_STATUS_ERROR;
}

static az_amqp_transport_status _process(az_amqp_transport* transport)
{
  az_amqp_sample_transport* s = _impl(transport);
  if (s->phase == _PHASE_READY)
  {
    return AZ_AMQP_TRANSPORT_STATUS_OK;
  }

  if (s->phase == _PHASE_CONNECTING)
  {
    _sock sock = (_sock)s->file_descriptor;

    // Confirm the non-blocking connect has completed (socket writable) before proceeding. On
    // Windows a still-in-progress connect reports SO_ERROR == 0, so checking writability is what
    // guarantees the connection is up; otherwise the first TLS send fails with WSAENOTCONN.
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    struct timeval zero = { 0, 0 };
    if (select((int)(sock + 1), NULL, &write_fds, NULL, &zero) <= 0)
    {
      return AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE; // still connecting
    }

    int so_error = 0;
    socklen_t len = (socklen_t)sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len) != 0)
    {
      _set_error(s, _sock_last_error(), "getsockopt(SO_ERROR) failed");
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    if (so_error != 0)
    {
      _set_error(s, so_error, "TCP connect failed");
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    if (!s->options.tls_enabled)
    {
      s->phase = _PHASE_READY;
      return AZ_AMQP_TRANSPORT_STATUS_OK;
    }
    if (!_tls_active(s))
    {
      _set_error(
          s,
          0,
          "TLS requested but this build has no TLS backend; build with AZ_AMQP_SAMPLE_USE_OPENSSL "
          "(Linux) to reach AMQPS endpoints such as Azure Service Bus");
      return AZ_AMQP_TRANSPORT_STATUS_ERROR;
    }
    s->phase = _PHASE_TLS_HANDSHAKE;
  }

#if defined(AZ_AMQP_SAMPLE_USE_OPENSSL)
  if (s->phase == _PHASE_TLS_HANDSHAKE)
  {
    az_amqp_transport_status ts = _tls_handshake(s);
    if (ts == AZ_AMQP_TRANSPORT_STATUS_OK)
    {
      s->phase = _PHASE_READY;
    }
    return ts;
  }
#elif defined(_WIN32)
  if (s->phase == _PHASE_TLS_HANDSHAKE)
  {
    az_amqp_transport_status ts = _sch_handshake(s);
    if (ts == AZ_AMQP_TRANSPORT_STATUS_OK)
    {
      s->phase = _PHASE_READY;
    }
    return ts;
  }
#endif

  return AZ_AMQP_TRANSPORT_STATUS_OK;
}

static az_amqp_transport_status
_read(az_amqp_transport* transport, az_span destination, size_t* out_bytes)
{
  az_amqp_sample_transport* s = _impl(transport);
  *out_bytes = 0;

#if defined(AZ_AMQP_SAMPLE_USE_OPENSSL)
  if (_tls_active(s) && s->tls_session != NULL)
  {
    SSL* ssl = (SSL*)s->tls_session;
    int n = SSL_read(ssl, az_span_ptr(destination), (int)az_span_size(destination));
    if (n > 0)
    {
      *out_bytes = (size_t)n;
      return AZ_AMQP_TRANSPORT_STATUS_OK;
    }
    int err = SSL_get_error(ssl, n);
    if (err == SSL_ERROR_WANT_READ)
    {
      return AZ_AMQP_TRANSPORT_STATUS_WANT_READ;
    }
    if (err == SSL_ERROR_WANT_WRITE)
    {
      return AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE;
    }
    if (err == SSL_ERROR_ZERO_RETURN)
    {
      return AZ_AMQP_TRANSPORT_STATUS_CLOSED;
    }
    _set_ssl_error(s, "SSL_read failed");
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
#elif defined(_WIN32)
  if (_tls_active(s))
  {
    az_amqp_transport_status rs = _sch_read(s, destination, out_bytes);
    if (rs == AZ_AMQP_TRANSPORT_STATUS_CLOSED)
    {
      _sch_dbg("peer closed the connection AFTER the TLS handshake (at the AMQP/app layer)");
    }
    return rs;
  }
#endif

  _sock sock = (_sock)s->file_descriptor;
  int n = recv(sock, (char*)az_span_ptr(destination), (int)az_span_size(destination), 0);
  if (n > 0)
  {
    *out_bytes = (size_t)n;
    return AZ_AMQP_TRANSPORT_STATUS_OK;
  }
  if (n == 0)
  {
    return AZ_AMQP_TRANSPORT_STATUS_CLOSED;
  }
  if (_sock_would_block())
  {
    return AZ_AMQP_TRANSPORT_STATUS_WANT_READ;
  }
  _set_error(s, _sock_last_error(), "recv() failed");
  return AZ_AMQP_TRANSPORT_STATUS_ERROR;
}

static az_amqp_transport_status
_write(az_amqp_transport* transport, az_span source, size_t* out_bytes)
{
  az_amqp_sample_transport* s = _impl(transport);
  *out_bytes = 0;

#if defined(AZ_AMQP_SAMPLE_USE_OPENSSL)
  if (_tls_active(s) && s->tls_session != NULL)
  {
    SSL* ssl = (SSL*)s->tls_session;
    int n = SSL_write(ssl, az_span_ptr(source), (int)az_span_size(source));
    if (n > 0)
    {
      *out_bytes = (size_t)n;
      return AZ_AMQP_TRANSPORT_STATUS_OK;
    }
    int err = SSL_get_error(ssl, n);
    if (err == SSL_ERROR_WANT_WRITE)
    {
      return AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE;
    }
    if (err == SSL_ERROR_WANT_READ)
    {
      return AZ_AMQP_TRANSPORT_STATUS_WANT_READ;
    }
    _set_ssl_error(s, "SSL_write failed");
    return AZ_AMQP_TRANSPORT_STATUS_ERROR;
  }
#elif defined(_WIN32)
  if (_tls_active(s))
  {
    return _sch_write(s, source, out_bytes);
  }
#endif

  _sock sock = (_sock)s->file_descriptor;
  int n = send(sock, (char const*)az_span_ptr(source), (int)az_span_size(source), 0);
  if (n > 0)
  {
    *out_bytes = (size_t)n;
    return AZ_AMQP_TRANSPORT_STATUS_OK;
  }
  if (_sock_would_block())
  {
    return AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE;
  }
  _set_error(s, _sock_last_error(), "send() failed");
  return AZ_AMQP_TRANSPORT_STATUS_ERROR;
}

static az_amqp_transport_status _close(az_amqp_transport* transport)
{
  az_amqp_sample_transport* s = _impl(transport);
#if defined(AZ_AMQP_SAMPLE_USE_OPENSSL)
  if (s->tls_session != NULL)
  {
    (void)SSL_shutdown((SSL*)s->tls_session);
    SSL_free((SSL*)s->tls_session);
    s->tls_session = NULL;
  }
  if (s->tls_context != NULL)
  {
    SSL_CTX_free((SSL_CTX*)s->tls_context);
    s->tls_context = NULL;
  }
#elif defined(_WIN32)
  if (_tls_active(s))
  {
    _sch_close();
  }
#endif
  if (s->file_descriptor >= 0)
  {
    _sock_close((_sock)s->file_descriptor);
    s->file_descriptor = -1;
  }
  s->phase = _PHASE_CLOSED;
  return AZ_AMQP_TRANSPORT_STATUS_OK;
}

static void _get_last_error(az_amqp_transport* transport, int32_t* out_status, az_span* out_message)
{
  az_amqp_sample_transport* s = _impl(transport);
  *out_status = s->last_error_status;
  *out_message = az_span_create_from_str((char*)s->last_error_message);
}

static az_amqp_transport_vtable const _vtable = {
  .open = _open,
  .process = _process,
  .read = _read,
  .write = _write,
  .close = _close,
  .get_last_error = _get_last_error,
};

AZ_NODISCARD az_result az_amqp_sample_transport_init(
    az_amqp_transport* out_transport,
    az_amqp_sample_transport* storage,
    az_amqp_transport_options const* options)
{
  if (out_transport == NULL || storage == NULL || options == NULL)
  {
    return AZ_ERROR_ARG;
  }
  memset(storage, 0, sizeof(*storage));
  storage->options = *options;
  storage->file_descriptor = -1;
  storage->phase = _PHASE_NEED_CONNECT;
  storage->last_error_message[0] = 0;
  return az_amqp_transport_init(out_transport, &_vtable, storage);
}

int64_t az_amqp_sample_transport_get_socket(az_amqp_sample_transport const* storage)
{
  return storage->file_descriptor;
}
