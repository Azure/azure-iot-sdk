// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Tests for the OpenSSL-backed "managed" certificate provider (D5). Links
 * OpenSSL only to decode and validate the CSR the provider produces. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

/* Build a certificate for @p subject_key, signed by a throwaway issuer key, as
 * base64 DER (heap; caller frees) - the on-the-wire form the store hook
 * receives. @p serial makes two certificates for the same key differ.
 * @p trailing junk bytes are appended to the DER before encoding. */
static char* make_cert_base64_with_trailing(EVP_PKEY* subject_key, long serial, size_t trailing)
{
  EVP_PKEY* key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  assert_non_null(key);
  X509* x = X509_new();
  assert_non_null(x);
  ASN1_INTEGER_set(X509_get_serialNumber(x), serial);
  X509_gmtime_adj(X509_getm_notBefore(x), 0);
  X509_gmtime_adj(X509_getm_notAfter(x), 3600);
  assert_int_equal(1, X509_set_pubkey(x, subject_key));
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
  unsigned char* buf = malloc((size_t)der_len + trailing);
  assert_non_null(buf);
  memcpy(buf, der, (size_t)der_len);
  memset(buf + der_len, 0x05, trailing);
  int total = der_len + (int)trailing;
  size_t cap = (((size_t)total + 2) / 3) * 4 + 1;
  char* b64 = malloc(cap);
  assert_non_null(b64);
  int b64_len = EVP_EncodeBlock((unsigned char*)b64, buf, total);
  assert_true(b64_len > 0);
  b64[b64_len] = '\0';

  free(buf);
  OPENSSL_free(der);
  X509_free(x);
  EVP_PKEY_free(key);
  return b64;
}

static char* make_cert_base64(EVP_PKEY* subject_key, long serial)
{
  return make_cert_base64_with_trailing(subject_key, serial, 0);
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
  char* cert_b64 = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
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
  az_iot_certificate_provider_managed_options opts
      = az_iot_certificate_provider_managed_options_default();
  opts.bootstrap_cert_pem_path = BOOT_CRT;
  opts.bootstrap_key_pem_path = BOOT_KEY;
  opts.trusted_ca_pem_path = TRUST_CA;
  opts.operational_key_pem_path = OP_KEY;
  opts.operational_cert_pem_path = OP_CERT;
  return opts;
}

static void managed_options_default_has_no_paths_and_no_rotation(void** state)
{
  (void)state;
  az_iot_certificate_provider_managed_options opts
      = az_iot_certificate_provider_managed_options_default();
  assert_null(opts.bootstrap_cert_pem_path);
  assert_null(opts.bootstrap_key_pem_path);
  assert_null(opts.trusted_ca_pem_path);
  assert_null(opts.operational_key_pem_path);
  assert_null(opts.operational_cert_pem_path);
  assert_int_equal(opts.key_type, AZ_IOT_MANAGED_KEY_EC_P256);
  assert_false(opts.rotate_key_on_csr_request);
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

/* base64 DER of every certificate in @p path, concatenated with ';' (heap;
 * caller frees). Empty string when the file has none. */
static char* read_chain_base64(const char* path)
{
  char* out = calloc(1, 1);
  assert_non_null(out);
  BIO* b = BIO_new_file(path, "rb");
  if (!b)
  {
    return out;
  }
  X509* x;
  while ((x = PEM_read_bio_X509(b, NULL, NULL, NULL)) != NULL)
  {
    unsigned char* der = NULL;
    int der_len = i2d_X509(x, &der);
    assert_true(der_len > 0);
    size_t cur = strlen(out);
    size_t add = (((size_t)der_len + 2) / 3) * 4 + 2;
    out = realloc(out, cur + add);
    assert_non_null(out);
    if (cur > 0)
    {
      out[cur++] = ';';
    }
    int n = EVP_EncodeBlock((unsigned char*)out + cur, der, der_len);
    assert_true(n > 0);
    out[cur + (size_t)n] = '\0';
    OPENSSL_free(der);
    X509_free(x);
  }
  BIO_free(b);
  return out;
}

static bool file_exists(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (f)
  {
    fclose(f);
    return true;
  }
  return false;
}

/* Temporary files the provider left next to OP_CERT or OP_KEY. */
static int count_temp_files(void)
{
  int n = 0;
#if defined(_WIN32)
  static const char* const patterns[] = { OP_CERT ".*", OP_KEY ".*" };
  for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i)
  {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(patterns[i], &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
      do
      {
        /* "name.*" also matches "name" itself. */
        if (strcmp(fd.cFileName, OP_CERT) != 0 && strcmp(fd.cFileName, OP_KEY) != 0)
        {
          n++;
        }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
  }
#else
  DIR* d = opendir(".");
  assert_non_null(d);
  struct dirent* e;
  while ((e = readdir(d)) != NULL)
  {
    if (strncmp(e->d_name, OP_CERT ".", strlen(OP_CERT ".")) == 0
        || strncmp(e->d_name, OP_KEY ".", strlen(OP_KEY ".")) == 0)
    {
      n++;
    }
  }
  closedir(d);
#endif
  return n;
}

/* Renewal replaces the identity rather than appending to it: a file that
 * accumulated every chain ever issued would present a stale leaf on connect. */
static void managed_store_overwrites_a_previously_issued_chain(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  char* first = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  char* second = make_cert_base64((EVP_PKEY*)prov.operational_key, 2);
  assert_string_not_equal(first, second);

  az_span chain1[1] = { az_span_create_from_str(first) };
  az_iot_issued_certificate issued1 = { .certificates = chain1, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued1));
  az_span chain2[1] = { az_span_create_from_str(second) };
  az_iot_issued_certificate issued2 = { .certificates = chain2, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued2));
  az_iot_certificate_provider_managed_deinit(&prov);

  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, second);
  assert_int_equal(0, count_temp_files());

  free(on_disk);
  free(first);
  free(second);
  remove_test_files();
}

/* A chain that does not parse is refused and never becomes the identity. */
static void managed_a_stored_chain_that_is_not_a_certificate_is_refused(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  az_span chain[1] = { AZ_SPAN_FROM_STR("bm90LWEtY2VydGlmaWNhdGU=") };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  assert_int_not_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));
  assert_false(file_exists(OP_CERT));
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov2);
  remove_test_files();
}

/* A failed store is all-or-nothing: the previous certificate stays in use, in
 * this process and after a restart, so a failed renewal cannot break the next
 * connect. */
static void managed_a_failed_store_keeps_the_previous_certificate(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain1[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued1 = { .certificates = chain1, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued1));

  az_span chain2[1] = { AZ_SPAN_FROM_STR("bm90LWEtY2VydGlmaWNhdGU=") };
  az_iot_issued_certificate issued2 = { .certificates = chain2, .count = 1 };
  assert_int_not_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued2));

  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));
  assert_string_equal(OP_CERT, mat.client_cert_path);
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, good);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_true(prov2.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(on_disk);
  free(good);
  remove_test_files();
}

/* The leaf must certify the operational key: a chain issued for another key
 * would fail every connect, so it is refused and the previous one kept. */
static void managed_store_refuses_a_chain_for_another_key(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain1[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued1 = { .certificates = chain1, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued1));

  EVP_PKEY* other = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  assert_non_null(other);
  char* foreign = make_cert_base64(other, 2);
  az_span chain2[2] = { az_span_create_from_str(foreign), az_span_create_from_str(good) };
  az_iot_issued_certificate issued2 = { .certificates = chain2, .count = 2 };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &issued2));

  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, good);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov);

  free(on_disk);
  free(foreign);
  free(good);
  EVP_PKEY_free(other);
  remove_test_files();
}

/* Every entry must parse, not just the leaf: a matching leaf followed by an
 * entry that is not a certificate is refused and the previous chain kept. */
static void managed_store_refuses_a_chain_with_a_bad_trailing_entry(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain1[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued1 = { .certificates = chain1, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued1));

  char* next = make_cert_base64((EVP_PKEY*)prov.operational_key, 2);
  az_span chain2[2]
      = { az_span_create_from_str(next), AZ_SPAN_FROM_STR("bm90LWEtY2VydGlmaWNhdGU=") };
  az_iot_issued_certificate issued2 = { .certificates = chain2, .count = 2 };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &issued2));

  /* An entry that is not base64 is refused the same way. */
  az_span chain3[2] = { az_span_create_from_str(next), AZ_SPAN_FROM_STR("!!!!") };
  az_iot_issued_certificate issued3 = { .certificates = chain3, .count = 2 };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &issued3));

  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, good);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov);

  free(on_disk);
  free(next);
  free(good);
  remove_test_files();
}

/* An empty entry is not a certificate: a matching leaf followed by one is
 * refused and the previous chain kept. */
static void managed_store_refuses_a_chain_with_an_empty_entry(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain1[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued1 = { .certificates = chain1, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued1));

  char* next = make_cert_base64((EVP_PKEY*)prov.operational_key, 2);
  az_span chain2[2] = { az_span_create_from_str(next), AZ_SPAN_EMPTY };
  az_iot_issued_certificate issued2 = { .certificates = chain2, .count = 2 };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &issued2));

  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, good);
  az_iot_certificate_provider_managed_deinit(&prov);

  free(on_disk);
  free(next);
  free(good);
  remove_test_files();
}

/* A persisted chain whose trailing entry is truncated is not served after a
 * restart, even though its leaf is valid. */
static void managed_a_persisted_chain_with_a_truncated_entry_is_not_used(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));
  az_iot_certificate_provider_managed_deinit(&prov);

  /* Control: the file as written is usable after a restart. */
  az_iot_certificate_provider_managed check;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&check, &opts));
  assert_true(check.has_operational);
  az_iot_certificate_provider_managed_deinit(&check);

  FILE* f = fopen(OP_CERT, "ab");
  assert_non_null(f);
  const char tail[] = "-----BEGIN CERTIFICATE-----\nMIIB\n";
  assert_int_equal((int)sizeof(tail) - 1, (int)fwrite(tail, 1, sizeof(tail) - 1, f));
  assert_int_equal(0, fclose(f));

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(good);
  remove_test_files();
}

/* Base64 of @p text (heap; caller frees). */
static char* base64_of(const char* text)
{
  size_t n = strlen(text);
  char* out = malloc(((n + 2) / 3) * 4 + 1);
  assert_non_null(out);
  int len = EVP_EncodeBlock((unsigned char*)out, (const unsigned char*)text, (int)n);
  assert_true(len > 0);
  out[len] = '\0';
  return out;
}

/* A DER certificate followed by more bytes is not a certificate: refused at
 * store, and not used when found in the persisted file. */
static void managed_a_certificate_with_trailing_bytes_is_refused(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain1[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued1 = { .certificates = chain1, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued1));

  char* padded = make_cert_base64_with_trailing((EVP_PKEY*)prov.operational_key, 2, 4);
  az_span chain2[1] = { az_span_create_from_str(padded) };
  az_iot_issued_certificate issued2 = { .certificates = chain2, .count = 1 };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &issued2));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, good);
  az_iot_certificate_provider_managed_deinit(&prov);

  /* The same block written to the file directly is not used at init. */
  FILE* f = fopen(OP_CERT, "wb");
  assert_non_null(f);
  assert_true(fputs("-----BEGIN CERTIFICATE-----\n", f) >= 0);
  assert_true(fputs(padded, f) >= 0);
  assert_true(fputs("\n-----END CERTIFICATE-----\n", f) >= 0);
  assert_int_equal(0, fclose(f));
  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(on_disk);
  free(padded);
  free(good);
  remove_test_files();
}

/* Text after the chain that is not a certificate is refused, whether it rides
 * in an issued entry or is appended to the persisted file. */
static void managed_a_chain_followed_by_other_text_is_refused(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain1[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued1 = { .certificates = chain1, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued1));

  /* An entry that is already PEM is written through as is. */
  char* next = make_cert_base64((EVP_PKEY*)prov.operational_key, 2);
  size_t pem_cap = strlen(next) + 128;
  char* pem = malloc(pem_cap);
  assert_non_null(pem);
  (void)snprintf(
      pem,
      pem_cap,
      "-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\nnot a certificate\n",
      next);
  char* entry = base64_of(pem);
  az_span chain2[1] = { az_span_create_from_str(entry) };
  az_iot_issued_certificate issued2 = { .certificates = chain2, .count = 1 };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &issued2));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, good);
  az_iot_certificate_provider_managed_deinit(&prov);

  FILE* fp = fopen(OP_CERT, "ab");
  assert_non_null(fp);
  assert_true(fputs("not a certificate\n", fp) >= 0);
  assert_int_equal(0, fclose(fp));
  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(on_disk);
  free(entry);
  free(pem);
  free(next);
  free(good);
  remove_test_files();
}

/* A persisted chain padded with a little trailing whitespace is still used. */
static void managed_a_persisted_chain_with_trailing_whitespace_is_used(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));
  az_iot_certificate_provider_managed_deinit(&prov);

  FILE* fp = fopen(OP_CERT, "ab");
  assert_non_null(fp);
  assert_true(fputs("\n\n", fp) >= 0);
  assert_int_equal(0, fclose(fp));
  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_true(prov2.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(good);
  remove_test_files();
}

/* Exactly @p total bytes (heap; caller frees): the PEM block of @p cert_b64,
 * then newlines. */
static char* pem_padded_to(const char* cert_b64, size_t total)
{
  char* out = malloc(total + 1);
  assert_non_null(out);
  size_t n = 0;
  static const char begin[] = "-----BEGIN CERTIFICATE-----\n";
  static const char end[] = "\n-----END CERTIFICATE-----\n";
  memcpy(out, begin, sizeof(begin) - 1);
  n += sizeof(begin) - 1;
  size_t len = strlen(cert_b64);
  for (size_t off = 0; off < len; off += 64)
  {
    size_t chunk = len - off < 64 ? len - off : 64;
    memcpy(out + n, cert_b64 + off, chunk);
    n += chunk;
    if (off + chunk < len)
    {
      out[n++] = '\n';
    }
  }
  memcpy(out + n, end, sizeof(end) - 1);
  n += sizeof(end) - 1;
  assert_true(n <= total);
  memset(out + n, '\n', total - n);
  out[total] = '\0';
  return out;
}

/* The 64 KiB bound is inclusive, at init and at store: exactly 64 KiB is
 * used, one byte more is not. */
static void managed_the_chain_size_bound_is_exactly_64_kib(void** state)
{
  (void)state;
  const size_t limit = 64u * 1024u;
  az_iot_certificate_provider_managed_options opts = test_options();
  for (size_t extra = 0; extra <= 1; ++extra)
  {
    remove_test_files();
    az_iot_certificate_provider_managed prov;
    assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
    char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
    char* pem = pem_padded_to(good, limit + extra);

    /* Store: one already-PEM entry decoding to exactly limit + extra bytes. */
    size_t b64_cap = ((limit + extra + 2) / 3) * 4 + 1;
    char* entry = malloc(b64_cap);
    assert_non_null(entry);
    int b64_len
        = EVP_EncodeBlock((unsigned char*)entry, (const unsigned char*)pem, (int)(limit + extra));
    assert_true(b64_len > 0);
    entry[b64_len] = '\0';
    az_span chain[1] = { az_span_create_from_str(entry) };
    az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
    assert_int_equal(
        extra == 0 ? AZ_IOT_OK : AZ_IOT_ERR_INVALID_ARG,
        prov.base.vtable->store_issued_certificate(&prov.base, &issued));
    az_iot_certificate_provider_managed_deinit(&prov);

    /* Init: the same bytes as the persisted file. */
    FILE* f = fopen(OP_CERT, "wb");
    assert_non_null(f);
    assert_int_equal(limit + extra, fwrite(pem, 1, limit + extra, f));
    assert_int_equal(0, fclose(f));
    az_iot_certificate_provider_managed prov2;
    assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
    assert_int_equal(extra == 0, prov2.has_operational);
    az_iot_certificate_provider_managed_deinit(&prov2);

    free(entry);
    free(pem);
    free(good);
  }
  remove_test_files();
}

/* A persisted chain larger than the provider reads back is not used. */
static void managed_an_oversized_persisted_chain_is_not_used(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));
  az_iot_certificate_provider_managed_deinit(&prov);

  FILE* fp = fopen(OP_CERT, "ab");
  assert_non_null(fp);
  const size_t pad_len = 64u * 1024u;
  char* pad = malloc(pad_len);
  assert_non_null(pad);
  memset(pad, '\n', pad_len);
  assert_true(fwrite(pad, 1, pad_len, fp) == pad_len);
  free(pad);
  assert_int_equal(0, fclose(fp));
  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(good);
  remove_test_files();
}

/* A chain larger than init() reads back is refused at store, keeping the
 * previous one, so a successful store is always usable after a restart. */
static void managed_store_refuses_an_oversized_chain(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span one[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued = { .certificates = one, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));

  /* Every entry is a valid certificate; only the total size is over. */
  size_t count = (64u * 1024u) / strlen(good) + 1u;
  az_span* many = malloc(count * sizeof(az_span));
  assert_non_null(many);
  for (size_t i = 0; i < count; ++i)
  {
    many[i] = one[0];
  }
  az_iot_issued_certificate big = { .certificates = many, .count = count };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &big));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, good);
  assert_true(prov.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov);

  free(on_disk);
  free(many);
  free(good);
  remove_test_files();
}

/* An entry too large for the chain limit is refused; it is sized from its
 * encoded length, so it is never decoded or buffered. */
static void managed_store_refuses_an_oversized_entry_before_decoding(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  /* Valid base64 (zeros) whose decoded size alone exceeds the limit. */
  const size_t big_len = 128u * 1024u;
  uint8_t* big = malloc(big_len);
  assert_non_null(big);
  memset(big, 'A', big_len);
  az_span entry[1] = { az_span_create(big, (int32_t)big_len) };
  az_iot_issued_certificate issued = { .certificates = entry, .count = 1 };
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, prov.base.vtable->store_issued_certificate(&prov.base, &issued));
  assert_false(prov.has_operational);

  az_iot_certificate_provider_managed_deinit(&prov);
  free(big);
  remove_test_files();
}

/* A chain on disk that no longer matches the key (the key was replaced) is not
 * served as the operational identity after a restart. */
static void managed_a_persisted_chain_for_another_key_is_not_used(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));
  az_iot_certificate_provider_managed_deinit(&prov);

  /* Lose the key: init generates a new one, which the stored chain does not certify. */
  assert_int_equal(0, remove(OP_KEY));
  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);
  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, prov2.base.vtable->load(&prov2.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(good);
  remove_test_files();
}

/* Public key a CSR (base64 DER) was made for. Caller frees. */
static EVP_PKEY* csr_public_key(const char* csr_b64)
{
  X509_REQ* req = decode_csr(csr_b64);
  assert_non_null(req);
  EVP_PKEY* pk = X509_REQ_get_pubkey(req);
  assert_non_null(pk);
  X509_REQ_free(req);
  return pk;
}

/* Get a CSR from @p prov and return the key it was made for. Caller frees. */
static EVP_PKEY* request_csr_key(az_iot_certificate_provider_managed* prov)
{
  az_iot_certificate_signing_request csr;
  memset(&csr, 0, sizeof(csr));
  assert_int_equal(AZ_IOT_OK, prov->base.vtable->get_csr(&prov->base, "my-device-id", &csr));
  EVP_PKEY* pk = csr_public_key(csr.csr_base64);
  prov->base.vtable->release_csr(&prov->base, &csr);
  return pk;
}

static bool key_file_is(const char* path, EVP_PKEY* expected)
{
  BIO* b = BIO_new_file(path, "rb");
  assert_non_null(b);
  EVP_PKEY* k = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
  BIO_free(b);
  assert_non_null(k);
  bool same = EVP_PKEY_eq(k, expected) == 1;
  EVP_PKEY_free(k);
  return same;
}

static az_iot_result store_one(az_iot_certificate_provider_managed* prov, const char* cert_b64)
{
  az_span chain[1] = { az_span_create_from_str((char*)(uintptr_t)cert_b64) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  return prov->base.vtable->store_issued_certificate(&prov->base, &issued);
}

static az_iot_certificate_provider_managed_options rotation_options(void)
{
  az_iot_certificate_provider_managed_options opts = test_options();
  opts.rotate_key_on_csr_request = true;
  return opts;
}

/* Default: CSRs are over the operational key, which is never replaced. */
static void managed_csrs_use_the_operational_key_by_default(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  EVP_PKEY* op = (EVP_PKEY*)prov.operational_key;

  EVP_PKEY* k1 = request_csr_key(&prov);
  EVP_PKEY* k2 = request_csr_key(&prov);
  assert_int_equal(1, EVP_PKEY_eq(k1, op));
  assert_int_equal(1, EVP_PKEY_eq(k2, op));
  assert_null(prov.pending_key);

  char* cert = make_cert_base64(op, 1);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, cert));
  assert_ptr_equal(op, prov.operational_key);
  assert_true(key_file_is(OP_KEY, op));

  az_iot_certificate_provider_managed_deinit(&prov);
  free(cert);
  EVP_PKEY_free(k1);
  EVP_PKEY_free(k2);
  remove_test_files();
}

/* An oversized chain file is never served, so a rotation does not back it up
 * (nor read it past the size bound) and still completes. */
static void managed_rotation_does_not_back_up_an_oversized_chain(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  EVP_PKEY* csr_key = request_csr_key(&prov);

  FILE* f = fopen(OP_CERT, "wb");
  assert_non_null(f);
  for (size_t i = 0; i < 65u * 1024u; ++i)
  {
    assert_int_not_equal(EOF, fputc('A', f));
  }
  assert_int_equal(0, fclose(f));

  char* renewed = make_cert_base64(csr_key, 2);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, renewed));
  assert_true(key_file_is(OP_KEY, csr_key));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, renewed);
  assert_int_equal(0, count_temp_files());

  az_iot_certificate_provider_managed_deinit(&prov);
  free(on_disk);
  free(renewed);
  EVP_PKEY_free(csr_key);
  remove_test_files();
}

/* With rotation, every CSR carries a new key, never the one currently in use. */
static void managed_each_csr_uses_a_new_key(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  EVP_PKEY* k1 = request_csr_key(&prov);
  EVP_PKEY* k2 = request_csr_key(&prov);
  assert_int_not_equal(1, EVP_PKEY_eq(k1, k2));
  assert_int_not_equal(1, EVP_PKEY_eq(k1, (EVP_PKEY*)prov.operational_key));
  assert_int_not_equal(1, EVP_PKEY_eq(k2, (EVP_PKEY*)prov.operational_key));
  /* The key file still holds the current key until a chain arrives. */
  assert_true(key_file_is(OP_KEY, (EVP_PKEY*)prov.operational_key));

  /* Newest wins: a chain for the superseded CSR is refused and leaves the
   * newest pending, which then completes. */
  char* stale = make_cert_base64(k1, 1);
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, store_one(&prov, stale));
  assert_non_null(prov.pending_key);
  char* fresh = make_cert_base64(k2, 2);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, fresh));
  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov.operational_key, k2));
  assert_true(key_file_is(OP_KEY, k2));

  az_iot_certificate_provider_managed_deinit(&prov);
  free(fresh);
  free(stale);
  EVP_PKEY_free(k1);
  EVP_PKEY_free(k2);
  remove_test_files();
}

/* A chain for the CSR key makes that key the operational key, on disk and in
 * memory, and the pair survives a restart. */
static void managed_a_chain_for_the_csr_key_rotates_the_key(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));

  EVP_PKEY* old_key = EVP_PKEY_dup((EVP_PKEY*)prov.operational_key);
  assert_non_null(old_key);
  EVP_PKEY* csr_key = request_csr_key(&prov);
  char* cert = make_cert_base64(csr_key, 1);
#if !defined(_WIN32)
  /* The replacement is owner-only even if the old key file was not. */
  assert_int_equal(0, chmod(OP_KEY, 0644));
#endif
  assert_int_equal(AZ_IOT_OK, store_one(&prov, cert));

  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov.operational_key, csr_key));
  assert_null(prov.pending_key);
  assert_true(key_file_is(OP_KEY, csr_key));
  assert_false(key_file_is(OP_KEY, old_key));
#if !defined(_WIN32)
  struct stat st;
  assert_int_equal(0, stat(OP_KEY, &st));
  assert_int_equal(0600, st.st_mode & 0777);
#endif
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_true(prov2.has_operational);
  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov2.operational_key, csr_key));
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(cert);
  EVP_PKEY_free(csr_key);
  EVP_PKEY_free(old_key);
  remove_test_files();
}

/* A chain for neither key changes nothing, and the pending CSR can still
 * complete afterwards. */
static void managed_a_refused_chain_keeps_the_pair_and_the_pending_csr(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* first = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, first));

  EVP_PKEY* csr_key = request_csr_key(&prov);
  EVP_PKEY* other = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  assert_non_null(other);
  char* foreign = make_cert_base64(other, 2);
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, store_one(&prov, foreign));
  assert_true(key_file_is(OP_KEY, (EVP_PKEY*)prov.operational_key));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, first);
  assert_int_equal(0, count_temp_files());

  char* renewed = make_cert_base64(csr_key, 3);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, renewed));
  assert_true(key_file_is(OP_KEY, csr_key));
  az_iot_certificate_provider_managed_deinit(&prov);

  free(on_disk);
  free(renewed);
  free(foreign);
  free(first);
  EVP_PKEY_free(other);
  EVP_PKEY_free(csr_key);
  remove_test_files();
}

/* The pending key is memory-only: after a restart its chain is refused. */
static void managed_a_pending_key_does_not_survive_a_restart(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  EVP_PKEY* csr_key = request_csr_key(&prov);
  az_iot_certificate_provider_managed_deinit(&prov);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_null(prov2.pending_key);
  char* cert = make_cert_base64(csr_key, 1);
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, store_one(&prov2, cert));
  assert_false(prov2.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(cert);
  EVP_PKEY_free(csr_key);
  remove_test_files();
}

/* A chain for the current key is still accepted without rotating, and leaves
 * the pending CSR in place. */
static void managed_a_chain_for_the_current_key_does_not_rotate(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  EVP_PKEY* current = EVP_PKEY_dup((EVP_PKEY*)prov.operational_key);
  assert_non_null(current);
  EVP_PKEY* csr_key = request_csr_key(&prov);

  char* cert = make_cert_base64(current, 1);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, cert));
  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov.operational_key, current));
  assert_true(key_file_is(OP_KEY, current));
  assert_non_null(prov.pending_key);
  az_iot_certificate_provider_managed_deinit(&prov);

  free(cert);
  EVP_PKEY_free(csr_key);
  EVP_PKEY_free(current);
  remove_test_files();
}

#if defined(_WIN32)
/* True when @p path has a protected DACL (nothing inherited) granting full
 * access to exactly Owner Rights and SYSTEM, and nothing else. */
static bool has_owner_only_dacl(const char* path)
{
  PACL dacl = NULL;
  PSECURITY_DESCRIPTOR sd = NULL;
  if (GetNamedSecurityInfoA(
          path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd)
      != ERROR_SUCCESS)
  {
    return false;
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  ACL_SIZE_INFORMATION info = { 0 };
  bool ok = dacl != NULL && GetSecurityDescriptorControl(sd, &control, &revision)
      && (control & SE_DACL_PROTECTED) != 0
      && GetAclInformation(dacl, &info, (DWORD)sizeof(info), AclSizeInformation)
      && info.AceCount == 2;

  BYTE owner_rights[SECURITY_MAX_SID_SIZE];
  BYTE system_sid[SECURITY_MAX_SID_SIZE];
  DWORD n1 = (DWORD)sizeof(owner_rights);
  DWORD n2 = (DWORD)sizeof(system_sid);
  ok = ok && CreateWellKnownSid(WinCreatorOwnerRightsSid, NULL, owner_rights, &n1)
      && CreateWellKnownSid(WinLocalSystemSid, NULL, system_sid, &n2);
  bool saw_owner = false;
  bool saw_system = false;
  for (DWORD i = 0; ok && i < info.AceCount; ++i)
  {
    ACCESS_ALLOWED_ACE* ace = NULL;
    ok = GetAce(dacl, i, (LPVOID*)&ace) && ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE
        && (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
    PSID sid = ok ? (PSID)&ace->SidStart : NULL;
    if (ok && EqualSid(sid, owner_rights))
    {
      saw_owner = true;
    }
    else if (ok && EqualSid(sid, system_sid))
    {
      saw_system = true;
    }
    else
    {
      ok = false;
    }
  }
  LocalFree(sd);
  return ok && saw_owner && saw_system;
}

/* The key and chain carry an owner-only DACL, and reload after a restart. */
static void managed_written_files_have_an_owner_only_dacl(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  assert_true(has_owner_only_dacl(OP_KEY));

  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));
  assert_true(has_owner_only_dacl(OP_CERT));
  az_iot_certificate_provider_managed_deinit(&prov);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_true(prov2.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov2);
  free(good);
  remove_test_files();
}
#endif

#if !defined(_WIN32)
#define LINK_TARGET "az_iot_managed_test_link_target.pem"
#define LINK_TARGET_KEY "az_iot_managed_test_link_target_key.pem"

static mode_t file_mode(const char* path)
{
  struct stat st;
  assert_int_equal(0, lstat(path, &st));
  return st.st_mode;
}

/* The key and chain are exactly 0600 whatever the umask, including one that
 * removes the owner's own bits, and reload after a restart. */
static void managed_written_files_are_owner_only(void** state)
{
  (void)state;
  static const mode_t masks[] = { 022, 0777 };
  for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i)
  {
    remove_test_files();
    mode_t old_umask = umask(masks[i]);
    az_iot_certificate_provider_managed_options opts = test_options();
    az_iot_certificate_provider_managed prov;
    assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
    assert_int_equal(0600, file_mode(OP_KEY) & 0777);

    char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
    az_span chain[1] = { az_span_create_from_str(good) };
    az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
    assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));
    assert_int_equal(0600, file_mode(OP_CERT) & 0777);
    az_iot_certificate_provider_managed_deinit(&prov);
    (void)umask(old_umask);

    az_iot_certificate_provider_managed prov2;
    assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
    assert_true(prov2.has_operational);
    az_iot_certificate_provider_managed_deinit(&prov2);
    free(good);
  }
  remove_test_files();
}

/* A write that fails part-way leaves the previous chain in place and no
 * temporary file behind. The file-size limit makes write() fail with EFBIG. */
static void managed_a_failed_write_keeps_the_previous_chain(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain1[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued1 = { .certificates = chain1, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued1));

  char* next = make_cert_base64((EVP_PKEY*)prov.operational_key, 2);
  az_span chain2[1] = { az_span_create_from_str(next) };
  az_iot_issued_certificate issued2 = { .certificates = chain2, .count = 1 };
  struct rlimit saved;
  assert_int_equal(0, getrlimit(RLIMIT_FSIZE, &saved));
  struct rlimit small = saved;
  small.rlim_cur = 64;
  void (*saved_handler)(int) = signal(SIGXFSZ, SIG_IGN);
  assert_int_equal(0, setrlimit(RLIMIT_FSIZE, &small));
  az_iot_result rc = prov.base.vtable->store_issued_certificate(&prov.base, &issued2);
  assert_int_equal(0, setrlimit(RLIMIT_FSIZE, &saved));
  (void)signal(SIGXFSZ, saved_handler);
  assert_int_equal(AZ_IOT_ERR_INTERNAL, rc);

  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, good);
  assert_int_equal(0, count_temp_files());
  assert_true(prov.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov);

  free(on_disk);
  free(next);
  free(good);
  remove_test_files();
}

/* A rename that fails leaves the destination as it was and no temporary file
 * behind. A directory at the destination cannot be replaced by a file. */
static void managed_a_failed_rename_keeps_the_destination(void** state)
{
  (void)state;
  remove_test_files();
  assert_int_equal(0, mkdir(OP_CERT, 0700));
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  assert_false(prov.has_operational);

  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  assert_int_equal(
      AZ_IOT_ERR_INTERNAL, prov.base.vtable->store_issued_certificate(&prov.base, &issued));

  struct stat st;
  assert_int_equal(0, stat(OP_CERT, &st));
  assert_true(S_ISDIR(st.st_mode));
  assert_int_equal(0, count_temp_files());
  assert_false(prov.has_operational);
  az_iot_certificate_provider_managed_deinit(&prov);

  free(good);
  assert_int_equal(0, rmdir(OP_CERT));
  remove_test_files();
}

/* init() loads an existing key without rewriting it, so its mode is kept. */
static void managed_init_keeps_an_existing_key_file_mode(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  az_iot_certificate_provider_managed_deinit(&prov);

  assert_int_equal(0, chmod(OP_KEY, 0640));
  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_int_equal(0640, file_mode(OP_KEY) & 0777);
  az_iot_certificate_provider_managed_deinit(&prov2);
  remove_test_files();
}

/* A link at either destination is replaced, never written through. */
static void managed_writes_replace_a_link_instead_of_following_it(void** state)
{
  (void)state;
  remove_test_files();
  (void)remove(LINK_TARGET);
  (void)remove(LINK_TARGET_KEY);
  FILE* v = fopen(LINK_TARGET, "wb");
  assert_non_null(v);
  assert_int_equal(6, (int)fwrite("target", 1, 6, v));
  assert_int_equal(0, fclose(v));
  assert_int_equal(0, symlink(LINK_TARGET, OP_CERT));
  assert_int_equal(0, symlink(LINK_TARGET_KEY, OP_KEY)); /* dangling: init generates a key */

  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  assert_true(S_ISREG(file_mode(OP_KEY)));
  assert_false(file_exists(LINK_TARGET_KEY));

  char* good = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  az_span chain[1] = { az_span_create_from_str(good) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  assert_int_equal(AZ_IOT_OK, prov.base.vtable->store_issued_certificate(&prov.base, &issued));
  assert_true(S_ISREG(file_mode(OP_CERT)));
  az_iot_certificate_provider_managed_deinit(&prov);

  char buf[16] = { 0 };
  v = fopen(LINK_TARGET, "rb");
  assert_non_null(v);
  assert_int_equal(6, (int)fread(buf, 1, sizeof(buf) - 1, v));
  assert_int_equal(0, fclose(v));
  assert_string_equal(buf, "target");

  free(good);
  (void)remove(LINK_TARGET);
  remove_test_files();
}

/* A previous chain that exists but cannot be read cannot be backed up, so a
 * rotation is refused rather than committed without a way back. Permissions do
 * not restrict root, so the test is skipped there. */
static void managed_an_unreadable_previous_chain_blocks_rotation(void** state)
{
  (void)state;
  if (geteuid() == 0)
  {
    skip();
  }
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* first = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, first));
  EVP_PKEY* old_key = EVP_PKEY_dup((EVP_PKEY*)prov.operational_key);
  assert_non_null(old_key);
  EVP_PKEY* csr_key = request_csr_key(&prov);
  assert_int_equal(0, chmod(OP_CERT, 0));

  char* renewed = make_cert_base64(csr_key, 2);
  assert_int_equal(AZ_IOT_ERR_INTERNAL, store_one(&prov, renewed));
  assert_true(key_file_is(OP_KEY, old_key));
  assert_non_null(prov.pending_key);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov);

  assert_int_equal(0, chmod(OP_CERT, 0600));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, first);

  free(on_disk);
  free(renewed);
  free(first);
  EVP_PKEY_free(csr_key);
  EVP_PKEY_free(old_key);
  remove_test_files();
}

#endif

static void make_dir(const char* path)
{
#if defined(_WIN32)
  assert_true(CreateDirectoryA(path, NULL) != 0);
#else
  assert_int_equal(0, mkdir(path, 0700));
#endif
}

static void remove_dir(const char* path)
{
#if defined(_WIN32)
  assert_true(RemoveDirectoryA(path) != 0);
#else
  assert_int_equal(0, rmdir(path));
#endif
}

/* Names the provider gives staged files next to a path, and look-alikes it
 * must leave alone. */
#if defined(_WIN32)
#define STAGED_1(p) p ".aziot-0123abcd.tmp"
#define STAGED_2(p) p ".aziot-DEADBEEF.tmp"
#define NOT_STAGED_1(p) p ".aziot-0123abc.tmp"
#define NOT_STAGED_2(p) p ".aziot-0123abcg.tmp"
#else
#define STAGED_1(p) p ".aziot-abc123"
#define STAGED_2(p) p ".aziot-ZZZZZZ"
#define NOT_STAGED_1(p) p ".aziot-abc12"
#define NOT_STAGED_2(p) p ".aziot-abc1234"
#endif

/* Write @p cert_b64 (base64 DER) to @p path as a PEM chain of one. */
static void write_chain_file(const char* path, const char* cert_b64)
{
  FILE* f = fopen(path, "wb");
  assert_non_null(f);
  fputs("-----BEGIN CERTIFICATE-----\n", f);
  for (size_t off = 0, n = strlen(cert_b64); off < n; off += 64)
  {
    size_t chunk = n - off < 64 ? n - off : 64;
    assert_int_equal((int)chunk, (int)fwrite(cert_b64 + off, 1, chunk, f));
    fputs("\n", f);
  }
  fputs("-----END CERTIFICATE-----\n", f);
  assert_int_equal(0, fclose(f));
}

static void write_key_pem(const char* path, EVP_PKEY* key)
{
  BIO* b = BIO_new_file(path, "wb");
  assert_non_null(b);
  assert_int_equal(1, PEM_write_bio_PrivateKey(b, key, NULL, NULL, 0, NULL, NULL));
  BIO_free(b);
}

/* A chain that cannot be replaced changes nothing, and the key is never
 * touched. A directory at the chain path makes it unreplaceable. */
static void managed_a_chain_that_cannot_be_replaced_changes_nothing(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  EVP_PKEY* old_key = EVP_PKEY_dup((EVP_PKEY*)prov.operational_key);
  assert_non_null(old_key);
  EVP_PKEY* csr_key = request_csr_key(&prov);
  make_dir(OP_CERT);

  char* cert = make_cert_base64(csr_key, 1);
  assert_int_equal(AZ_IOT_ERR_INTERNAL, store_one(&prov, cert));
  assert_true(key_file_is(OP_KEY, old_key));
  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov.operational_key, old_key));
  assert_non_null(prov.pending_key);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov);

  remove_dir(OP_CERT);
  free(cert);
  EVP_PKEY_free(csr_key);
  EVP_PKEY_free(old_key);
  remove_test_files();
}

/* If the key cannot follow the new chain, the previous chain is put back. A
 * directory at the key path makes the key rename fail; with no key file left
 * there, the operational identity is no longer served until a later store puts
 * the key back. */
static void managed_a_failed_key_rename_restores_the_chain(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* first = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, first));
  EVP_PKEY* old_key = EVP_PKEY_dup((EVP_PKEY*)prov.operational_key);
  assert_non_null(old_key);
  EVP_PKEY* csr_key = request_csr_key(&prov);
  assert_int_equal(0, remove(OP_KEY));
  make_dir(OP_KEY);

  char* renewed = make_cert_base64(csr_key, 2);
  assert_int_equal(AZ_IOT_ERR_INTERNAL, store_one(&prov, renewed));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, first);
  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov.operational_key, old_key));
  assert_non_null(prov.pending_key);
  assert_false(prov.has_operational);
  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, prov.base.vtable->load(&prov.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));
  assert_int_equal(0, count_temp_files());

  /* A chain for the current key needs the key file back: refused while it
   * cannot be written, then the key is rewritten and the chain served. */
  assert_int_not_equal(AZ_IOT_OK, store_one(&prov, first));
  assert_false(prov.has_operational);
  remove_dir(OP_KEY);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, first));
  assert_true(prov.has_operational);
  assert_true(key_file_is(OP_KEY, old_key));
  az_iot_certificate_provider_managed_deinit(&prov);

  free(on_disk);
  free(renewed);
  free(first);
  EVP_PKEY_free(csr_key);
  EVP_PKEY_free(old_key);
  remove_test_files();
}

/* Same, with no previous chain: the new one is removed rather than left
 * beside a key it does not certify. */
static void managed_a_failed_key_rename_removes_a_first_chain(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  EVP_PKEY* csr_key = request_csr_key(&prov);
  assert_int_equal(0, remove(OP_KEY));
  make_dir(OP_KEY);

  char* cert = make_cert_base64(csr_key, 1);
  assert_int_equal(AZ_IOT_ERR_INTERNAL, store_one(&prov, cert));
  assert_false(file_exists(OP_CERT));
  assert_false(prov.has_operational);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov);

  remove_dir(OP_KEY);
  free(cert);
  EVP_PKEY_free(csr_key);
  remove_test_files();
}

static void touch(const char* path)
{
  FILE* t = fopen(path, "wb");
  assert_non_null(t);
  assert_int_equal(0, fclose(t));
}

/* init() deletes the temporary files a stopped process left staged next to
 * the key or chain, and nothing else. */
static void managed_init_removes_only_its_own_stale_temp_files(void** state)
{
  (void)state;
  remove_test_files();
  static const char* const stale[] = { STAGED_1(OP_KEY), STAGED_2(OP_CERT) };
  static const char* const kept[]
      = { OP_KEY ".backup", NOT_STAGED_1(OP_KEY), NOT_STAGED_2(OP_CERT), STAGED_1("x" OP_KEY) };
  for (size_t i = 0; i < sizeof(stale) / sizeof(stale[0]); ++i)
  {
    touch(stale[i]);
  }
  for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); ++i)
  {
    touch(kept[i]);
  }

  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  az_iot_certificate_provider_managed_deinit(&prov);

  for (size_t i = 0; i < sizeof(stale) / sizeof(stale[0]); ++i)
  {
    assert_false(file_exists(stale[i]));
  }
  for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); ++i)
  {
    assert_true(file_exists(kept[i]));
    assert_int_equal(0, remove(kept[i]));
  }
  remove_test_files();
}

#if !defined(_WIN32)
/* Only regular files are treated as staged: a FIFO with a staged name is not
 * read (it would block init()), and a directory is not deleted. POSIX only. */
static void managed_init_ignores_staged_names_that_are_not_files(void** state)
{
  (void)state;
  remove_test_files();
  assert_int_equal(0, mkfifo(STAGED_1(OP_KEY), 0600));
  make_dir(STAGED_2(OP_CERT));

  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  assert_non_null(prov.operational_key);
  az_iot_certificate_provider_managed_deinit(&prov);

  struct stat st;
  assert_int_equal(0, lstat(STAGED_1(OP_KEY), &st)); /* fopen() would block on a FIFO */
  assert_true(S_ISFIFO(st.st_mode));
  assert_int_equal(0, lstat(STAGED_2(OP_CERT), &st));
  assert_true(S_ISDIR(st.st_mode));
  assert_int_equal(0, remove(STAGED_1(OP_KEY)));
  remove_dir(STAGED_2(OP_CERT));
  remove_test_files();
}
#endif

/* A configured file that a differently spelled path puts in the key's temp
 * namespace is matched by identity, so the sweep never deletes it. */
static void managed_init_never_sweeps_a_configured_file_under_an_alias(void** state)
{
  (void)state;
  remove_test_files();
  touch(STAGED_1(OP_KEY));
  az_iot_certificate_provider_managed_options opts = test_options();
  opts.operational_key_pem_path = "./" OP_KEY;
  opts.trusted_ca_pem_path = STAGED_1(OP_KEY);
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  az_iot_certificate_provider_managed_deinit(&prov);
  assert_true(file_exists(STAGED_1(OP_KEY)));
  assert_int_equal(0, remove(STAGED_1(OP_KEY)));
  remove_test_files();
}

/* A missing directory holds no staged files: init() still tries to generate
 * the key, and reports that it cannot write it rather than succeeding without
 * one. */
static void managed_init_in_a_missing_directory_fails_the_key_write(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  opts.operational_key_pem_path = "az_iot_managed_test_no_such_dir/key.pem";
  opts.operational_cert_pem_path = "az_iot_managed_test_no_such_dir/cert.pem";
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_ERR_INTERNAL, az_iot_certificate_provider_managed_init(&prov, &opts));
  remove_test_files();
}

/* A configured path inside the key's or chain's temporary-file namespace is
 * refused: init() would otherwise sweep it as a stale staged file. */
static void managed_init_refuses_a_path_in_a_temp_namespace(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed prov;
  az_iot_certificate_provider_managed_options opts = test_options();
  opts.operational_cert_pem_path = OP_KEY ".aziot-abc123";
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_managed_init(&prov, &opts));
  opts = test_options();
  opts.trusted_ca_pem_path = OP_CERT ".aziot-0123abcd.tmp";
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_managed_init(&prov, &opts));
  opts = test_options();
  opts.bootstrap_key_pem_path = OP_KEY ".aziot-";
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_managed_init(&prov, &opts));
  assert_false(file_exists(OP_KEY));
  remove_test_files();
}

/* A process stopped after renaming the new chain but before the key: init()
 * renames the staged new key into place, so the new pair is used. Recovery
 * does not depend on the rotation option. */
static void managed_init_finishes_a_rotation_stopped_after_the_chain(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* first = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, first));
  EVP_PKEY* csr_key = request_csr_key(&prov);
  EVP_PKEY* new_key = EVP_PKEY_dup((EVP_PKEY*)prov.pending_key);
  assert_non_null(new_key);
  az_iot_certificate_provider_managed_deinit(&prov);

  char* renewed = make_cert_base64(csr_key, 2);
  write_chain_file(OP_CERT, renewed);
  write_key_pem(STAGED_1(OP_KEY), new_key);
  write_chain_file(STAGED_2(OP_CERT), first);

  az_iot_certificate_provider_managed prov2;
  az_iot_certificate_provider_managed_options plain = test_options();
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &plain));
  assert_true(prov2.has_operational);
  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov2.operational_key, new_key));
  assert_true(key_file_is(OP_KEY, new_key));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, renewed);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(on_disk);
  free(renewed);
  free(first);
  EVP_PKEY_free(new_key);
  EVP_PKEY_free(csr_key);
  remove_test_files();
}

/* Same, but the staged new key is gone (it is never kept after a failed store):
 * init() puts the staged previous chain back, so the previous pair is used. */
static void managed_init_undoes_a_rotation_it_cannot_finish(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* first = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, first));
  EVP_PKEY* old_key = EVP_PKEY_dup((EVP_PKEY*)prov.operational_key);
  assert_non_null(old_key);
  EVP_PKEY* csr_key = request_csr_key(&prov);
  az_iot_certificate_provider_managed_deinit(&prov);

  char* renewed = make_cert_base64(csr_key, 2);
  write_chain_file(OP_CERT, renewed);
  write_chain_file(STAGED_1(OP_CERT), first);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_true(prov2.has_operational);
  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov2.operational_key, old_key));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, first);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(on_disk);
  free(renewed);
  free(first);
  EVP_PKEY_free(csr_key);
  EVP_PKEY_free(old_key);
  remove_test_files();
}

/* A staged previous chain is kept while the key file exists but does not load:
 * no key is generated over it, and once the key loads again the pair is
 * restored. With the key file missing, nothing can pair with it: it is
 * discarded and a new key is generated. */
static void managed_init_keeps_a_staged_chain_while_the_key_does_not_load(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* first = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, first));
  EVP_PKEY* old_key = EVP_PKEY_dup((EVP_PKEY*)prov.operational_key);
  assert_non_null(old_key);
  EVP_PKEY* csr_key = request_csr_key(&prov);
  az_iot_certificate_provider_managed_deinit(&prov);

  char* renewed = make_cert_base64(csr_key, 2);
  write_chain_file(OP_CERT, renewed);
  write_chain_file(STAGED_1(OP_CERT), first);
  write_chain_file(OP_KEY, renewed); /* exists, but is not a key */

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);
  assert_null(prov2.operational_key);
  assert_int_equal(1, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov2);

  write_key_pem(OP_KEY, old_key);
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_true(prov2.has_operational);
  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov2.operational_key, old_key));
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, first);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov2);

  write_chain_file(OP_CERT, renewed);
  write_chain_file(STAGED_1(OP_CERT), first);
  assert_int_equal(0, remove(OP_KEY));
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_non_null(prov2.operational_key);
  assert_int_not_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov2.operational_key, old_key));
  assert_false(prov2.has_operational);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov2);

  free(on_disk);
  free(renewed);
  free(first);
  EVP_PKEY_free(csr_key);
  EVP_PKEY_free(old_key);
  remove_test_files();
}

/* Recovery that cannot complete keeps the staged files, and the next init()
 * finishes it. A directory at the chain path stops the first attempt. */
static void managed_init_keeps_staged_files_until_recovery_succeeds(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = test_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  char* first = make_cert_base64((EVP_PKEY*)prov.operational_key, 1);
  assert_int_equal(AZ_IOT_OK, store_one(&prov, first));
  az_iot_certificate_provider_managed_deinit(&prov);

  assert_int_equal(0, remove(OP_CERT));
  make_dir(OP_CERT);
  write_chain_file(STAGED_1(OP_CERT), first);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);
  assert_true(file_exists(STAGED_1(OP_CERT)));
  az_iot_certificate_provider_managed_deinit(&prov2);

  remove_dir(OP_CERT);
  az_iot_certificate_provider_managed prov3;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov3, &opts));
  assert_true(prov3.has_operational);
  char* on_disk = read_chain_base64(OP_CERT);
  assert_string_equal(on_disk, first);
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov3);

  free(on_disk);
  free(first);
  remove_test_files();
}

/* With no loadable key and a recovery that cannot complete, init() still
 * succeeds in bootstrap-only mode and generates no key over the one a retry
 * restores. A directory at the key path blocks the first attempt. */
static void managed_init_stays_bootstrap_only_while_recovery_is_blocked(void** state)
{
  (void)state;
  remove_test_files();
  az_iot_certificate_provider_managed_options opts = rotation_options();
  az_iot_certificate_provider_managed prov;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov, &opts));
  EVP_PKEY* csr_key = request_csr_key(&prov);
  EVP_PKEY* new_key = EVP_PKEY_dup((EVP_PKEY*)prov.pending_key);
  assert_non_null(new_key);
  az_iot_certificate_provider_managed_deinit(&prov);

  char* renewed = make_cert_base64(csr_key, 1);
  write_chain_file(OP_CERT, renewed);
  write_key_pem(STAGED_1(OP_KEY), new_key);
  assert_int_equal(0, remove(OP_KEY));
  make_dir(OP_KEY);

  az_iot_certificate_provider_managed prov2;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov2, &opts));
  assert_false(prov2.has_operational);
  az_iot_certificate_material mat;
  memset(&mat, 0, sizeof(mat));
  assert_int_equal(AZ_IOT_OK, prov2.base.vtable->load(&prov2.base, AZ_IOT_CRED_BOOTSTRAP, 0, &mat));
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, prov2.base.vtable->load(&prov2.base, AZ_IOT_CRED_OPERATIONAL, 0, &mat));
  az_iot_certificate_signing_request csr;
  memset(&csr, 0, sizeof(csr));
  assert_int_equal(
      AZ_IOT_ERR_NOT_INITIALIZED, prov2.base.vtable->get_csr(&prov2.base, "my-device-id", &csr));
  assert_true(file_exists(STAGED_1(OP_KEY)));
  az_iot_certificate_provider_managed_deinit(&prov2);

  remove_dir(OP_KEY);
  az_iot_certificate_provider_managed prov3;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_managed_init(&prov3, &opts));
  assert_true(prov3.has_operational);
  assert_int_equal(1, EVP_PKEY_eq((EVP_PKEY*)prov3.operational_key, new_key));
  assert_int_equal(0, count_temp_files());
  az_iot_certificate_provider_managed_deinit(&prov3);

  free(renewed);
  EVP_PKEY_free(new_key);
  EVP_PKEY_free(csr_key);
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
    cmocka_unit_test(managed_a_stored_chain_that_is_not_a_certificate_is_refused),
    cmocka_unit_test(managed_a_failed_store_keeps_the_previous_certificate),
    cmocka_unit_test(managed_store_refuses_a_chain_for_another_key),
    cmocka_unit_test(managed_a_persisted_chain_for_another_key_is_not_used),
    cmocka_unit_test(managed_store_refuses_a_chain_with_a_bad_trailing_entry),
    cmocka_unit_test(managed_store_refuses_a_chain_with_an_empty_entry),
    cmocka_unit_test(managed_a_persisted_chain_with_a_truncated_entry_is_not_used),
    cmocka_unit_test(managed_a_chain_followed_by_other_text_is_refused),
    cmocka_unit_test(managed_a_certificate_with_trailing_bytes_is_refused),
    cmocka_unit_test(managed_a_persisted_chain_with_trailing_whitespace_is_used),
    cmocka_unit_test(managed_an_oversized_persisted_chain_is_not_used),
    cmocka_unit_test(managed_the_chain_size_bound_is_exactly_64_kib),
    cmocka_unit_test(managed_store_refuses_an_oversized_chain),
    cmocka_unit_test(managed_store_refuses_an_oversized_entry_before_decoding),
#if defined(_WIN32)
    cmocka_unit_test(managed_written_files_have_an_owner_only_dacl),
#endif
    cmocka_unit_test(managed_options_default_has_no_paths_and_no_rotation),
    cmocka_unit_test(managed_csrs_use_the_operational_key_by_default),
    cmocka_unit_test(managed_each_csr_uses_a_new_key),
    cmocka_unit_test(managed_rotation_does_not_back_up_an_oversized_chain),
    cmocka_unit_test(managed_a_chain_for_the_csr_key_rotates_the_key),
    cmocka_unit_test(managed_a_refused_chain_keeps_the_pair_and_the_pending_csr),
    cmocka_unit_test(managed_a_pending_key_does_not_survive_a_restart),
    cmocka_unit_test(managed_a_chain_for_the_current_key_does_not_rotate),
#if !defined(_WIN32)
    cmocka_unit_test(managed_written_files_are_owner_only),
    cmocka_unit_test(managed_init_keeps_an_existing_key_file_mode),
    cmocka_unit_test(managed_a_failed_write_keeps_the_previous_chain),
    cmocka_unit_test(managed_a_failed_rename_keeps_the_destination),
    cmocka_unit_test(managed_writes_replace_a_link_instead_of_following_it),
    cmocka_unit_test(managed_an_unreadable_previous_chain_blocks_rotation),
#endif
    cmocka_unit_test(managed_a_chain_that_cannot_be_replaced_changes_nothing),
    cmocka_unit_test(managed_a_failed_key_rename_restores_the_chain),
    cmocka_unit_test(managed_a_failed_key_rename_removes_a_first_chain),
    cmocka_unit_test(managed_init_removes_only_its_own_stale_temp_files),
    cmocka_unit_test(managed_init_refuses_a_path_in_a_temp_namespace),
    cmocka_unit_test(managed_init_in_a_missing_directory_fails_the_key_write),
    cmocka_unit_test(managed_init_never_sweeps_a_configured_file_under_an_alias),
#if !defined(_WIN32)
    cmocka_unit_test(managed_init_ignores_staged_names_that_are_not_files),
#endif
    cmocka_unit_test(managed_init_finishes_a_rotation_stopped_after_the_chain),
    cmocka_unit_test(managed_init_undoes_a_rotation_it_cannot_finish),
    cmocka_unit_test(managed_init_keeps_a_staged_chain_while_the_key_does_not_load),
    cmocka_unit_test(managed_init_keeps_staged_files_until_recovery_succeeds),
    cmocka_unit_test(managed_init_stays_bootstrap_only_while_recovery_is_blocked),
    cmocka_unit_test(the_sign_hook_is_not_offered_by_this_provider),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
