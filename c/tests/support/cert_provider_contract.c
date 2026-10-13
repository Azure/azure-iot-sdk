// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Provider-independent contract suite for az_iot_certificate_provider. See
 * cert_provider_contract.h. */

#include "cert_provider_contract.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "azure/iot/az_iot_connection_client.h"

#define CSR_SUBJECT "az-iot-contract-device"

static const cert_provider_contract_fixture* g_fixture;
static EVP_PKEY* g_ca_key;
static X509* g_ca;

/* ---- provider lifetime ---- */

/** @brief Create the provider over freshly reset state. */
static az_iot_certificate_provider* fresh(void)
{
  g_fixture->reset(g_fixture->ctx);
  az_iot_certificate_provider* p = g_fixture->create(g_fixture->ctx);
  assert_non_null(p);
  assert_non_null(p->vtable);
  /* Every case calls these; a missing one fails here instead of crashing. */
  assert_non_null(p->vtable->load);
  assert_non_null(p->vtable->release);
  assert_non_null(p->vtable->deinit);
  return p;
}

/** @brief Destroy @p p through its vtable and delete what it persisted. */
static void finish(az_iot_certificate_provider* p)
{
  p->vtable->deinit(p);
  g_fixture->reset(g_fixture->ctx);
}

/* ---- certificates ---- */

static EVP_PKEY* new_key(void)
{
  EVP_PKEY* k = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  assert_non_null(k);
  return k;
}

/** @brief A certificate for @p subject_key, issued by the suite's CA. */
static X509* issue(EVP_PKEY* subject_key, long serial)
{
  X509* x = X509_new();
  assert_non_null(x);
  assert_int_equal(1, ASN1_INTEGER_set(X509_get_serialNumber(x), serial));
  assert_non_null(X509_gmtime_adj(X509_getm_notBefore(x), 0));
  assert_non_null(X509_gmtime_adj(X509_getm_notAfter(x), 3600));
  assert_int_equal(1, X509_set_pubkey(x, subject_key));
  X509_NAME* name = X509_NAME_new();
  assert_non_null(name);
  assert_int_equal(
      1,
      X509_NAME_add_entry_by_txt(
          name, "CN", MBSTRING_UTF8, (const unsigned char*)CSR_SUBJECT, -1, -1, 0));
  assert_int_equal(1, X509_set_subject_name(x, name));
  X509_NAME_free(name);
  assert_int_equal(1, X509_set_issuer_name(x, X509_get_subject_name(g_ca)));
  assert_true(X509_sign(x, g_ca_key, EVP_sha256()) > 0);
  return x;
}

static void make_ca(void)
{
  g_ca_key = new_key();
  g_ca = X509_new();
  assert_non_null(g_ca);
  assert_int_equal(1, ASN1_INTEGER_set(X509_get_serialNumber(g_ca), 1));
  assert_non_null(X509_gmtime_adj(X509_getm_notBefore(g_ca), 0));
  assert_non_null(X509_gmtime_adj(X509_getm_notAfter(g_ca), 3600));
  assert_int_equal(1, X509_set_pubkey(g_ca, g_ca_key));
  X509_NAME* name = X509_get_subject_name(g_ca);
  assert_int_equal(
      1,
      X509_NAME_add_entry_by_txt(
          name, "CN", MBSTRING_UTF8, (const unsigned char*)"az-iot-contract-ca", -1, -1, 0));
  assert_int_equal(1, X509_set_issuer_name(g_ca, name));
  assert_true(X509_sign(g_ca, g_ca_key, EVP_sha256()) > 0);
}

static char* base64_of(const unsigned char* data, size_t len)
{
  char* b64 = (char*)malloc(((len + 2) / 3) * 4 + 1);
  assert_non_null(b64);
  int n = EVP_EncodeBlock((unsigned char*)b64, data, (int)len);
  assert_true(n > 0);
  b64[n] = '\0';
  return b64;
}

/** @brief Wire form: base64 of the certificate's DER. */
static char* der_entry(X509* x)
{
  unsigned char* der = NULL;
  int len = i2d_X509(x, &der);
  assert_true(len > 0);
  char* b64 = base64_of(der, (size_t)len);
  OPENSSL_free(der);
  return b64;
}

/** @brief Wire form: base64 of the whole PEM certificate. */
static char* pem_entry(X509* x)
{
  BIO* m = BIO_new(BIO_s_mem());
  assert_non_null(m);
  assert_int_equal(1, PEM_write_bio_X509(m, x));
  char* data = NULL;
  long n = BIO_get_mem_data(m, &data);
  assert_true(n > 0);
  char* b64 = base64_of((const unsigned char*)data, (size_t)n);
  BIO_free(m);
  return b64;
}

typedef char* (*entry_fn)(X509* x);

/** @brief Store [@p leaf, CA] in the @p entry wire form. */
static az_iot_result store(az_iot_certificate_provider* p, X509* leaf, entry_fn entry)
{
  char* e[2] = { entry(leaf), entry(g_ca) };
  az_span chain[2] = { az_span_create_from_str(e[0]), az_span_create_from_str(e[1]) };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 2 };
  az_iot_result rc = p->vtable->store_issued_certificate(p, &issued);
  free(e[0]);
  free(e[1]);
  return rc;
}

/* ---- reading what the provider hands out ---- */

/** @brief Open @p pem (in memory) if set, else the file at @p path; NULL if neither. */
static BIO* open_material(const char* pem, const char* path)
{
  return pem ? BIO_new_mem_buf(pem, -1) : (path ? BIO_new_file(path, "rb") : NULL);
}

/**
 * @brief Check a credential load() returned: a client certificate, and a key
 * that certificate certifies, or a key reference / sign() hook the connection
 * client accepts. Every form set (PEM string and file path) is checked: an
 * adapter may use either. A v2 sign() must sign a digest; its signature format
 * belongs to the adapter and is not verified.
 *
 * @return The client certificate; caller frees.
 */
static X509* check_material(az_iot_certificate_provider* p, const az_iot_certificate_material* m)
{
  const char* cert_pem[2] = { m->client_cert_pem, NULL };
  const char* cert_path[2] = { NULL, m->client_cert_path };
  X509* x = NULL;
  for (size_t i = 0; i < 2; ++i)
  {
    if (!cert_pem[i] && !cert_path[i])
    {
      continue;
    }
    BIO* b = open_material(cert_pem[i], cert_path[i]);
    assert_non_null(b); /* a path that is set must open */
    X509* c = PEM_read_bio_X509(b, NULL, NULL, NULL);
    BIO_free(b);
    assert_non_null(c);
    if (!x)
    {
      x = c;
    }
    else
    {
      assert_int_equal(0, X509_cmp(x, c));
      X509_free(c);
    }
  }
  assert_non_null(x);

  /* As the connection client: sign() counts from vtable v2, and a key URI
   * needs crypto_engine_id unless sign() is offered. */
  bool has_sign = p->vtable->version >= 2 && p->vtable->sign != NULL;
  assert_false(m->client_key_uri != NULL && m->crypto_engine_id == NULL && !has_sign);
  const char* key_pem[2] = { m->client_key_pem, NULL };
  const char* key_path[2] = { NULL, m->client_key_path };
  for (size_t i = 0; i < 2; ++i)
  {
    if (!key_pem[i] && !key_path[i])
    {
      continue;
    }
    BIO* b = open_material(key_pem[i], key_path[i]);
    assert_non_null(b);
    EVP_PKEY* k = PEM_read_bio_PrivateKey(b, NULL, NULL, (void*)m->client_key_password);
    BIO_free(b);
    assert_non_null(k);
    assert_int_equal(1, X509_check_private_key(x, k));
    EVP_PKEY_free(k);
  }
  if (!m->client_key_pem && !m->client_key_path)
  {
    assert_true(m->client_key_uri != NULL || has_sign);
  }
  if (has_sign)
  {
    /* The client forwards sign() whenever set. Its signature format is the
     * adapter's; only that it signs is checked. */
    uint8_t digest[32];
    memset(digest, 0x5a, sizeof(digest));
    uint8_t sig[1024];
    size_t sig_len = 0;
    assert_int_equal(
        AZ_IOT_OK, p->vtable->sign(p, digest, sizeof(digest), sig, sizeof(sig), &sig_len));
    assert_true(sig_len > 0 && sig_len <= sizeof(sig));
  }
  return x;
}

/** @brief Assert the PEM in @p b is exactly [@p leaf, CA]. Frees @p b. */
static void assert_chain(BIO* b, X509* leaf)
{
  assert_non_null(b);
  X509* expected[] = { leaf, g_ca };
  for (size_t i = 0; i < 2; ++i)
  {
    X509* x = PEM_read_bio_X509(b, NULL, NULL, NULL);
    assert_non_null(x);
    assert_int_equal(0, X509_cmp(expected[i], x));
    X509_free(x);
  }
  /* Nothing but whitespace may follow. */
  char rest[256];
  int n = 0;
  while ((n = BIO_read(b, rest, (int)sizeof(rest))) > 0)
  {
    for (int i = 0; i < n; ++i)
    {
      assert_true(rest[i] != '\0' && strchr(" \t\r\n", rest[i]) != NULL);
    }
  }
  BIO_free(b);
}

/**
 * @brief Check @p role's credentials from index @p first up to
 * AZ_IOT_MAX_CERTS_PER_ROLE; a non-OK load() ends them and is AZ_IOT_ERR_NOT_FOUND.
 */
static void check_role_from(az_iot_certificate_provider* p, az_iot_cert_role role, uint8_t first)
{
  az_iot_result rc = AZ_IOT_OK;
  for (uint8_t i = first; i < AZ_IOT_MAX_CERTS_PER_ROLE && rc == AZ_IOT_OK; ++i)
  {
    az_iot_certificate_material m;
    memset(&m, 0, sizeof(m));
    rc = p->vtable->load(p, role, i, &m);
    if (rc == AZ_IOT_OK)
    {
      X509_free(check_material(p, &m));
      p->vtable->release(p, &m);
    }
    else
    {
      assert_int_equal(AZ_IOT_ERR_NOT_FOUND, rc);
    }
  }
}

/**
 * @brief Assert load(OPERATIONAL, 0) serves exactly [@p leaf, CA] in every form
 * set, and the operational fallback indexes are usable.
 */
static void assert_serves_chain(az_iot_certificate_provider* p, X509* leaf)
{
  az_iot_certificate_material m;
  memset(&m, 0, sizeof(m));
  assert_int_equal(AZ_IOT_OK, p->vtable->load(p, AZ_IOT_CRED_OPERATIONAL, 0, &m));
  X509_free(check_material(p, &m));
  if (m.client_cert_pem)
  {
    assert_chain(open_material(m.client_cert_pem, NULL), leaf);
  }
  if (m.client_cert_path)
  {
    assert_chain(open_material(NULL, m.client_cert_path), leaf);
  }
  p->vtable->release(p, &m);
  check_role_from(p, AZ_IOT_CRED_OPERATIONAL, 1); /* fallback indexes */
}

/** @brief get_csr(), checked: a self-signed request naming @p subject. Returns its key. */
static EVP_PKEY* csr_key(az_iot_certificate_provider* p, const char* subject)
{
  az_iot_certificate_signing_request csr;
  memset(&csr, 0, sizeof(csr));
  assert_int_equal(AZ_IOT_OK, p->vtable->get_csr(p, subject, &csr));
  assert_non_null(csr.csr_base64);

  size_t b64_len = strlen(csr.csr_base64);
  assert_true(b64_len > 0);
  assert_null(strpbrk(csr.csr_base64, "-\r\n "));
  unsigned char* der = (unsigned char*)malloc((b64_len / 4) * 3 + 1);
  assert_non_null(der);
  int der_len = EVP_DecodeBlock(der, (const unsigned char*)csr.csr_base64, (int)b64_len);
  assert_true(der_len > 0);
  /* EVP_DecodeBlock counts padding bytes; the request must use all the rest. */
  der_len
      -= (csr.csr_base64[b64_len - 1] == '=') + (b64_len > 1 && csr.csr_base64[b64_len - 2] == '=');
  const unsigned char* q = der;
  X509_REQ* req = d2i_X509_REQ(NULL, &q, der_len);
  assert_non_null(req);
  assert_true(q == der + der_len);
  free(der);
  p->vtable->release_csr(p, &csr);

  EVP_PKEY* k = X509_REQ_get_pubkey(req);
  assert_non_null(k);
  assert_int_equal(1, X509_REQ_verify(req, k));
  char cn[128] = { 0 };
  assert_true(
      X509_NAME_get_text_by_NID(X509_REQ_get_subject_name(req), NID_commonName, cn, sizeof(cn))
      > 0);
  assert_string_equal(subject, cn);
  X509_REQ_free(req);
  return k;
}

/* ---- core: every provider ---- */

static void vtable_is_well_formed(void** state)
{
  (void)state;
  az_iot_certificate_provider* p = fresh();
  const az_iot_certificate_provider_vtable* v = p->vtable;
  assert_true(v->version >= 1 && v->version <= AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION);
  assert_non_null(v->load);
  assert_non_null(v->release);
  assert_non_null(v->deinit);
  if (v->version >= 2 && v->get_csr)
  {
    assert_non_null(v->release_csr);
    assert_non_null(v->store_issued_certificate);
  }
  finish(p);
}

/* Every certificate a role returns, from index 0 up to AZ_IOT_MAX_CERTS_PER_ROLE,
 * is a usable credential; a non-OK result ends the role and is
 * AZ_IOT_ERR_NOT_FOUND. */
static void each_role_returns_usable_credentials(void** state)
{
  (void)state;
  az_iot_certificate_provider* p = fresh();
  const az_iot_cert_role roles[] = { AZ_IOT_CRED_BOOTSTRAP, AZ_IOT_CRED_OPERATIONAL };
  for (size_t r = 0; r < sizeof(roles) / sizeof(roles[0]); ++r)
  {
    check_role_from(p, roles[r], 0);
  }
  finish(p);
}

/* ---- CSR enrollment: providers with get_csr() ---- */

static void csr_is_signed_and_names_the_subject(void** state)
{
  (void)state;
  az_iot_certificate_provider* p = fresh();
  EVP_PKEY* a = csr_key(p, CSR_SUBJECT);
  EVP_PKEY* b = csr_key(p, "another-device");
  assert_int_equal(1, EVP_PKEY_eq(a, b)); /* the subject does not pick the key */
  EVP_PKEY_free(a);
  EVP_PKEY_free(b);
  finish(p);
}

static void csrs_reuse_the_operational_key(void** state)
{
  (void)state;
  az_iot_certificate_provider* p = fresh();
  EVP_PKEY* a = csr_key(p, CSR_SUBJECT);
  EVP_PKEY* b = csr_key(p, CSR_SUBJECT);
  assert_int_equal(1, EVP_PKEY_eq(a, b));
  EVP_PKEY_free(a);
  EVP_PKEY_free(b);
  finish(p);
}

static void operational_is_not_found_before_a_store(void** state)
{
  (void)state;
  az_iot_certificate_provider* p = fresh();
  EVP_PKEY_free(csr_key(p, CSR_SUBJECT));
  az_iot_certificate_material m;
  memset(&m, 0, sizeof(m));
  assert_int_equal(AZ_IOT_ERR_NOT_FOUND, p->vtable->load(p, AZ_IOT_CRED_OPERATIONAL, 0, &m));
  finish(p);
}

/** @brief Store a chain for the CSR key in @p entry form; load() serves it. */
static void stored_chain_is_served(entry_fn entry)
{
  az_iot_certificate_provider* p = fresh();
  EVP_PKEY* k = csr_key(p, CSR_SUBJECT);
  X509* leaf = issue(k, 10);
  assert_int_equal(AZ_IOT_OK, store(p, leaf, entry));

  assert_serves_chain(p, leaf);

  /* Renewal: the next request still uses the key the stored chain certifies. */
  EVP_PKEY* again = csr_key(p, CSR_SUBJECT);
  assert_int_equal(1, EVP_PKEY_eq(k, again));
  EVP_PKEY_free(again);

  X509_free(leaf);
  EVP_PKEY_free(k);
  finish(p);
}

static void a_der_chain_is_stored_and_served(void** state)
{
  (void)state;
  stored_chain_is_served(der_entry);
}

static void a_pem_chain_is_stored_and_served(void** state)
{
  (void)state;
  stored_chain_is_served(pem_entry);
}

/* A refused store leaves the previous chain in place. */
static void a_chain_for_another_key_is_refused_and_the_previous_kept(void** state)
{
  (void)state;
  az_iot_certificate_provider* p = fresh();
  EVP_PKEY* k = csr_key(p, CSR_SUBJECT);
  X509* leaf = issue(k, 20);
  assert_int_equal(AZ_IOT_OK, store(p, leaf, der_entry));

  EVP_PKEY* other = new_key();
  X509* foreign = issue(other, 21);
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, store(p, foreign, der_entry));
  assert_int_equal(AZ_IOT_ERR_INVALID_ARG, store(p, foreign, pem_entry));

  az_iot_issued_certificate empty = { .certificates = NULL, .count = 0 };
  assert_int_not_equal(AZ_IOT_OK, p->vtable->store_issued_certificate(p, &empty));

  assert_serves_chain(p, leaf);

  X509_free(foreign);
  EVP_PKEY_free(other);
  X509_free(leaf);
  EVP_PKEY_free(k);
  finish(p);
}

/* A later store replaces the earlier chain. */
static void a_new_chain_replaces_the_previous(void** state)
{
  (void)state;
  az_iot_certificate_provider* p = fresh();
  EVP_PKEY* k = csr_key(p, CSR_SUBJECT);
  X509* first = issue(k, 30);
  X509* second = issue(k, 31);
  assert_int_equal(AZ_IOT_OK, store(p, first, der_entry));
  assert_int_equal(AZ_IOT_OK, store(p, second, pem_entry));

  assert_serves_chain(p, second);

  X509_free(second);
  X509_free(first);
  EVP_PKEY_free(k);
  finish(p);
}

/* Persisting providers: a new instance over the same state serves the stored
 * chain and keeps the key. */
static void a_restart_keeps_the_chain_and_key(void** state)
{
  (void)state;
  if (!g_fixture->persists)
  {
    skip();
  }
  az_iot_certificate_provider* p = fresh();
  EVP_PKEY* k = csr_key(p, CSR_SUBJECT);
  X509* leaf = issue(k, 40);
  assert_int_equal(AZ_IOT_OK, store(p, leaf, der_entry));
  p->vtable->deinit(p);

  p = g_fixture->create(g_fixture->ctx);
  assert_non_null(p);
  assert_serves_chain(p, leaf);
  EVP_PKEY* again = csr_key(p, CSR_SUBJECT);
  assert_int_equal(1, EVP_PKEY_eq(k, again));
  EVP_PKEY_free(again);

  X509_free(leaf);
  EVP_PKEY_free(k);
  finish(p);
}

int cert_provider_contract_run(const cert_provider_contract_fixture* fixture)
{
  g_fixture = fixture;
  make_ca();

  const struct CMUnitTest core[] = {
    cmocka_unit_test(vtable_is_well_formed),
    cmocka_unit_test(each_role_returns_usable_credentials),
  };
  int failed = cmocka_run_group_tests_name(fixture->name, core, NULL, NULL);

  fixture->reset(fixture->ctx);
  az_iot_certificate_provider* p = fixture->create(fixture->ctx);
  /* A missing CSR hook already fails vtable_is_well_formed. */
  bool has_csr = p && p->vtable && p->vtable->version >= 2 && p->vtable->get_csr
      && p->vtable->release_csr && p->vtable->store_issued_certificate;
  if (p && p->vtable && p->vtable->deinit)
  {
    p->vtable->deinit(p);
  }
  fixture->reset(fixture->ctx);

  if (has_csr)
  {
    const struct CMUnitTest csr[] = {
      cmocka_unit_test(csr_is_signed_and_names_the_subject),
      cmocka_unit_test(csrs_reuse_the_operational_key),
      cmocka_unit_test(operational_is_not_found_before_a_store),
      cmocka_unit_test(a_der_chain_is_stored_and_served),
      cmocka_unit_test(a_pem_chain_is_stored_and_served),
      cmocka_unit_test(a_chain_for_another_key_is_refused_and_the_previous_kept),
      cmocka_unit_test(a_new_chain_replaces_the_previous),
      cmocka_unit_test(a_restart_keeps_the_chain_and_key),
    };
    char name[128];
    (void)snprintf(name, sizeof(name), "%s: CSR enrollment", fixture->name);
    failed += cmocka_run_group_tests_name(name, csr, NULL, NULL);
  }

  X509_free(g_ca);
  EVP_PKEY_free(g_ca_key);
  g_ca = NULL;
  g_ca_key = NULL;
  return failed;
}
