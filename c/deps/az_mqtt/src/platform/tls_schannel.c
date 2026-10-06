// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file tls_schannel.c
 * @brief Internal: TLS layer through Schannel, over another transport.
 */

#include "az_mqtt_io_layers_internal.h"

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/az_span.h>
#include <azure/core/internal/az_precondition_internal.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include <windows.h>

#include <schannel.h>
#include <security.h>
#include <wincrypt.h>

/** @brief Size of each TLS buffer (received records, decrypted data, outgoing records). */
#define _IO_BUFFER_SIZE 65536

/** @brief _tls_transport.stage. */
enum
{
  _STAGE_IDLE, ///< Not connecting.
  _STAGE_LOWER, ///< The transport below connects.
  _STAGE_HANDSHAKE, ///< TLS handshake running.
  _STAGE_OPEN, ///< Connected (TLS, or bytes passed through).
};

typedef struct
{
  az_mqtt_transport base; ///< Must be first.
  _az_mqtt_io_layer* lower;
  _az_mqtt_io_layer_errors errors;
  CredHandle cred;
  CtxtHandle context;
  bool has_cred; ///< TLS for this connect.
  bool has_context;
  /** @brief The handshake needs more bytes from below before the next step. */
  bool needs_read;
  /** @brief The handshake's last token is queued: only out remains to send. */
  bool handshake_done;
  uint8_t stage;
  /** @brief close_notify sent (or tried) on this connection. */
  bool closing;
  SecPkgContext_StreamSizes sizes;
  /** @brief Server name (SNI, certificate check); trust anchor (NULL: system store). */
  char host[256];
  HCERTSTORE custom_root;
  /** @brief Received, not yet processed, bytes. */
  int32_t in_len;
  /** @brief Decrypted bytes not yet delivered: [plain_pos, plain_len). */
  int32_t plain_pos;
  int32_t plain_len;
  /** @brief Outgoing bytes not yet sent: [out_pos, out_len). */
  int32_t out_pos;
  int32_t out_len;
  uint8_t in[_IO_BUFFER_SIZE];
  uint8_t plain[_IO_BUFFER_SIZE];
  uint8_t out[_IO_BUFFER_SIZE];
} _tls_transport;

#define _T(t) ((_tls_transport*)(t))
#define _LOWER_T(transport) (&(transport)->lower->base)

/** @brief ISC_REQ_* of every InitializeSecurityContext() call. */
#define _REQUEST_FLAGS                                                       \
  (ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY \
   | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM)

static az_result _span_to_cstr(az_span src, char* buf, int32_t buf_size)
{
  int32_t const len = az_span_size(src);
  if (len >= buf_size)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  memcpy(buf, az_span_ptr(src), (size_t)len);
  buf[len] = '\0';
  return AZ_OK;
}

static void _report(
    _tls_transport* transport,
    az_mqtt_native_error_source source,
    DWORD code,
    az_result rc)
{
  _az_mqtt_io_layer_report(&transport->errors, source, (int32_t)code, rc);
}

/** @brief Report @p err (Win32 error, SECURITY_STATUS) as a TLS error; AZ_MQTT_ERROR_TRANSPORT. */
static az_result _setup_failure(_tls_transport* transport, DWORD err)
{
  _report(transport, AZ_MQTT_NATIVE_ERROR_TLS, err, AZ_MQTT_ERROR_TRANSPORT);
  return AZ_MQTT_ERROR_TRANSPORT;
}

// ──────────────────────── Trust ──────────────────────────────

/** @brief Read @p path (PEM or DER, up to 64 KiB) into @p der_out as DER. */
static az_result _read_cert_file(
    _tls_transport* transport,
    az_span path,
    uint8_t* der_out,
    DWORD der_capacity,
    DWORD* out_der_len)
{
  char path_str[260];
  az_result const rc = _span_to_cstr(path, path_str, (int32_t)sizeof(path_str));
  if (az_result_failed(rc))
  {
    return rc;
  }

  HANDLE h = CreateFileA(
      path_str, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
  {
    return _setup_failure(transport, GetLastError());
  }

  LARGE_INTEGER file_size;
  BOOL const size_ok = GetFileSizeEx(h, &file_size);
  if (!size_ok || file_size.QuadPart <= 0 || file_size.QuadPart > 64 * 1024)
  {
    DWORD const err = size_ok ? ERROR_INVALID_DATA : GetLastError(); // Empty or over 64 KiB.
    CloseHandle(h);
    return _setup_failure(transport, err);
  }

  DWORD const file_len = (DWORD)file_size.QuadPart;
  char file_buf[(64 * 1024) + 1];
  DWORD read_bytes = 0;
  BOOL const ok = ReadFile(h, file_buf, file_len, &read_bytes, NULL);
  DWORD const read_error = ok ? ERROR_HANDLE_EOF : GetLastError(); // Short read: EOF.
  CloseHandle(h);
  if (!ok || read_bytes != file_len)
  {
    return _setup_failure(transport, read_error);
  }
  file_buf[file_len] = '\0';

  if (strstr(file_buf, "-----BEGIN CERTIFICATE-----") != NULL)
  {
    DWORD decoded_len = der_capacity;
    if (!CryptStringToBinaryA(
            file_buf, file_len, CRYPT_STRING_BASE64HEADER, der_out, &decoded_len, NULL, NULL))
    {
      return _setup_failure(transport, GetLastError());
    }
    *out_der_len = decoded_len;
    return AZ_OK;
  }
  if (file_len > der_capacity)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  memcpy(der_out, file_buf, file_len);
  *out_der_len = file_len;
  return AZ_OK;
}

/** @brief A store with the CA in @p ca_cert_path (*@p out_store NULL if none given). */
static az_result _build_custom_root(
    _tls_transport* transport,
    az_span ca_cert_path,
    HCERTSTORE* out_store)
{
  *out_store = NULL;
  if (az_span_size(ca_cert_path) == 0)
  {
    return AZ_OK;
  }

  uint8_t der[64 * 1024];
  DWORD der_len = 0;
  az_result const rc = _read_cert_file(transport, ca_cert_path, der, (DWORD)sizeof(der), &der_len);
  if (az_result_failed(rc))
  {
    return rc;
  }

  HCERTSTORE store = CertOpenStore(
      CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
  if (store == NULL)
  {
    return _setup_failure(transport, GetLastError());
  }
  if (!CertAddEncodedCertificateToStore(
          store,
          X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
          der,
          der_len,
          CERT_STORE_ADD_REPLACE_EXISTING,
          NULL))
  {
    DWORD const err = GetLastError();
    CertCloseStore(store, 0);
    return _setup_failure(transport, err);
  }
  *out_store = store;
  return AZ_OK;
}

/**
 * @brief Check @p server_cert chains to the custom root (else the system store) and names the
 * host. SCH_CRED_MANUAL_CRED_VALIDATION turns Schannel's own check off. Revocation is not checked
 * (as with the OpenSSL and mbedTLS layers).
 */
static az_result _validate_server_certificate(_tls_transport* transport, PCCERT_CONTEXT server_cert)
{
  wchar_t host_w[256];
  if (MultiByteToWideChar(
          CP_UTF8, 0, transport->host, -1, host_w, (int)(sizeof(host_w) / sizeof(host_w[0])))
      <= 0)
  {
    return _setup_failure(transport, GetLastError());
  }

  HCERTCHAINENGINE chain_engine = NULL;
  if (transport->custom_root != NULL)
  {
    CERT_CHAIN_ENGINE_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    // Through hExclusiveTrustedPeople: the size every implementation with hExclusiveRoot takes.
    cfg.cbSize
        = (DWORD)(offsetof(CERT_CHAIN_ENGINE_CONFIG, hExclusiveTrustedPeople) + sizeof(HCERTSTORE));
    cfg.hExclusiveRoot = transport->custom_root;
    if (!CertCreateCertificateChainEngine(&cfg, &chain_engine))
    {
      return _setup_failure(transport, GetLastError());
    }
  }

  // A certificate with an Extended Key Usage must allow server authentication.
  static LPSTR server_auth[] = { szOID_PKIX_KP_SERVER_AUTH };
  CERT_CHAIN_PARA chain_para;
  memset(&chain_para, 0, sizeof(chain_para));
  chain_para.cbSize = sizeof(chain_para);
  chain_para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
  chain_para.RequestedUsage.Usage.cUsageIdentifier = 1;
  chain_para.RequestedUsage.Usage.rgpszUsageIdentifier = server_auth;
  PCCERT_CHAIN_CONTEXT chain = NULL;
  if (!CertGetCertificateChain(
          chain_engine, server_cert, NULL, server_cert->hCertStore, &chain_para, 0, NULL, &chain)
      || chain == NULL)
  {
    _report(transport, AZ_MQTT_NATIVE_ERROR_TLS_VERIFY, GetLastError(), AZ_MQTT_ERROR_TLS_VERIFY);
    if (chain_engine != NULL)
    {
      CertFreeCertificateChainEngine(chain_engine);
    }
    return AZ_MQTT_ERROR_TLS_VERIFY;
  }

  HTTPSPolicyCallbackData https_policy;
  memset(&https_policy, 0, sizeof(https_policy));
  https_policy.cbStruct = sizeof(https_policy);
  https_policy.dwAuthType = AUTHTYPE_SERVER;
  https_policy.pwszServerName = host_w;
  CERT_CHAIN_POLICY_PARA policy_para;
  memset(&policy_para, 0, sizeof(policy_para));
  policy_para.cbSize = sizeof(policy_para);
  policy_para.pvExtraPolicyPara = &https_policy;
  CERT_CHAIN_POLICY_STATUS policy_status;
  memset(&policy_status, 0, sizeof(policy_status));
  policy_status.cbSize = sizeof(policy_status);

  BOOL const policy_ok = CertVerifyCertificateChainPolicy(
      CERT_CHAIN_POLICY_SSL, chain, &policy_para, &policy_status);
  DWORD policy_error = policy_ok ? policy_status.dwError : GetLastError(); // Before cleanup.
  // Rejected by the SSL policy too, but not by every implementation of it: checked here as well.
  static struct
  {
    DWORD trust;
    HRESULT error;
  } const rejected[] = {
    { CERT_TRUST_IS_PARTIAL_CHAIN | CERT_TRUST_IS_CYCLIC, CERT_E_CHAINING },
    { CERT_TRUST_IS_UNTRUSTED_ROOT, CERT_E_UNTRUSTEDROOT },
    { CERT_TRUST_IS_NOT_SIGNATURE_VALID, TRUST_E_CERT_SIGNATURE },
    { CERT_TRUST_IS_NOT_TIME_VALID, CERT_E_EXPIRED },
    { CERT_TRUST_IS_NOT_VALID_FOR_USAGE, CERT_E_WRONG_USAGE },
  };
  for (size_t i = 0; policy_error == 0 && i < sizeof(rejected) / sizeof(rejected[0]); ++i)
  {
    if ((chain->TrustStatus.dwErrorStatus & rejected[i].trust) != 0)
    {
      policy_error = (DWORD)rejected[i].error;
    }
  }
  CertFreeCertificateChain(chain);
  if (chain_engine != NULL)
  {
    CertFreeCertificateChainEngine(chain_engine);
  }
  if (!policy_ok || policy_error != 0)
  {
    _report(transport, AZ_MQTT_NATIVE_ERROR_TLS_VERIFY, policy_error, AZ_MQTT_ERROR_TLS_VERIFY);
    return AZ_MQTT_ERROR_TLS_VERIFY;
  }
  return AZ_OK;
}

// ──────────────────────── Session ────────────────────────────

/** @brief Point @p desc at the @p count buffers in @p buffers. */
static void _desc(SecBufferDesc* desc, SecBuffer* buffers, unsigned long count)
{
  desc->ulVersion = SECBUFFER_VERSION;
  desc->cBuffers = count;
  desc->pBuffers = buffers;
}

/** @brief Free the TLS session (not the connection below). */
static void _tls_free(_tls_transport* transport)
{
  if (transport->has_context)
  {
    DeleteSecurityContext(&transport->context);
  }
  if (transport->has_cred)
  {
    FreeCredentialsHandle(&transport->cred);
  }
  if (transport->custom_root != NULL)
  {
    CertCloseStore(transport->custom_root, 0);
    transport->custom_root = NULL;
  }
  transport->has_context = false;
  transport->has_cred = false;
  transport->needs_read = false;
  transport->handshake_done = false;
  transport->in_len = 0;
  transport->plain_pos = 0;
  transport->plain_len = 0;
  transport->out_pos = 0;
  transport->out_len = 0;
}

/** @brief Reject TLS options Schannel cannot honour here, before any socket work. */
static az_result _check_tls_options(az_mqtt_tls_options const* tls_options)
{
  az_result const rc = az_mqtt_tls_options_check(tls_options);
  if (az_result_failed(rc))
  {
    return rc;
  }
  // Not implemented for Schannel: refuse rather than connect without the identity, trust or
  // hook the caller asked for.
  if (az_span_size(tls_options->client_cert_path) > 0
      || az_span_size(tls_options->client_cert_pem) > 0
      || az_span_size(tls_options->ca_cert_pem) > 0 || tls_options->configure != NULL)
  {
    return AZ_MQTT_ERROR_NOT_SUPPORTED;
  }
  return AZ_OK;
}

/** @brief Acquire credentials and keep what the handshake needs; reads @p tls_options only here. */
static az_result _tls_prepare(
    _tls_transport* transport,
    az_span host,
    az_mqtt_tls_options const* tls_options)
{
  az_result rc = _span_to_cstr(host, transport->host, (int32_t)sizeof(transport->host));
  if (az_result_succeeded(rc))
  {
    rc = _build_custom_root(transport, tls_options->ca_cert_path, &transport->custom_root);
  }
  if (az_result_failed(rc))
  {
    return rc;
  }

  SCHANNEL_CRED cred;
  memset(&cred, 0, sizeof(cred));
  cred.dwVersion = SCHANNEL_CRED_VERSION;
  cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO;
  TimeStamp expiry;
  SECURITY_STATUS const sec = AcquireCredentialsHandleA(
      NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL, &transport->cred, &expiry);
  if (sec != SEC_E_OK)
  {
    return _setup_failure(transport, (DWORD)sec);
  }
  transport->has_cred = true;
  return AZ_OK;
}

/** @brief Queue the token in @p token (then freed) to be sent; false if it does not fit. */
static bool _queue_token(_tls_transport* transport, SecBuffer* token)
{
  bool fits = true;
  if (token->pvBuffer != NULL)
  {
    fits = token->cbBuffer <= sizeof(transport->out);
    if (fits && token->cbBuffer > 0)
    {
      memcpy(transport->out, token->pvBuffer, token->cbBuffer);
      transport->out_pos = 0;
      transport->out_len = (int32_t)token->cbBuffer;
    }
    FreeContextBuffer(token->pvBuffer);
    token->pvBuffer = NULL;
  }
  return fits;
}

/**
 * @brief Send the queued bytes below until @p deadline; resumable.
 * @retval AZ_MQTT_ERROR_TIMEOUT Not all sent yet.
 * @retval other Failed below (reported there).
 */
static az_result _flush(_tls_transport* transport, int64_t deadline)
{
  while (transport->out_pos < transport->out_len)
  {
    int32_t const wait_ms = _az_mqtt_io_layer_remaining(deadline);
    int32_t sent = 0;
    az_result const rc = _az_mqtt_io_layer_send_some(
        transport->lower,
        az_span_create(
            transport->out + transport->out_pos, transport->out_len - transport->out_pos),
        wait_ms,
        &sent);
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (sent == 0 && wait_ms == 0)
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }
    transport->out_pos += sent;
  }
  transport->out_pos = 0;
  transport->out_len = 0;
  return AZ_OK;
}

/**
 * @brief Receive more bytes from below into in, until @p deadline.
 * @retval AZ_MQTT_ERROR_TIMEOUT Nothing in time.
 * @retval other Failed below (reported there); AZ_ERROR_NOT_ENOUGH_SPACE: in is full.
 */
static az_result _fill(_tls_transport* transport, int64_t deadline)
{
  if (transport->in_len == (int32_t)sizeof(transport->in))
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  for (;;)
  {
    int32_t const wait_ms = _az_mqtt_io_layer_remaining(deadline);
    az_span received;
    az_result const rc = az_mqtt_transport_receive(
        _LOWER_T(transport),
        az_span_create(
            transport->in + transport->in_len, (int32_t)sizeof(transport->in) - transport->in_len),
        wait_ms,
        &received);
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (az_span_size(received) > 0)
    {
      transport->in_len += az_span_size(received);
      return AZ_OK;
    }
    if (wait_ms == 0)
    {
      return AZ_MQTT_ERROR_TIMEOUT;
    }
  }
}

/** @brief Keep only the last @p extra bytes of in (SECBUFFER_EXTRA: not consumed). */
static void _keep_extra(_tls_transport* transport, unsigned long extra)
{
  memmove(transport->in, transport->in + (transport->in_len - (int32_t)extra), extra);
  transport->in_len = (int32_t)extra;
}

/**
 * @brief Drive the handshake until done, failed or @p deadline; resumable.
 * Failures below were reported there (as AZ_MQTT_ERROR_TLS_HANDSHAKE).
 */
static az_result _tls_handshake(_tls_transport* transport, int64_t deadline)
{
  for (;;)
  {
    az_result rc = _flush(transport, deadline);
    if (az_result_failed(rc))
    {
      return rc == AZ_MQTT_ERROR_TIMEOUT ? rc : AZ_MQTT_ERROR_TLS_HANDSHAKE;
    }
    if (transport->handshake_done)
    {
      break;
    }
    if (transport->needs_read)
    {
      rc = _fill(transport, deadline);
      if (az_result_failed(rc))
      {
        return rc == AZ_MQTT_ERROR_TIMEOUT ? rc : AZ_MQTT_ERROR_TLS_HANDSHAKE;
      }
      transport->needs_read = false;
    }

    SecBuffer out_buf = { 0, SECBUFFER_TOKEN, NULL };
    SecBufferDesc out_desc;
    _desc(&out_desc, &out_buf, 1);
    SecBuffer in_bufs[2] = { { (unsigned long)transport->in_len, SECBUFFER_TOKEN, transport->in },
                             { 0, SECBUFFER_EMPTY, NULL } };
    SecBufferDesc in_desc;
    _desc(&in_desc, in_bufs, 2);
    DWORD out_flags = 0;
    TimeStamp expiry;
    SECURITY_STATUS const status = InitializeSecurityContextA(
        &transport->cred,
        transport->has_context ? &transport->context : NULL,
        transport->host,
        _REQUEST_FLAGS,
        0,
        SECURITY_NATIVE_DREP,
        transport->has_context ? &in_desc : NULL,
        0,
        &transport->context,
        &out_desc,
        &out_flags,
        &expiry);
    if (!transport->has_context && (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED))
    {
      transport->has_context = true;
    }
    if (!_queue_token(transport, &out_buf))
    {
      _report(
          transport,
          AZ_MQTT_NATIVE_ERROR_TLS,
          (DWORD)SEC_E_BUFFER_TOO_SMALL,
          AZ_MQTT_ERROR_TLS_HANDSHAKE);
      return AZ_MQTT_ERROR_TLS_HANDSHAKE;
    }
    if (status == SEC_E_INCOMPLETE_MESSAGE)
    {
      transport->needs_read = true;
      continue;
    }
    if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
    {
      _report(transport, AZ_MQTT_NATIVE_ERROR_TLS, (DWORD)status, AZ_MQTT_ERROR_TLS_HANDSHAKE);
      (void)_flush(transport, 0); // The alert, if any: without waiting.
      return AZ_MQTT_ERROR_TLS_HANDSHAKE;
    }
    if (transport->has_context && in_bufs[1].BufferType == SECBUFFER_EXTRA)
    {
      _keep_extra(transport, in_bufs[1].cbBuffer);
    }
    else
    {
      transport->in_len = 0;
    }
    transport->handshake_done = status == SEC_E_OK;
    transport->needs_read = status == SEC_I_CONTINUE_NEEDED && transport->in_len == 0;
  }

  SECURITY_STATUS sec
      = QueryContextAttributesA(&transport->context, SECPKG_ATTR_STREAM_SIZES, &transport->sizes);
  PCCERT_CONTEXT server_cert = NULL;
  if (sec == SEC_E_OK)
  {
    sec = QueryContextAttributesA(
        &transport->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, (PVOID)&server_cert);
  }
  if (sec != SEC_E_OK || server_cert == NULL)
  {
    _report(transport, AZ_MQTT_NATIVE_ERROR_TLS, (DWORD)sec, AZ_MQTT_ERROR_TLS_HANDSHAKE);
    return AZ_MQTT_ERROR_TLS_HANDSHAKE;
  }
  az_result const rc = _validate_server_certificate(transport, server_cert);
  CertFreeCertificateContext(server_cert);
  return rc;
}

/**
 * @brief Send close_notify, once per connection, if the session is up: without waiting on the
 * layer below. Native errors it meets are reported (from below, with their result; Schannel's,
 * with AZ_MQTT_ERROR_TRANSPORT).
 */
static void _send_close_notify(_tls_transport* transport)
{
  if (transport->stage != _STAGE_OPEN || !transport->has_context || transport->closing)
  {
    return;
  }
  transport->closing = true;
  DWORD type = SCHANNEL_SHUTDOWN;
  SecBuffer control = { sizeof(type), SECBUFFER_TOKEN, NULL };
  control.pvBuffer = &type;
  SecBufferDesc control_desc;
  _desc(&control_desc, &control, 1);
  SECURITY_STATUS status = ApplyControlToken(&transport->context, &control_desc);
  if (status == SEC_E_OK)
  {
    SecBuffer out_buf = { 0, SECBUFFER_TOKEN, NULL };
    SecBufferDesc out_desc;
    _desc(&out_desc, &out_buf, 1);
    DWORD out_flags = 0;
    TimeStamp expiry;
    status = InitializeSecurityContextA(
        &transport->cred,
        &transport->context,
        transport->host,
        _REQUEST_FLAGS,
        0,
        SECURITY_NATIVE_DREP,
        NULL,
        0,
        &transport->context,
        &out_desc,
        &out_flags,
        &expiry);
    transport->out_pos = 0;
    transport->out_len = 0;
    if (!_queue_token(transport, &out_buf))
    {
      status = SEC_E_BUFFER_TOO_SMALL;
    }
  }
  if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
  {
    _report(transport, AZ_MQTT_NATIVE_ERROR_TLS, (DWORD)status, AZ_MQTT_ERROR_TRANSPORT);
    return;
  }
  (void)_flush(transport, 0); // Never waits; failures below are reported there.
}

// ──────────────────────── Transport ──────────────────────────

static void _close(az_mqtt_transport* t)
{
  _send_close_notify(_T(t));
  _tls_free(_T(t));
  _T(t)->stage = _STAGE_IDLE;
  az_mqtt_transport_close(_LOWER_T(_T(t)));
}

static az_result _connect_start(
    az_mqtt_transport* t,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  _tls_transport* const transport = _T(t);
  _close(t); // A connect replaces the connection.
  transport->closing = false;
  transport->errors.connect_attempt++;
  if (tls_options != NULL)
  {
    az_result rc = _check_tls_options(tls_options);
    if (az_result_succeeded(rc))
    {
      rc = _tls_prepare(transport, host, tls_options);
    }
    if (az_result_failed(rc))
    {
      _tls_free(transport);
      return rc;
    }
  }
  az_result const rc = az_mqtt_transport_connect_start(_LOWER_T(transport), host, port, NULL);
  if (az_result_failed(rc))
  {
    _tls_free(transport);
    return rc;
  }
  transport->stage = _STAGE_LOWER;
  return AZ_OK;
}

static az_result _connect_poll(az_mqtt_transport* t, int32_t timeout_ms)
{
  _tls_transport* const transport = _T(t);
  int64_t const deadline = _az_mqtt_io_layer_deadline(timeout_ms);
  if (transport->stage == _STAGE_OPEN || transport->stage == _STAGE_IDLE)
  {
    return transport->stage == _STAGE_OPEN ? AZ_OK : AZ_MQTT_ERROR_INVALID_STATE;
  }
  if (transport->stage == _STAGE_LOWER)
  {
    az_result const rc = az_mqtt_transport_connect_poll(_LOWER_T(transport), timeout_ms);
    if (az_result_failed(rc))
    {
      if (rc != AZ_MQTT_ERROR_TIMEOUT)
      {
        _close(t);
      }
      return rc;
    }
    transport->stage = transport->has_cred ? _STAGE_HANDSHAKE : _STAGE_OPEN;
  }
  if (transport->stage == _STAGE_HANDSHAKE)
  {
    transport->errors.phase_result = AZ_MQTT_ERROR_TLS_HANDSHAKE;
    az_result const rc = _tls_handshake(transport, deadline);
    transport->errors.phase_result = AZ_OK;
    if (az_result_failed(rc))
    {
      if (rc != AZ_MQTT_ERROR_TIMEOUT)
      {
        _close(t);
      }
      return rc;
    }
    transport->stage = _STAGE_OPEN;
  }
  return AZ_OK;
}

static az_result _send(az_mqtt_transport* t, az_span data)
{
  _tls_transport* const transport = _T(t);
  if (transport->stage != _STAGE_OPEN)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  if (!transport->has_cred)
  {
    return az_mqtt_transport_send(_LOWER_T(transport), data);
  }
  int64_t const deadline = _az_mqtt_io_layer_deadline(AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS);
  int32_t const header = (int32_t)transport->sizes.cbHeader;
  int32_t const trailer = (int32_t)transport->sizes.cbTrailer;
  int32_t max_chunk = (int32_t)transport->sizes.cbMaximumMessage;
  if (max_chunk > (int32_t)sizeof(transport->out) - header - trailer)
  {
    max_chunk = (int32_t)sizeof(transport->out) - header - trailer;
  }
  while (az_span_size(data) > 0)
  {
    int32_t const chunk = az_span_size(data) < max_chunk ? az_span_size(data) : max_chunk;
    memcpy(transport->out + header, az_span_ptr(data), (size_t)chunk);
    SecBuffer bufs[4] = {
      { (unsigned long)header, SECBUFFER_STREAM_HEADER, transport->out },
      { (unsigned long)chunk, SECBUFFER_DATA, transport->out + header },
      { (unsigned long)trailer, SECBUFFER_STREAM_TRAILER, transport->out + header + chunk },
      { 0, SECBUFFER_EMPTY, NULL },
    };
    SecBufferDesc desc;
    _desc(&desc, bufs, 4);
    SECURITY_STATUS const sec = EncryptMessage(&transport->context, 0, &desc, 0);
    az_result rc = AZ_OK;
    if (sec != SEC_E_OK)
    {
      rc = AZ_MQTT_ERROR_TRANSPORT;
      _report(transport, AZ_MQTT_NATIVE_ERROR_TLS, (DWORD)sec, rc);
    }
    else
    {
      transport->out_pos = 0;
      transport->out_len = (int32_t)(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
      rc = _flush(transport, deadline);
    }
    if (az_result_failed(rc))
    {
      // A partial packet may be on the wire: the connection is unusable.
      transport->stage = _STAGE_IDLE;
      return rc;
    }
    data = az_span_slice_to_end(data, chunk);
  }
  return AZ_OK;
}

/** @brief Deliver decrypted bytes not yet delivered into @p buffer, if any. */
static bool _deliver(_tls_transport* transport, az_span buffer, az_span* out_received)
{
  int32_t n = transport->plain_len - transport->plain_pos;
  if (n <= 0)
  {
    return false;
  }
  if (n > az_span_size(buffer))
  {
    n = az_span_size(buffer);
  }
  memcpy(az_span_ptr(buffer), transport->plain + transport->plain_pos, (size_t)n);
  transport->plain_pos += n;
  if (transport->plain_pos == transport->plain_len)
  {
    transport->plain_pos = 0;
    transport->plain_len = 0;
  }
  *out_received = az_span_slice(buffer, 0, n);
  return true;
}

/**
 * @brief Decrypt the next record in in, if complete, into plain.
 * @param[out] out_incomplete No complete record in in.
 */
static az_result _decrypt(_tls_transport* transport, bool* out_incomplete)
{
  *out_incomplete = transport->in_len == 0;
  if (*out_incomplete)
  {
    return AZ_OK;
  }
  SecBuffer bufs[4] = {
    { (unsigned long)transport->in_len, SECBUFFER_DATA, transport->in },
    { 0, SECBUFFER_EMPTY, NULL },
    { 0, SECBUFFER_EMPTY, NULL },
    { 0, SECBUFFER_EMPTY, NULL },
  };
  SecBufferDesc desc;
  _desc(&desc, bufs, 4);
  SECURITY_STATUS const sec = DecryptMessage(&transport->context, &desc, 0, NULL);
  if (sec == SEC_E_INCOMPLETE_MESSAGE)
  {
    *out_incomplete = true;
    return AZ_OK;
  }
  if (sec == SEC_I_CONTEXT_EXPIRED)
  {
    return AZ_MQTT_ERROR_CONNECTION_CLOSED; // close_notify from the peer.
  }
  if (sec != SEC_E_OK)
  {
    _report(transport, AZ_MQTT_NATIVE_ERROR_TLS, (DWORD)sec, AZ_MQTT_ERROR_TRANSPORT);
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  SecBuffer const* data = NULL;
  SecBuffer const* extra = NULL;
  for (int i = 0; i < 4; ++i)
  {
    if (bufs[i].BufferType == SECBUFFER_DATA)
    {
      data = &bufs[i];
    }
    else if (bufs[i].BufferType == SECBUFFER_EXTRA)
    {
      extra = &bufs[i];
    }
  }
  if (data != NULL && data->cbBuffer > 0)
  {
    // A record holds at most cbMaximumMessage (16 KiB) bytes: it fits.
    memcpy(transport->plain, data->pvBuffer, data->cbBuffer);
    transport->plain_pos = 0;
    transport->plain_len = (int32_t)data->cbBuffer;
  }
  if (extra != NULL && extra->cbBuffer > 0)
  {
    _keep_extra(transport, extra->cbBuffer);
  }
  else
  {
    transport->in_len = 0;
  }
  return AZ_OK;
}

static az_result _receive(
    az_mqtt_transport* t,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received)
{
  _tls_transport* const transport = _T(t);
  *out_received = AZ_SPAN_EMPTY;
  if (transport->stage != _STAGE_OPEN)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  if (!transport->has_cred)
  {
    return az_mqtt_transport_receive(_LOWER_T(transport), buffer, timeout_ms, out_received);
  }
  int64_t const deadline = _az_mqtt_io_layer_deadline(timeout_ms);
  for (;;)
  {
    if (_deliver(transport, buffer, out_received))
    {
      return AZ_OK;
    }
    bool incomplete = false;
    az_result rc = _decrypt(transport, &incomplete);
    if (az_result_succeeded(rc) && incomplete)
    {
      rc = _fill(transport, deadline);
      if (rc == AZ_MQTT_ERROR_TIMEOUT)
      {
        return AZ_OK; // Timed out (a partial record waits for the next call).
      }
    }
    if (az_result_failed(rc))
    {
      transport->stage = _STAGE_IDLE;
      return rc;
    }
  }
}

static void _shutdown(az_mqtt_transport* t)
{
  _send_close_notify(_T(t));
  az_mqtt_transport_shutdown(_LOWER_T(_T(t)));
}

static az_result _set_proxy(az_mqtt_transport* t, az_mqtt_proxy_options const* proxy)
{
  return az_mqtt_transport_set_proxy(_LOWER_T(_T(t)), proxy);
}

static void _set_error_callback(
    az_mqtt_transport* t,
    az_mqtt_transport_error_fn callback,
    void* context)
{
  _T(t)->errors.callback = callback;
  _T(t)->errors.context = context;
}

static az_mqtt_transport_vtable const _vtable = {
  _connect_start, _connect_poll, _send,      _receive,
  _shutdown,      _close,        _set_proxy, _set_error_callback,
};

int32_t _az_mqtt_tls_transport_sizeof(void) { return (int32_t)sizeof(_tls_transport); }

az_result _az_mqtt_tls_transport_init(az_mqtt_transport* transport, _az_mqtt_io_layer* lower)
{
  _az_PRECONDITION_NOT_NULL(transport);
  _az_PRECONDITION_NOT_NULL(lower);
  _tls_transport* const tls = _T(transport);
  memset(tls, 0, sizeof(*tls));
  tls->base.vtable = &_vtable;
  tls->lower = lower;
  _az_mqtt_io_layer_errors_attach(&tls->errors, &lower->base);
  return AZ_OK;
}
