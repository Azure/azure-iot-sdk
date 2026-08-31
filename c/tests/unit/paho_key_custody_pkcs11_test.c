// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Paho key custody against a REAL PKCS#11 token (D8).
 *
 * Built only when AZ_IOT_BUILD_PKCS11_TESTS is on, i.e. when a token is going
 * to be provided -- the same rule the broker-gated conformance suites follow.
 * It does NOT excuse itself when the environment is missing: a suite that
 * quietly passes with nothing configured is a suite nobody notices has stopped
 * running.
 *
 * Required environment:
 *   AZ_IOT_TEST_PKCS11_KEY_URI  RFC 7512 URI of a private key in the token,
 *                               including whatever pin-value the token needs.
 *   AZ_IOT_TEST_PKCS11_PROVIDER OpenSSL provider id (default "pkcs11").
 *
 * The decisive case is use_the_reference_the_way_paho_does: it feeds the
 * reference the adapter produced to SSL_CTX_use_PrivateKey_file, which is
 * verbatim what Paho does with MQTTAsync_SSLOptions::privateKey. If that
 * succeeds, the TLS handshake signs inside the token. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "az_iot_paho_key_custody.h"

static const char* g_key_uri = NULL;
static const char* g_provider_id = NULL;

static int group_setup(void** state)
{
  (void)state;
  g_key_uri = getenv("AZ_IOT_TEST_PKCS11_KEY_URI");
  g_provider_id = getenv("AZ_IOT_TEST_PKCS11_PROVIDER");
  if (!g_provider_id || g_provider_id[0] == '\0')
  {
    g_provider_id = "pkcs11";
  }
  if (!g_key_uri || g_key_uri[0] == '\0')
  {
    fprintf(
        stderr,
        "pkcs11-custody: AZ_IOT_TEST_PKCS11_KEY_URI is unset or empty, but this suite was built "
        "with AZ_IOT_BUILD_PKCS11_TESTS=ON. Point it at a private key in a PKCS#11 token, or "
        "configure with AZ_IOT_BUILD_PKCS11_TESTS=OFF so the suite is not registered.\n");
    return 1;
  }
  return 0;
}

static void tls_options_for_token(az_iot_mqtt_tls_options* tls)
{
  memset(tls, 0, sizeof(*tls));
  tls->client_key_uri = g_key_uri;
  tls->crypto_engine_id = g_provider_id;
}

static char* read_file(const char* path, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  assert_non_null(f);
  assert_int_equal(fseek(f, 0, SEEK_END), 0);
  long n = ftell(f);
  assert_true(n > 0);
  rewind(f);
  char* buf = (char*)calloc(1, (size_t)n + 1);
  assert_non_null(buf);
  assert_int_equal(fread(buf, 1, (size_t)n, f), (size_t)n);
  fclose(f);
  *out_len = (size_t)n;
  return buf;
}

/* The key resolves, and what lands on disk is a reference -- never key bytes. */
static void a_token_key_yields_a_reference_not_a_key(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  tls_options_for_token(&tls);

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_OK);
  assert_non_null(path);
  assert_string_equal(path, s.key_ref_path);

  size_t len = 0;
  char* pem = read_file(path, &len);
  assert_null(strstr(pem, "-----BEGIN PRIVATE KEY-----"));
  assert_null(strstr(pem, "-----BEGIN EC PRIVATE KEY-----"));
  assert_null(strstr(pem, "-----BEGIN RSA PRIVATE KEY-----"));
  free(pem);

  az_iot_paho_key_custody_release(&s);
  /* The reference file does not outlive the connection that used it. */
  assert_null(s.key_ref_path);
  FILE* gone = fopen(path, "rb");
  assert_null(gone);
}

/* The proof that the handshake will sign in hardware: OpenSSL loads the
 * reference as the client private key exactly as Paho does. */
static void use_the_reference_the_way_paho_does(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  tls_options_for_token(&tls);

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_OK);

  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  assert_non_null(ctx);
  int rc = SSL_CTX_use_PrivateKey_file(ctx, path, SSL_FILETYPE_PEM);
  if (rc != 1)
  {
    ERR_print_errors_fp(stderr);
  }
  assert_int_equal(rc, 1);
  SSL_CTX_free(ctx);

  az_iot_paho_key_custody_release(&s);
}

/* Preparing twice in a row -- what a reconnect does -- leaves exactly one
 * reference file behind, not one per attempt. */
static void re_preparing_replaces_the_reference(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  tls_options_for_token(&tls);

  const char* first = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &first), AZ_IOT_OK);
  char kept[512];
  int kept_len = snprintf(kept, sizeof(kept), "%s", first);
  assert_true(kept_len > 0 && (size_t)kept_len < sizeof(kept));

  az_iot_paho_key_custody_release(&s);
  const char* second = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &second), AZ_IOT_OK);
  assert_non_null(second);

  FILE* stale = fopen(kept, "rb");
  assert_null(stale);
  az_iot_paho_key_custody_release(&s);
}

/* A key that resolves through the provider but is named by a URI the adapter
 * has no standard reference form for: the reference PEM itself, reached over
 * file:. There is then nothing to hand the TLS stack, and the adapter says so
 * instead of connecting without a key. */
static void a_key_with_no_expressible_reference_is_refused(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  tls_options_for_token(&tls);

  /* Take a copy of a reference the adapter produced, so this case does not
   * hard-code the provider's PEM format. */
  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_OK);
  size_t len = 0;
  char* pem = read_file(path, &len);
  az_iot_paho_key_custody_release(&s);

  char copy_path[512];
  const char* dir = getenv("TMPDIR");
  if (!dir || dir[0] == '\0')
  {
    dir = "/tmp";
  }
  int copy_len = snprintf(copy_path, sizeof(copy_path), "%s/az-iot-ut-keyref-copy.pem", dir);
  assert_true(copy_len > 0 && (size_t)copy_len < sizeof(copy_path));
  FILE* f = fopen(copy_path, "w");
  assert_non_null(f);
  assert_int_equal(fwrite(pem, 1, len, f), len);
  fclose(f);
  free(pem);

  char file_uri[600];
  int file_uri_len = snprintf(file_uri, sizeof(file_uri), "file:%s", copy_path);
  assert_true(file_uri_len > 0 && (size_t)file_uri_len < sizeof(file_uri));
  tls.client_key_uri = file_uri;

  const char* unused = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &unused), AZ_IOT_ERR_TLS);
  az_iot_paho_key_custody_release(&s);
  remove(copy_path);
}

/* Does the reference file contain `needle` anywhere in the bytes it actually
 * encodes?
 *
 * Searching the file text is not enough and is the trap this replaced: a PEM
 * body is base64, so a URI embedded in the DER never appears literally and a
 * plain strstr() reports "clean" for a file that does carry the secret. The
 * body is decoded first, so the answer holds whichever branch produced the
 * reference. */
static bool reference_bytes_contain(const char* path, const char* needle)
{
  size_t len = 0;
  char* pem = read_file(path, &len);

  BIO* bio = BIO_new_mem_buf(pem, (int)len);
  assert_non_null(bio);

  char* name = NULL;
  char* header = NULL;
  unsigned char* der = NULL;
  long der_len = 0;
  bool found = false;

  if (PEM_read_bio(bio, &name, &header, &der, &der_len) == 1)
  {
    size_t nlen = strlen(needle);
    for (long off = 0; der_len >= (long)nlen && off + (long)nlen <= der_len; ++off)
    {
      if (memcmp(der + off, needle, nlen) == 0)
      {
        found = true;
        break;
      }
    }
  }
  else
  {
    /* Not PEM at all: fall back to the raw bytes rather than silently passing. */
    found = strstr(pem, needle) != NULL;
  }

  OPENSSL_free(name);
  OPENSSL_free(header);
  OPENSSL_free(der);
  BIO_free(bio);
  free(pem);
  return found;
}

/* The PIN itself must never end up in the reference file, but naming where the
 * PIN lives must still work -- otherwise a PIN-protected token could not be
 * expressed as a reference at all.
 *
 * This matters on the fallback branch, the one that embeds the URI verbatim.
 * `pin-value` puts the secret in a file that outlives the call, which is the
 * outcome this module exists to prevent, so it is refused. `pin-source` only
 * names a file the provider reads, and the reference has to stay independently
 * loadable -- OpenSSL logs in to the token when it later resolves the file --
 * so it is allowed.
 *
 * Refusal rather than silently stripping pin-value: a reference with the PIN
 * removed would fail to log in later anyway, so failing here names the reason
 * while it is still known. */
static void a_uri_with_an_inline_pin_is_refused(void** state)
{
  (void)state;
  /* A clean base, so appending a query cannot produce a second '?'.
   *
   * Truncation is checked rather than assumed: a silently shortened URI names a
   * different object, so the case would still pass or fail while asserting
   * something other than what it says. */
  char base[1024];
  int base_len = snprintf(base, sizeof(base), "%s", g_key_uri);
  assert_true(base_len > 0 && (size_t)base_len < sizeof(base));
  char* q = strchr(base, '?');
  if (q)
  {
    *q = '\0';
  }

  char uri[1200];
  int uri_len = snprintf(uri, sizeof(uri), "%s?pin-value=1234", base);
  assert_true(uri_len > 0 && (size_t)uri_len < sizeof(uri));

  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  tls_options_for_token(&tls);
  tls.client_key_uri = uri;

  const char* path = NULL;
  az_iot_result r = az_iot_paho_key_custody_prepare(&s, &tls, &path);

  /* A provider that encodes its own reference never embeds the URI, so it is
   * allowed to succeed -- but then the PIN must not be in what it wrote. */
  if (r == AZ_IOT_OK)
  {
    assert_non_null(s.key_ref_path);
    assert_false(reference_bytes_contain(s.key_ref_path, "pin-value"));
    assert_false(reference_bytes_contain(s.key_ref_path, "1234"));
  }
  else
  {
    assert_int_equal(r, AZ_IOT_ERR_TLS);
    assert_null(s.key_ref_path);
  }
  az_iot_paho_key_custody_release(&s);
}

/* The other half: a token whose PIN is named by pin-source resolves, and the
 * reference written for it carries the pin-source pointer but no PIN. This is
 * how the provisioning script spells the URI, so a regression that refused
 * pin-source too would make every PIN-protected token unusable. */
static void a_uri_naming_a_pin_source_is_accepted(void** state)
{
  (void)state;
  if (strstr(g_key_uri, "pin-source") == NULL)
  {
    /* The token was supplied without a pin-source; nothing to assert. */
    return;
  }

  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  tls_options_for_token(&tls);

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_OK);
  assert_non_null(s.key_ref_path);

  assert_false(reference_bytes_contain(s.key_ref_path, "pin-value"));

  az_iot_paho_key_custody_release(&s);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_token_key_yields_a_reference_not_a_key),
    cmocka_unit_test(use_the_reference_the_way_paho_does),
    cmocka_unit_test(re_preparing_replaces_the_reference),
    cmocka_unit_test(a_key_with_no_expressible_reference_is_refused),
    cmocka_unit_test(a_uri_with_an_inline_pin_is_refused),
    cmocka_unit_test(a_uri_naming_a_pin_source_is_accepted),
  };
  return cmocka_run_group_tests(tests, group_setup, NULL);
}
