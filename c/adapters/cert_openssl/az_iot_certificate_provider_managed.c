// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL 3.0+ implementation of the "managed" certificate provider (D5).
 *
 * Uses only public OpenSSL 3.0 APIs: EVP_PKEY_Q_keygen for key generation,
 * the X509_REQ_* family for PKCS#10, and BIO for reads. Writes are built in a
 * memory BIO and written with OS calls (write_file_private), so no CRT FILE*
 * crosses the OpenSSL boundary and the file stays clean under MSVC /W4 /WX. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "az_iot_certificate_provider_managed.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* Lean: keeps wincrypt.h out, whose X509_* macros clash with OpenSSL. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

/* Operational key parameters. */
#define MANAGED_RSA_KEY_BITS 2048
#define MANAGED_EC_CURVE_NAME "P-256"

/* An optional C string a caller may leave unset either way: NULL and "" both
 * mean "not supplied". Stated positively so call sites read as "this was
 * supplied" rather than a negated absence; the parameter is parenthesised so an
 * expression argument cannot misparse.
 *
 * Duplicated from c/src/core/internal/span_writer.h on purpose: adapters may
 * include only public azure/iot headers, so they cannot reach that one. The
 * Paho adapter carries the same copy for the same reason. */
#define is_nonempty_cstr(s) ((s) != NULL && (s)[0] != '\0')

/* What a PEM certificate starts with, used both to frame one and to tell an
 * already-PEM payload from base64 DER after decoding. */
#define PEM_CERT_PREFIX "-----BEGIN CERTIFICATE-----"

/* PEM framing written around a certificate the service issues as base64 DER. */
#define PEM_CERT_BEGIN PEM_CERT_PREFIX "\n"
#define PEM_CERT_END "\n-----END CERTIFICATE-----\n"

/* Longest base64 line a PEM body may contain (RFC 7468 recommends 64). */
#define PEM_LINE_LEN 64

/* Encoded length (excluding NUL) of base64 over `binary_len` bytes. */
#define BASE64_ENCODED_LEN(binary_len) ((((binary_len) + 2) / 3) * 4)

#ifdef _WIN32
/* Owner-only DACL for written files: full access for the owner and SYSTEM,
 * protected from inheriting the directory's ACEs. */
#define MANAGED_PRIVATE_SDDL "D:P(A;;FA;;;OW)(A;;FA;;;SY)"
/* Attempts at a unique temporary name before giving up. */
#define MANAGED_TMP_ATTEMPTS 16
#else
/* mkstemp() template appended to the destination path. */
#define MANAGED_TMP_TEMPLATE ".XXXXXX"
#endif

/* --------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static char* dup_str(const char* s)
{
  if (!s)
  {
    return NULL;
  }
  size_t n = strlen(s);
  char* out = malloc(n + 1);
  if (!out)
  {
    return NULL;
  }
  memcpy(out, s, n + 1);
  return out;
}

static EVP_PKEY* generate_key(int key_type)
{
  if (key_type == AZ_IOT_MANAGED_KEY_RSA_2048)
  {
    return EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)MANAGED_RSA_KEY_BITS);
  }
  return EVP_PKEY_Q_keygen(NULL, NULL, "EC", MANAGED_EC_CURVE_NAME);
}

static EVP_PKEY* load_key_file(const char* path)
{
  BIO* b = BIO_new_file(path, "rb");
  if (!b)
  {
    ERR_clear_error();
    return NULL;
  }
  EVP_PKEY* k = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
  BIO_free(b);
  return k;
}

#ifdef _WIN32
/**
 * @brief Create a new file with an owner-only DACL, failing if it exists.
 * @return The open handle, or INVALID_HANDLE_VALUE (errors other than a name
 * collision leave *collided false).
 */
static HANDLE create_private_file(const char* path, bool* collided)
{
  *collided = false;
  PSECURITY_DESCRIPTOR sd = NULL;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
          MANAGED_PRIVATE_SDDL, SDDL_REVISION_1, &sd, NULL))
  {
    return INVALID_HANDLE_VALUE;
  }
  SECURITY_ATTRIBUTES sa = { (DWORD)sizeof(sa), sd, FALSE };
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
  {
    DWORD err = GetLastError();
    *collided = (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS);
  }
  LocalFree(sd);
  return h;
}
#endif

/**
 * @brief Write @p data to a new file next to @p path that only the current
 * user can access (0600 / owner-only DACL), created exclusively under a unique
 * name so an existing file or link there is never opened, and flush it.
 *
 * @param[out] out_tmp The staged file's path (heap; pass to commit_file() or
 * discard_file()). NULL on failure, which leaves nothing behind.
 */
static az_iot_result stage_file(const char* path, const char* data, size_t len, char** out_tmp)
{
  *out_tmp = NULL;
  size_t path_len = strlen(path);
#ifdef _WIN32
  /* "<path>.<8 hex>.tmp" */
  char* tmp = (char*)malloc(path_len + 14);
  if (!tmp)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  HANDLE h = INVALID_HANDLE_VALUE;
  unsigned long seed = (unsigned long)GetTickCount() ^ ((unsigned long)GetCurrentProcessId() << 16)
      ^ (unsigned long)(uintptr_t)tmp;
  for (int i = 0; i < MANAGED_TMP_ATTEMPTS && h == INVALID_HANDLE_VALUE; ++i)
  {
    seed = seed * 1103515245UL + 12345UL;
    (void)snprintf(tmp, path_len + 14, "%s.%08lx.tmp", path, seed & 0xFFFFFFFFUL);
    bool collided = false;
    h = create_private_file(tmp, &collided);
    if (h == INVALID_HANDLE_VALUE && !collided)
    {
      break;
    }
  }
  if (h == INVALID_HANDLE_VALUE)
  {
    free(tmp);
    return AZ_IOT_ERR_INTERNAL;
  }
  bool ok = true;
  size_t off = 0;
  while (ok && off < len)
  {
    DWORD chunk = (len - off) > 0x40000000u ? 0x40000000u : (DWORD)(len - off);
    DWORD written = 0;
    ok = WriteFile(h, data + off, chunk, &written, NULL) && written > 0;
    off += written;
  }
  ok = ok && FlushFileBuffers(h);
  ok = CloseHandle(h) && ok;
  if (!ok)
  {
    (void)DeleteFileA(tmp);
  }
#else
  char* tmp = (char*)malloc(path_len + sizeof(MANAGED_TMP_TEMPLATE));
  if (!tmp)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  memcpy(tmp, path, path_len);
  memcpy(tmp + path_len, MANAGED_TMP_TEMPLATE, sizeof(MANAGED_TMP_TEMPLATE));
  int fd = mkstemp(tmp); /* O_CREAT|O_EXCL; its 0600 is reduced by the umask */
  if (fd < 0)
  {
    free(tmp);
    return AZ_IOT_ERR_INTERNAL;
  }
  if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) /* exactly 0600, whatever the umask */
  {
    (void)close(fd);
    (void)unlink(tmp);
    free(tmp);
    return AZ_IOT_ERR_INTERNAL;
  }
  bool ok = true;
  size_t off = 0;
  while (ok && off < len)
  {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0 && errno == EINTR)
    {
      continue;
    }
    ok = n > 0;
    off += ok ? (size_t)n : 0;
  }
  ok = ok && fsync(fd) == 0;
  ok = (close(fd) == 0) && ok;
  if (!ok)
  {
    (void)unlink(tmp);
  }
#endif
  if (!ok)
  {
    free(tmp);
    return AZ_IOT_ERR_INTERNAL;
  }
  *out_tmp = tmp;
  return AZ_IOT_OK;
}

/** @brief Remove a staged file and free its path. NULL is a no-op. */
static void discard_file(char* tmp)
{
  if (tmp)
  {
    (void)remove(tmp);
    free(tmp);
  }
}

/**
 * @brief Rename staged file @p tmp over @p path in one step, replacing a link
 * at @p path rather than following it. Frees @p tmp; on failure removes it and
 * leaves @p path untouched.
 */
static az_iot_result commit_file(char* tmp, const char* path)
{
#ifdef _WIN32
  bool ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  bool ok = rename(tmp, path) == 0;
#endif
  if (!ok)
  {
    discard_file(tmp);
    return AZ_IOT_ERR_INTERNAL;
  }
  free(tmp);
  return AZ_IOT_OK;
}

/**
 * @brief Stage the contents of memory BIO @p mem next to @p path (stage_file),
 * then wipe the BIO's buffer.
 */
static az_iot_result stage_bio(const char* path, BIO* mem, char** out_tmp)
{
  *out_tmp = NULL;
  char* data = NULL;
  long len = BIO_get_mem_data(mem, &data);
  if (len <= 0 || !data)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  az_iot_result rc = stage_file(path, data, (size_t)len, out_tmp);
  OPENSSL_cleanse(data, (size_t)len);
  return rc;
}

/** @brief Replace @p path with the contents of @p mem, all-or-nothing. */
static az_iot_result write_bio_private(const char* path, BIO* mem)
{
  char* tmp = NULL;
  az_iot_result rc = stage_bio(path, mem, &tmp);
  return (rc == AZ_IOT_OK) ? commit_file(tmp, path) : rc;
}

/** @brief PEM-encode private @p key into a new memory BIO (NULL on failure). */
static BIO* key_to_bio(EVP_PKEY* key)
{
  BIO* mem = BIO_new(BIO_s_mem());
  if (mem && PEM_write_bio_PrivateKey(mem, key, NULL, NULL, 0, NULL, NULL) != 1)
  {
    BIO_free(mem);
    mem = NULL;
  }
  return mem;
}

static az_iot_result write_key_file(const char* path, EVP_PKEY* key)
{
  BIO* mem = key_to_bio(key);
  if (!mem)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  az_iot_result rc = write_bio_private(path, mem);
  BIO_free(mem);
  return rc;
}

/**
 * @brief Count the PEM certificate blocks in @p data, requiring that nothing
 * but whitespace lies outside them.
 *
 * PEM_read_bio_X509() skips any text before a BEGIN line, so on its own it
 * would accept a chain followed by unrelated bytes.
 *
 * @return The number of blocks, or 0 if there is other text or an unclosed
 * block.
 */
static size_t count_pem_certificates(const char* data, size_t len)
{
  static const char begin[] = "-----BEGIN CERTIFICATE-----";
  static const char end[] = "-----END CERTIFICATE-----";
  size_t blocks = 0;
  bool inside = false;
  size_t pos = 0;
  while (pos < len)
  {
    size_t eol = pos;
    while (eol < len && data[eol] != '\n')
    {
      eol++;
    }
    size_t line_end = eol;
    while (
        line_end > pos
        && (data[line_end - 1] == '\r' || data[line_end - 1] == ' ' || data[line_end - 1] == '\t'))
    {
      line_end--;
    }
    size_t line_len = line_end - pos;
    const char* line = data + pos;
    if (inside)
    {
      if (line_len == sizeof(end) - 1 && memcmp(line, end, line_len) == 0)
      {
        inside = false;
        blocks++;
      }
    }
    else if (line_len == sizeof(begin) - 1 && memcmp(line, begin, line_len) == 0)
    {
      inside = true;
    }
    else
    {
      for (size_t i = 0; i < line_len; ++i)
      {
        if (line[i] != ' ' && line[i] != '\t')
        {
          return 0;
        }
      }
    }
    pos = eol + 1;
  }
  return inside ? 0 : blocks;
}

/**
 * @brief True when @p data is a PEM chain of at least one certificate with
 * nothing else in it, every certificate parses, and the first (the leaf)
 * certifies @p key.
 *
 * @param[out] out_count Certificates parsed; may be NULL.
 */
static bool chain_matches_key(const char* data, size_t len, EVP_PKEY* key, size_t* out_count)
{
  size_t count = 0;
  size_t blocks = (data && len <= INT_MAX) ? count_pem_certificates(data, len) : 0;
  BIO* b = blocks > 0 ? BIO_new_mem_buf(data, (int)len) : NULL;
  bool ok = b != NULL && key != NULL;
  X509* cert = NULL;
  while (ok && (cert = PEM_read_bio_X509(b, NULL, NULL, NULL)) != NULL)
  {
    if (count == 0 && X509_check_private_key(cert, key) != 1)
    {
      ok = false;
    }
    X509_free(cert);
    count++;
  }
  BIO_free(b);
  ERR_clear_error();
  if (out_count)
  {
    *out_count = count;
  }
  return ok && count == blocks;
}

/**
 * @brief True when @p path holds a certificate chain that passes
 * chain_matches_key() for @p key.
 *
 * An empty, partial or unparseable file, or one issued for a different key,
 * is not a usable identity: TLS would fail with it on every connect.
 */
static bool operational_cert_is_valid(const char* path, EVP_PKEY* key)
{
  BIO* in = BIO_new_file(path, "rb");
  BIO* mem = in ? BIO_new(BIO_s_mem()) : NULL;
  bool ok = mem != NULL;
  char buf[1024];
  int n = 0;
  while (ok && (n = BIO_read(in, buf, (int)sizeof(buf))) > 0)
  {
    ok = BIO_write(mem, buf, n) == n;
  }
  if (ok)
  {
    char* data = NULL;
    long len = BIO_get_mem_data(mem, &data);
    ok = n == 0 && len > 0 && chain_matches_key(data, (size_t)len, key, NULL);
  }
  BIO_free(mem);
  BIO_free(in);
  ERR_clear_error();
  return ok;
}

/* --------------------------------------------------------------------------
 * vtable hooks
 * ------------------------------------------------------------------------ */

static az_iot_result managed_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  az_iot_certificate_provider_managed* m = (az_iot_certificate_provider_managed*)self;
  if (!m || !out)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!m->loaded)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  memset(out, 0, sizeof(*out));
  out->trusted_ca_path = m->trusted_ca_path;

  if (role == AZ_IOT_CRED_OPERATIONAL)
  {
    if (!m->has_operational)
    {
      return AZ_IOT_ERR_NOT_FOUND;
    }
    out->client_cert_path = m->operational_cert_path;
    out->client_key_path = m->operational_key_path;
  }
  else
  {
    out->client_cert_path = m->bootstrap_cert_path;
    out->client_key_path = m->bootstrap_key_path;
  }
  return AZ_IOT_OK;
}

static void managed_release(
    az_iot_certificate_provider* self,
    az_iot_certificate_material* material)
{
  (void)self;
  (void)material; /* paths are owned by the provider struct */
}

static az_iot_result managed_get_csr(
    az_iot_certificate_provider* self,
    const char* subject_common_name,
    az_iot_certificate_signing_request* out_csr)
{
  az_iot_certificate_provider_managed* m = (az_iot_certificate_provider_managed*)self;
  if (!m || !out_csr)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!m->loaded || !m->operational_key)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  out_csr->csr_base64 = NULL;

  /* Each CSR gets a new key; it becomes the operational key only once a chain
   * for it is stored, so the current identity keeps working until then. */
  EVP_PKEY* key = generate_key(m->key_type);
  if (!key)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  X509_REQ* req = NULL;
  X509_NAME* name = NULL;
  unsigned char* der = NULL;
  char* b64 = NULL;
  az_iot_result rc = AZ_IOT_ERR_INTERNAL;

  req = X509_REQ_new();
  if (!req)
  {
    goto done;
  }
  if (X509_REQ_set_version(req, 0L) != 1)
  {
    goto done; /* PKCS#10 v1 */
  }

  name = X509_NAME_new();
  if (!name)
  {
    goto done;
  }
  {
    const char* cn
        = is_nonempty_cstr(subject_common_name) ? subject_common_name : "azure-iot-device";
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char*)cn, -1, -1, 0)
        != 1)
    {
      goto done;
    }
  }
  if (X509_REQ_set_subject_name(req, name) != 1)
  {
    goto done;
  }
  if (X509_REQ_set_pubkey(req, key) != 1)
  {
    goto done;
  }
  if (X509_REQ_sign(req, key, EVP_sha256()) == 0)
  {
    goto done;
  }

  {
    int der_len = i2d_X509_REQ(req, &der);
    if (der_len <= 0 || der == NULL)
    {
      goto done;
    }

    size_t b64_cap = BASE64_ENCODED_LEN((size_t)der_len) + 1;
    b64 = malloc(b64_cap);
    if (!b64)
    {
      rc = AZ_IOT_ERR_OUT_OF_MEMORY;
      goto done;
    }
    int b64_len = EVP_EncodeBlock((unsigned char*)b64, der, der_len);
    if (b64_len <= 0)
    {
      goto done;
    }
    b64[b64_len] = '\0';
  }

  out_csr->csr_base64 = b64;
  b64 = NULL; /* ownership transferred to caller (freed via release_csr) */
  EVP_PKEY_free((EVP_PKEY*)m->pending_key); /* superseded: its CSR can no longer complete */
  m->pending_key = key;
  key = NULL;
  rc = AZ_IOT_OK;

done:
  EVP_PKEY_free(key);
  if (der)
  {
    OPENSSL_free(der);
  }
  if (b64)
  {
    free(b64);
  }
  if (name)
  {
    X509_NAME_free(name);
  }
  if (req)
  {
    X509_REQ_free(req);
  }
  return rc;
}

static void managed_release_csr(
    az_iot_certificate_provider* self,
    az_iot_certificate_signing_request* csr)
{
  (void)self;
  if (csr && csr->csr_base64)
  {
    free((void*)csr->csr_base64);
    csr->csr_base64 = NULL;
  }
}

/* Write one issued certificate to `b` as PEM.
 *
 * The chain arrives base64-encoded, but what that base64 covers is not fixed:
 * the provisioning service encodes a whole PEM certificate, while the DER form
 * the header documents is what a plain base64-DER encoder produces. Wrapping an
 * already-PEM payload in a second set of BEGIN/END lines yields a file no TLS
 * stack can parse, which surfaces only much later as a handshake failure, so
 * decode first and write whichever form came back.
 *
 * Returns true on success. */
static bool write_issued_cert(BIO* b, const uint8_t* base64, int base64_len)
{
  bool ok = false;
  /* Decoded output is always shorter than its base64; +1 so the result can be
   * examined as a string. */
  unsigned char* decoded = (unsigned char*)calloc(1, (size_t)base64_len + 1);
  if (decoded == NULL)
  {
    return false;
  }

  int decoded_len = EVP_DecodeBlock(decoded, base64, base64_len);
  if (decoded_len <= 0)
  {
    /* Not decodable base64, so neither form can be recovered from it. Writing it
     * anyway would persist a certificate file that cannot be parsed and would
     * only be discovered later, as a connection failure. */
    free(decoded);
    return false;
  }

  /* EVP_DecodeBlock rounds up to a multiple of three and counts the '=' padding
   * as data, so trim it back before the content is inspected or written. */
  if (base64_len >= 2 && base64[base64_len - 1] == '=')
  {
    decoded_len--;
    if (base64[base64_len - 2] == '=')
    {
      decoded_len--;
    }
  }

  if (decoded_len <= 0)
  {
    free(decoded);
    return false;
  }

  if ((size_t)decoded_len >= strlen(PEM_CERT_PREFIX)
      && memcmp(decoded, PEM_CERT_PREFIX, strlen(PEM_CERT_PREFIX)) == 0)
  {
    /* Already PEM: write it through unchanged, and guarantee the newline that
     * separates it from the next certificate in the chain. */
    ok = BIO_write(b, decoded, decoded_len) == decoded_len;
    if (ok && decoded[decoded_len - 1] != '\n')
    {
      ok = BIO_puts(b, "\n") > 0;
    }
  }
  else
  {
    /* Base64 DER: frame it, breaking the body into PEM-length lines. A single
     * unbroken line is accepted by some parsers and rejected by others. */
    ok = BIO_puts(b, PEM_CERT_BEGIN) > 0;
    for (int off = 0; ok && off < base64_len; off += PEM_LINE_LEN)
    {
      int chunk = base64_len - off < PEM_LINE_LEN ? base64_len - off : PEM_LINE_LEN;
      ok = BIO_write(b, base64 + off, chunk) == chunk;
      if (ok && off + chunk < base64_len)
      {
        ok = BIO_puts(b, "\n") > 0;
      }
    }
    if (ok)
    {
      ok = BIO_puts(b, PEM_CERT_END) > 0;
    }
  }

  free(decoded);
  return ok;
}

static az_iot_result managed_store(
    az_iot_certificate_provider* self,
    const az_iot_issued_certificate* issued)
{
  az_iot_certificate_provider_managed* m = (az_iot_certificate_provider_managed*)self;
  if (!m || !issued)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!m->loaded)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  if (!issued->certificates || issued->count == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* All-or-nothing: build the chain in memory, refuse it unless every entry
   * parses as a certificate and the leaf certifies a key this provider holds,
   * then replace the file(s). A failure leaves the previous key and
   * certificate in use. */
  BIO* mem = BIO_new(BIO_s_mem());
  if (!mem)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }

  /* Write each issued cert (leaf first). */
  az_iot_result rc = AZ_IOT_OK;
  size_t written = 0;
  for (size_t i = 0; i < issued->count; ++i)
  {
    az_span cert = issued->certificates[i];
    int len = (int)az_span_size(cert);
    if (len <= 0)
    {
      rc = AZ_IOT_ERR_INVALID_ARG; /* every issued entry must be a certificate */
      break;
    }
    if (!write_issued_cert(mem, az_span_ptr(cert), len))
    {
      rc = AZ_IOT_ERR_INTERNAL;
      break;
    }
    written++;
  }

  /* The leaf must certify the pending CSR key (rotate) or the current key. */
  EVP_PKEY* pending = (EVP_PKEY*)m->pending_key;
  bool rotate = false;
  if (rc == AZ_IOT_OK)
  {
    char* data = NULL;
    long data_len = BIO_get_mem_data(mem, &data);
    size_t parsed = 0;
    size_t len = data_len > 0 ? (size_t)data_len : 0;
    rotate = pending && chain_matches_key(data, len, pending, &parsed) && parsed == written;
    if (!rotate
        && (!chain_matches_key(data, len, (EVP_PKEY*)m->operational_key, &parsed)
            || parsed != written))
    {
      rc = AZ_IOT_ERR_INVALID_ARG;
    }
  }

  if (rc == AZ_IOT_OK && !rotate)
  {
    rc = write_bio_private(m->operational_cert_path, mem);
  }
  else if (rc == AZ_IOT_OK)
  {
    /* Stage both files, then swap key before chain. Only a failed final rename
     * can split the pair; the previous key is then restored so they match. */
    char* key_tmp = NULL;
    char* cert_tmp = NULL;
    BIO* key_mem = key_to_bio(pending);
    rc = key_mem ? stage_bio(m->operational_key_path, key_mem, &key_tmp) : AZ_IOT_ERR_INTERNAL;
    BIO_free(key_mem);
    if (rc == AZ_IOT_OK)
    {
      rc = stage_bio(m->operational_cert_path, mem, &cert_tmp);
    }
    if (rc == AZ_IOT_OK)
    {
      rc = commit_file(key_tmp, m->operational_key_path);
      key_tmp = NULL;
      if (rc == AZ_IOT_OK)
      {
        rc = commit_file(cert_tmp, m->operational_cert_path);
        cert_tmp = NULL;
        if (rc != AZ_IOT_OK)
        {
          (void)write_key_file(m->operational_key_path, (EVP_PKEY*)m->operational_key);
        }
      }
    }
    discard_file(key_tmp);
    discard_file(cert_tmp);
    if (rc == AZ_IOT_OK)
    {
      EVP_PKEY_free((EVP_PKEY*)m->operational_key);
      m->operational_key = pending;
      m->pending_key = NULL;
    }
  }
  BIO_free(mem);

  if (rc == AZ_IOT_OK)
  {
    m->has_operational = true;
  }
  return rc;
}

static void managed_deinit_vtable(az_iot_certificate_provider* self)
{
  az_iot_certificate_provider_managed_deinit((az_iot_certificate_provider_managed*)self);
}

static const az_iot_certificate_provider_vtable s_managed_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = managed_load,
  .release = managed_release,
  .deinit = managed_deinit_vtable,
  .get_csr = managed_get_csr,
  .release_csr = managed_release_csr,
  .store_issued_certificate = managed_store,
};

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

void az_iot_certificate_provider_managed_deinit(az_iot_certificate_provider_managed* provider)
{
  if (!provider)
  {
    return;
  }
  if (provider->operational_key)
  {
    EVP_PKEY_free((EVP_PKEY*)provider->operational_key);
  }
  EVP_PKEY_free((EVP_PKEY*)provider->pending_key);
  free(provider->bootstrap_cert_path);
  free(provider->bootstrap_key_path);
  free(provider->trusted_ca_path);
  free(provider->operational_key_path);
  free(provider->operational_cert_path);
  memset(provider, 0, sizeof(*provider));
}

az_iot_result az_iot_certificate_provider_managed_init(
    az_iot_certificate_provider_managed* provider,
    const az_iot_certificate_provider_managed_options* opts)
{
  if (!provider || !opts)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!is_nonempty_cstr(opts->bootstrap_cert_pem_path)
      || !is_nonempty_cstr(opts->bootstrap_key_pem_path)
      || !is_nonempty_cstr(opts->operational_key_pem_path)
      || !is_nonempty_cstr(opts->operational_cert_pem_path))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(provider, 0, sizeof(*provider));
  provider->base.vtable = &s_managed_vtable;
  provider->key_type = (int)opts->key_type;

  provider->bootstrap_cert_path = dup_str(opts->bootstrap_cert_pem_path);
  provider->bootstrap_key_path = dup_str(opts->bootstrap_key_pem_path);
  provider->operational_key_path = dup_str(opts->operational_key_pem_path);
  provider->operational_cert_path = dup_str(opts->operational_cert_pem_path);
  if (!provider->bootstrap_cert_path || !provider->bootstrap_key_path
      || !provider->operational_key_path || !provider->operational_cert_path)
  {
    az_iot_certificate_provider_managed_deinit(provider);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  if (is_nonempty_cstr(opts->trusted_ca_pem_path))
  {
    provider->trusted_ca_path = dup_str(opts->trusted_ca_pem_path);
    if (!provider->trusted_ca_path)
    {
      az_iot_certificate_provider_managed_deinit(provider);
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }

  /* Operational key: load if present on disk, else generate and persist. */
  EVP_PKEY* key = load_key_file(provider->operational_key_path);
  if (!key)
  {
    key = generate_key(provider->key_type);
    if (!key)
    {
      az_iot_certificate_provider_managed_deinit(provider);
      return AZ_IOT_ERR_INTERNAL;
    }
    az_iot_result wr = write_key_file(provider->operational_key_path, key);
    if (wr != AZ_IOT_OK)
    {
      EVP_PKEY_free(key);
      az_iot_certificate_provider_managed_deinit(provider);
      return wr;
    }
  }
  provider->operational_key = key;

  /* An operational cert persisted by a previous run, issued for this key, means
   * we can connect with the OPERATIONAL identity immediately (no re-enrollment). */
  provider->has_operational = operational_cert_is_valid(provider->operational_cert_path, key);

  provider->loaded = true;
  return AZ_IOT_OK;
}
