// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Paho adapter: non-extractable key custody (D8).
 *
 * Every case here is deterministic against a stock OpenSSL: the "default"
 * provider always loads, so the resolve and extractable-key branches can be
 * driven without an HSM. The one path that genuinely needs PKCS#11 hardware --
 * a provider returning a key it will not export -- is exercised by the
 * SoftHSM2-backed e2e leg, not here. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "az_iot_paho_key_custody.h"

/* A software EC P-256 key. Test material only -- it authenticates nothing and
 * is here precisely so the adapter can be caught refusing to copy an
 * EXTRACTABLE key into the reference file. */
static const char k_software_key_pem[]
    = "-----BEGIN PRIVATE KEY-----\n"
      "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgHCsZic6vyzA2wQBw\n"
      "4MZtYJTGSoHZ8X/WDcllwOLofSGhRANCAASVEx1J3/gQ0Jv1ji3ltb4pBmgeCisb\n"
      "MXtVqn8vFkldMQtreR/++C6c5D69CKxYhPOXVAjpScUWfAy9Kn3ugcGf\n"
      "-----END PRIVATE KEY-----\n";

static char g_key_path[512];
static char g_key_uri[600];

/* The temporary directory, spelled the way each platform spells it. */
static const char* temp_dir(void)
{
  static const char* const names[] = { "TMPDIR", "TEMP", "TMP" };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
  {
    const char* v = getenv(names[i]);
    if (v && v[0] != '\0')
    {
      return v;
    }
  }
#if defined(_WIN32)
  return ".";
#else
  return "/tmp";
#endif
}

/* The adapter distinguishes its refusals in the LOG, not in the result code --
 * they are all AZ_IOT_ERR_TLS to the caller. Capturing the log is therefore the
 * only way a case can assert it took the branch it meant to, rather than
 * passing because some earlier step failed for an unrelated reason. */
static char g_last_error[1024];

static void capture_sink(
    void* user_ctx,
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* msg)
{
  (void)component;
  (void)user_ctx;
  (void)file;
  (void)line;
  if (level == AZ_IOT_LOG_LEVEL_ERROR && msg != NULL)
  {
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
  }
}

static void capture_reset(void) { g_last_error[0] = '\0'; }

static int group_setup(void** state)
{
  (void)state;
  snprintf(g_key_path, sizeof(g_key_path), "%s/az-iot-ut-softkey.pem", temp_dir());
  FILE* f = fopen(g_key_path, "w");
  assert_non_null(f);
  assert_true(fwrite(k_software_key_pem, 1, sizeof(k_software_key_pem) - 1, f) > 0);
  fclose(f);

  /* A plain path, not a "file:" URI: OSSL_STORE falls back to the file scheme
   * for a scheme-less string, and that sidesteps the differences between what
   * counts as a valid file URI on Windows and elsewhere. */
  snprintf(g_key_uri, sizeof(g_key_uri), "%s", g_key_path);

  static az_iot_log_sink sink;
  sink.sink = capture_sink;
  sink.user_ctx = NULL;
  sink.min_level = AZ_IOT_LOG_LEVEL_ERROR;
  az_iot_log_set_global_sink(&sink);
  return 0;
}

static int group_teardown(void** state)
{
  (void)state;
  az_iot_log_set_global_sink(NULL);
  remove(g_key_path);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* requested() -- the input to the adapter's use_ssl decision                */
/* ------------------------------------------------------------------------- */

static az_iot_result sign_stub(
    void* ctx,
    const uint8_t* digest,
    size_t digest_len,
    uint8_t* out_sig,
    size_t out_sig_cap,
    size_t* out_sig_len)
{
  (void)ctx;
  (void)digest;
  (void)digest_len;
  (void)out_sig;
  (void)out_sig_cap;
  (void)out_sig_len;
  return AZ_IOT_OK;
}

/* A URI-only or sign-only credential is a TLS credential. If this said
 * otherwise the adapter would fall through to a plaintext connect for exactly
 * the credential that most needs TLS. */
static void a_key_reference_counts_as_tls_material(void** state)
{
  (void)state;
  assert_false(az_iot_paho_key_custody_requested(NULL));

  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  assert_false(az_iot_paho_key_custody_requested(&tls));

  tls.client_key_path = "/dev/null/key.pem";
  assert_false(az_iot_paho_key_custody_requested(&tls));

  memset(&tls, 0, sizeof(tls));
  tls.client_key_uri = "pkcs11:object=k";
  assert_true(az_iot_paho_key_custody_requested(&tls));

  memset(&tls, 0, sizeof(tls));
  tls.sign = sign_stub;
  assert_true(az_iot_paho_key_custody_requested(&tls));
}

/* ------------------------------------------------------------------------- */
/* prepare() / release()                                                     */
/* ------------------------------------------------------------------------- */

static void null_arguments_are_rejected(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  const char* path = NULL;

  assert_int_equal(az_iot_paho_key_custody_prepare(NULL, &tls, &path), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, NULL, &path), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, NULL), AZ_IOT_ERR_INVALID_ARG);

  /* release() tolerates a NULL and a never-prepared state. */
  az_iot_paho_key_custody_release(NULL);
  az_iot_paho_key_custody_release(&s);
}

/* No custody requested: the caller's own key path passes through untouched,
 * which is every ordinary PEM/file credential. */
static void an_ordinary_key_path_passes_through(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  tls.client_key_path = "/dev/null/key.pem";

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_OK);
  assert_string_equal(path, "/dev/null/key.pem");
  az_iot_paho_key_custody_release(&s);
}

#if defined(AZ_IOT_PAHO_KEY_CUSTODY)

/* Paho's SSL options expose no SSL_CTX and no key callback, so a sign() hook
 * with no key reference cannot be honoured here. Saying so is the point: the
 * alternative is a handshake with no client key at all. */
static void a_sign_hook_without_a_key_uri_is_not_supported(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  tls.sign = sign_stub;

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_paho_key_custody_release(&s);
}

static void a_key_uri_without_an_engine_id_is_not_supported(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  tls.client_key_uri = "pkcs11:object=device-key;type=private";

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_paho_key_custody_release(&s);
}

static void an_unknown_engine_id_is_not_supported(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  tls.client_key_uri = "pkcs11:object=device-key;type=private";
  tls.crypto_engine_id = "no-such-provider-exists";

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_paho_key_custody_release(&s);
}

/* The provider loads but the URI names nothing. This is the diagnostic the
 * change exists for: it fails here, naming the URI, instead of inside the
 * handshake. */
static void an_unresolvable_key_uri_fails_before_the_handshake(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  tls.client_key_uri = "file:/nonexistent/az-iot-ut/no-such-key.pem";
  tls.crypto_engine_id = "default";

  const char* path = NULL;
  capture_reset();
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_ERR_TLS);
  assert_non_null(strstr(g_last_error, "could not resolve client_key_uri"));
  az_iot_paho_key_custody_release(&s);
}

/* A URI that resolves to an ordinary, exportable private key must be refused.
 * Writing it to the reference file would put private key material on disk from
 * the one code path whose entire purpose is that it never does. */
static void an_extractable_key_is_refused(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  tls.client_key_uri = g_key_uri;
  tls.crypto_engine_id = "default";

  const char* path = NULL;
  capture_reset();
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_ERR_TLS);
  /* The key really was resolved and then refused -- not merely unresolvable. */
  assert_non_null(strstr(g_last_error, "EXTRACTABLE"));
  /* Nothing was left behind. */
  assert_null(s.key_ref_path);
  az_iot_paho_key_custody_release(&s);
}

/* A URI longer than the log buffer is truncated rather than overflowing it, and
 * a URI carrying a PIN is not echoed whole into a log line. Neither is
 * observable in the result, so the assertion is the refusal still arriving
 * intact -- the redaction runs on the way there. */
static void an_over_long_key_uri_is_handled(void** state)
{
  (void)state;
  char long_uri[512];
  size_t n = 0;
  n += (size_t)snprintf(long_uri + n, sizeof(long_uri) - n, "pkcs11:token=ut");
  while (n < sizeof(long_uri) - 32)
  {
    n += (size_t)snprintf(long_uri + n, sizeof(long_uri) - n, ";object=aaaaaaaaaaaaaaaa");
  }
  snprintf(long_uri + n, sizeof(long_uri) - n, "?pin-value=1234");

  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  tls.client_key_uri = long_uri;
  tls.crypto_engine_id = "no-such-provider-exists";

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_paho_key_custody_release(&s);
}

/* The extractability gate is what stops a real private key being written to
 * disk, so it must find the banner wherever it sits -- not only at offset 0.
 * Nothing guarantees an encoder puts the BEGIN line first: a leading newline or
 * a textual preamble ahead of the block would hide a genuine key from a check
 * anchored at the start, and the result is the file this module exists to never
 * write.
 *
 * Driven directly because no provider emits these shapes on request. */
static void an_extractable_key_is_detected_anywhere_in_the_buffer(void** state)
{
  (void)state;
  static const char* const k_hiding_places[] = {
    "\n-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n",
    "\r\n\r\n-----BEGIN RSA PRIVATE KEY-----\nAAAA\n",
    "Bag Attributes: friendlyName=x\n-----BEGIN EC PRIVATE KEY-----\nAAAA\n",
    "   -----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n",
    /* Parenthesised: the concatenation is deliberate (one buffer holding a
       certificate block followed by a key), and clang warns on an unparenthesised
       adjacent pair inside an array initialiser. */
    ("-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n"
     "-----BEGIN DSA PRIVATE KEY-----\nAAAA\n"),
  };
  for (size_t i = 0; i < sizeof(k_hiding_places) / sizeof(k_hiding_places[0]); ++i)
  {
    assert_true(az_iot_paho_key_custody_pem_carries_private_key(
        k_hiding_places[i], strlen(k_hiding_places[i])));
  }

  /* And the references this path exists to ACCEPT are not mistaken for keys.
   * "TSS2 PRIVATE KEY" is the tpm2 reference label and contains the words
   * "PRIVATE KEY", so a looser rule than full-banner matching would reject it
   * and break the feature. */
  static const char* const k_references[] = {
    "-----BEGIN PKCS#11 PROVIDER URI-----\nAAAA\n-----END PKCS#11 PROVIDER URI-----\n",
    "-----BEGIN TSS2 PRIVATE KEY-----\nAAAA\n-----END TSS2 PRIVATE KEY-----\n",
    "",
  };
  for (size_t i = 0; i < sizeof(k_references) / sizeof(k_references[0]); ++i)
  {
    assert_false(
        az_iot_paho_key_custody_pem_carries_private_key(k_references[i], strlen(k_references[i])));
  }

  /* Bounded by len, not by a NUL: a banner past the end must not be read. */
  static const char k_past_end[] = "-----BEGIN PRIVATE KEY-----";
  assert_false(az_iot_paho_key_custody_pem_carries_private_key(k_past_end, 10));
  assert_false(az_iot_paho_key_custody_pem_carries_private_key(NULL, 0));
}

/* release() is idempotent and re-preparable: the adapter calls it at the top of
 * every connect so a reconnect does not accumulate reference files. */
static void release_is_idempotent(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  tls.client_key_uri = g_key_uri;
  tls.crypto_engine_id = "default";

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_ERR_TLS);
  az_iot_paho_key_custody_release(&s);
  az_iot_paho_key_custody_release(&s);
  assert_null(s.key_ref_path);
}

/* release() unlinks the file, it does not merely forget the path. Reaching a
 * state that HAS a path needs a token, so the state is built by hand here --
 * which is all release() looks at. Without this, the only cases that reach
 * release() are ones whose prepare() failed before creating anything, so the
 * unlink never runs and a release() that leaked the file on disk would still
 * pass every test in this file. */
static void release_removes_the_reference_file(void** state)
{
  (void)state;
  char path[512];
  snprintf(path, sizeof(path), "%s/az-iot-ut-keyref-release.pem", temp_dir());
  FILE* f = fopen(path, "w");
  assert_non_null(f);
  fputs("-----BEGIN PKCS#11 PROVIDER URI-----\n", f);
  fclose(f);

  size_t n = strlen(path);
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  /* Freed by release(), so it has to come from the same allocator. */
  s.key_ref_path = (char*)malloc(n + 1);
  assert_non_null(s.key_ref_path);
  memcpy(s.key_ref_path, path, n + 1);

  az_iot_paho_key_custody_release(&s);

  assert_null(s.key_ref_path);
  FILE* gone = fopen(path, "rb");
  if (gone)
  {
    fclose(gone);
    remove(path);
    fail_msg("release() left the key reference file behind: %s", path);
  }
}

#else /* !AZ_IOT_PAHO_KEY_CUSTODY */

/* Built without custody support, a key reference must be refused rather than
 * quietly ignored -- ignoring it connects with no client key. */
static void custody_without_support_is_refused(void** state)
{
  (void)state;
  az_iot_paho_key_custody s;
  memset(&s, 0, sizeof(s));
  az_iot_mqtt_tls_options tls;
  memset(&tls, 0, sizeof(tls));
  tls.client_key_uri = "pkcs11:object=device-key;type=private";
  tls.crypto_engine_id = "pkcs11";

  const char* path = NULL;
  assert_int_equal(az_iot_paho_key_custody_prepare(&s, &tls, &path), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_paho_key_custody_release(&s);
}

#endif /* AZ_IOT_PAHO_KEY_CUSTODY */

/* ------------------------------------------------------------------------- */
/* the adapter refuses the connect rather than proceeding without a key      */
/* ------------------------------------------------------------------------- */

static void connect_with_an_unusable_key_reference_fails(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_paho_factory_create_v3_1_1();
  assert_non_null(f);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);

  az_iot_mqtt_connect_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "broker.invalid";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.tls.client_cert_path = "/dev/null/device.pem";
  opts.tls.client_key_uri = "pkcs11:object=device-key;type=private";
  opts.tls.crypto_engine_id = "no-such-provider-exists";

  assert_int_equal(c->iface->connect(c, &opts), AZ_IOT_ERR_NOT_SUPPORTED);

  c->iface->destroy(c);
  az_iot_paho_factory_destroy(f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_key_reference_counts_as_tls_material),
    cmocka_unit_test(null_arguments_are_rejected),
    cmocka_unit_test(an_ordinary_key_path_passes_through),
#if defined(AZ_IOT_PAHO_KEY_CUSTODY)
    cmocka_unit_test(a_sign_hook_without_a_key_uri_is_not_supported),
    cmocka_unit_test(a_key_uri_without_an_engine_id_is_not_supported),
    cmocka_unit_test(an_unknown_engine_id_is_not_supported),
    cmocka_unit_test(an_unresolvable_key_uri_fails_before_the_handshake),
    cmocka_unit_test(an_extractable_key_is_refused),
    cmocka_unit_test(an_extractable_key_is_detected_anywhere_in_the_buffer),
    cmocka_unit_test(an_over_long_key_uri_is_handled),
    cmocka_unit_test(release_is_idempotent),
    cmocka_unit_test(release_removes_the_reference_file),
#else
    cmocka_unit_test(custody_without_support_is_refused),
#endif
    cmocka_unit_test(connect_with_an_unusable_key_reference_fails),
  };
  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
