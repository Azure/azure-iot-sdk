// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_types.h>

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_span.h>

#include <stdio.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <schannel.h>
#include <security.h>
#include <wincrypt.h>

#pragma comment(lib, "ws2_32.lib")

#define AZ_MQTT_SCHANNEL_IO_BUFFER_SIZE 65536

struct az_mqtt_transport
{
  SOCKET socket_fd;
  bool connected;

#ifdef AZ_MQTT_TLS_SCHANNEL
  bool tls_active;
  bool has_cred;
  bool has_context;

  CredHandle cred_handle;
  CtxtHandle context_handle;
  SecPkgContext_StreamSizes stream_sizes;
  PCCERT_CONTEXT remote_server_cert;

  uint8_t encrypted_in[AZ_MQTT_SCHANNEL_IO_BUFFER_SIZE];
  int32_t encrypted_in_len;

  uint8_t decrypted_pending[AZ_MQTT_SCHANNEL_IO_BUFFER_SIZE];
  int32_t decrypted_pending_len;
  int32_t decrypted_pending_pos;

  uint8_t tls_send_buf[AZ_MQTT_SCHANNEL_IO_BUFFER_SIZE];
#endif
};

AZ_NODISCARD int32_t az_mqtt_transport_sizeof(void) { return (int32_t)sizeof(az_mqtt_transport); }

static bool _wsa_inited = false;

static az_result _ensure_wsa(void)
{
  if (!_wsa_inited)
  {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
      return AZ_MQTT_ERROR_TRANSPORT;
    }
    _wsa_inited = true;
  }
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt_transport_init(az_mqtt_transport* transport)
{
  _az_PRECONDITION_NOT_NULL(transport);

  az_result rc = _ensure_wsa();
  if (az_result_failed(rc))
  {
    return rc;
  }

  memset(transport, 0, sizeof(*transport));
  transport->socket_fd = INVALID_SOCKET;
  return AZ_OK;
}

static az_result _span_to_cstr(az_span src, char* buf, int32_t buf_size)
{
  int32_t len = az_span_size(src);
  if (len >= buf_size)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  memcpy(buf, az_span_ptr(src), (size_t)len);
  buf[len] = '\0';
  return AZ_OK;
}

static az_result _tcp_connect(az_mqtt_transport* transport, az_span host, uint16_t port)
{
  char host_str[256];
  az_result rc = _span_to_cstr(host, host_str, (int32_t)sizeof(host_str));
  if (az_result_failed(rc))
  {
    return rc;
  }

  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = NULL;
  if (getaddrinfo(host_str, port_str, &hints, &res) != 0 || res == NULL)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  SOCKET fd = INVALID_SOCKET;
  for (struct addrinfo* rp = res; rp != NULL; rp = rp->ai_next)
  {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == INVALID_SOCKET)
    {
      continue;
    }
    if (connect(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0)
    {
      break;
    }
    closesocket(fd);
    fd = INVALID_SOCKET;
  }
  freeaddrinfo(res);

  if (fd == INVALID_SOCKET)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  transport->socket_fd = fd;
  return AZ_OK;
}

static az_result _socket_wait_readable(SOCKET fd, int32_t timeout_ms, bool* out_ready)
{
  fd_set read_fds;
  FD_ZERO(&read_fds);
  FD_SET(fd, &read_fds);

  struct timeval tv;
  struct timeval* tv_ptr = NULL;

  if (timeout_ms >= 0)
  {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    tv_ptr = &tv;
  }

  int sel = select(0 /* ignored on Windows */, &read_fds, NULL, NULL, tv_ptr);
  if (sel < 0)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  *out_ready = (sel > 0);
  return AZ_OK;
}

static az_result _socket_recv_timeout(SOCKET fd, uint8_t* buf, int32_t capacity, int32_t timeout_ms, int32_t* out_n)
{
  bool ready = false;
  az_result rc = _socket_wait_readable(fd, timeout_ms, &ready);
  if (az_result_failed(rc))
  {
    return rc;
  }
  if (!ready)
  {
    *out_n = 0;
    return AZ_OK;
  }

  int n = recv(fd, (char*)buf, capacity, 0);
  if (n < 0)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  *out_n = n;
  return AZ_OK;
}

static az_result _socket_send_all(SOCKET fd, uint8_t const* data, int32_t len)
{
  int32_t sent_total = 0;
  while (sent_total < len)
  {
    int n = send(fd, (char const*)data + sent_total, len - sent_total, 0);
    if (n <= 0)
    {
      return AZ_MQTT_ERROR_TRANSPORT;
    }
    sent_total += n;
  }
  return AZ_OK;
}

#ifdef AZ_MQTT_TLS_SCHANNEL
static bool _handle_is_valid(SecHandle const* handle)
{
  return (handle->dwLower != 0 || handle->dwUpper != 0);
}

static void _reset_security_handle(SecHandle* handle)
{
  handle->dwLower = 0;
  handle->dwUpper = 0;
}

static az_result _host_to_wstr(az_span host, wchar_t* out_host, int32_t out_host_len)
{
  char host_ascii[256];
  az_result rc = _span_to_cstr(host, host_ascii, (int32_t)sizeof(host_ascii));
  if (az_result_failed(rc))
  {
    return rc;
  }

  int wlen = MultiByteToWideChar(CP_UTF8, 0, host_ascii, -1, out_host, out_host_len);
  if (wlen <= 0)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  return AZ_OK;
}

static az_result _decode_cert_file_to_der(
    az_mqtt_tls_options const* tls_options,
    uint8_t* der_out,
    DWORD der_capacity,
    DWORD* out_der_len)
{
  char cert_path[260];
  az_result rc = _span_to_cstr(tls_options->ca_cert_path, cert_path, (int32_t)sizeof(cert_path));
  if (az_result_failed(rc))
  {
    return rc;
  }

  HANDLE h = CreateFileA(cert_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  LARGE_INTEGER file_size;
  if (!GetFileSizeEx(h, &file_size) || file_size.QuadPart <= 0 || file_size.QuadPart > 64 * 1024)
  {
    CloseHandle(h);
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  uint32_t file_len = (uint32_t)file_size.QuadPart;
  char file_buf[(64 * 1024) + 1];
  DWORD read_bytes = 0;
  BOOL ok = ReadFile(h, file_buf, file_len, &read_bytes, NULL);
  CloseHandle(h);
  if (!ok || read_bytes != file_len)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  file_buf[file_len] = '\0';

  if (strstr(file_buf, "-----BEGIN CERTIFICATE-----") != NULL)
  {
    DWORD decoded_len = 0;
    if (!CryptStringToBinaryA(file_buf, file_len, CRYPT_STRING_BASE64HEADER, NULL, &decoded_len, NULL, NULL))
    {
      return AZ_MQTT_ERROR_TRANSPORT;
    }

    if (decoded_len > der_capacity)
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    if (!CryptStringToBinaryA(
            file_buf,
            file_len,
            CRYPT_STRING_BASE64HEADER,
            der_out,
            &decoded_len,
            NULL,
            NULL))
    {
      return AZ_MQTT_ERROR_TRANSPORT;
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

static az_result _build_custom_root_store(
    az_mqtt_tls_options const* tls_options,
    HCERTSTORE* out_store)
{
  *out_store = NULL;

  if (az_span_size(tls_options->ca_cert_path) == 0)
  {
    return AZ_OK;
  }

  uint8_t der_buf[64 * 1024];
  DWORD der_len = 0;
  az_result rc = _decode_cert_file_to_der(tls_options, der_buf, (DWORD)sizeof(der_buf), &der_len);
  if (az_result_failed(rc))
  {
    return rc;
  }

  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, X509_ASN_ENCODING, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
  if (store == NULL)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  if (!CertAddEncodedCertificateToStore(
          store,
          X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
          der_buf,
          der_len,
          CERT_STORE_ADD_REPLACE_EXISTING,
          NULL))
  {
    CertCloseStore(store, 0);
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  *out_store = store;
  return AZ_OK;
}

static az_result _validate_server_certificate(
    az_span host,
    az_mqtt_tls_options const* tls_options,
    PCCERT_CONTEXT server_cert)
{
  wchar_t host_w[256];
  az_result rc = _host_to_wstr(host, host_w, (int32_t)sizeof(host_w) / (int32_t)sizeof(host_w[0]));
  if (az_result_failed(rc))
  {
    return rc;
  }

  HCERTSTORE custom_root = NULL;
  rc = _build_custom_root_store(tls_options, &custom_root);
  if (az_result_failed(rc))
  {
    return rc;
  }

  HCERTCHAINENGINE chain_engine = NULL;
  if (custom_root != NULL)
  {
    CERT_CHAIN_ENGINE_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cbSize = sizeof(cfg);
    cfg.hExclusiveRoot = custom_root;

    if (!CertCreateCertificateChainEngine(&cfg, &chain_engine))
    {
      CertCloseStore(custom_root, 0);
      return AZ_MQTT_ERROR_TRANSPORT;
    }
  }

  CERT_CHAIN_PARA chain_para;
  memset(&chain_para, 0, sizeof(chain_para));
  chain_para.cbSize = sizeof(chain_para);

  PCCERT_CHAIN_CONTEXT chain_ctx = NULL;
  BOOL chain_ok = CertGetCertificateChain(
      chain_engine,
      server_cert,
      NULL,
      server_cert->hCertStore,
      &chain_para,
      0,
      NULL,
      &chain_ctx);

  if (!chain_ok || chain_ctx == NULL)
  {
    if (chain_engine != NULL)
    {
      CertFreeCertificateChainEngine(chain_engine);
    }
    if (custom_root != NULL)
    {
      CertCloseStore(custom_root, 0);
    }
    return AZ_MQTT_ERROR_TRANSPORT;
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

  BOOL policy_ok = CertVerifyCertificateChainPolicy(
      CERT_CHAIN_POLICY_SSL, chain_ctx, &policy_para, &policy_status);

  CertFreeCertificateChain(chain_ctx);
  if (chain_engine != NULL)
  {
    CertFreeCertificateChainEngine(chain_engine);
  }
  if (custom_root != NULL)
  {
    CertCloseStore(custom_root, 0);
  }

  if (!policy_ok || policy_status.dwError != 0)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  return AZ_OK;
}

static az_result _schannel_setup(
    az_mqtt_transport* transport,
    az_span host,
    az_mqtt_tls_options const* tls_options)
{
  _reset_security_handle(&transport->cred_handle);
  _reset_security_handle(&transport->context_handle);

  SCHANNEL_CRED cred;
  memset(&cred, 0, sizeof(cred));
  cred.dwVersion = SCHANNEL_CRED_VERSION;
  cred.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO;

  TimeStamp expiry;
  SECURITY_STATUS sec = AcquireCredentialsHandleA(
      NULL,
      UNISP_NAME_A,
      SECPKG_CRED_OUTBOUND,
      NULL,
      &cred,
      NULL,
      NULL,
      &transport->cred_handle,
      &expiry);
  if (sec != SEC_E_OK)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  transport->has_cred = true;

  char host_str[256];
  az_result rc = _span_to_cstr(host, host_str, (int32_t)sizeof(host_str));
  if (az_result_failed(rc))
  {
    return rc;
  }

  DWORD req_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
      | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
  DWORD out_flags = 0;

  transport->encrypted_in_len = 0;

  for (;;)
  {
    SecBuffer out_buf;
    SecBufferDesc out_desc;
    memset(&out_buf, 0, sizeof(out_buf));
    memset(&out_desc, 0, sizeof(out_desc));
    out_buf.BufferType = SECBUFFER_TOKEN;
    out_desc.ulVersion = SECBUFFER_VERSION;
    out_desc.cBuffers = 1;
    out_desc.pBuffers = &out_buf;

    SECURITY_STATUS status;
    CtxtHandle new_context;
    _reset_security_handle(&new_context);

    if (!transport->has_context)
    {
      status = InitializeSecurityContextA(
          &transport->cred_handle,
          NULL,
          host_str,
          req_flags,
          0,
          SECURITY_NATIVE_DREP,
          NULL,
          0,
          &new_context,
          &out_desc,
          &out_flags,
          &expiry);
    }
    else
    {
      SecBuffer in_bufs[2];
      SecBufferDesc in_desc;
      memset(&in_bufs, 0, sizeof(in_bufs));
      memset(&in_desc, 0, sizeof(in_desc));
      in_bufs[0].BufferType = SECBUFFER_TOKEN;
      in_bufs[0].pvBuffer = transport->encrypted_in;
      in_bufs[0].cbBuffer = (unsigned long)transport->encrypted_in_len;
      in_bufs[1].BufferType = SECBUFFER_EMPTY;
      in_desc.ulVersion = SECBUFFER_VERSION;
      in_desc.cBuffers = 2;
      in_desc.pBuffers = in_bufs;

      status = InitializeSecurityContextA(
          &transport->cred_handle,
          &transport->context_handle,
          host_str,
          req_flags,
          0,
          SECURITY_NATIVE_DREP,
          &in_desc,
          0,
          &new_context,
          &out_desc,
          &out_flags,
          &expiry);

      if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED)
      {
        if (in_bufs[1].BufferType == SECBUFFER_EXTRA)
        {
          memmove(
              transport->encrypted_in,
              transport->encrypted_in + (transport->encrypted_in_len - (int32_t)in_bufs[1].cbBuffer),
              in_bufs[1].cbBuffer);
          transport->encrypted_in_len = (int32_t)in_bufs[1].cbBuffer;
        }
        else
        {
          transport->encrypted_in_len = 0;
        }
      }

      if (status == SEC_E_INCOMPLETE_MESSAGE)
      {
        if (transport->encrypted_in_len == (int32_t)sizeof(transport->encrypted_in))
        {
          return AZ_MQTT_ERROR_TRANSPORT;
        }

        int32_t n = 0;
        rc = _socket_recv_timeout(
            transport->socket_fd,
            transport->encrypted_in + transport->encrypted_in_len,
            (int32_t)sizeof(transport->encrypted_in) - transport->encrypted_in_len,
            -1,
            &n);
        if (az_result_failed(rc) || n <= 0)
        {
          return AZ_MQTT_ERROR_TRANSPORT;
        }
        transport->encrypted_in_len += n;
        continue;
      }
    }

    if ((status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) && _handle_is_valid(&new_context))
    {
      transport->context_handle = new_context;
      transport->has_context = true;
    }

    if (out_buf.cbBuffer > 0 && out_buf.pvBuffer != NULL)
    {
      rc = _socket_send_all(transport->socket_fd, (uint8_t const*)out_buf.pvBuffer, (int32_t)out_buf.cbBuffer);
      FreeContextBuffer(out_buf.pvBuffer);
      if (az_result_failed(rc))
      {
        return rc;
      }
    }

    if (status == SEC_E_OK)
    {
      break;
    }

    if (status != SEC_I_CONTINUE_NEEDED)
    {
      return AZ_MQTT_ERROR_TRANSPORT;
    }

    if (transport->encrypted_in_len == 0)
    {
      int32_t n = 0;
      rc = _socket_recv_timeout(
          transport->socket_fd,
          transport->encrypted_in,
          (int32_t)sizeof(transport->encrypted_in),
          -1,
          &n);
      if (az_result_failed(rc) || n <= 0)
      {
        return AZ_MQTT_ERROR_TRANSPORT;
      }
      transport->encrypted_in_len = n;
    }
  }

  sec = QueryContextAttributesA(
      &transport->context_handle,
      SECPKG_ATTR_STREAM_SIZES,
      &transport->stream_sizes);
  if (sec != SEC_E_OK)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }

  // SCH_CRED_MANUAL_CRED_VALIDATION turns Schannel's own check off, so the
  // chain and host name must be validated here before any MQTT byte is sent.
  PCCERT_CONTEXT server_cert = NULL;
  sec = QueryContextAttributesA(
      &transport->context_handle, SECPKG_ATTR_REMOTE_CERT_CONTEXT, (PVOID)&server_cert);
  if (sec != SEC_E_OK || server_cert == NULL)
  {
    return AZ_MQTT_ERROR_TRANSPORT;
  }
  rc = _validate_server_certificate(host, tls_options, server_cert);
  CertFreeCertificateContext(server_cert);
  if (az_result_failed(rc))
  {
    return rc;
  }

  transport->tls_active = true;
  transport->decrypted_pending_len = 0;
  transport->decrypted_pending_pos = 0;

  return AZ_OK;
}

static az_result _schannel_send(az_mqtt_transport* transport, az_span data)
{
  uint8_t* ptr = az_span_ptr(data);
  int32_t remaining = az_span_size(data);

  while (remaining > 0)
  {
    int32_t max_payload = (int32_t)transport->stream_sizes.cbMaximumMessage;
    int32_t chunk = remaining < max_payload ? remaining : max_payload;

    int32_t header = (int32_t)transport->stream_sizes.cbHeader;
    int32_t trailer = (int32_t)transport->stream_sizes.cbTrailer;
    int32_t total = header + chunk + trailer;

    if (total > (int32_t)sizeof(transport->tls_send_buf))
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    memcpy(transport->tls_send_buf + header, ptr, (size_t)chunk);

    SecBuffer bufs[4];
    SecBufferDesc desc;
    memset(&bufs, 0, sizeof(bufs));
    memset(&desc, 0, sizeof(desc));

    bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
    bufs[0].pvBuffer = transport->tls_send_buf;
    bufs[0].cbBuffer = (unsigned long)header;

    bufs[1].BufferType = SECBUFFER_DATA;
    bufs[1].pvBuffer = transport->tls_send_buf + header;
    bufs[1].cbBuffer = (unsigned long)chunk;

    bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
    bufs[2].pvBuffer = transport->tls_send_buf + header + chunk;
    bufs[2].cbBuffer = (unsigned long)trailer;

    bufs[3].BufferType = SECBUFFER_EMPTY;

    desc.ulVersion = SECBUFFER_VERSION;
    desc.cBuffers = 4;
    desc.pBuffers = bufs;

    SECURITY_STATUS sec = EncryptMessage(&transport->context_handle, 0, &desc, 0);
    if (sec != SEC_E_OK)
    {
      return AZ_MQTT_ERROR_TRANSPORT;
    }

    int32_t to_send = (int32_t)(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
    az_result rc = _socket_send_all(transport->socket_fd, transport->tls_send_buf, to_send);
    if (az_result_failed(rc))
    {
      return rc;
    }

    ptr += chunk;
    remaining -= chunk;
  }

  return AZ_OK;
}

static az_result _schannel_consume_decrypted(az_mqtt_transport* transport, az_span buffer, az_span* out_received)
{
  int32_t available = transport->decrypted_pending_len - transport->decrypted_pending_pos;
  if (available <= 0)
  {
    *out_received = AZ_SPAN_EMPTY;
    return AZ_OK;
  }

  int32_t to_copy = available;
  if (to_copy > az_span_size(buffer))
  {
    to_copy = az_span_size(buffer);
  }

  memcpy(
      az_span_ptr(buffer),
      transport->decrypted_pending + transport->decrypted_pending_pos,
      (size_t)to_copy);
  transport->decrypted_pending_pos += to_copy;

  if (transport->decrypted_pending_pos >= transport->decrypted_pending_len)
  {
    transport->decrypted_pending_len = 0;
    transport->decrypted_pending_pos = 0;
  }

  *out_received = az_span_slice(buffer, 0, to_copy);
  return AZ_OK;
}

static az_result _schannel_receive(
    az_mqtt_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received)
{
  az_result rc = _schannel_consume_decrypted(transport, buffer, out_received);
  if (az_result_failed(rc) || az_span_size(*out_received) > 0)
  {
    return rc;
  }

  for (;;)
  {
    if (transport->encrypted_in_len > 0)
    {
      SecBuffer bufs[4];
      SecBufferDesc desc;
      memset(&bufs, 0, sizeof(bufs));
      memset(&desc, 0, sizeof(desc));

      bufs[0].BufferType = SECBUFFER_DATA;
      bufs[0].pvBuffer = transport->encrypted_in;
      bufs[0].cbBuffer = (unsigned long)transport->encrypted_in_len;
      bufs[1].BufferType = SECBUFFER_EMPTY;
      bufs[2].BufferType = SECBUFFER_EMPTY;
      bufs[3].BufferType = SECBUFFER_EMPTY;

      desc.ulVersion = SECBUFFER_VERSION;
      desc.cBuffers = 4;
      desc.pBuffers = bufs;

      SECURITY_STATUS sec = DecryptMessage(&transport->context_handle, &desc, 0, NULL);
      if (sec == SEC_E_OK)
      {
        SecBuffer* data_buf = NULL;
        SecBuffer* extra_buf = NULL;

        for (int i = 0; i < 4; ++i)
        {
          if (bufs[i].BufferType == SECBUFFER_DATA)
          {
            data_buf = &bufs[i];
          }
          else if (bufs[i].BufferType == SECBUFFER_EXTRA)
          {
            extra_buf = &bufs[i];
          }
        }

        if (data_buf != NULL && data_buf->cbBuffer > 0)
        {
          if ((int32_t)data_buf->cbBuffer > (int32_t)sizeof(transport->decrypted_pending))
          {
            return AZ_ERROR_NOT_ENOUGH_SPACE;
          }

          memcpy(transport->decrypted_pending, data_buf->pvBuffer, data_buf->cbBuffer);
          transport->decrypted_pending_len = (int32_t)data_buf->cbBuffer;
          transport->decrypted_pending_pos = 0;
        }

        if (extra_buf != NULL && extra_buf->cbBuffer > 0)
        {
          memmove(
              transport->encrypted_in,
              transport->encrypted_in + (transport->encrypted_in_len - (int32_t)extra_buf->cbBuffer),
              extra_buf->cbBuffer);
          transport->encrypted_in_len = (int32_t)extra_buf->cbBuffer;
        }
        else
        {
          transport->encrypted_in_len = 0;
        }

        if (transport->decrypted_pending_len > 0)
        {
          return _schannel_consume_decrypted(transport, buffer, out_received);
        }

        continue;
      }
      else if (sec == SEC_E_INCOMPLETE_MESSAGE)
      {
        // Need more encrypted bytes from socket.
      }
      else if (sec == SEC_I_CONTEXT_EXPIRED)
      {
        transport->connected = false;
        return AZ_MQTT_ERROR_TRANSPORT;
      }
      else
      {
        return AZ_MQTT_ERROR_TRANSPORT;
      }
    }

    if (transport->encrypted_in_len >= (int32_t)sizeof(transport->encrypted_in))
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    int32_t n = 0;
    rc = _socket_recv_timeout(
        transport->socket_fd,
        transport->encrypted_in + transport->encrypted_in_len,
        (int32_t)sizeof(transport->encrypted_in) - transport->encrypted_in_len,
        timeout_ms,
        &n);
    if (az_result_failed(rc))
    {
      return rc;
    }

    if (n == 0)
    {
      *out_received = AZ_SPAN_EMPTY;
      return AZ_OK;
    }

    transport->encrypted_in_len += n;
  }
}

static void _schannel_shutdown(az_mqtt_transport* transport)
{
  if (transport->has_context && _handle_is_valid(&transport->context_handle))
  {
    DeleteSecurityContext(&transport->context_handle);
    _reset_security_handle(&transport->context_handle);
  }

  if (transport->has_cred && _handle_is_valid(&transport->cred_handle))
  {
    FreeCredentialsHandle(&transport->cred_handle);
    _reset_security_handle(&transport->cred_handle);
  }

  if (transport->remote_server_cert != NULL)
  {
    CertFreeCertificateContext(transport->remote_server_cert);
    transport->remote_server_cert = NULL;
  }

  transport->tls_active = false;
  transport->has_context = false;
  transport->has_cred = false;
  transport->encrypted_in_len = 0;
  transport->decrypted_pending_len = 0;
  transport->decrypted_pending_pos = 0;
}
#endif // AZ_MQTT_TLS_SCHANNEL

AZ_NODISCARD az_result az_mqtt_transport_connect(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  _az_PRECONDITION_NOT_NULL(transport);

#ifdef AZ_MQTT_TLS_SCHANNEL
  if (tls_options != NULL)
  {
    az_result const check = az_mqtt_tls_options_check(tls_options);
    if (az_result_failed(check))
    {
      return check; // Same contract as the other backends.
    }
    // Not implemented for Schannel yet: refuse rather than connect without
    // the identity, trust or hook the caller asked for.
    if (az_span_size(tls_options->client_cert_path) > 0
        || az_span_size(tls_options->client_cert_pem) > 0
        || az_span_size(tls_options->ca_cert_pem) > 0 || tls_options->configure != NULL)
    {
      return AZ_MQTT_ERROR_NOT_SUPPORTED;
    }
  }
#else
  if (tls_options != NULL)
  {
    // Built without TLS: never fall back to plaintext.
    return AZ_MQTT_ERROR_NOT_SUPPORTED;
  }
#endif

  az_result rc = _tcp_connect(transport, host, port);
  if (az_result_failed(rc))
  {
    return rc;
  }

#ifdef AZ_MQTT_TLS_SCHANNEL
  if (tls_options != NULL)
  {
    rc = _schannel_setup(transport, host, tls_options);
    if (az_result_failed(rc))
    {
      _schannel_shutdown(transport);
      closesocket(transport->socket_fd);
      transport->socket_fd = INVALID_SOCKET;
      return rc;
    }
  }
#else
  (void)host;
#endif

  transport->connected = true;
  return AZ_OK;
}

AZ_NODISCARD int64_t az_mqtt_transport_clock_ms(void) { return (int64_t)GetTickCount64(); }

AZ_NODISCARD az_result az_mqtt_transport_connect_start(
    az_mqtt_transport* transport,
    az_span host,
    uint16_t port,
    az_mqtt_tls_options const* tls_options)
{
  // Schannel connects synchronously; az_mqtt_transport_connect_poll() only reports the result.
  return az_mqtt_transport_connect(transport, host, port, tls_options);
}

AZ_NODISCARD az_result
az_mqtt_transport_connect_poll(az_mqtt_transport* transport, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(transport);
  (void)timeout_ms;
  return transport->connected ? AZ_OK : AZ_MQTT_ERROR_TRANSPORT;
}

AZ_NODISCARD az_result az_mqtt_transport_send(az_mqtt_transport* transport, az_span data)
{
  _az_PRECONDITION_NOT_NULL(transport);

#ifdef AZ_MQTT_TLS_SCHANNEL
  if (transport->tls_active)
  {
    return _schannel_send(transport, data);
  }
#endif

  return _socket_send_all(transport->socket_fd, az_span_ptr(data), az_span_size(data));
}

AZ_NODISCARD az_result az_mqtt_transport_receive(
    az_mqtt_transport* transport,
    az_span buffer,
    int32_t timeout_ms,
    az_span* out_received)
{
  _az_PRECONDITION_NOT_NULL(transport);
  _az_PRECONDITION_NOT_NULL(out_received);

  *out_received = AZ_SPAN_EMPTY;

#ifdef AZ_MQTT_TLS_SCHANNEL
  if (transport->tls_active)
  {
    return _schannel_receive(transport, buffer, timeout_ms, out_received);
  }
#endif

  int32_t n = 0;
  az_result rc = _socket_recv_timeout(
      transport->socket_fd,
      az_span_ptr(buffer),
      az_span_size(buffer),
      timeout_ms,
      &n);
  if (az_result_failed(rc))
  {
    return rc;
  }

  if (n == 0)
  {
    return AZ_OK;
  }

  *out_received = az_span_slice(buffer, 0, n);
  return AZ_OK;
}

void az_mqtt_transport_close(az_mqtt_transport* transport)
{
  if (transport == NULL)
  {
    return;
  }

#ifdef AZ_MQTT_TLS_SCHANNEL
  _schannel_shutdown(transport);
#endif

  if (transport->socket_fd != INVALID_SOCKET)
  {
    closesocket(transport->socket_fd);
    transport->socket_fd = INVALID_SOCKET;
  }

  transport->connected = false;
}
