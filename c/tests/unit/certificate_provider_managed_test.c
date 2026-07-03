// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Tests for the OpenSSL-backed "managed" certificate provider (D5). Links
 * OpenSSL only to decode and validate the CSR the provider produces. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "az_iot_certificate_provider_managed.h"

#include <openssl/evp.h>
#include <openssl/x509.h>

/* Test-local file paths (created in the test working directory). */
#define OP_KEY   "az_iot_managed_test_op_key.pem"
#define OP_CERT  "az_iot_managed_test_op_cert.pem"
#define BOOT_CRT "bootstrap-cert.pem"
#define BOOT_KEY "bootstrap-key.pem"
#define TRUST_CA "trusted-ca.pem"

static void remove_test_files(void)
{
    remove(OP_KEY);
    remove(OP_CERT);
}

/* Decode a base64 (no-newline) CSR into an X509_REQ. Caller frees. */
static X509_REQ* decode_csr(const char* b64)
{
    size_t b64_len = strlen(b64);
    size_t cap = (b64_len / 4) * 3 + 1;
    unsigned char* der = (unsigned char*)malloc(cap);
    assert_non_null(der);
    int der_len = EVP_DecodeBlock(der, (const unsigned char*)b64, (int)b64_len);
    assert_true(der_len > 0);
    const unsigned char* p = der;
    X509_REQ* req = d2i_X509_REQ(NULL, &p, (long)der_len);
    free(der);
    return req;
}

static void managed_init_generates_key_and_valid_csr(void** state)
{
    (void)state;
    remove_test_files();

    az_iot_certificate_provider_managed_options_t opts = {
        .bootstrap_cert_pem_path   = BOOT_CRT,
        .bootstrap_key_pem_path    = BOOT_KEY,
        .trusted_ca_pem_path       = TRUST_CA,
        .operational_key_pem_path  = OP_KEY,
        .operational_cert_pem_path = OP_CERT,
        .key_type                  = AZ_IOT_MANAGED_KEY_EC_P256,
    };
    az_iot_certificate_provider_managed_t prov;
    assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
    assert_non_null(prov.base.vtable);
    assert_int_equal(AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION, prov.base.vtable->version);

    /* CSR carries the requested CN and is self-consistent (verifies with its
     * own public key). */
    az_iot_certificate_signing_request_t csr;
    memset(&csr, 0, sizeof(csr));
    assert_int_equal(AZ_IOT_OK,
        prov.base.vtable->get_csr(&prov.base, "my-device-id", &csr));
    assert_non_null(csr.csr_base64);
    assert_true(strlen(csr.csr_base64) > 0);

    X509_REQ* req = decode_csr(csr.csr_base64);
    assert_non_null(req);

    char cn[128] = {0};
    int cn_len = X509_NAME_get_text_by_NID(
        X509_REQ_get_subject_name(req), NID_commonName, cn, sizeof(cn));
    assert_true(cn_len > 0);
    assert_string_equal(cn, "my-device-id");

    EVP_PKEY* pk = X509_REQ_get_pubkey(req);
    assert_non_null(pk);
    assert_int_equal(1, X509_REQ_verify(req, pk));
    EVP_PKEY_free(pk);
    X509_REQ_free(req);

    prov.base.vtable->release_csr(&prov.base, &csr);
    assert_null(csr.csr_base64);

    /* Before any issuance, BOOTSTRAP load returns the bootstrap paths and
     * OPERATIONAL load reports nothing to serve yet. */
    az_iot_certificate_material_t mat;
    memset(&mat, 0, sizeof(mat));
    assert_int_equal(AZ_IOT_OK,
        prov.base.vtable->load(&prov.base, AZ_IOT_CRED_BOOTSTRAP, &mat));
    assert_string_equal(BOOT_CRT, mat.client_cert_path);
    assert_string_equal(BOOT_KEY, mat.client_key_path);
    assert_string_equal(TRUST_CA, mat.trusted_ca_path);

    memset(&mat, 0, sizeof(mat));
    assert_int_equal(AZ_IOT_ERR_NOT_FOUND,
        prov.base.vtable->load(&prov.base, AZ_IOT_CRED_OPERATIONAL, &mat));

    az_iot_certificate_provider_managed_deinit(&prov);
    remove_test_files();
}

static void managed_store_persists_and_survives_restart(void** state)
{
    (void)state;
    remove_test_files();

    az_iot_certificate_provider_managed_options_t opts = {
        .bootstrap_cert_pem_path   = BOOT_CRT,
        .bootstrap_key_pem_path    = BOOT_KEY,
        .operational_key_pem_path  = OP_KEY,
        .operational_cert_pem_path = OP_CERT,
        .key_type                  = AZ_IOT_MANAGED_KEY_RSA_2048,
    };
    az_iot_certificate_provider_managed_t prov;
    assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

    const char* chain[2] = {
        "-----BEGIN CERTIFICATE-----\nQUJD\n-----END CERTIFICATE-----\n",
        "-----BEGIN CERTIFICATE-----\nWFla\n-----END CERTIFICATE-----\n",
    };
    az_iot_issued_certificate_t issued = {
        .client_cert_chain_pem = chain,
        .count = 2,
    };
    assert_int_equal(AZ_IOT_OK,
        prov.base.vtable->store_issued_certificate(&prov.base, &issued));

    /* After storing, OPERATIONAL load serves the persisted cert + op key. */
    az_iot_certificate_material_t mat;
    memset(&mat, 0, sizeof(mat));
    assert_int_equal(AZ_IOT_OK,
        prov.base.vtable->load(&prov.base, AZ_IOT_CRED_OPERATIONAL, &mat));
    assert_string_equal(OP_CERT, mat.client_cert_path);
    assert_string_equal(OP_KEY, mat.client_key_path);

    az_iot_certificate_provider_managed_deinit(&prov);

    /* Simulate a process restart: a fresh provider over the same paths loads
     * the persisted key and immediately has an operational identity (no
     * re-enrollment) and can still produce a CSR from the loaded key. */
    az_iot_certificate_provider_managed_t prov2;
    assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
    assert_true(prov2.has_operational);

    memset(&mat, 0, sizeof(mat));
    assert_int_equal(AZ_IOT_OK,
        prov2.base.vtable->load(&prov2.base, AZ_IOT_CRED_OPERATIONAL, &mat));
    assert_string_equal(OP_CERT, mat.client_cert_path);

    az_iot_certificate_signing_request_t csr;
    memset(&csr, 0, sizeof(csr));
    assert_int_equal(AZ_IOT_OK,
        prov2.base.vtable->get_csr(&prov2.base, "my-device-id", &csr));
    assert_non_null(csr.csr_base64);
    prov2.base.vtable->release_csr(&prov2.base, &csr);

    az_iot_certificate_provider_managed_deinit(&prov2);
    remove_test_files();
}

static void managed_init_rejects_bad_args(void** state)
{
    (void)state;
    az_iot_certificate_provider_managed_t prov;

    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_certificate_provider_managed_init(NULL, NULL));

    az_iot_certificate_provider_managed_options_t opts = {
        .bootstrap_cert_pem_path   = NULL, /* required, missing */
        .bootstrap_key_pem_path    = BOOT_KEY,
        .operational_key_pem_path  = OP_KEY,
        .operational_cert_pem_path = OP_CERT,
    };
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_certificate_provider_managed_init(&prov, &opts));

    opts.bootstrap_cert_pem_path = BOOT_CRT;
    opts.operational_cert_pem_path = ""; /* required, empty */
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG,
        az_iot_certificate_provider_managed_init(&prov, &opts));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(managed_init_generates_key_and_valid_csr),
        cmocka_unit_test(managed_store_persists_and_survives_restart),
        cmocka_unit_test(managed_init_rejects_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
