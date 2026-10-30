// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The esp_secure_cert certificate provider on the desktop, over fakes of esp_secure_cert_mgr and
 * PSA (support/fake_esp_secure_cert). Built twice: with CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL (the
 * key is a PSA key over the DS context) and without (the key is the partition's PEM). Both run the
 * certificate provider contract suite. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "az_iot_certificate_provider_esp_secure_cert.h"
#include "cert_provider_contract.h"
#include "fake_esp_secure_cert.h"
#include "test_x509.h"

static char g_cert_pem[4096];
static char g_key_pem[4096];

/** @brief A self-signed P-256 certificate and its key, as PEM in g_cert_pem and g_key_pem. */
static void make_identity(void)
{
  EVP_PKEY* k = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  X509* x = X509_new();
  assert_non_null(k);
  assert_non_null(x);
  assert_int_equal(1, ASN1_INTEGER_set(X509_get_serialNumber(x), 1));
  assert_non_null(X509_gmtime_adj(X509_getm_notBefore(x), 0));
  assert_non_null(X509_gmtime_adj(X509_getm_notAfter(x), 3600));
  assert_int_equal(1, X509_set_pubkey(x, k));
  assert_int_equal(1, test_x509_set_self_signed_name(x, "az-iot-esp-secure-cert"));
  assert_true(X509_sign(x, k, EVP_sha256()) > 0);
  BIO* b = BIO_new(BIO_s_mem());
  assert_int_equal(1, PEM_write_bio_X509(b, x));
  int n = BIO_read(b, g_cert_pem, (int)sizeof(g_cert_pem) - 1);
  assert_true(n > 0);
  g_cert_pem[n] = '\0';
  BIO_free(b);
  b = BIO_new(BIO_s_mem());
  assert_int_equal(1, PEM_write_bio_PrivateKey(b, k, NULL, NULL, 0, NULL, NULL));
  n = BIO_read(b, g_key_pem, (int)sizeof(g_key_pem) - 1);
  assert_true(n > 0);
  g_key_pem[n] = '\0';
  BIO_free(b);
  X509_free(x);
  EVP_PKEY_free(k);
}

static int setup(void** state)
{
  (void)state;
  fake_esp_secure_cert_reset(g_cert_pem, g_key_pem);
  return 0;
}

static void init_rejects_null(void** state)
{
  (void)state;
  assert_int_equal(
      AZ_IOT_ERR_INVALID_ARG, az_iot_certificate_provider_esp_secure_cert_init(NULL, NULL));
  az_iot_certificate_provider_esp_secure_cert_deinit(NULL);
}

static void a_partition_without_a_device_certificate_fails_init(void** state)
{
  (void)state;
  g_fake_esp_secure_cert.device_cert = NULL;
  az_iot_certificate_provider_esp_secure_cert p;
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, az_iot_certificate_provider_esp_secure_cert_init(&p, NULL));
  assert_null(p.base.vtable);
  assert_int_equal(0, g_fake_esp_secure_cert.ds_context_reads);
  assert_int_equal(0, g_fake_esp_secure_cert.imports);
}

/* The partition's certificate may not be NUL-terminated; every role gets one terminated copy,
 * and nothing left over in the caller's storage. */
static void load_returns_the_certificate_for_every_role(void** state)
{
  (void)state;
  az_iot_certificate_provider_esp_secure_cert p;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_esp_secure_cert_init(&p, "CA"));
  assert_int_equal(1, g_fake_esp_secure_cert.device_cert_frees);
  const az_iot_cert_role roles[] = { AZ_IOT_CRED_BOOTSTRAP, AZ_IOT_CRED_OPERATIONAL };
  for (size_t i = 0; i < 2; ++i)
  {
    az_iot_certificate_material m;
    memset(&m, 0xA5, sizeof(m));
    assert_int_equal(AZ_IOT_OK, p.base.vtable->load(&p.base, roles[i], 0, &m));
    assert_string_equal(g_cert_pem, m.client_cert_pem);
    assert_string_equal("CA", m.trusted_ca_pem);
    assert_null(m.client_cert_path);
    assert_null(m.client_key_path);
    assert_null(m.client_key_password);
    assert_null(m.client_key_uri);
    assert_null(m.crypto_engine_id);
    p.base.vtable->release(&p.base, &m);
    assert_int_equal(AZ_IOT_ERR_NOT_FOUND, p.base.vtable->load(&p.base, roles[i], 1, &m));
  }
  assert_int_equal(1, g_fake_esp_secure_cert.device_cert_reads);
  az_iot_certificate_provider_esp_secure_cert_deinit(&p);
}

static void load_after_deinit_is_refused(void** state)
{
  (void)state;
  az_iot_certificate_provider_esp_secure_cert p;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_esp_secure_cert_init(&p, NULL));
  const az_iot_certificate_provider_vtable* v = p.base.vtable;
  az_iot_certificate_provider_esp_secure_cert_deinit(&p);
  az_iot_certificate_material m;
  assert_int_equal(AZ_IOT_ERR_NOT_INITIALIZED, v->load(&p.base, AZ_IOT_CRED_BOOTSTRAP, 0, &m));
}

#if defined(CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL)
/* init() imports the DS context as an RSA signing key that lives in the DS peripheral; load()
 * hands out only its id, and no private key is ever read. */
static void the_ds_context_becomes_a_psa_signing_key(void** state)
{
  (void)state;
  az_iot_certificate_provider_esp_secure_cert p;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_esp_secure_cert_init(&p, NULL));
  const psa_key_attributes_t* a = &g_fake_esp_secure_cert.imported_attributes;
  assert_int_equal(1, g_fake_esp_secure_cert.imports);
  assert_int_equal(PSA_KEY_LIFETIME_ESP_RSA_DS, a->lifetime);
  assert_int_equal(PSA_KEY_TYPE_RSA_KEY_PAIR, a->type);
  assert_int_equal(2048, a->bits);
  assert_int_equal(PSA_KEY_USAGE_SIGN_HASH, a->usage);
  assert_int_equal(PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_ANY_HASH), a->algorithm);
  assert_ptr_equal(p.ds_context, g_fake_esp_secure_cert.imported_ds_context);

  az_iot_certificate_material m;
  assert_int_equal(AZ_IOT_OK, p.base.vtable->load(&p.base, AZ_IOT_CRED_BOOTSTRAP, 0, &m));
  assert_int_not_equal(0, m.client_key_psa_id);
  assert_null(m.client_key_pem);
  uint32_t const id = m.client_key_psa_id;
  p.base.vtable->release(&p.base, &m);
  assert_int_equal(0, g_fake_esp_secure_cert.private_key_reads);

  // The key outlives release(): destroyed, then its context freed, only at deinit.
  assert_int_equal(0, g_fake_esp_secure_cert.destroys);
  assert_int_equal(0, g_fake_esp_secure_cert.ds_context_frees);
  az_iot_certificate_provider_esp_secure_cert_deinit(&p);
  assert_int_equal(1, g_fake_esp_secure_cert.destroys);
  assert_int_equal(id, g_fake_esp_secure_cert.destroyed_key);
  assert_int_equal(1, g_fake_esp_secure_cert.ds_context_frees);
}

static void a_refused_import_fails_init(void** state)
{
  (void)state;
  g_fake_esp_secure_cert.import_status = PSA_ERROR_NOT_SUPPORTED;
  az_iot_certificate_provider_esp_secure_cert p;
  assert_int_equal(AZ_IOT_ERR_INTERNAL, az_iot_certificate_provider_esp_secure_cert_init(&p, NULL));
  assert_null(p.base.vtable);
  assert_int_equal(1, g_fake_esp_secure_cert.ds_context_frees);
  assert_int_equal(0, g_fake_esp_secure_cert.destroys);
}

static void a_partition_without_a_ds_context_fails_init(void** state)
{
  (void)state;
  g_fake_esp_secure_cert.has_ds_context = false;
  az_iot_certificate_provider_esp_secure_cert p;
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, az_iot_certificate_provider_esp_secure_cert_init(&p, NULL));
  assert_int_equal(0, g_fake_esp_secure_cert.imports);
}
#else
/* The PEM key is read once for overlapping loans and freed by the last release(); the
 * partition's buffer is wiped before it is handed back, as a RAM copy is freed unwiped. */
static void the_key_is_in_memory_only_while_loaned(void** state)
{
  (void)state;
  az_iot_certificate_provider_esp_secure_cert p;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_esp_secure_cert_init(&p, NULL));
  assert_int_equal(0, g_fake_esp_secure_cert.private_key_reads);
  az_iot_certificate_material a;
  az_iot_certificate_material b;
  assert_int_equal(AZ_IOT_OK, p.base.vtable->load(&p.base, AZ_IOT_CRED_BOOTSTRAP, 0, &a));
  assert_int_equal(AZ_IOT_OK, p.base.vtable->load(&p.base, AZ_IOT_CRED_OPERATIONAL, 0, &b));
  assert_int_equal(1, g_fake_esp_secure_cert.private_key_reads);
  assert_int_equal(1, g_fake_esp_secure_cert.private_key_frees);
  assert_int_equal(0, g_fake_esp_secure_cert.private_keys_freed_unwiped);
  assert_string_equal(g_key_pem, a.client_key_pem);
  assert_ptr_equal(a.client_key_pem, b.client_key_pem);
  assert_int_equal(0, a.client_key_psa_id);
  p.base.vtable->release(&p.base, &a);
  assert_non_null(p.client_key_pem);
  p.base.vtable->release(&p.base, &b);
  assert_null(p.client_key_pem);
  assert_int_equal(AZ_IOT_OK, p.base.vtable->load(&p.base, AZ_IOT_CRED_BOOTSTRAP, 0, &a));
  assert_int_equal(2, g_fake_esp_secure_cert.private_key_reads);
  az_iot_certificate_provider_esp_secure_cert_deinit(&p); // Drops the outstanding loan.
}

static void a_partition_without_a_key_fails_the_load(void** state)
{
  (void)state;
  g_fake_esp_secure_cert.private_key = NULL;
  az_iot_certificate_provider_esp_secure_cert p;
  assert_int_equal(AZ_IOT_OK, az_iot_certificate_provider_esp_secure_cert_init(&p, NULL));
  az_iot_certificate_material m;
  assert_int_equal(
      AZ_IOT_ERR_NOT_FOUND, p.base.vtable->load(&p.base, AZ_IOT_CRED_BOOTSTRAP, 0, &m));
  assert_int_equal(0, p.client_key_refs);
  az_iot_certificate_provider_esp_secure_cert_deinit(&p);
}
#endif

static az_iot_certificate_provider_esp_secure_cert g_contract_provider;

static void contract_reset(void* ctx)
{
  (void)ctx;
  fake_esp_secure_cert_reset(g_cert_pem, g_key_pem);
}

static az_iot_certificate_provider* contract_create(void* ctx)
{
  (void)ctx;
  return az_iot_certificate_provider_esp_secure_cert_init(&g_contract_provider, NULL) == AZ_IOT_OK
      ? &g_contract_provider.base
      : NULL;
}

int main(void)
{
  make_identity();
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup(init_rejects_null, setup),
    cmocka_unit_test_setup(a_partition_without_a_device_certificate_fails_init, setup),
    cmocka_unit_test_setup(load_returns_the_certificate_for_every_role, setup),
    cmocka_unit_test_setup(load_after_deinit_is_refused, setup),
#if defined(CONFIG_ESP_SECURE_CERT_DS_PERIPHERAL)
    cmocka_unit_test_setup(the_ds_context_becomes_a_psa_signing_key, setup),
    cmocka_unit_test_setup(a_refused_import_fails_init, setup),
    cmocka_unit_test_setup(a_partition_without_a_ds_context_fails_init, setup),
#else
    cmocka_unit_test_setup(the_key_is_in_memory_only_while_loaned, setup),
    cmocka_unit_test_setup(a_partition_without_a_key_fails_the_load, setup),
#endif
  };
  int failed = cmocka_run_group_tests(tests, NULL, NULL);
  const cert_provider_contract_fixture fixture = {
    .name = "esp_secure_cert",
    .reset = contract_reset,
    .create = contract_create,
    .persists = false,
  };
  contract_reset(NULL);
  failed += cert_provider_contract_run(&fixture);
  return failed;
}
