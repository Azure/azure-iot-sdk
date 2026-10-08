// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

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
#define OP_KEY "az_iot_managed_test_op_key.pem"
#define OP_CERT "az_iot_managed_test_op_cert.pem"
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
  X509_NAME_add_entry_by_txt(
      name, "CN", MBSTRING_UTF8, (const unsigned char*)"az-iot-test", -1, -1, 0);
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

  az_iot_certificate_provider_managed_options opts = {
    .bootstrap_cert_pem_path = BOOT_CRT,
    .bootstrap_key_pem_path = BOOT_KEY,
    .trusted_ca_pem_path = TRUST_CA,
    .operational_key_pem_path = OP_KEY,
    .operational_cert_pem_path = OP_CERT,
    .key_type = AZ_IOT_MANAGED_KEY_EC_P256,
  };
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  assert_non_null(prov.base.vtable);
  assert_int_equal(AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION, prov.base.vtable->version);

  /* CSR carries the requested CN and is self-consistent (verifies with its
   * own public key). */
  az_iot_certificate_signing_request csr;
  memset(&csr, 0, sizeof(csr));
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->get_csr(&prov.base, "my-device-id", &csr));
  assert_non_null(csr.csr_base64);
  assert_true(strlen(csr.csr_base64) > 0);

  X509_REQ* req = decode_csr(csr.csr_base64);
  assert_non_null(req);

  char cn[128] = { 0 };
  int cn_len
      = X509_NAME_get_text_by_NID(X509_REQ_get_subject_name(req), NID_commonName, cn, sizeof(cn));
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
  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
  assert_string_equal(BOOT_CRT, mat.client_cert_path);
  assert_string_equal(BOOT_KEY, mat.client_key_path);
  assert_string_equal(TRUST_CA, mat.trusted_ca_path);

  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));

  /* One certificate per role: past index 0 NOT_FOUND; arguments are still checked. */
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_BOOTSTRAP, 1, &mat));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->load(NULL, AZ_IOT_CRED_BOOTSTRAP, 1, &mat));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_BOOTSTRAP, 1, NULL));

  az_iot_certificate_provider_managed_deinit(&prov);
  remove_test_files();
}

static void managed_store_persists_and_survives_restart(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = {
    .bootstrap_cert_pem_path = BOOT_CRT,
    .bootstrap_key_pem_path = BOOT_KEY,
    .operational_key_pem_path = OP_KEY,
    .operational_cert_pem_path = OP_CERT,
    .key_type = AZ_IOT_MANAGED_KEY_RSA_2048,
  };
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  /* A real cert as base64 DER (the wire form). The provider PEM-wraps it; the
   * restart check then rejects empty/garbage, so the chain must be valid. */
  char* cert_b64 = make_self_signed_cert_base64();
  az_span chain[2] = { az_span_create_from_str(cert_b64), az_span_create_from_str(cert_b64) };
  az_iot_issued_certificate issued = {
    .certificates = chain,
    .count = 2,
  };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));

  /* After storing, OPERATIONAL load serves the persisted cert + op key. */
  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));
  assert_string_equal(OP_CERT, mat.client_cert_path);
  assert_string_equal(OP_KEY, mat.client_key_path);

  az_iot_certificate_provider_managed_deinit(&prov);

  /* Simulate a process restart: a fresh provider over the same paths loads
   * the persisted key and immediately has an operational identity (no
   * re-enrollment) and can still produce a CSR from the loaded key. */
  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_true(prov2.has_operational);

  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_OK, prov2.base.vtable->load(&prov2.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));
  assert_string_equal(OP_CERT, mat.client_cert_path);

  az_iot_certificate_signing_request csr;
  memset(&csr, 0, sizeof(csr));
  assert_int_equal(AZ_IOT_OK, prov2.base.vtable->get_csr(&prov2.base, "my-device-id", &csr));
  assert_non_null(csr.csr_base64);
  prov2.base.vtable->release_csr(&prov2.base, &csr);

  az_iot_certificate_provider_managed_deinit(&prov2);
  free(cert_b64);
  remove_test_files();
}

static void managed_init_rejects_bad_args(void** state)
{
  (void)state;
  az_iot_certificate_provider_managed prov;

  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_managed_init(NULL, NULL));

  az_iot_certificate_provider_managed_options opts = {
    .bootstrap_cert_pem_path = NULL, /* key without certificate */
    .bootstrap_key_pem_path = BOOT_KEY,
    .operational_key_pem_path = OP_KEY,
    .operational_cert_pem_path = OP_CERT,
  };
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_managed_init(&prov, &opts));

  opts.bootstrap_cert_pem_path = BOOT_CRT;
  opts.operational_cert_pem_path = ""; /* required, empty */
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_managed_init(&prov, &opts));
}

/* Shared options over the test-local paths. */
static az_iot_certificate_provider_managed_options test_options(void)
{
  az_iot_certificate_provider_managed_options opts = {
    .bootstrap_cert_pem_path = BOOT_CRT,
    .bootstrap_key_pem_path = BOOT_KEY,
    .trusted_ca_pem_path = TRUST_CA,
    .operational_key_pem_path = OP_KEY,
    .operational_cert_pem_path = OP_CERT,
    .key_type = AZ_IOT_MANAGED_KEY_EC_P256,
  };
  return opts;
}

static void managed_load_rejects_null_arguments(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->load(NULL, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_BOOTSTRAP, 0, NULL));

  az_iot_certificate_provider_managed_deinit(&prov);
  remove_test_files();
}

/* The connect path uses this code to decide it must still enroll: reporting
 * anything else would send a device at the hub with no operational identity. */
static void managed_operational_load_without_a_stored_chain_is_not_found(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  assert_false(prov.has_operational);

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));

  az_iot_certificate_provider_managed_deinit(&prov);
  remove_test_files();
}

/* SAS onboarding: no bootstrap identity. The connect path treats NOT_FOUND as
 * "no certificate for this role" and moves on to SAS. */
static void managed_without_a_bootstrap_identity_has_no_bootstrap_certificate(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  opts.bootstrap_cert_pem_path = NULL;
  opts.bootstrap_key_pem_path = NULL;
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));

  az_iot_certificate_signing_request csr = { 0 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->get_csr(&prov.base, "dev", &csr));
  assert_non_null(csr.csr_base64);
  prov.base.vtable->release_csr(&prov.base, &csr);

  az_iot_certificate_provider_managed_deinit(&prov);
  remove_test_files();
}

/* The material is borrowed from the provider struct, so release() must leave it
 * alone; freeing there would dangle the paths the connection is still using. */
static void managed_release_leaves_the_material_usable(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
  prov.base.vtable->release(&prov.base, &mat);

  az_iot_certificate_material mat2;
  memset(&mat2, 0, sizeof(mat2));
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat2));
  assert_string_equal(BOOT_CRT, mat2.client_cert_path);

  az_iot_certificate_provider_managed_deinit(&prov);
  remove_test_files();
}

static void managed_get_csr_rejects_null_arguments(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  az_iot_certificate_signing_request csr;
  memset(&csr, 0, sizeof(csr));
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->get_csr(NULL, "cn", &csr));
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->get_csr(&prov.base, "cn", NULL));

  az_iot_certificate_provider_managed_deinit(&prov);
  remove_test_files();
}

static void managed_store_rejects_null_arguments(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(NULL, NULL));
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, NULL));

  /* An issuance with nothing in it is a protocol error, not an empty success:
   * accepting it would flip has_operational with no certificate on disk. */
  az_iot_issued_certificate empty = { .certificates = NULL, .count = 0 };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &empty));
  assert_false(prov.has_operational);

  az_span chain[1] = { AZ_SPAN_EMPTY };
  az_iot_issued_certificate no_count = { .certificates = chain, .count = 0 };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &no_count));

  az_iot_certificate_provider_managed_deinit(&prov);
  remove_test_files();
}

static void managed_deinit_tolerates_null(void** state)
{
  (void)state;
  az_iot_certificate_provider_managed_deinit(NULL);
}

/* The vtable's deinit is what a generic owner of an az_iot_certificate_provider
 * calls; it has to reach the concrete destroy. */
static void managed_deinit_through_the_vtable_destroys_the_provider(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  const az_iot_certificate_provider_vtable* vt = prov.base.vtable;
  assert_non_null(vt->deinit);
  vt->deinit(&prov.base);

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_INITIALIZED, vt->load(&prov.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
  remove_test_files();
}

/* Renewal replaces the identity rather than appending to it: a file that
 * accumulated every chain ever issued would present a stale leaf on connect.
 *
 * Proven through the public API rather than by inspecting the file: store a
 * valid chain, then store a chain that is not a certificate, then restart. If
 * the second store had appended, the valid first certificate would still be
 * there and the restart would find a usable operational identity. */
static void managed_store_overwrites_a_previously_issued_chain(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  char* good = make_self_signed_cert_base64();
  az_span chain1[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued1 = { .certificates = chain1, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued1));

  /* A restart here would find the good chain, which is the control for the
   * assertion below. */
  az_iot_certificate_provider_managed_deinit(&prov);
  az_iot_certificate_provider_managed check;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&check, &opts));
  assert_true(check.has_operational);
  az_iot_certificate_provider_managed_deinit(&check);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  az_span chain2[1] = { AZ_SPAN_FROM_STR("bm90LWEtY2VydGlmaWNhdGU=") };
  az_iot_issued_certificate issued2 = { .certificates = chain2, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov2.base.vtable->store_issued_certificate(&prov2.base, &issued2));
  az_iot_certificate_provider_managed_deinit(&prov2);

  az_iot_certificate_provider_managed prov3;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov3, &opts));
  assert_false(prov3.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov3);

  free(good);
  remove_test_files();
}

static void managed_a_stored_chain_that_is_not_a_certificate_is_rejected_on_restart(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  az_span chain[1] = { AZ_SPAN_FROM_STR("bm90LWEtY2VydGlmaWNhdGU=") };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));
  az_iot_certificate_provider_managed_deinit(&prov);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, prov2.base.vtable->load(&prov2.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));

  az_iot_certificate_provider_managed_deinit(&prov2);
  remove_test_files();
}

/* Neither bundled provider implements the optional sign() hook, so the connect
 * path must keep checking it for NULL before calling it. Pinning that here
 * makes adding an implementation a deliberate act rather than a surprise. */
static void the_sign_hook_is_not_offered_by_this_provider(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  assert_null(prov.base.vtable->sign);

  az_iot_certificate_provider_managed_deinit(&prov);
  remove_test_files();
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(managed_init_generates_key_and_valid_csr),
    cmocka_unit_test(managed_store_persists_and_survives_restart),
    cmocka_unit_test(managed_init_rejects_bad_args),
    cmocka_unit_test(managed_load_rejects_null_arguments),
    cmocka_unit_test(managed_operational_load_without_a_stored_chain_is_not_found),
    cmocka_unit_test(managed_without_a_bootstrap_identity_has_no_bootstrap_certificate),
    cmocka_unit_test(managed_release_leaves_the_material_usable),
    cmocka_unit_test(managed_get_csr_rejects_null_arguments),
    cmocka_unit_test(managed_store_rejects_null_arguments),
    cmocka_unit_test(managed_deinit_tolerates_null),
    cmocka_unit_test(managed_deinit_through_the_vtable_destroys_the_provider),
    cmocka_unit_test(managed_store_overwrites_a_previously_issued_chain),
    cmocka_unit_test(managed_a_stored_chain_that_is_not_a_certificate_is_rejected_on_restart),
    cmocka_unit_test(the_sign_hook_is_not_offered_by_this_provider),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
