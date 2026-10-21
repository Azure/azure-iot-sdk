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
#include <stdio.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include <windows.h>

#include <ncrypt.h>
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
  /** @brief The server's certificate request was answered (client certificate, or none). */
  bool cert_request_answered;
  /** @brief ISC_REQ_* of InitializeSecurityContext(): _REQUEST_FLAGS, then USE_SUPPLIED_CREDS. */
  unsigned long request_flags;
  /** @brief The credentials without the client certificate, once replaced by those with it. */
  CredHandle cred_initial;
  bool has_cred_initial;
  /**
   * @brief Server bytes recorded in plain (idle until open) until the certificate request, with a
   * client certificate to present: Schannel gives no server certificate before the handshake ends.
   */
  bool capturing;
  bool capture_overflow;
  int32_t captured;
  uint8_t stage;
  /** @brief close_notify sent (or tried) on this connection. */
  bool closing;
  SecPkgContext_StreamSizes sizes;
  /** @brief Server name (SNI, certificate check); trust anchor (NULL: system store). */
  char host[256];
  HCERTSTORE custom_root;
  /**
   * @brief Client certificate (NULL: none), the store holding it and its chain, and its key:
   * persisted (Schannel takes no ephemeral key) and deleted with the session.
   */
  PCCERT_CONTEXT client_cert;
  HCERTSTORE client_store;
  NCRYPT_KEY_HANDLE client_key;
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

/**
 * @brief ISC_REQ_* of InitializeSecurityContext() until the server requests a client certificate.
 * Without USE_SUPPLIED_CREDS, that request returns SEC_I_INCOMPLETE_CREDENTIALS (credentials have
 * SCH_CRED_NO_DEFAULT_CREDS: none is picked from the user's store), so it can be answered; from
 * then on with USE_SUPPLIED_CREDS (_tls_transport.request_flags).
 */
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

/** @brief Offset of @p needle in @p hay[0, @p hay_len), or -1. */
static int32_t _find(char const* hay, int32_t hay_len, char const* needle)
{
  int32_t const n = (int32_t)strlen(needle);
  for (int32_t i = 0; i + n <= hay_len; i++)
  {
    if (memcmp(hay + i, needle, (size_t)n) == 0)
    {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Add DER certificate @p der to @p store. With @p first (may be NULL) still NULL, it gets
 * this certificate (CertFreeCertificateContext() it).
 */
static az_result _add_der(
    _tls_transport* transport,
    HCERTSTORE store,
    BYTE const* der,
    DWORD len,
    PCCERT_CONTEXT* first)
{
  if (!CertAddEncodedCertificateToStore(
          store,
          X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
          der,
          len,
          CERT_STORE_ADD_REPLACE_EXISTING,
          first != NULL && *first == NULL ? first : NULL))
  {
    return _setup_failure(transport, GetLastError());
  }
  return AZ_OK;
}

#define _PEM_BEGIN "-----BEGIN CERTIFICATE-----"
#define _PEM_END "-----END CERTIFICATE-----"
/** @brief Largest certificate, as PEM text and as DER. */
#define _PEM_MAX (32 * 1024)
#define _DER_MAX (24 * 1024)

/**
 * @brief Add every certificate in @p path to @p store: each CERTIFICATE block of a PEM file (a
 * bundle of any length, read in pieces), or the single certificate of a DER file. @p first: as
 * _add_der().
 */
static az_result _add_cert_file(
    _tls_transport* transport,
    az_span path,
    HCERTSTORE store,
    PCCERT_CONTEXT* first)
{
  char path_str[260];
  az_result rc = _span_to_cstr(path, path_str, (int32_t)sizeof(path_str));
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

  char text[_PEM_MAX];
  BYTE der[_DER_MAX];
  int32_t len = 0;
  int32_t added = 0;
  bool eof = false;
  bool pem = false;
  rc = AZ_OK;
  while (az_result_succeeded(rc))
  {
    if (!eof && len < (int32_t)sizeof(text))
    {
      DWORD got = 0;
      if (!ReadFile(h, text + len, (DWORD)((int32_t)sizeof(text) - len), &got, NULL))
      {
        rc = _setup_failure(transport, GetLastError());
        break;
      }
      eof = got == 0;
      len += (int32_t)got;
    }
    int32_t const begin = _find(text, len, _PEM_BEGIN);
    if (begin < 0)
    {
      if (!pem && added == 0 && eof)
      {
        // No PEM block anywhere: a DER file, which must fit in one read.
        if (len == 0 || len > (int32_t)sizeof(der))
        {
          rc = _setup_failure(transport, ERROR_INVALID_DATA);
          break;
        }
        memcpy(der, text, (size_t)len);
        rc = _add_der(transport, store, der, (DWORD)len, first);
        added += az_result_succeeded(rc) ? 1 : 0;
        break;
      }
      if (eof)
      {
        break;
      }
      // Keep a tail that may hold the start of a marker; a DER file is decided at its end.
      int32_t const keep = pem ? (int32_t)sizeof(_PEM_BEGIN) - 2 : len;
      if (pem && len > keep)
      {
        memmove(text, text + len - keep, (size_t)keep);
        len = keep;
      }
      else if (!pem && len == (int32_t)sizeof(text))
      {
        pem = true; // Too large for DER: only PEM is accepted from here on.
      }
      continue;
    }
    pem = true;
    int32_t const end_at = _find(text + begin, len - begin, _PEM_END);
    if (end_at < 0)
    {
      if (eof || (begin == 0 && len == (int32_t)sizeof(text)))
      {
        rc = _setup_failure(transport, ERROR_INVALID_DATA); // Truncated, or over _PEM_MAX.
        break;
      }
      memmove(text, text + begin, (size_t)(len - begin));
      len -= begin;
      continue;
    }
    int32_t const block_len = end_at + (int32_t)sizeof(_PEM_END) - 1;
    DWORD der_len = (DWORD)sizeof(der);
    if (!CryptStringToBinaryA(
            text + begin, (DWORD)block_len, CRYPT_STRING_BASE64HEADER, der, &der_len, NULL, NULL))
    {
      rc = _setup_failure(transport, GetLastError());
      break;
    }
    rc = _add_der(transport, store, der, der_len, first);
    added++;
    int32_t const consumed = begin + block_len;
    memmove(text, text + consumed, (size_t)(len - consumed));
    len -= consumed;
  }
  CloseHandle(h);
  if (az_result_succeeded(rc) && added == 0)
  {
    rc = _setup_failure(transport, ERROR_INVALID_DATA);
  }
  return rc;
}

/** @brief Add every CERTIFICATE block of @p pem to @p store. @p first: as _add_der(). */
static az_result _add_cert_pem(
    _tls_transport* transport,
    az_span pem,
    HCERTSTORE store,
    PCCERT_CONTEXT* first)
{
  char const* text = (char const*)az_span_ptr(pem);
  int32_t len = az_span_size(pem);
  int32_t added = 0;
  BYTE der[_DER_MAX];
  for (int32_t begin = _find(text, len, _PEM_BEGIN); begin >= 0;
       begin = _find(text, len, _PEM_BEGIN))
  {
    int32_t const end_at = _find(text + begin, len - begin, _PEM_END);
    if (end_at < 0)
    {
      return _setup_failure(transport, ERROR_INVALID_DATA); // Truncated.
    }
    int32_t const block_len = end_at + (int32_t)sizeof(_PEM_END) - 1;
    DWORD der_len = (DWORD)sizeof(der);
    if (!CryptStringToBinaryA(
            text + begin, (DWORD)block_len, CRYPT_STRING_BASE64HEADER, der, &der_len, NULL, NULL))
    {
      return _setup_failure(transport, GetLastError());
    }
    az_result const rc = _add_der(transport, store, der, der_len, first);
    if (az_result_failed(rc))
    {
      return rc;
    }
    added++;
    text += begin + block_len;
    len -= begin + block_len;
  }
  return added > 0 ? AZ_OK : _setup_failure(transport, ERROR_INVALID_DATA);
}

/** @brief A new in-memory certificate store. */
static az_result _new_store(_tls_transport* transport, HCERTSTORE* out_store)
{
  *out_store = CertOpenStore(
      CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
  return *out_store != NULL ? AZ_OK : _setup_failure(transport, GetLastError());
}

/**
 * @brief A store with the CAs of ca_cert_path or ca_cert_pem of @p tls_options (*@p out_store
 * NULL if neither is given).
 */
static az_result _build_custom_root(
    _tls_transport* transport,
    az_mqtt_tls_options const* tls_options,
    HCERTSTORE* out_store)
{
  *out_store = NULL;
  if (az_span_size(tls_options->ca_cert_path) == 0 && az_span_size(tls_options->ca_cert_pem) == 0)
  {
    return AZ_OK;
  }

  HCERTSTORE store = NULL;
  az_result rc = _new_store(transport, &store);
  if (az_result_failed(rc))
  {
    return rc;
  }
  rc = az_span_size(tls_options->ca_cert_path) > 0
      ? _add_cert_file(transport, tls_options->ca_cert_path, store, NULL)
      : _add_cert_pem(transport, tls_options->ca_cert_pem, store, NULL);
  if (az_result_failed(rc))
  {
    CertCloseStore(store, 0);
    return rc;
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

// ──────────────────────── Client identity ────────────────────

/** @brief Read the whole file @p path, at most @p size bytes, into @p buf. */
static az_result _read_file(
    _tls_transport* transport,
    az_span path,
    char* buf,
    int32_t size,
    int32_t* out_len)
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
  DWORD err = ERROR_SUCCESS;
  LARGE_INTEGER file_size;
  if (!GetFileSizeEx(h, &file_size))
  {
    err = GetLastError();
  }
  else if (file_size.QuadPart > size)
  {
    err = ERROR_FILE_TOO_LARGE;
  }
  int32_t len = 0;
  DWORD got = 1;
  while (err == ERROR_SUCCESS && got > 0 && len < size)
  {
    if (!ReadFile(h, buf + len, (DWORD)(size - len), &got, NULL))
    {
      err = GetLastError();
    }
    len += (int32_t)got;
  }
  char extra;
  if (err == ERROR_SUCCESS && len == size && ReadFile(h, &extra, 1, &got, NULL) && got > 0)
  {
    err = ERROR_FILE_TOO_LARGE; // Grew since GetFileSizeEx().
  }
  CloseHandle(h);
  *out_len = len;
  return err == ERROR_SUCCESS ? AZ_OK : _setup_failure(transport, err);
}

/** @brief Wrap private key @p key of algorithm @p alg_oid (@p params: DER) as PKCS#8. */
static az_result _wrap_pkcs8(
    _tls_transport* transport,
    char const* alg_oid,
    BYTE const* params,
    DWORD params_len,
    BYTE* key,
    DWORD key_len,
    BYTE** out,
    DWORD* out_len)
{
  CRYPT_PRIVATE_KEY_INFO info;
  memset(&info, 0, sizeof(info));
  info.Algorithm.pszObjId = (LPSTR)(uintptr_t)alg_oid;
  info.Algorithm.Parameters.cbData = params_len;
  info.Algorithm.Parameters.pbData = (BYTE*)(uintptr_t)params;
  info.PrivateKey.cbData = key_len;
  info.PrivateKey.pbData = key;
  if (!CryptEncodeObjectEx(
          X509_ASN_ENCODING,
          PKCS_PRIVATE_KEY_INFO,
          &info,
          CRYPT_ENCODE_ALLOC_FLAG,
          NULL,
          out,
          out_len))
  {
    return _setup_failure(transport, GetLastError());
  }
  return AZ_OK;
}

/** @brief The DER named-curve parameters of the EC private key (SEC1) @p der, or NULL. */
static BYTE const* _ec_curve_params(BYTE const* der, DWORD der_len, DWORD* out_len)
{
  static struct
  {
    char const* oid;
    BYTE der[10];
    DWORD len;
  } const curves[] = {
    { szOID_ECC_CURVE_P256, { 0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 }, 10 },
    { szOID_ECC_CURVE_P384, { 0x06, 0x05, 0x2B, 0x81, 0x04, 0x00, 0x22 }, 7 },
    { szOID_ECC_CURVE_P521, { 0x06, 0x05, 0x2B, 0x81, 0x04, 0x00, 0x23 }, 7 },
  };
  CRYPT_ECC_PRIVATE_KEY_INFO* info = NULL;
  DWORD info_len = 0;
  BYTE const* params = NULL;
  if (CryptDecodeObjectEx(
          X509_ASN_ENCODING,
          X509_ECC_PRIVATE_KEY,
          der,
          der_len,
          CRYPT_DECODE_ALLOC_FLAG,
          NULL,
          &info,
          &info_len))
  {
    for (size_t i = 0; info->szCurveOid != NULL && i < sizeof(curves) / sizeof(curves[0]); i++)
    {
      if (strcmp(info->szCurveOid, curves[i].oid) == 0)
      {
        params = curves[i].der;
        *out_len = curves[i].len;
      }
    }
    SecureZeroMemory(info, info_len);
    LocalFree(info);
  }
  return params;
}

/**
 * @brief The unencrypted private key of PEM @p text as PKCS#8 DER in *@p out (LocalFree() it): a
 * PRIVATE KEY (PKCS#8), RSA PRIVATE KEY (PKCS#1) or EC PRIVATE KEY (SEC1; P-256, P-384, P-521)
 * block.
 */
static az_result _key_to_pkcs8(
    _tls_transport* transport,
    char const* text,
    int32_t len,
    BYTE** out,
    DWORD* out_len)
{
  static char const* const labels[] = { "PRIVATE KEY", "RSA PRIVATE KEY", "EC PRIVATE KEY" };
  static BYTE const rsa_params[] = { 0x05, 0x00 }; // NULL
  for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
  {
    char begin_marker[40];
    char end_marker[40];
    (void)snprintf(begin_marker, sizeof(begin_marker), "-----BEGIN %s-----", labels[i]);
    (void)snprintf(end_marker, sizeof(end_marker), "-----END %s-----", labels[i]);
    int32_t const begin = _find(text, len, begin_marker);
    int32_t const end_at = begin < 0 ? -1 : _find(text + begin, len - begin, end_marker);
    if (end_at < 0)
    {
      continue;
    }
    BYTE der[_DER_MAX];
    DWORD der_len = (DWORD)sizeof(der);
    az_result rc = AZ_OK;
    if (!CryptStringToBinaryA(
            text + begin,
            (DWORD)(end_at + (int32_t)strlen(end_marker)),
            CRYPT_STRING_BASE64HEADER,
            der,
            &der_len,
            NULL,
            NULL))
    {
      rc = _setup_failure(transport, GetLastError());
    }
    else if (i == 0)
    {
      *out = (BYTE*)LocalAlloc(LMEM_FIXED, der_len);
      if (*out == NULL)
      {
        rc = _setup_failure(transport, GetLastError());
      }
      else
      {
        memcpy(*out, der, der_len);
        *out_len = der_len;
      }
    }
    else if (i == 1)
    {
      rc = _wrap_pkcs8(
          transport, szOID_RSA_RSA, rsa_params, sizeof(rsa_params), der, der_len, out, out_len);
    }
    else
    {
      DWORD params_len = 0;
      BYTE const* const params = _ec_curve_params(der, der_len, &params_len);
      rc = params == NULL
          ? _setup_failure(transport, (DWORD)NTE_NOT_SUPPORTED)
          : _wrap_pkcs8(
              transport, szOID_ECC_PUBLIC_KEY, params, params_len, der, der_len, out, out_len);
    }
    SecureZeroMemory(der, sizeof(der));
    return rc;
  }
  return _setup_failure(transport, ERROR_INVALID_DATA);
}

/** @brief Import PKCS#8 @p pk8 as a new persisted key of the user's software KSP, named @p name. */
static az_result _import_key(_tls_transport* transport, BYTE* pk8, DWORD pk8_len, wchar_t* name)
{
  NCRYPT_PROV_HANDLE provider = 0;
  SECURITY_STATUS status = NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0);
  if (status != ERROR_SUCCESS)
  {
    return _setup_failure(transport, (DWORD)status);
  }
  NCryptBuffer param;
  param.cbBuffer = (ULONG)((wcslen(name) + 1) * sizeof(wchar_t));
  param.BufferType = NCRYPTBUFFER_PKCS_KEY_NAME;
  param.pvBuffer = name;
  NCryptBufferDesc params;
  params.ulVersion = NCRYPTBUFFER_VERSION;
  params.cBuffers = 1;
  params.pBuffers = &param;
  NCRYPT_KEY_HANDLE key = 0;
  // Named and not finalized: NCryptFinalizeKey() persists it.
  status = NCryptImportKey(
      provider,
      0,
      NCRYPT_PKCS8_PRIVATE_KEY_BLOB,
      &params,
      &key,
      pk8,
      pk8_len,
      NCRYPT_DO_NOT_FINALIZE_FLAG | NCRYPT_SILENT_FLAG);
  if (status == ERROR_SUCCESS)
  {
    status = NCryptFinalizeKey(key, NCRYPT_SILENT_FLAG);
    if (status != ERROR_SUCCESS)
    {
      NCryptFreeObject(key);
    }
  }
  NCryptFreeObject(provider);
  if (status != ERROR_SUCCESS)
  {
    return _setup_failure(transport, (DWORD)status);
  }
  transport->client_key = key;
  return AZ_OK;
}

/** @brief Whether the public key of the client key is the client certificate's. */
static az_result _check_key_matches(_tls_transport* transport)
{
  DWORD len = 0;
  if (!CryptExportPublicKeyInfo(transport->client_key, 0, X509_ASN_ENCODING, NULL, &len))
  {
    return _setup_failure(transport, GetLastError());
  }
  CERT_PUBLIC_KEY_INFO* info = (CERT_PUBLIC_KEY_INFO*)LocalAlloc(LMEM_FIXED, len);
  if (info == NULL)
  {
    return _setup_failure(transport, GetLastError());
  }
  DWORD err = ERROR_SUCCESS;
  if (!CryptExportPublicKeyInfo(transport->client_key, 0, X509_ASN_ENCODING, info, &len))
  {
    err = GetLastError();
  }
  else if (!CertComparePublicKeyInfo(
               X509_ASN_ENCODING, &transport->client_cert->pCertInfo->SubjectPublicKeyInfo, info))
  {
    err = (DWORD)NTE_BAD_PUBLIC_KEY;
  }
  LocalFree(info);
  return err == ERROR_SUCCESS ? AZ_OK : _setup_failure(transport, err);
}

/**
 * @brief Add the chain certificates of the client store (all but the leaf and self-issued ones)
 * to the user's intermediate CA store, where Schannel looks for the chain it sends; they stay.
 */
static az_result _publish_chain(_tls_transport* transport)
{
  HCERTSTORE user_ca = NULL;
  DWORD err = ERROR_SUCCESS;
  PCCERT_CONTEXT cert = NULL;
  while (err == ERROR_SUCCESS
         && (cert = CertEnumCertificatesInStore(transport->client_store, cert)) != NULL)
  {
    if (CertCompareCertificate(
            X509_ASN_ENCODING, cert->pCertInfo, transport->client_cert->pCertInfo)
        || CertCompareCertificateName(
            X509_ASN_ENCODING, &cert->pCertInfo->Issuer, &cert->pCertInfo->Subject))
    {
      continue;
    }
    if (user_ca == NULL)
    {
      user_ca = CertOpenStore(
          CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"CA");
      if (user_ca == NULL)
      {
        err = GetLastError();
        break;
      }
    }
    if (!CertAddCertificateContextToStore(user_ca, cert, CERT_STORE_ADD_USE_EXISTING, NULL))
    {
      err = GetLastError();
    }
  }
  if (cert != NULL)
  {
    CertFreeCertificateContext(cert); // Left early.
  }
  if (user_ca != NULL)
  {
    CertCloseStore(user_ca, 0);
  }
  return err == ERROR_SUCCESS ? AZ_OK : _setup_failure(transport, err);
}

/**
 * @brief Load the client certificate (with any chain after it) and its key of @p tls_options; the
 * key is imported for the session and tied to the certificate. Freed by _tls_free().
 */
static az_result _load_client_identity(
    _tls_transport* transport,
    az_mqtt_tls_options const* tls_options)
{
  az_result rc = _new_store(transport, &transport->client_store);
  if (az_result_succeeded(rc))
  {
    HCERTSTORE const store = transport->client_store;
    rc = az_span_size(tls_options->client_cert_path) > 0
        ? _add_cert_file(transport, tls_options->client_cert_path, store, &transport->client_cert)
        : _add_cert_pem(transport, tls_options->client_cert_pem, store, &transport->client_cert);
  }
  if (az_result_succeeded(rc))
  {
    rc = _publish_chain(transport);
  }
  if (az_result_failed(rc))
  {
    return rc;
  }

  // az_mqtt_tls_options_check() and _check_tls_options(): the key is a file or PEM in memory.
  char const* key_text = (char const*)az_span_ptr(tls_options->client_key_pem);
  int32_t key_len = az_span_size(tls_options->client_key_pem);
  char file_buf[_PEM_MAX];
  bool const from_file = az_span_size(tls_options->client_key_path) > 0;
  if (from_file)
  {
    rc = _read_file(
        transport, tls_options->client_key_path, file_buf, (int32_t)sizeof(file_buf), &key_len);
    key_text = file_buf;
  }
  BYTE* pk8 = NULL;
  DWORD pk8_len = 0;
  if (az_result_succeeded(rc))
  {
    rc = _key_to_pkcs8(transport, key_text, key_len, &pk8, &pk8_len);
  }
  if (from_file)
  {
    SecureZeroMemory(file_buf, sizeof(file_buf));
  }
  if (az_result_failed(rc))
  {
    return rc;
  }

  // Unique per connection; the process id and time keep other processes' names apart.
  static volatile LONG counter;
  char name_a[64];
  (void)snprintf(
      name_a,
      sizeof(name_a),
      "az_mqtt-%08lx-%08lx-%08lx",
      (unsigned long)GetCurrentProcessId(),
      (unsigned long)InterlockedIncrement(&counter),
      (unsigned long)GetTickCount());
  wchar_t name[64];
  size_t i = 0;
  for (; name_a[i] != '\0'; i++)
  {
    name[i] = (wchar_t)name_a[i];
  }
  name[i] = L'\0';
  rc = _import_key(transport, pk8, pk8_len, name);
  SecureZeroMemory(pk8, pk8_len);
  LocalFree(pk8);
  if (az_result_succeeded(rc))
  {
    rc = _check_key_matches(transport);
  }
  if (az_result_succeeded(rc))
  {
    static wchar_t provider_name[] = MS_KEY_STORAGE_PROVIDER;
    CRYPT_KEY_PROV_INFO prov_info;
    memset(&prov_info, 0, sizeof(prov_info));
    prov_info.pwszContainerName = name;
    prov_info.pwszProvName = provider_name;
    if (!CertSetCertificateContextProperty(
            transport->client_cert, CERT_KEY_PROV_INFO_PROP_ID, 0, &prov_info))
    {
      rc = _setup_failure(transport, GetLastError());
    }
  }
  return rc;
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
  if (transport->has_cred_initial)
  {
    FreeCredentialsHandle(&transport->cred_initial);
    transport->has_cred_initial = false;
  }
  if (transport->custom_root != NULL)
  {
    CertCloseStore(transport->custom_root, 0);
    transport->custom_root = NULL;
  }
  if (transport->client_cert != NULL)
  {
    CertFreeCertificateContext(transport->client_cert);
    transport->client_cert = NULL;
  }
  if (transport->client_store != NULL)
  {
    CertCloseStore(transport->client_store, 0);
    transport->client_store = NULL;
  }
  if (transport->client_key != 0)
  {
    (void)NCryptDeleteKey(transport->client_key, NCRYPT_SILENT_FLAG); // Frees the handle too.
    transport->client_key = 0;
  }
  transport->has_context = false;
  transport->has_cred = false;
  transport->needs_read = false;
  transport->handshake_done = false;
  transport->cert_request_answered = false;
  transport->request_flags = _REQUEST_FLAGS;
  transport->capturing = false;
  transport->capture_overflow = false;
  transport->captured = 0;
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
  // Not available with Schannel: refuse rather than connect without the key or hook asked for.
  if (tls_options->client_key_psa_id != 0 || az_span_size(tls_options->client_key_uri) > 0
      || tls_options->configure != NULL)
  {
    return AZ_MQTT_ERROR_NOT_SUPPORTED;
  }
  return AZ_OK;
}

/**
 * @brief Schannel credentials into @p out, with the client certificate if @p with_cert. Schannel's
 * own server check is off (SCH_CRED_MANUAL_CRED_VALIDATION): _validate_server_certificate(). With a
 * client certificate, TLS 1.2 only: its server Certificate message, in the clear, is validated
 * before the client certificate is sent.
 */
static SECURITY_STATUS _acquire_cred(_tls_transport* transport, bool with_cert, CredHandle* out)
{
  SCHANNEL_CRED cred;
  memset(&cred, 0, sizeof(cred));
  cred.dwVersion = SCHANNEL_CRED_VERSION;
  cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_DEFAULT_CREDS;
  if (transport->client_cert != NULL)
  {
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
  }
  if (with_cert)
  {
    cred.cCreds = 1;
    cred.paCred = &transport->client_cert;
  }
  TimeStamp expiry;
  return AcquireCredentialsHandleA(
      NULL, UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL, out, &expiry);
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
    rc = _build_custom_root(transport, tls_options, &transport->custom_root);
  }
  if (az_result_succeeded(rc)
      && (az_span_size(tls_options->client_cert_path) > 0
          || az_span_size(tls_options->client_cert_pem) > 0))
  {
    rc = _load_client_identity(transport, tls_options);
  }
  if (az_result_failed(rc))
  {
    return rc;
  }

  // Without the client certificate: it is given only to a server that passed validation.
  transport->capturing = transport->client_cert != NULL;
  SECURITY_STATUS const sec = _acquire_cred(transport, false, &transport->cred);
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
      if (transport->capturing)
      {
        int32_t const n = az_span_size(received);
        if (n > (int32_t)sizeof(transport->plain) - transport->captured)
        {
          transport->capture_overflow = true;
        }
        else
        {
          memcpy(transport->plain + transport->captured, az_span_ptr(received), (size_t)n);
          transport->captured += n;
        }
      }
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
/**
 * @brief The server certificate received so far, validated; on failure reported, and NULL.
 * CertFreeCertificateContext() it.
 */
static az_result _received_server_certificate(_tls_transport* transport, PCCERT_CONTEXT* out)
{
  *out = NULL;
  SECURITY_STATUS const sec = QueryContextAttributesA(
      &transport->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, (PVOID)out);
  if (sec != SEC_E_OK || *out == NULL)
  {
    _report(transport, AZ_MQTT_NATIVE_ERROR_TLS_VERIFY, (DWORD)sec, AZ_MQTT_ERROR_TLS_VERIFY);
    return AZ_MQTT_ERROR_TLS_VERIFY;
  }
  az_result const rc = _validate_server_certificate(transport, *out);
  if (az_result_failed(rc))
  {
    CertFreeCertificateContext(*out);
    *out = NULL;
  }
  return rc;
}

/** @brief Big-endian unsigned integer of @p n bytes at @p p. */
static int32_t _be(uint8_t const* p, int n)
{
  int32_t v = 0;
  for (int i = 0; i < n; i++)
  {
    v = (v << 8) | p[i];
  }
  return v;
}

/**
 * @brief The certificates of the server's Certificate message, from the handshake records captured
 * (TLS 1.2: in the clear), into a new *@p out_store; *@p out the first (the server's). Without one
 * (TLS 1.3: encrypted), reported, and AZ_MQTT_ERROR_TLS_VERIFY. Consumes the capture.
 */
static az_result _captured_server_certificate(
    _tls_transport* transport,
    HCERTSTORE* out_store,
    PCCERT_CONTEXT* out)
{
  uint8_t* const buf = transport->plain;
  int32_t const len = transport->capture_overflow ? 0 : transport->captured;
  // Join the fragments of the leading handshake records (type 22), in place.
  int32_t hs_len = 0;
  for (int32_t at = 0; at + 5 <= len && buf[at] == 22;)
  {
    int32_t const frag = _be(buf + at + 3, 2);
    if (frag > len - at - 5)
    {
      break;
    }
    memmove(buf + hs_len, buf + at + 5, (size_t)frag);
    hs_len += frag;
    at += 5 + frag;
  }
  transport->captured = 0;
  // Handshake messages: type (1), length (3), body. Certificate (11): list length (3), then each
  // certificate: length (3), DER.
  for (int32_t at = 0; at + 4 <= hs_len;)
  {
    int32_t const msg_len = _be(buf + at + 1, 3);
    if (msg_len > hs_len - at - 4)
    {
      break;
    }
    uint8_t const* const body = buf + at + 4;
    if (buf[at] == 11 && msg_len >= 3 && _be(body, 3) == msg_len - 3)
    {
      az_result rc = _new_store(transport, out_store);
      for (int32_t c = 3; az_result_succeeded(rc) && c < msg_len;)
      {
        int32_t const cert_len = c + 3 <= msg_len ? _be(body + c, 3) : -1;
        if (cert_len <= 0 || cert_len > msg_len - c - 3)
        {
          rc = AZ_MQTT_ERROR_TLS_VERIFY;
          break;
        }
        rc = _add_der(transport, *out_store, body + c + 3, (DWORD)cert_len, out);
        c += 3 + cert_len;
      }
      if (az_result_succeeded(rc) && *out != NULL)
      {
        return AZ_OK;
      }
      break;
    }
    at += 4 + msg_len;
  }
  _report(
      transport,
      AZ_MQTT_NATIVE_ERROR_TLS_VERIFY,
      (DWORD)SEC_E_ILLEGAL_MESSAGE,
      AZ_MQTT_ERROR_TLS_VERIFY);
  return AZ_MQTT_ERROR_TLS_VERIFY;
}

/**
 * @brief Answer the server's certificate request with the client certificate: validate the server
 * first (nothing about the client is sent to an untrusted one), then use credentials holding it.
 */
static az_result _present_client_certificate(_tls_transport* transport)
{
  HCERTSTORE store = NULL;
  PCCERT_CONTEXT server_cert = NULL;
  transport->capturing = false;
  az_result rc = _captured_server_certificate(transport, &store, &server_cert);
  if (az_result_succeeded(rc))
  {
    rc = _validate_server_certificate(transport, server_cert);
  }
  if (server_cert != NULL)
  {
    CertFreeCertificateContext(server_cert);
  }
  if (store != NULL)
  {
    CertCloseStore(store, 0);
  }
  if (az_result_failed(rc))
  {
    return rc;
  }
  CredHandle cred;
  SECURITY_STATUS const sec = _acquire_cred(transport, true, &cred);
  if (sec != SEC_E_OK)
  {
    _report(transport, AZ_MQTT_NATIVE_ERROR_TLS, (DWORD)sec, AZ_MQTT_ERROR_TLS_HANDSHAKE);
    return AZ_MQTT_ERROR_TLS_HANDSHAKE;
  }
  transport->cred_initial = transport->cred; // The context may still refer to it.
  transport->has_cred_initial = true;
  transport->cred = cred;
  return AZ_OK;
}

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
        transport->request_flags,
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
    // The server requested a client certificate: call again, with the same input, holding it (if
    // any) and with USE_SUPPLIED_CREDS. Without one, a server that requires one then refuses.
    if (status == SEC_I_INCOMPLETE_CREDENTIALS && transport->has_context
        && !transport->cert_request_answered)
    {
      transport->cert_request_answered = true;
      transport->request_flags = _REQUEST_FLAGS | ISC_REQ_USE_SUPPLIED_CREDS;
      if (transport->client_cert != NULL)
      {
        rc = _present_client_certificate(transport);
        if (az_result_failed(rc))
        {
          return rc;
        }
      }
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

  transport->capturing = false; // plain holds decrypted data from here on.
  transport->captured = 0;
  SECURITY_STATUS const sec
      = QueryContextAttributesA(&transport->context, SECPKG_ATTR_STREAM_SIZES, &transport->sizes);
  if (sec != SEC_E_OK)
  {
    _report(transport, AZ_MQTT_NATIVE_ERROR_TLS, (DWORD)sec, AZ_MQTT_ERROR_TLS_HANDSHAKE);
    return AZ_MQTT_ERROR_TLS_HANDSHAKE;
  }
  PCCERT_CONTEXT server_cert = NULL;
  az_result const rc = _received_server_certificate(transport, &server_cert);
  if (server_cert != NULL)
  {
    CertFreeCertificateContext(server_cert);
  }
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
        transport->request_flags,
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
