// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Non-extractable key custody for the Paho adapter (D8). Rationale and the
 * contract for every function live in az_iot_paho_key_custody.h. */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "az_iot_paho_key_custody.h"
#include "azure/iot/az_iot_log.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#if defined(AZ_IOT_PAHO_KEY_CUSTODY)
#include <openssl/asn1t.h>
#include <openssl/bio.h>
#include <openssl/encoder.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/provider.h>
#include <openssl/store.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif /* AZ_IOT_PAHO_KEY_CUSTODY */

bool az_iot_paho_key_custody_requested(const az_iot_mqtt_tls_options* tls)
{
  return tls != NULL && (tls->client_key_uri != NULL || tls->sign != NULL);
}

#if !defined(AZ_IOT_PAHO_KEY_CUSTODY)

az_iot_result az_iot_paho_key_custody_prepare(
    az_iot_paho_key_custody* state,
    const az_iot_mqtt_tls_options* tls,
    const char** out_private_key_path)
{
  if (!state || !tls || !out_private_key_path)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (az_iot_paho_key_custody_requested(tls))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_PAHO,
        "a non-extractable key was requested but the adapter was built without "
        "key custody support (needs OpenSSL 3.0+)");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  *out_private_key_path = tls->client_key_path;
  return AZ_IOT_OK;
}

void az_iot_paho_key_custody_release(az_iot_paho_key_custody* state) { (void)state; }

#else /* AZ_IOT_PAHO_KEY_CUSTODY */

/* PEM banners that carry real private-key bytes. Anything else an OpenSSL
 * encoder emits for a key ("PKCS#11 PROVIDER URI", "TSS2 PRIVATE KEY", ...) is
 * a reference to a key held elsewhere, which is the only thing this path is
 * willing to write to disk. */
static const char* const s_extractable_banners[] = {
  "-----BEGIN PRIVATE KEY-----",     "-----BEGIN ENCRYPTED PRIVATE KEY-----",
  "-----BEGIN RSA PRIVATE KEY-----", "-----BEGIN EC PRIVATE KEY-----",
  "-----BEGIN DSA PRIVATE KEY-----",
};

bool az_iot_paho_key_custody_pem_carries_private_key(const char* pem, size_t len)
{
  if (!pem)
  {
    return false;
  }
  /* Search the WHOLE buffer, not just its start. Nothing guarantees the encoder
   * put the BEGIN line at offset 0 -- leading newlines, or a textual preamble
   * ahead of the block, would hide a real private key from a check anchored at
   * the start, and this check is the only thing standing between an extractable
   * key and a file on disk. A false negative here is the failure this module
   * exists to prevent, so it errs toward looking everywhere.
   *
   * The banners are matched in full rather than by a looser rule such as
   * "contains PRIVATE KEY": the tpm2 reference block is labelled
   * "TSS2 PRIVATE KEY", so a substring rule would reject the very references
   * this path is meant to accept. */
  for (size_t i = 0; i < sizeof(s_extractable_banners) / sizeof(s_extractable_banners[0]); ++i)
  {
    const char* banner = s_extractable_banners[i];
    size_t blen = strlen(banner);
    if (len < blen)
    {
      continue;
    }
    for (size_t off = 0; off + blen <= len; ++off)
    {
      if (memcmp(pem + off, banner, blen) == 0)
      {
        return true;
      }
    }
  }
  return false;
}

/* RFC 7512 puts credentials in the URI's query component ("?pin-value=...",
 * "?pin-source=..."), and a key URI ends up in error logs. Copy only the path
 * component, which names the object and nothing secret, and mark the rest.
 * `out` is always NUL-terminated. */
static void redact_key_uri(const char* uri, char* out, size_t out_cap)
{
  const char* query = strchr(uri, '?');
  size_t keep = query ? (size_t)(query - uri) : strlen(uri);
  const char* suffix = query ? "?<redacted>" : "";
  size_t suffix_len = strlen(suffix);
  if (keep + suffix_len + 1 > out_cap)
  {
    keep = (out_cap > suffix_len + 1) ? out_cap - suffix_len - 1 : 0;
  }
  memcpy(out, uri, keep);
  memcpy(out + keep, suffix, suffix_len);
  out[keep + suffix_len] = '\0';
}

/* Case-insensitive search, for URI attribute names. strcasestr is not portable
 * and MSVC has no equivalent. */
static bool contains_ci(const char* haystack, const char* needle)
{
  size_t nlen = strlen(needle);
  if (nlen == 0)
  {
    return true;
  }
  for (const char* p = haystack; *p != '\0'; ++p)
  {
    size_t i = 0;
    while (i < nlen && p[i] != '\0'
           && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
    {
      ++i;
    }
    if (i == nlen)
    {
      return true;
    }
  }
  return false;
}

/* Does the URI's query component carry the token PIN ITSELF?
 *
 * RFC 7512 spells two things there. pin-value is the PIN in plain text, and
 * writing that into a file that outlives this call puts the credential on disk
 * -- the one outcome this module exists to avoid. pin-source only NAMES where
 * the PIN lives, so it carries no secret and the file it points at keeps its
 * own permissions.
 *
 * The distinction is not academic: the reference file has to stay loadable on
 * its own, because SSL_CTX_use_PrivateKey_file resolves it later and the
 * provider must log in to the token to do so. Refusing pin-source as well would
 * leave a PIN-protected token with no way to express itself as a reference at
 * all, which is why only pin-value is treated as secret here. Everything else
 * in the query -- module-path and friends -- is configuration the decoder
 * legitimately needs. */
static bool uri_query_carries_pin(const char* uri)
{
  const char* query = strchr(uri, '?');
  if (!query)
  {
    return false;
  }
  return contains_ci(query, "pin-value");
}

/* Enough for a realistic PKCS#11 or TPM object URI; longer ones are truncated,
 * which is a diagnostic, not a decision. */
#define AZ_IOT_KEY_URI_LOG_MAX 256

static void log_openssl_errors(const char* what)
{
  unsigned long e;
  while ((e = ERR_get_error()) != 0)
  {
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_PAHO, "%s: %s", what, buf);
  }
}

/* Make the OpenSSL 3.x provider named by `id` available: "pkcs11"
 * (pkcs11-provider), "tpm2" (tpm2-openssl), or any other the integrator has
 * installed.
 *
 * A provider is a PROCESS-wide resource and is deliberately never unloaded
 * here. Unloading one while another connection still holds keys obtained
 * through it tears those keys out from under it, and OpenSSL keeps it alive for
 * the process anyway -- so per-connection unloading would add nothing but that
 * failure mode.
 *
 * Legacy OpenSSL ENGINEs are deliberately not attempted. They are deprecated in
 * OpenSSL 3.0, and -- decisively for this path -- a key an ENGINE returns is a
 * legacy object with no reference form, so it cannot be expressed as the PEM
 * that is the only thing Paho can be handed. An ENGINE-only stack gets a clear
 * AZ_IOT_ERR_NOT_SUPPORTED naming the id rather than a key that silently is not
 * used. */
static az_iot_result load_crypto_backend(const char* id)
{
  if (OSSL_PROVIDER_available(NULL, id))
  {
    return AZ_IOT_OK;
  }
  /* retain_fallbacks = 1: the default provider stays available, so ordinary
   * certificate and CA parsing keeps working alongside the custody key. */
  if (OSSL_PROVIDER_try_load(NULL, id, 1))
  {
    AZ_IOT_LOG_INFOF(
        AZ_IOT_LOG_COMPONENT_PAHO,
        "loaded OpenSSL provider '%s' for non-extractable key custody",
        id);
    return AZ_IOT_OK;
  }
  ERR_clear_error();
  AZ_IOT_LOG_ERRORF(
      AZ_IOT_LOG_COMPONENT_PAHO,
      "crypto_engine_id '%s' names no loadable OpenSSL provider; install it (e.g. "
      "pkcs11-provider for PKCS#11, tpm2-openssl for TPM 2.0) or point OPENSSL_MODULES at it",
      id);
  return AZ_IOT_ERR_NOT_SUPPORTED;
}

/* Resolve the key reference. OSSL_STORE covers every provider-backed URI scheme
 * (pkcs11:, tpm2:, ...) behind one call. */
static EVP_PKEY* resolve_key(const char* uri)
{
  OSSL_STORE_CTX* store = OSSL_STORE_open(uri, NULL, NULL, NULL, NULL);
  if (!store)
  {
    return NULL;
  }
  EVP_PKEY* pkey = NULL;
  while (!OSSL_STORE_eof(store))
  {
    OSSL_STORE_INFO* info = OSSL_STORE_load(store);
    if (!info)
    {
      break;
    }
    if (OSSL_STORE_INFO_get_type(info) == OSSL_STORE_INFO_PKEY)
    {
      pkey = OSSL_STORE_INFO_get1_PKEY(info);
    }
    OSSL_STORE_INFO_free(info);
    if (pkey)
    {
      break;
    }
  }
  OSSL_STORE_close(store);
  return pkey;
}

/* The interchange format for "this key lives in a PKCS#11 token": a DER
 * SEQUENCE of a fixed description string and the RFC 7512 URI, PEM-wrapped
 * under the "PKCS#11 PROVIDER URI" label. It carries no key material at all --
 * only the name of the object inside the token.
 *
 * It is written here rather than obtained from the provider because a provider
 * ENCODER for it is optional (pkcs11-provider emits it only when the integrator
 * sets pkcs11-encode-provider-uri-to-pem in openssl.cnf). The DECODER side is
 * always registered, and that is the side that matters: it is what makes
 * SSL_CTX_use_PrivateKey_file -- and therefore Paho -- resolve this file back
 * to the key inside the token. */
#define AZ_IOT_PK11_URI_PEM_LABEL "PKCS#11 PROVIDER URI"
#define AZ_IOT_PK11_URI_DESCRIPTION "PKCS#11 Provider URI v1.0"

typedef struct az_iot_pk11_uri
{
  ASN1_VISIBLESTRING* description;
  ASN1_UTF8STRING* uri;
} az_iot_pk11_uri;

/* Only the allocator pair and the item template are generated: nothing here
 * ever DECODES this structure -- the provider's own decoder does that when
 * OpenSSL reads the file back -- so IMPLEMENT_ASN1_FUNCTIONS would emit a d2i
 * that no call site can reach. Both are external, so they are declared to
 * satisfy -Wmissing-prototypes; the az_iot_ prefix keeps them clear of the
 * vendored dependencies. */
DECLARE_ASN1_ITEM(az_iot_pk11_uri)
DECLARE_ASN1_ALLOC_FUNCTIONS(az_iot_pk11_uri)

/* clang-format off */
/* These are OpenSSL's ASN.1 template macros, not declarations clang-format can
 * parse: they expand to a static table plus function definitions, and none of
 * them ends in a semicolon. Left to itself the formatter reflows the block into
 * something that compiles but cannot be read -- it folds the following
 * function's return type into the macro.
 *
 * The re-enable sits inside the next function body rather than after the
 * macros: clang-format does not recover its parse state at the `on` marker
 * while the unterminated macro expression is still open, so an earlier marker
 * gets re-indented itself and the file stops matching the gate. */
ASN1_SEQUENCE(az_iot_pk11_uri) = {
    ASN1_SIMPLE(az_iot_pk11_uri, description, ASN1_VISIBLESTRING),
    ASN1_SIMPLE(az_iot_pk11_uri, uri, ASN1_UTF8STRING)
} ASN1_SEQUENCE_END(az_iot_pk11_uri)

IMPLEMENT_ASN1_ALLOC_FUNCTIONS(az_iot_pk11_uri)

/* Encode `uri` as the reference above, PEM-wrapped, into a memory BIO (caller
 * frees). NULL if it could not be built. */
static BIO* encode_pk11_uri_reference(const char* uri)
{
  /* clang-format on */
  az_iot_pk11_uri* obj = az_iot_pk11_uri_new();
  if (!obj)
  {
    return NULL;
  }
  unsigned char* der = NULL;
  int der_len = -1;
  if (ASN1_STRING_set(
          obj->description,
          AZ_IOT_PK11_URI_DESCRIPTION,
          (int)(sizeof(AZ_IOT_PK11_URI_DESCRIPTION) - 1))
      && ASN1_STRING_set(obj->uri, uri, (int)strlen(uri)))
  {
    der_len = ASN1_item_i2d((ASN1_VALUE*)obj, &der, ASN1_ITEM_rptr(az_iot_pk11_uri));
  }
  az_iot_pk11_uri_free(obj);
  if (der_len <= 0)
  {
    OPENSSL_free(der);
    return NULL;
  }

  BIO* mem = BIO_new(BIO_s_mem());
  int ok = mem != NULL && PEM_write_bio(mem, AZ_IOT_PK11_URI_PEM_LABEL, "", der, (long)der_len);
  OPENSSL_free(der);
  if (!ok)
  {
    BIO_free(mem);
    return NULL;
  }
  return mem;
}

/* Ask the provider to express the resolved key itself. A backend that keeps the
 * key non-extractable emits its own reference form here (tpm2-openssl writes a
 * TSS2 PRIVATE KEY block); one that will not, or cannot, emits nothing and the
 * caller falls back. A backend holding an ordinary software key emits the key,
 * which is what the extractability check exists to catch.
 *
 * Returns the PEM in a memory BIO (caller frees), or NULL. */
static BIO* encode_key_through_provider(EVP_PKEY* pkey)
{
  BIO* mem = BIO_new(BIO_s_mem());
  if (!mem)
  {
    return NULL;
  }
  OSSL_ENCODER_CTX* ectx = OSSL_ENCODER_CTX_new_for_pkey(
      pkey, OSSL_KEYMGMT_SELECT_PRIVATE_KEY, "PEM", "PrivateKeyInfo", NULL);
  int ok = ectx != NULL && OSSL_ENCODER_to_bio(ectx, mem);
  OSSL_ENCODER_CTX_free(ectx);
  ERR_clear_error();
  char* pem = NULL;
  if (!ok || BIO_get_mem_data(mem, &pem) <= 0)
  {
    BIO_free(mem);
    return NULL;
  }
  return mem;
}

/* Create a private, empty file and return its path (heap, caller frees). */
static char* create_private_temp_file(void)
{
#if defined(_WIN32)
  char dir[MAX_PATH + 1];
  DWORD n = GetTempPathA((DWORD)sizeof(dir), dir);
  if (n == 0 || n > MAX_PATH)
  {
    return NULL;
  }
  char* path = (char*)malloc(MAX_PATH + 1);
  if (!path)
  {
    return NULL;
  }
  if (GetTempFileNameA(dir, "azk", 0, path) == 0)
  {
    free(path);
    return NULL;
  }
  return path;
#else
  const char* dir = getenv("TMPDIR");
  if (!dir || dir[0] == '\0')
  {
    dir = "/tmp";
  }
  static const char suffix[] = "/az-iot-keyref-XXXXXX";
  size_t dir_len = strlen(dir);
  char* path = (char*)malloc(dir_len + sizeof(suffix));
  if (!path)
  {
    return NULL;
  }
  /* NOLINTNEXTLINE(bugprone-not-null-terminated-result): the next memcpy adds the NUL. */
  memcpy(path, dir, dir_len);
  memcpy(path + dir_len, suffix, sizeof(suffix));
  int fd = mkstemp(path); /* 0600, and the name cannot be raced */
  if (fd < 0)
  {
    free(path);
    return NULL;
  }
  close(fd);
  return path;
#endif
}

/* Can OpenSSL turn these reference bytes back into a usable private key?
 *
 * This is the exact operation Paho performs on the file
 * (SSL_CTX_use_PrivateKey_file -> PEM_read_bio_PrivateKey_ex), and it is not a
 * given: a provider registers the DECODER for its own reference form
 * separately from anything else, and older builds ship without one. Checking
 * here turns "the file is unreadable" into a named error at connect time
 * instead of a PEM failure inside the handshake -- which is the failure mode
 * this whole path exists to remove. */
static bool reference_round_trips(const char* pem, long len)
{
  BIO* in = BIO_new_mem_buf(pem, (int)len);
  if (!in)
  {
    return false;
  }
  EVP_PKEY* decoded = PEM_read_bio_PrivateKey_ex(in, NULL, NULL, NULL, NULL, NULL);
  BIO_free(in);
  if (!decoded)
  {
    return false;
  }
  EVP_PKEY_free(decoded);
  return true;
}

/* Write `pem` (`len` bytes) to a fresh private temporary file and record the
 * path on `state`. */
static az_iot_result store_reference_pem(az_iot_paho_key_custody* state, const char* pem, long len)
{
  char* path = create_private_temp_file();
  if (!path)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_PAHO, "could not create a temporary file for the key reference");
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  /* Binary mode: the bytes must land exactly as produced. On Windows "w" would
   * translate newlines, which is a difference this file has no reason to
   * carry. */
  BIO* out = BIO_new_file(path, "wb");
  int written = out != NULL && BIO_write(out, pem, (int)len) == (int)len;
  BIO_free(out);
  if (!written)
  {
    log_openssl_errors("could not write the key reference file");
    (void)remove(path); /* best effort */
    free(path);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  state->key_ref_path = path;
  return AZ_IOT_OK;
}

static az_iot_result write_key_reference(
    az_iot_paho_key_custody* state,
    EVP_PKEY* pkey,
    const char* uri)
{
  /* The provider's own reference form first, because it is the general answer
   * and the only one a non-PKCS#11 backend such as tpm2-openssl has. The
   * standard PKCS#11 URI reference is the fallback, for the common case of a
   * provider that registers only a decoder for it. */
  BIO* mem = encode_key_through_provider(pkey);
  if (!mem && strncmp(uri, "pkcs11:", 7) == 0)
  {
    /* The fallback embeds the URI itself in the file. A URI carrying the token
     * PIN would therefore persist that PIN to disk for as long as the
     * connection lives -- in a path whose whole purpose is that the credential
     * never becomes bytes on disk. Refuse instead, exactly as an extractable
     * key is refused below.
     *
     * Only this branch is affected. A provider that emits its own reference
     * form never embeds the URI, so a PIN in the URI is harmless there: it is
     * used to open the token in memory and goes no further. */
    if (uri_query_carries_pin(uri))
    {
      char safe_uri[AZ_IOT_KEY_URI_LOG_MAX];
      redact_key_uri(uri, safe_uri, sizeof(safe_uri));
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_PAHO,
          "'%s' carries the token PIN in its query (pin-value), and the only reference this "
          "provider can express is the URI itself -- writing it would persist the PIN to disk. Use "
          "pin-source to name a file the provider reads the PIN from, or a provider that encodes "
          "its own key reference",
          safe_uri);
      return AZ_IOT_ERR_TLS;
    }
    mem = encode_pk11_uri_reference(uri);
  }
  if (!mem)
  {
    log_openssl_errors("could not build a key reference");
    char safe_uri[AZ_IOT_KEY_URI_LOG_MAX];
    redact_key_uri(uri, safe_uri, sizeof(safe_uri));
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_PAHO,
        "'%s' resolved to a key expressible neither by its own provider nor as a PKCS#11 URI "
        "reference, so there is nothing to hand the TLS stack",
        safe_uri);
    return AZ_IOT_ERR_TLS;
  }

  char* pem = NULL;
  long pem_len = BIO_get_mem_data(mem, &pem);
  if (az_iot_paho_key_custody_pem_carries_private_key(pem, (size_t)pem_len))
  {
    /* The whole point of the key-reference path is that the key never becomes
     * bytes. Writing it out here would do exactly that, silently. */
    OPENSSL_cleanse(pem, (size_t)pem_len);
    BIO_free(mem);
    char safe_uri[AZ_IOT_KEY_URI_LOG_MAX];
    redact_key_uri(uri, safe_uri, sizeof(safe_uri));
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_PAHO,
        "'%s' resolved to an EXTRACTABLE private key; refusing to write private key material "
        "to disk. Point client_key_path at the key instead, or use a key the token keeps "
        "non-extractable",
        safe_uri);
    return AZ_IOT_ERR_TLS;
  }

  if (!reference_round_trips(pem, pem_len))
  {
    log_openssl_errors("the key reference cannot be decoded back");
    BIO_free(mem);
    char safe_uri[AZ_IOT_KEY_URI_LOG_MAX];
    redact_key_uri(uri, safe_uri, sizeof(safe_uri));
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_PAHO,
        "the reference built for '%s' cannot be decoded by this OpenSSL installation, so the "
        "TLS stack could not load it either. The provider must register a DECODER for its own "
        "reference form -- pkcs11-provider 0.5 or later, or tpm2-openssl",
        safe_uri);
    return AZ_IOT_ERR_TLS;
  }

  az_iot_result r = store_reference_pem(state, pem, pem_len);
  BIO_free(mem);
  return r;
}

az_iot_result az_iot_paho_key_custody_prepare(
    az_iot_paho_key_custody* state,
    const az_iot_mqtt_tls_options* tls,
    const char** out_private_key_path)
{
  if (!state || !tls || !out_private_key_path)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!az_iot_paho_key_custody_requested(tls))
  {
    *out_private_key_path = tls->client_key_path;
    return AZ_IOT_OK;
  }

  if (tls->client_key_uri == NULL)
  {
    /* sign() with no key reference. Paho's SSL options expose neither the
     * SSL_CTX nor a key callback, so there is no seam to route a signature
     * through; upstream Paho has none either. Any BYO adapter still receives
     * the hook through az_iot_mqtt_tls_options. */
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_PAHO,
        "a provider sign() hook without a key URI cannot be honoured by this "
        "adapter -- Paho exposes no TLS key callback. Supply client_key_uri + "
        "crypto_engine_id, or use an adapter that consumes tls.sign");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (tls->crypto_engine_id == NULL)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_PAHO,
        "client_key_uri was set without crypto_engine_id; nothing names the "
        "provider that owns the key");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  char safe_uri[AZ_IOT_KEY_URI_LOG_MAX];
  redact_key_uri(tls->client_key_uri, safe_uri, sizeof(safe_uri));

  az_iot_result r = load_crypto_backend(tls->crypto_engine_id);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  EVP_PKEY* pkey = resolve_key(tls->client_key_uri);
  if (!pkey)
  {
    log_openssl_errors("could not resolve the key URI");
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_PAHO,
        "provider '%s' could not resolve client_key_uri '%s'",
        tls->crypto_engine_id,
        safe_uri);
    return AZ_IOT_ERR_TLS;
  }

  r = write_key_reference(state, pkey, tls->client_key_uri);
  EVP_PKEY_free(pkey);
  if (r != AZ_IOT_OK)
  {
    az_iot_paho_key_custody_release(state);
    return r;
  }

  AZ_IOT_LOG_INFOF(
      AZ_IOT_LOG_COMPONENT_PAHO,
      "TLS will sign with the non-extractable key '%s' via '%s'",
      safe_uri,
      tls->crypto_engine_id);
  *out_private_key_path = state->key_ref_path;
  return AZ_IOT_OK;
}

void az_iot_paho_key_custody_release(az_iot_paho_key_custody* state)
{
  if (!state)
  {
    return;
  }
  if (state->key_ref_path)
  {
    (void)remove(state->key_ref_path); /* best effort */
    free(state->key_ref_path);
    state->key_ref_path = NULL;
  }
}

#endif /* AZ_IOT_PAHO_KEY_CUSTODY */
