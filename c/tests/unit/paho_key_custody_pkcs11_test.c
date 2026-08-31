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

#include <openssl/err.h>
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
  snprintf(kept, sizeof(kept), "%s", first);

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
  snprintf(copy_path, sizeof(copy_path), "%s/az-iot-ut-keyref-copy.pem", dir);
  FILE* f = fopen(copy_path, "w");
  assert_non_null(f);
  assert_int_equal(fwrite(pem, 1, len, f), len);
  fclose(f);
  free(pem);

  char file_uri[600];
  snprintf(file_uri, sizeof(file_uri), "file:%s", copy_path);
  tls.client_key_uri = file_uri;

  const char* unused = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &unused), AZ_IOT_ERR_TLS);
  az_iot_paho_key_custody_release(&s);
  remove(copy_path);
}

/* A URI carrying the token PIN must never end up in the reference file. This is
 * the fallback branch -- the one that embeds the URI itself -- so a PIN in the
 * query would otherwise be persisted to disk for the life of the connection,
 * in the code path whose entire purpose is that the credential never lands
 * there.
 *
 * Refusal is the expected outcome, not a stripped URI: a reference with the PIN
 * removed would fail to log in later anyway, so failing here names the reason
 * while it is still known. A provider that emits its own reference form is
 * unaffected and takes the branch above.
 *
 * Both spellings RFC 7512 defines are covered. */
static void a_uri_carrying_the_pin_is_refused(void** state)
{
  (void)state;
  static const char* const k_pin_uris[] = { "?pin-value=1234", "?pin-source=file:/tmp/az-iot-pin" };

  for (size_t i = 0; i < sizeof(k_pin_uris) / sizeof(k_pin_uris[0]); ++i)
  {
    char uri[1024];
    snprintf(uri, sizeof(uri), "%s%s", g_key_uri, k_pin_uris[i]);

    az_iot_paho_key_custody s;
    memset(&s, 0, sizeof(s));
    az_iot_mqtt_tls_options tls;
    tls_options_for_token(&tls);
    tls.client_key_uri = uri;

    const char* path = NULL;
    az_iot_result r = az_iot_paho_key_custody_prepare(&s, &tls, &path);

    /* A provider that encodes its own reference never embeds the URI, so it is
     * allowed to succeed -- but then the PIN must not be in the file it wrote. */
    if (r == AZ_IOT_OK)
    {
      assert_non_null(s.key_ref_path);
      size_t len = 0;
      char* pem = read_file(s.key_ref_path, &len);
      assert_null(strstr(pem, "pin-value"));
      assert_null(strstr(pem, "pin-source"));
      assert_null(strstr(pem, "1234"));
      free(pem);
    }
    else
    {
      assert_int_equal(r, AZ_IOT_ERR_TLS);
      assert_null(s.key_ref_path);
    }
    az_iot_paho_key_custody_release(&s);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_token_key_yields_a_reference_not_a_key),
    cmocka_unit_test(use_the_reference_the_way_paho_does),
    cmocka_unit_test(re_preparing_replaces_the_reference),
    cmocka_unit_test(a_key_with_no_expressible_reference_is_refused),
    cmocka_unit_test(a_uri_carrying_the_pin_is_refused),
  };
  return cmocka_run_group_tests(tests, group_setup, NULL);
}
