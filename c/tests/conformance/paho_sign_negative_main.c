// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Negative control for the sign-route custody case.
 *
 * No adapter in this repository implements the sign route: Paho exposes no TLS
 * key callback, so az_iot_paho_key_custody refuses tls.sign with
 * AZ_IOT_ERR_NOT_SUPPORTED. The positive case
 * key_custody_sign_hook_completes_a_tls_handshake therefore cannot run in CI,
 * and a case that never runs anywhere is a case that can be silently neutered
 * -- wire the hook to nothing, drop the assertion, stop dispatching it -- with
 * every pipeline still green. That is the regression this guards.
 *
 * It declares _SIGN, supplies a sign hook and a certificate, and requires the
 * suite to FAIL with exactly one failing case, which must be the sign one. So:
 *
 *   - the case is dispatched when the capability is declared and material
 *     supplied (a skipped case would leave the run green),
 *   - the case actually drives tls.sign, evidenced by the SPECIFIC failure:
 *     connect() itself must return AZ_IOT_ERR_NOT_SUPPORTED, which is how the
 *     adapter refuses this route. Merely requiring the case to fail is not
 *     enough -- strip tls.sign out of it and the connect then fails later, on
 *     the handshake, for having a certificate and no key at all. Measured: the
 *     wired case fails at the connect assertion, the stripped one at the
 *     subsequent wait. Only the first is evidence about the sign route,
 *   - nothing else broke (any other failing case fails this control too, rather
 *     than being mistaken for the expected one),
 *   - the hook was never actually called: if it were, the route was accepted,
 *     not refused, and this control's premise no longer holds.
 *
 * Runs both suite kinds, and in both adapter build modes: without
 * AZ_IOT_PAHO_KEY_CUSTODY the adapter still refuses a sign hook with the same
 * result, so there is no configuration in which this exits green without having
 * dispatched the case.
 *
 * Needs no PKCS#11 token: the certificate is generated here and the hook is a
 * stub, because the adapter refuses the route before either is used. It runs in
 * a directory of its own, because a case that fails never reaches its own
 * teardown.
 *
 * WHEN AN ADAPTER GAINS SIGN SUPPORT this control must be deleted, not
 * adjusted: its premise is that the bundled adapter cannot honour the route.
 * The positive case then runs for real and proves more than this ever could.
 */
#include "../conformance/az_iot_conformance.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

#include <io.h>
#include <process.h>
#define az_iot_getpid _getpid
#define az_iot_dup _dup
#define az_iot_dup2 _dup2
#define az_iot_fileno _fileno
#define az_iot_close _close
#include <direct.h>
#define az_iot_mkdir(p) _mkdir(p)
#define az_iot_rmdir _rmdir
#define az_iot_chdir _chdir
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define az_iot_getpid getpid
#define az_iot_mkdir(p) mkdir((p), 0700)
#define az_iot_rmdir rmdir
#define az_iot_chdir chdir
#define az_iot_dup dup
#define az_iot_dup2 dup2
#define az_iot_fileno fileno
#define az_iot_close close
#endif

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

/* The temporary directory, spelled the way each platform spells it. Windows
 * sets TEMP/TMP and not TMPDIR, and this harness builds there. */
static const char* temp_dir(void)
{
  static const char* const names[] = { "TMPDIR", "TEMP", "TMP" };
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
  {
    const char* v = getenv(names[i]);
    if (v != NULL && v[0] != '\0')
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

/* Delete the certificates the suite leaves behind in the working directory.
 *
 * The case this control expects to fail aborts at its first assertion, so the
 * teardown after it -- which removes the CA it wrote -- never runs. That is
 * true of any failing case for any adapter, and not something to fix from
 * here, so the control runs inside a directory of its own and clears it. */
static void sweep_working_directory(void)
{
#ifdef _WIN32
  WIN32_FIND_DATAA found;
  HANDLE h = FindFirstFileA("az_iot_conf_pem_*", &found);
  if (h == INVALID_HANDLE_VALUE)
  {
    return;
  }
  do
  {
    remove(found.cFileName);
  } while (FindNextFileA(h, &found));
  FindClose(h);
#else
  DIR* d = opendir(".");
  if (d == NULL)
  {
    return;
  }
  const struct dirent* e;
  while ((e = readdir(d)) != NULL)
  {
    if (strncmp(e->d_name, "az_iot_conf_pem_", strlen("az_iot_conf_pem_")) == 0)
    {
      remove(e->d_name);
    }
  }
  closedir(d);
#endif
}

#define EXPECTED_FAILING_CASE "key_custody_sign_hook_completes_a_tls_handshake"

/* Set if the adapter ever calls the hook. It must not: this control's whole
 * claim is that the route is refused, so a run where the hook was invoked
 * proves the opposite and is rejected below. */
static int g_sign_called = 0;

/* Never called, and deliberately NOT returning AZ_IOT_ERR_NOT_SUPPORTED.
 *
 * That is the code this control turns on, so returning it here would make the
 * evidence ambiguous: an adapter that started calling the hook and propagated
 * its result would produce exactly the refusal the control is looking for and
 * be certified as refusing the route it had in fact accepted. The distinct code
 * plus the flag above remove that reading entirely. */
static az_iot_result negative_sign_stub(
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
  g_sign_called = 1;
  return AZ_IOT_ERR_INTERNAL;
}

/* A throwaway self-signed certificate, so the run carries real material rather
 * than a path that happens not to be read. */
static int write_self_signed_cert(const char* path)
{
  int ok = 0;
  EVP_PKEY* pkey = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  X509* x509 = (pkey != NULL) ? X509_new() : NULL;
  FILE* out = NULL;

  if (x509 == NULL)
  {
    goto done;
  }
  if (!ASN1_INTEGER_set(X509_get_serialNumber(x509), 1) || !X509_set_version(x509, 2)
      || X509_gmtime_adj(X509_get_notBefore(x509), 0) == NULL
      || X509_gmtime_adj(X509_get_notAfter(x509), 3600) == NULL || !X509_set_pubkey(x509, pkey))
  {
    goto done;
  }

  X509_NAME* name = X509_get_subject_name(x509);
  if (name == NULL
      || !X509_NAME_add_entry_by_txt(
          name, "CN", MBSTRING_ASC, (const unsigned char*)"az-iot-sign-negative", -1, -1, 0)
      || !X509_set_issuer_name(x509, name) || !X509_sign(x509, pkey, EVP_sha256()))
  {
    goto done;
  }

  out = fopen(path, "wb");
  if (out == NULL)
  {
    goto done;
  }
  ok = PEM_write_X509(out, x509);

done:
  if (out != NULL)
  {
    fclose(out);
  }
  X509_free(x509);
  EVP_PKEY_free(pkey);
  return ok ? 0 : -1;
}

/* Count of "[  FAILED  ] <name>" lines, and whether the expected case is among
 * them. cmocka prints that summary line once per failing case and repeats it in
 * the trailing list, so names are counted, not lines. */
static int scan_output(
    const char* path,
    int* out_saw_expected,
    int* out_other_failures,
    int* out_saw_refusal)
{
  char line[512];
  char refusal[64];
  FILE* f = fopen(path, "r");
  if (f == NULL)
  {
    return -1;
  }

  /* How cmocka renders the failed connect assertion in the sign case. Built
   * from the enum rather than hard-coded, so renumbering the results cannot
   * quietly turn this into a check for the wrong code. */
  snprintf(refusal, sizeof(refusal), "%#x != 0", (unsigned)AZ_IOT_ERR_NOT_SUPPORTED);

  *out_saw_expected = 0;
  *out_other_failures = 0;
  *out_saw_refusal = 0;

  while (fgets(line, (int)sizeof(line), f) != NULL)
  {
    if (strstr(line, refusal) != NULL)
    {
      *out_saw_refusal = 1;
    }

    const char* marker = strstr(line, "[  FAILED  ] ");
    if (marker == NULL)
    {
      continue;
    }
    marker += strlen("[  FAILED  ] ");

    /* The whole name, not a substring: a different case whose name merely
     * contains this one would otherwise read as the required dispatch. */
    size_t name_len = 0;
    while (marker[name_len] != '\0' && marker[name_len] != '\n' && marker[name_len] != '\r'
           && marker[name_len] != ' ' && marker[name_len] != '\t')
    {
      ++name_len;
    }

    if (name_len == strlen(EXPECTED_FAILING_CASE)
        && strncmp(marker, EXPECTED_FAILING_CASE, name_len) == 0)
    {
      *out_saw_expected = 1;
    }
    else if (strstr(marker, "test(s), listed below") == NULL)
    {
      /* The group summary line ("N test(s), listed below") is not a case. */
      *out_other_failures = 1;
      fprintf(stderr, "sign-negative: unexpected failing case: %s", marker);
    }
  }

  fclose(f);
  return 0;
}

/* One controlled run of `suite_kind`. Returns 0 when the sign case behaved as
 * this control requires. */
static int run_control(
    az_iot_conformance_suite suite_kind,
    const char* cert_path,
    const char* label)
{
  az_iot_mqtt_factory* f = (suite_kind == AZ_IOT_CONFORMANCE_SUITE_V5)
      ? az_iot_paho_factory_create_v5()
      : az_iot_paho_factory_create_v3_1_1();
  if (f == NULL)
  {
    fprintf(stderr, "sign-negative[%s]: could not create the factory\n", label);
    return 1;
  }

  az_iot_conformance_options opts = { 0 };
  opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_SIGN;
  opts.sign = negative_sign_stub;
  opts.sign_ctx = NULL;
  opts.client_cert_path = cert_path;

  char log_path[64];
  snprintf(log_path, sizeof(log_path), "suite-%s.log", label);

  g_sign_called = 0;

  /* The suite's own output is the evidence, so it is captured rather than shown.
   * BOTH streams: cmocka prints the case list to stdout and the failed
   * assertion -- which carries the result code this control turns on -- to
   * stderr. The descriptors are saved and put back so the verdict below still
   * reaches the terminal. */
  FILE* log = fopen(log_path, "w+");
  int saved_out = (log != NULL) ? az_iot_dup(1) : -1;
  int saved_err = (log != NULL) ? az_iot_dup(2) : -1;
  if (log == NULL || saved_out < 0 || saved_err < 0 || az_iot_dup2(az_iot_fileno(log), 1) < 0
      || az_iot_dup2(az_iot_fileno(log), 2) < 0)
  {
    fprintf(stderr, "sign-negative[%s]: could not capture the suite's output\n", label);
    az_iot_paho_factory_destroy(f);
    return 1;
  }

  int suite_rc = az_iot_conformance_run_with_options(suite_kind, f, &opts);

  fflush(stdout);
  fflush(stderr);
  az_iot_dup2(saved_out, 1);
  az_iot_dup2(saved_err, 2);
  az_iot_close(saved_out);
  az_iot_close(saved_err);
  fclose(log);
  az_iot_paho_factory_destroy(f);

  int saw_expected = 0;
  int other_failures = 0;
  int saw_refusal = 0;
  int scan_rc = scan_output(log_path, &saw_expected, &other_failures, &saw_refusal);

  int rc = 0;
  if (g_sign_called)
  {
    fprintf(
        stderr,
        "sign-negative[%s]: the adapter CALLED the sign hook. It is no longer refusing the route,"
        " so this control is asserting something that is no longer true: delete it and let the"
        " positive case prove the route instead.\n",
        label);
    rc = 1;
  }
  else if (scan_rc != 0)
  {
    fprintf(stderr, "sign-negative[%s]: could not read the captured output\n", label);
    rc = 1;
  }
  else if (suite_rc == 0)
  {
    fprintf(
        stderr,
        "sign-negative[%s]: the suite PASSED while declaring"
        " AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_SIGN against an adapter that cannot honour tls.sign."
        " The sign case is not being run, or no longer drives the route.\n",
        label);
    rc = 1;
  }
  else if (!saw_expected)
  {
    fprintf(
        stderr,
        "sign-negative[%s]: the suite failed, but not on " EXPECTED_FAILING_CASE ". The sign case"
        " was not dispatched.\n",
        label);
    rc = 1;
  }
  else if (!saw_refusal)
  {
    fprintf(
        stderr,
        "sign-negative[%s]: " EXPECTED_FAILING_CASE " failed, but not by the adapter refusing the"
        " route -- connect() did not return AZ_IOT_ERR_NOT_SUPPORTED. The case is failing for some"
        " other reason, so it is no longer evidence that it drives tls.sign.\n",
        label);
    rc = 1;
  }
  else if (other_failures)
  {
    fprintf(
        stderr,
        "sign-negative[%s]: " EXPECTED_FAILING_CASE " failed as required, but so did other cases,"
        " so this run is not evidence about the sign route. Fix those first.\n",
        label);
    rc = 1;
  }
  else
  {
    fprintf(
        stderr,
        "sign-negative[%s]: OK -- " EXPECTED_FAILING_CASE " ran and failed with"
        " AZ_IOT_ERR_NOT_SUPPORTED, and nothing else failed, so the sign case is dispatched and"
        " does drive tls.sign.\n",
        label);
  }

  remove(log_path);
  return rc;
}

int main(void)
{
  /* Both suite kinds. The dispatch is shared, but only a run proves that: a
   * change that filtered the case out of the v5 list alone would leave
   * az_iot_conformance_paho_v5 green, since that harness declares the URI route
   * only. */
  static const struct
  {
    az_iot_conformance_suite kind;
    const char* label;
  } suites[] = { { AZ_IOT_CONFORMANCE_SUITE_V3_1_1, "v3" }, { AZ_IOT_CONFORMANCE_SUITE_V5, "v5" } };

  char work_dir[512];
  snprintf(
      work_dir, sizeof(work_dir), "%s/az-iot-sign-negative-%ld", temp_dir(), (long)az_iot_getpid());
  if (az_iot_mkdir(work_dir) != 0)
  {
    fprintf(stderr, "sign-negative: could not create the working directory %s\n", work_dir);
    return 1;
  }
  if (az_iot_chdir(work_dir) != 0)
  {
    fprintf(stderr, "sign-negative: could not enter the working directory %s\n", work_dir);
    az_iot_rmdir(work_dir);
    return 1;
  }

  int rc = 0;
  const char* cert_path = "client.pem";
  if (write_self_signed_cert(cert_path) != 0)
  {
    fprintf(stderr, "sign-negative: could not generate the client certificate\n");
    rc = 1;
  }
  else
  {
    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); ++i)
    {
      rc |= run_control(suites[i].kind, cert_path, suites[i].label);
    }
  }

  remove(cert_path);
  sweep_working_directory();
  if (az_iot_chdir("..") != 0)
  {
    /* Only cleanup is lost, so the verdict still stands. */
    fprintf(stderr, "sign-negative: could not leave %s; it will need removing by hand\n", work_dir);
    return rc;
  }
  az_iot_rmdir(work_dir);
  return rc;
}
