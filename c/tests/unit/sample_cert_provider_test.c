// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for the samples' app-owned certificate provider
 * (samples/authentication/custom_certificate_provider/sample_cert_provider.c
 * + sample_csr_openssl.c). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* umask, chmod, geteuid, setrlimit */
#endif

#include "sample_cert_provider.h"
#include "sample_csr_backend.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "../support/test_x509.h"

#define BOOT_CRT "az_iot_sample_test_boot.pem"
#define BOOT_KEY "az_iot_sample_test_boot_key.pem"
#define OP_KEY "az_iot_sample_test_op_key.pem"
#define OP_CERT "az_iot_sample_test_op_cert.pem"

static void remove_test_files(void)
{
  (void)remove(BOOT_CRT);
  (void)remove(BOOT_KEY);
  (void)remove(OP_KEY);
  (void)remove(OP_CERT);
}

static void write_text(const char* path, const char* text)
{
  FILE* f = fopen(path, "wb");
  assert_non_null(f);
  assert_true(fputs(text, f) >= 0);
  assert_int_equal(0, fclose(f));
}

static EVP_PKEY* new_key(void)
{
  EVP_PKEY* k = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  assert_non_null(k);
  return k;
}

static void write_key(const char* path, EVP_PKEY* key)
{
  BIO* b = BIO_new_file(path, "wb");
  assert_non_null(b);
  assert_int_equal(1, PEM_write_bio_PrivateKey(b, key, NULL, NULL, 0, NULL, NULL));
  BIO_free(b);
}

static EVP_PKEY* read_key(const char* path)
{
  BIO* b = BIO_new_file(path, "rb");
  assert_non_null(b);
  EVP_PKEY* k = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
  BIO_free(b);
  assert_non_null(k);
  return k;
}

/* A self-issued certificate for @p subject_key. */
static X509* make_cert(EVP_PKEY* subject_key, long serial)
{
  EVP_PKEY* issuer = new_key();
  X509* x = X509_new();
  assert_non_null(x);
  ASN1_INTEGER_set(X509_get_serialNumber(x), serial);
  X509_gmtime_adj(X509_getm_notBefore(x), 0);
  X509_gmtime_adj(X509_getm_notAfter(x), 3600);
  assert_int_equal(1, X509_set_pubkey(x, subject_key));
  assert_int_equal(1, test_x509_set_self_signed_name(x, "t"));
  assert_true(X509_sign(x, issuer, EVP_sha256()) > 0);
  EVP_PKEY_free(issuer);
  return x;
}

static char* base64_of(const unsigned char* data, size_t len)
{
  char* b64 = (char*)malloc(((len + 2) / 3) * 4 + 1);
  assert_non_null(b64);
  int n = EVP_EncodeBlock((unsigned char*)b64, data, (int)len);
  assert_true(n >= 0);
  b64[n] = '\0';
  return b64;
}

/* base64 of the certificate's DER, followed by @p trailing extra bytes. */
static char* der_entry(X509* x, size_t trailing)
{
  unsigned char* der = NULL;
  int len = i2d_X509(x, &der);
  assert_true(len > 0);
  unsigned char* buf = (unsigned char*)malloc((size_t)len + trailing);
  assert_non_null(buf);
  memcpy(buf, der, (size_t)len);
  memset(buf + len, 0x05, trailing);
  char* b64 = base64_of(buf, (size_t)len + trailing);
  free(buf);
  OPENSSL_free(der);
  return b64;
}

/* base64 of the PEM certificate followed by @p suffix, padded with newlines to
 * @p pad_to bytes when larger (0: no padding). */
static char* pem_entry(X509* x, const char* suffix, size_t pad_to)
{
  BIO* m = BIO_new(BIO_s_mem());
  assert_non_null(m);
  assert_int_equal(1, PEM_write_bio_X509(m, x));
  char* data = NULL;
  long n = BIO_get_mem_data(m, &data);
  size_t total = (size_t)n + strlen(suffix);
  size_t size = total > pad_to ? total : pad_to;
  unsigned char* buf = (unsigned char*)malloc(size);
  assert_non_null(buf);
  memcpy(buf, data, (size_t)n);
  memcpy(buf + n, suffix, strlen(suffix));
  memset(buf + total, '\n', size - total);
  char* b64 = base64_of(buf, size);
  free(buf);
  BIO_free(m);
  return b64;
}

static int matches(const char* entry)
{
  return sample_csr_backend_cert_matches_key(OP_KEY, entry, strlen(entry));
}

/* A certificate for the key matches, sent as base64 of DER or of PEM. */
static void sample_cert_matches_key_in_both_wire_forms(void** state)
{
  (void)state;
  remove_test_files();
  EVP_PKEY* key = new_key();
  write_key(OP_KEY, key);
  X509* x = make_cert(key, 1);
  char* der = der_entry(x, 0);
  char* pem = pem_entry(x, "", 0);
  char* pem_ws = pem_entry(x, "\n \t\r\n", 0);
  assert_true(matches(der));
  assert_true(matches(pem));
  assert_true(matches(pem_ws));
  free(der);
  free(pem);
  free(pem_ws);
  X509_free(x);
  EVP_PKEY_free(key);
  remove_test_files();
}

/* A certificate for another key, a malformed entry, or a missing key file
 * does not match; no key is created. */
static void sample_cert_mismatch_or_malformed_does_not_match(void** state)
{
  (void)state;
  remove_test_files();
  EVP_PKEY* other = new_key();
  X509* x = make_cert(other, 1);
  char* der = der_entry(x, 0);
  assert_false(matches(der));
  FILE* f = fopen(OP_KEY, "rb");
  assert_null(f);

  EVP_PKEY* key = new_key();
  write_key(OP_KEY, key);
  assert_false(matches(der));
  assert_false(matches("!!!!"));
  assert_false(matches("AAAA"));
  assert_false(sample_csr_backend_cert_matches_key(OP_KEY, der, 0));
  free(der);
  X509_free(x);
  EVP_PKEY_free(other);
  EVP_PKEY_free(key);
  remove_test_files();
}

/* An entry must hold exactly one certificate: bytes after the DER, or text
 * after the PEM block, are refused. */
static void sample_cert_with_trailing_data_does_not_match(void** state)
{
  (void)state;
  remove_test_files();
  EVP_PKEY* key = new_key();
  write_key(OP_KEY, key);
  X509* x = make_cert(key, 1);
  char* der = der_entry(x, 3);
  char* pem_text = pem_entry(x, "junk\n", 0);
  char* pem_twice = NULL;
  {
    BIO* m = BIO_new(BIO_s_mem());
    assert_int_equal(1, PEM_write_bio_X509(m, x));
    assert_int_equal(1, PEM_write_bio_X509(m, x));
    char* data = NULL;
    long n = BIO_get_mem_data(m, &data);
    pem_twice = base64_of((const unsigned char*)data, (size_t)n);
    BIO_free(m);
  }
  assert_false(matches(der));
  assert_false(matches(pem_text));
  assert_false(matches(pem_twice));
  free(der);
  free(pem_text);
  free(pem_twice);
  X509_free(x);
  EVP_PKEY_free(key);
  remove_test_files();
}

/* Entries are bounded at 64 KiB of base64: exactly 64 KiB is read, one more
 * base64 quantum is refused. */
static void sample_cert_entry_bound_is_64_kib(void** state)
{
  (void)state;
  remove_test_files();
  EVP_PKEY* key = new_key();
  write_key(OP_KEY, key);
  X509* x = make_cert(key, 1);
  char* at_bound = pem_entry(x, "", (64u * 1024u / 4u) * 3u);
  char* over = pem_entry(x, "", (64u * 1024u / 4u) * 3u + 3u);
  assert_int_equal(64 * 1024, (int)strlen(at_bound));
  assert_int_equal(64 * 1024 + 4, (int)strlen(over));
  assert_true(matches(at_bound));
  assert_false(matches(over));
  free(at_bound);
  free(over);
  X509_free(x);
  EVP_PKEY_free(key);
  remove_test_files();
}

static void provider_init(sample_cert_provider* p)
{
  write_text(BOOT_CRT, "x");
  write_text(BOOT_KEY, "x");
  sample_cert_provider_options o = {
    .bootstrap_cert_path = BOOT_CRT,
    .bootstrap_key_path = BOOT_KEY,
    .trusted_ca_path = NULL,
    .operational_key_path = OP_KEY,
    .operational_cert_path = OP_CERT,
  };
  assert_int_equal(AZ_IOT_OK, sample_cert_provider_init(p, &o));
}

static az_iot_result store(sample_cert_provider* p, char* leaf, char* inter)
{
  az_span chain[2] = { az_span_create_from_str(leaf), az_span_create_from_str(inter) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 2 };
  return p->base.vtable->store_issued_certificate(&p->base, &issued);
}

/* Number of certificates in the stored chain; *leaf_ok when the first
 * certifies @p key. */
static int stored_chain(EVP_PKEY* key, int* leaf_ok)
{
  BIO* b = BIO_new_file(OP_CERT, "rb");
  assert_non_null(b);
  int count = 0;
  X509* x = NULL;
  *leaf_ok = 0;
  while ((x = PEM_read_bio_X509(b, NULL, NULL, NULL)) != NULL)
  {
    if (count == 0)
    {
      *leaf_ok = X509_check_private_key(x, key) == 1;
    }
    count++;
    X509_free(x);
  }
  BIO_free(b);
  return count;
}

/* Store refuses a chain for another key, or with an entry that is not one
 * certificate, and keeps the previous one; both wire forms are stored as a
 * PEM chain TLS can read. */
static void sample_store_checks_the_key_and_writes_both_forms(void** state)
{
  (void)state;
  remove_test_files();
  sample_cert_provider p;
  provider_init(&p);
  az_iot_certificate_signing_request csr = { 0 };
  assert_int_equal(AZ_IOT_OK, p.base.vtable->get_csr(&p.base, "dev", &csr));
  p.base.vtable->release_csr(&p.base, &csr);
  EVP_PKEY* key = read_key(OP_KEY);
  EVP_PKEY* other = new_key();
  X509* leaf = make_cert(key, 1);
  X509* inter = make_cert(other, 2);

  write_text(OP_CERT, "previous");
  char* wrong = der_entry(inter, 0);
  char* inter_der = der_entry(inter, 0);
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, store(&p, wrong, inter_der));
  FILE* f = fopen(OP_CERT, "rb");
  assert_non_null(f);
  char buf[16] = { 0 };
  assert_int_equal(8, (int)fread(buf, 1, sizeof(buf) - 1, f));
  assert_int_equal(0, fclose(f));
  assert_string_equal(buf, "previous");

  int leaf_ok = 0;
  char* leaf_der = der_entry(leaf, 0);
  char junk[] = "!!!!";
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, store(&p, leaf_der, junk));
  char* inter_trailing = der_entry(inter, 3);
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, store(&p, leaf_der, inter_trailing));
  free(inter_trailing);
  f = fopen(OP_CERT, "rb");
  assert_non_null(f);
  memset(buf, 0, sizeof(buf));
  assert_int_equal(8, (int)fread(buf, 1, sizeof(buf) - 1, f));
  assert_int_equal(0, fclose(f));
  assert_string_equal(buf, "previous");

  assert_int_equal(AZ_IOT_OK, store(&p, leaf_der, inter_der));
  assert_int_equal(2, stored_chain(key, &leaf_ok));
  assert_true(leaf_ok);

  char* leaf_pem = pem_entry(leaf, "", 0);
  char* inter_pem = pem_entry(inter, "", 0);
  assert_int_equal(AZ_IOT_OK, store(&p, leaf_pem, inter_pem));
  assert_int_equal(2, stored_chain(key, &leaf_ok));
  assert_true(leaf_ok);

  sample_cert_provider_deinit(&p);
  free(wrong);
  free(inter_der);
  free(leaf_der);
  free(leaf_pem);
  free(inter_pem);
  X509_free(leaf);
  X509_free(inter);
  EVP_PKEY_free(key);
  EVP_PKEY_free(other);
  remove_test_files();
}

/* Init serves a stored chain only while it certifies the key: after the key
 * file is replaced (rotation while closed), the old chain is not used. */
static void sample_init_uses_a_stored_chain_only_for_its_key(void** state)
{
  (void)state;
  remove_test_files();
  EVP_PKEY* key = new_key();
  write_key(OP_KEY, key);
  X509* leaf = make_cert(key, 1);
  BIO* b = BIO_new_file(OP_CERT, "wb");
  assert_non_null(b);
  assert_int_equal(1, PEM_write_bio_X509(b, leaf));
  BIO_free(b);

  sample_cert_provider p;
  provider_init(&p);
  assert_true(p.has_operational);
  sample_cert_provider_deinit(&p);

  EVP_PKEY* rotated = new_key();
  write_key(OP_KEY, rotated);
  provider_init(&p);
  assert_false(p.has_operational);
  sample_cert_provider_deinit(&p);

  X509_free(leaf);
  EVP_PKEY_free(key);
  EVP_PKEY_free(rotated);
  remove_test_files();
}

static void sample_csr_generates_an_owner_only_key(void** state)
{
  (void)state;
  remove_test_files();
  char* csr = NULL;
#if !defined(_WIN32)
  /* A umask that leaves fopen()'s files readable by everyone. */
  mode_t previous_umask = umask(022);
#endif
  assert_int_equal(0, sample_csr_backend_get_csr(OP_KEY, "device", &csr));
  assert_non_null(csr);
  free(csr);
#if !defined(_WIN32)
  (void)umask(previous_umask);
  struct stat st;
  assert_int_equal(0, stat(OP_KEY, &st));
  assert_int_equal(0600, st.st_mode & 0777);
#endif
  EVP_PKEY* key = read_key(OP_KEY);
  /* The same key is loaded, not generated again, on the next call. */
  assert_int_equal(0, sample_csr_backend_get_csr(OP_KEY, "device", &csr));
  free(csr);
  EVP_PKEY* again = read_key(OP_KEY);
  assert_int_equal(1, EVP_PKEY_eq(key, again));
  EVP_PKEY_free(again);
  EVP_PKEY_free(key);
  remove_test_files();
}

static void sample_csr_does_not_replace_a_key_it_cannot_load(void** state)
{
  (void)state;
  remove_test_files();
  char* csr = NULL;
  /* Present but not a key: an error, and the file is left as it was. */
  write_text(OP_KEY, "not a key");
  assert_int_not_equal(0, sample_csr_backend_get_csr(OP_KEY, "device", &csr));
  FILE* f = fopen(OP_KEY, "rb");
  assert_non_null(f);
  char contents[16] = { 0 };
  assert_int_equal(9, fread(contents, 1, sizeof(contents) - 1, f));
  assert_int_equal(0, fclose(f));
  assert_string_equal("not a key", contents);
#if !defined(_WIN32)
  /* A real key the process cannot read (mode 0000; root reads it anyway). */
  if (geteuid() != 0)
  {
    EVP_PKEY* key = new_key();
    remove_test_files();
    write_key(OP_KEY, key);
    assert_int_equal(0, chmod(OP_KEY, 0));
    assert_int_not_equal(0, sample_csr_backend_get_csr(OP_KEY, "device", &csr));
    assert_int_equal(0, chmod(OP_KEY, 0600));
    EVP_PKEY* kept = read_key(OP_KEY);
    assert_int_equal(1, EVP_PKEY_eq(key, kept));
    EVP_PKEY_free(kept);
    EVP_PKEY_free(key);
  }
#endif
  remove_test_files();
}

static void sample_csr_removes_a_key_it_failed_to_write(void** state)
{
  (void)state;
#if !defined(_WIN32)
  remove_test_files();
  char* csr = NULL;
  /* Files may not exceed 16 bytes: the PEM write fails part way. */
  struct rlimit previous;
  assert_int_equal(0, getrlimit(RLIMIT_FSIZE, &previous));
  struct rlimit limited = previous;
  limited.rlim_cur = 16;
  void (*previous_handler)(int) = signal(SIGXFSZ, SIG_IGN);
  assert_int_equal(0, setrlimit(RLIMIT_FSIZE, &limited));
  int rc = sample_csr_backend_get_csr(OP_KEY, "device", &csr);
  assert_int_equal(0, setrlimit(RLIMIT_FSIZE, &previous));
  (void)signal(SIGXFSZ, previous_handler);
  assert_int_not_equal(0, rc);

  /* Nothing left behind, so the next attempt can create the key. */
  FILE* f = fopen(OP_KEY, "rb");
  assert_null(f);
  assert_int_equal(0, sample_csr_backend_get_csr(OP_KEY, "device", &csr));
  free(csr);
  remove_test_files();
#endif
  /* Windows has no portable way to cut a write short; the POSIX runs cover it. */
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(sample_cert_matches_key_in_both_wire_forms),
    cmocka_unit_test(sample_cert_mismatch_or_malformed_does_not_match),
    cmocka_unit_test(sample_cert_with_trailing_data_does_not_match),
    cmocka_unit_test(sample_cert_entry_bound_is_64_kib),
    cmocka_unit_test(sample_store_checks_the_key_and_writes_both_forms),
    cmocka_unit_test(sample_init_uses_a_stored_chain_only_for_its_key),
    cmocka_unit_test(sample_csr_generates_an_owner_only_key),
    cmocka_unit_test(sample_csr_does_not_replace_a_key_it_cannot_load),
    cmocka_unit_test(sample_csr_removes_a_key_it_failed_to_write),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
