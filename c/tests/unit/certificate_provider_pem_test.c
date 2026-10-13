// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Tests for the file/PEM-loader implementation of az_iot_certificate_provider. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_certificate_provider_pem.h"
#include "azure/iot/az_iot_result.h"

/* Synthetic PEM-shaped contents. The loader doesn't parse PEM, just slurps the
 * file verbatim, so any byte string round-trips. */
static const char k_cert_pem[]
    = "-----BEGIN CERTIFICATE-----\nMIIBkTCCATegAwIBA...==\n-----END CERTIFICATE-----\n";
static const char k_key_pem[]
    = "-----BEGIN EC PRIVATE KEY-----\nMHcCAQEEIE...==\n-----END EC PRIVATE KEY-----\n";
static const char k_ca_pem[]
    = "-----BEGIN CERTIFICATE-----\nMIIBmDCCAT6gAwIBA...==\n-----END CERTIFICATE-----\n";

#define PATH_BUF 256

typedef struct fixture
{
  char cert_path[PATH_BUF];
  char key_path[PATH_BUF];
  char ca_path[PATH_BUF];
} fixture;

/* Process-unique filename in the cwd, derived from PID + a monotonic counter
 * to avoid the tmpnam/tmpnam_s portability minefield. The cwd during ctest
 * runs is the test's binary directory, which is writable. */
static unsigned s_counter;

static void make_temp(char* out_path, const char* tag, const void* bytes, size_t n)
{
  /* Not inside the snprintf arguments: with _FORTIFY_SOURCE, snprintf may be a macro. */
#if defined(_WIN32)
  unsigned pid = (unsigned)_getpid();
#else
  unsigned pid = (unsigned)getpid();
#endif
  snprintf(out_path, PATH_BUF, "az_iot_pem_%u_%u_%s.pem", pid, ++s_counter, tag);
  FILE* f = fopen(out_path, "wb");
  assert_non_null(f);
  assert_int_equal(n, fwrite(bytes, 1, n, f));
  assert_int_equal(0, fclose(f));
}

static int setup_files(void** state)
{
  fixture* f = calloc(1, sizeof(*f));
  assert_non_null(f);
  make_temp(f->cert_path, "cert", k_cert_pem, sizeof(k_cert_pem) - 1);
  make_temp(f->key_path, "key", k_key_pem, sizeof(k_key_pem) - 1);
  make_temp(f->ca_path, "ca", k_ca_pem, sizeof(k_ca_pem) - 1);
  *state = f;
  return 0;
}

static int teardown_files(void** state)
{
  fixture* f = *state;
  if (!f)
  {
    return 0;
  }
  remove(f->cert_path);
  remove(f->key_path);
  remove(f->ca_path);
  free(f);
  return 0;
}

static void test_options_default_is_zeroed(void** state)
{
  (void)state;
  az_iot_certificate_provider_pem_options opts = az_iot_certificate_provider_pem_options_default();
  assert_null(opts.trusted_ca_pem_path);
  assert_null(opts.client_cert_pem_path);
  assert_null(opts.client_key_pem_path);
  assert_null(opts.client_key_password);
}

static void test_create_rejects_null(void** state)
{
  (void)state;
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_pem_init(&mgr, NULL));
  az_iot_certificate_provider_pem_options opts = { 0 };
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_pem_init(NULL, &opts));
}

static void test_create_rejects_missing_required_paths(void** state)
{
  (void)state;
  az_iot_certificate_provider_pem_options opts = { 0 };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_pem_init(&mgr, &opts));

  opts.client_cert_pem_path = "x";
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_pem_init(&mgr, &opts));

  opts.client_cert_pem_path = NULL;
  opts.client_key_pem_path = "x";
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_pem_init(&mgr, &opts));
}

static void test_create_fails_on_missing_file(void** state)
{
  (void)state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = "this-file-definitely-does-not-exist.pem",
    .client_key_pem_path = "this-file-definitely-does-not-exist.pem",
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_ERR_NOT_INITIALIZED, az_iot_certificate_provider_pem_init(&mgr, &opts));
}

static void test_load_returns_file_contents(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .trusted_ca_pem_path = f->ca_path,
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
    .client_key_password = "hunter2",
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));
  assert_non_null(mgr.base.vtable);

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));

  assert_non_null(mat.client_cert_pem);
  assert_string_equal(k_cert_pem, mat.client_cert_pem);
  assert_non_null(mat.client_key_pem);
  assert_string_equal(k_key_pem, mat.client_key_pem);
  assert_non_null(mat.trusted_ca_pem);
  assert_string_equal(k_ca_pem, mat.trusted_ca_pem);
  assert_non_null(mat.client_key_password);
  assert_string_equal("hunter2", mat.client_key_password);

  /* release() drops the key; a later load() reads it again. */
  mgr.base.vtable->release(&mgr.base, &mat);

  az_iot_certificate_material mat2;
  memset(&mat2, 0, sizeof(mat2));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat2));
  assert_string_equal(k_cert_pem, mat2.client_cert_pem);
  assert_string_equal(k_key_pem, mat2.client_key_pem);
  assert_string_equal("hunter2", mat2.client_key_password);
  mgr.base.vtable->release(&mgr.base, &mat2);

  az_iot_certificate_provider_pem_deinit(&mgr);
}

static void test_load_without_optional_fields(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path, .client_key_pem_path = f->key_path,
    /* trusted_ca_pem_path and client_key_password intentionally NULL */
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
  assert_null(mat.trusted_ca_pem);
  assert_null(mat.client_key_password);
  assert_non_null(mat.client_cert_pem);
  assert_non_null(mat.client_key_pem);

  az_iot_certificate_provider_pem_deinit(&mgr);
}

/* Loading through the vtable after the provider is gone. Keeping the pointer is
 * how a caller that cached the vtable would reach a destroyed provider. */
static void test_load_after_deinit_is_refused(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));
  const az_iot_certificate_provider_vtable* vt = mgr.base.vtable;
  assert_non_null(vt);

  az_iot_certificate_provider_pem_deinit(&mgr);

  /* Refused rather than handing back the freed buffers the material used to
   * point at. */
  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_ERR_NOT_INITIALIZED, vt->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
}

static void test_load_rejects_null_arguments(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, mgr.base.vtable->load(NULL, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, NULL));
  /* At any index; past index 0 a valid call is NOT_FOUND (one certificate). */
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, mgr.base.vtable->load(NULL, AZ_IOT_CRED_BOOTSTRAP, 1, &mat));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 1, NULL));
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 1, &mat));

  az_iot_certificate_provider_pem_deinit(&mgr);
}

/* This provider is a static file loader: it holds one identity and hands it
 * back for either role. The connect path is what decides to fall back to
 * BOOTSTRAP, so an operational request here must not fail. */
static void test_load_of_an_operational_credential_returns_the_static_material(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));

  az_iot_certificate_material boot;
  az_iot_certificate_material oper;
  memset(&boot, 0, sizeof(boot));
  memset(&oper, 0, sizeof(oper));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &boot));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_OPERATIONAL, 0, &oper));

  assert_string_equal(boot.client_cert_pem, oper.client_cert_pem);
  assert_string_equal(boot.client_key_pem, oper.client_key_pem);

  az_iot_certificate_provider_pem_deinit(&mgr);
}

/* The key is read on load(), not kept from init(). */
static void test_init_does_not_keep_the_key(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));
  assert_null(mgr.client_key);
  assert_int_equal(0, mgr.client_key_refs);
  az_iot_certificate_provider_pem_deinit(&mgr);
}

/* Each load() is a loan of one key buffer; the last release() frees it and
 * clears the material handed out. */
static void test_the_last_release_drops_the_key(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
    .client_key_password = "hunter2",
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));

  az_iot_certificate_material a;
  az_iot_certificate_material b;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &a));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_OPERATIONAL, 0, &b));
  assert_ptr_equal(a.client_key_pem, b.client_key_pem);
  assert_int_equal(2, mgr.client_key_refs);

  mgr.base.vtable->release(&mgr.base, &a);
  assert_null(a.client_key_pem);
  assert_null(a.client_key_password);
  assert_null(a.client_cert_pem);
  /* b still holds a loan. */
  assert_string_equal(k_key_pem, b.client_key_pem);
  assert_int_equal(1, mgr.client_key_refs);

  mgr.base.vtable->release(&mgr.base, &b);
  assert_null(mgr.client_key);
  assert_int_equal(0, mgr.client_key_refs);
  assert_null(b.client_key_pem);
  /* The password is kept for the next load(). */
  assert_string_equal("hunter2", mgr.key_password);

  az_iot_certificate_provider_pem_deinit(&mgr);
}

/* A second release() of the same material must not end another loan. */
static void test_a_repeated_release_is_a_no_op(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));

  az_iot_certificate_material a;
  az_iot_certificate_material b;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &a));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &b));
  mgr.base.vtable->release(&mgr.base, &a);
  mgr.base.vtable->release(&mgr.base, &a);
  mgr.base.vtable->release(&mgr.base, NULL);
  mgr.base.vtable->release(NULL, &b);
  assert_int_equal(1, mgr.client_key_refs);
  assert_string_equal(k_key_pem, b.client_key_pem);

  mgr.base.vtable->release(&mgr.base, &b);
  assert_null(mgr.client_key);
  az_iot_certificate_provider_pem_deinit(&mgr);
}

/* Material that did not come from this provider's key ends no loan. */
static void test_release_of_foreign_material_ends_no_loan(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));

  az_iot_certificate_material a;
  memset(&a, 0, sizeof(a));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &a));
  az_iot_certificate_material foreign;
  memset(&foreign, 0, sizeof(foreign));
  foreign.client_key_pem = "not this provider's key";
  mgr.base.vtable->release(&mgr.base, &foreign);
  assert_int_equal(1, mgr.client_key_refs);
  assert_string_equal(k_key_pem, a.client_key_pem);

  mgr.base.vtable->release(&mgr.base, &a);
  az_iot_certificate_provider_pem_deinit(&mgr);
}

/* The key is read at load(), so a key file removed after init() fails the
 * load() rather than handing out a stale copy, and nothing is loaned. */
static void test_load_fails_when_the_key_file_is_gone(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));
  assert_int_equal(0, remove(f->key_path));

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_INITIALIZED, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
  assert_null(mgr.client_key);
  assert_int_equal(0, mgr.client_key_refs);
  az_iot_certificate_provider_pem_deinit(&mgr);
}

/* deinit() frees a key still on loan, so a missing release() leaks nothing. */
static void test_deinit_drops_a_key_still_on_loan(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
    .client_key_password = "hunter2",
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
  assert_non_null(mgr.client_key);

  az_iot_certificate_provider_pem_deinit(&mgr);
  assert_null(mgr.client_key);
  assert_null(mgr.key_password);
  assert_int_equal(0, mgr.client_key_refs);
}

static void test_deinit_tolerates_null(void** state)
{
  (void)state;
  az_iot_certificate_provider_pem_deinit(NULL);
}

static void test_deinit_is_idempotent(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .trusted_ca_pem_path = f->ca_path,
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
    .client_key_password = "hunter2",
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));

  az_iot_certificate_provider_pem_deinit(&mgr);
  /* The struct is zeroed by the first call, so the second must not free the
   * same buffers again. */
  az_iot_certificate_provider_pem_deinit(&mgr);
}

/* The vtable's deinit is what a generic owner of an az_iot_certificate_provider
 * calls; it has to reach the concrete destroy. */
static void test_deinit_through_the_vtable_destroys_the_provider(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_pem_init(&mgr, &opts));

  const az_iot_certificate_provider_vtable* vt = mgr.base.vtable;
  assert_non_null(vt->deinit);
  vt->deinit(&mgr.base);

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_ERR_NOT_INITIALIZED, vt->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
}

/* A readable certificate with an unreadable key is the shape a half-provisioned
 * device has. Init must fail and leave nothing allocated behind. */
static void test_create_fails_when_only_the_key_is_missing(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = "this-key-definitely-does-not-exist.pem",
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_ERR_NOT_INITIALIZED, az_iot_certificate_provider_pem_init(&mgr, &opts));
}

/* The CA path is optional, but naming one that cannot be read is a
 * configuration error rather than something to silently ignore: connecting
 * without the trust anchor the operator asked for is worse than not connecting. */
static void test_create_fails_when_the_named_ca_is_missing(void** state)
{
  fixture* f = *state;
  az_iot_certificate_provider_pem_options opts = {
    .trusted_ca_pem_path = "this-ca-definitely-does-not-exist.pem",
    .client_cert_pem_path = f->cert_path,
    .client_key_pem_path = f->key_path,
  };
  az_iot_certificate_provider_pem mgr;
  assert_int_equal(AZ_IOT_ERR_NOT_INITIALIZED, az_iot_certificate_provider_pem_init(&mgr, &opts));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_options_default_is_zeroed),
    cmocka_unit_test(test_create_rejects_null),
    cmocka_unit_test(test_create_rejects_missing_required_paths),
    cmocka_unit_test(test_create_fails_on_missing_file),
    cmocka_unit_test_setup_teardown(test_load_returns_file_contents, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(test_load_without_optional_fields, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(test_load_after_deinit_is_refused, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(test_load_rejects_null_arguments, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(
        test_load_of_an_operational_credential_returns_the_static_material,
        setup_files,
        teardown_files),
    cmocka_unit_test_setup_teardown(test_init_does_not_keep_the_key, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(
        test_the_last_release_drops_the_key, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(
        test_a_repeated_release_is_a_no_op, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(
        test_release_of_foreign_material_ends_no_loan, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(
        test_load_fails_when_the_key_file_is_gone, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(
        test_deinit_drops_a_key_still_on_loan, setup_files, teardown_files),
    cmocka_unit_test(test_deinit_tolerates_null),
    cmocka_unit_test_setup_teardown(test_deinit_is_idempotent, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(
        test_deinit_through_the_vtable_destroys_the_provider, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(
        test_create_fails_when_only_the_key_is_missing, setup_files, teardown_files),
    cmocka_unit_test_setup_teardown(
        test_create_fails_when_the_named_ca_is_missing, setup_files, teardown_files),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
