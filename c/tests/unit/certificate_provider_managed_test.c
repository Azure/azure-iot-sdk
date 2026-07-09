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

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
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

/* Build a real, self-signed certificate as base64 DER (heap; caller frees) - the
 * on-the-wire form the store hook receives. The provider PEM-wraps it, so the
 * persistence test still exercises the provider's parse-based validity check. */
static char* make_self_signed_cert_base64(void)
{
    EVP_PKEY* key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
    assert_non_null(key);
    X509* x = X509_new();
    assert_non_null(x);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    assert_int_equal(1, X509_set_pubkey(x, key));
    X509_NAME* name = X509_NAME_new();
    assert_non_null(name);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char*)"az-iot-test", -1, -1, 0);
    assert_int_equal(1, X509_set_subject_name(x, name));
    assert_int_equal(1, X509_set_issuer_name(x, name));
    X509_NAME_free(name);
    assert_true(X509_sign(x, key, EVP_sha256()) > 0);

    unsigned char* der = NULL;
    int der_len = i2d_X509(x, &der);
    assert_true(der_len > 0);
    size_t cap = (((size_t)der_len + 2) / 3) * 4 + 1;
    char* b64 = malloc(cap);
    assert_non_null(b64);
    int b64_len = EVP_EncodeBlock((unsigned char*)b64, der, der_len);
    assert_true(b64_len > 0);
    b64[b64_len] = '\0';

    OPENSSL_free(der);
    X509_free(x);
    EVP_PKEY_free(key);
    return b64;
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

    /* A real cert as base64 DER (the wire form). The provider PEM-wraps it; the
     * restart check then rejects empty/garbage, so the chain must be valid. */
    char* cert_b64 = make_self_signed_cert_base64();
    az_span chain[2] = { az_span_create_from_str(cert_b64), az_span_create_from_str(cert_b64) };
    az_iot_issued_certificate_t issued = {
        .certificates = chain,
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
    free(cert_b64);
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
