// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

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
#  include <process.h>
#else
#  include <unistd.h>
#endif

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_certificate_provider_pem.h"
#include "azure/iot/az_iot_result.h"

/* Synthetic PEM-shaped contents. The loader doesn't parse PEM, just slurps the
 * file verbatim, so any byte string round-trips. */
static const char k_cert_pem[] =
    "-----BEGIN CERTIFICATE-----\nMIIBkTCCATegAwIBA...==\n-----END CERTIFICATE-----\n";
static const char k_key_pem[] =
    "-----BEGIN EC PRIVATE KEY-----\nMHcCAQEEIE...==\n-----END EC PRIVATE KEY-----\n";
static const char k_ca_pem[] =
    "-----BEGIN CERTIFICATE-----\nMIIBmDCCAT6gAwIBA...==\n-----END CERTIFICATE-----\n";

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
    snprintf(out_path, PATH_BUF, "az_iot_pem_%u_%u_%s.pem",
        (unsigned)
#if defined(_WIN32)
            _getpid()
#else
            getpid()
#endif
        , ++s_counter, tag);
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
    make_temp(f->key_path,  "key",  k_key_pem,  sizeof(k_key_pem)  - 1);
    make_temp(f->ca_path,   "ca",   k_ca_pem,   sizeof(k_ca_pem)   - 1);
    *state = f;
    return 0;
}

static int teardown_files(void** state)
{
    fixture* f = *state;
    if (!f) return 0;
    remove(f->cert_path);
    remove(f->key_path);
    remove(f->ca_path);
    free(f);
    return 0;
}

static void test_create_rejects_null(void** state)
{
    (void)state;
    az_iot_certificate_provider_pem mgr;
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_certificate_provider_pem_init(&mgr, NULL));
    az_iot_certificate_provider_pem_options opts = { 0 };
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_certificate_provider_pem_init(NULL, &opts));
}

static void test_create_rejects_missing_required_paths(void** state)
{
    (void)state;
    az_iot_certificate_provider_pem_options opts = { 0 };
    az_iot_certificate_provider_pem mgr;
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_certificate_provider_pem_init(&mgr, &opts));

    opts.client_cert_pem_path = "x";
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_certificate_provider_pem_init(&mgr, &opts));

    opts.client_cert_pem_path = NULL;
    opts.client_key_pem_path = "x";
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_certificate_provider_pem_init(&mgr, &opts));
}

static void test_create_fails_on_missing_file(void** state)
{
    (void)state;
    az_iot_certificate_provider_pem_options opts = {
        .client_cert_pem_path = "this-file-definitely-does-not-exist.pem",
        .client_key_pem_path  = "this-file-definitely-does-not-exist.pem",
    };
    az_iot_certificate_provider_pem mgr;
    assert_int_equal(AZ_IOT_ERR_NOT_INITIALIZED,
        az_iot_certificate_provider_pem_init(&mgr, &opts));
}

static void test_load_returns_file_contents(void** state)
{
    fixture* f = *state;
    az_iot_certificate_provider_pem_options opts = {
        .trusted_ca_pem_path  = f->ca_path,
        .client_cert_pem_path = f->cert_path,
        .client_key_pem_path  = f->key_path,
        .client_key_password  = "hunter2",
    };
    az_iot_certificate_provider_pem mgr;
    assert_int_equal(AZ_IOT_OK,
        az_iot_certificate_provider_pem_init(&mgr, &opts));
    assert_non_null(mgr.base.vtable);

    az_iot_certificate_material mat;
    memset(&mat, 0, sizeof(mat));
    assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, &mat));

    assert_non_null(mat.client_cert_pem);
    assert_string_equal(k_cert_pem, mat.client_cert_pem);
    assert_non_null(mat.client_key_pem);
    assert_string_equal(k_key_pem, mat.client_key_pem);
    assert_non_null(mat.trusted_ca_pem);
    assert_string_equal(k_ca_pem, mat.trusted_ca_pem);
    assert_non_null(mat.client_key_password);
    assert_string_equal("hunter2", mat.client_key_password);

    /* release() is documented as a no-op for this loader; calling it must not
     * invalidate the buffers (a second load() must succeed and return the
     * same content). */
    mgr.base.vtable->release(&mgr.base, &mat);

    az_iot_certificate_material mat2;
    memset(&mat2, 0, sizeof(mat2));
    assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, &mat2));
    assert_string_equal(k_cert_pem, mat2.client_cert_pem);
    mgr.base.vtable->release(&mgr.base, &mat2);

    az_iot_certificate_provider_pem_destroy(&mgr);
}

static void test_load_without_optional_fields(void** state)
{
    fixture* f = *state;
    az_iot_certificate_provider_pem_options opts = {
        .client_cert_pem_path = f->cert_path,
        .client_key_pem_path  = f->key_path,
        /* trusted_ca_pem_path and client_key_password intentionally NULL */
    };
    az_iot_certificate_provider_pem mgr;
    assert_int_equal(AZ_IOT_OK,
        az_iot_certificate_provider_pem_init(&mgr, &opts));

    az_iot_certificate_material mat;
    memset(&mat, 0, sizeof(mat));
    assert_int_equal(AZ_IOT_OK, mgr.base.vtable->load(&mgr.base, AZ_IOT_CRED_BOOTSTRAP, &mat));
    assert_null(mat.trusted_ca_pem);
    assert_null(mat.client_key_password);
    assert_non_null(mat.client_cert_pem);
    assert_non_null(mat.client_key_pem);

    az_iot_certificate_provider_pem_destroy(&mgr);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_create_rejects_null),
        cmocka_unit_test(test_create_rejects_missing_required_paths),
        cmocka_unit_test(test_create_fails_on_missing_file),
        cmocka_unit_test_setup_teardown(test_load_returns_file_contents,
            setup_files, teardown_files),
        cmocka_unit_test_setup_teardown(test_load_without_optional_fields,
            setup_files, teardown_files),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
