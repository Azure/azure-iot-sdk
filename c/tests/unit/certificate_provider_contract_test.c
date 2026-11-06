// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The certificate provider contract suite (support/cert_provider_contract.c),
 * run against each provider this repo ships: PEM, managed (OpenSSL) and the
 * samples' provider. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "../support/test_x509.h"
#include "azure/iot/az_iot_certificate_provider_pem.h"

#include "../support/cert_provider_contract.h"
#include "sample_cert_provider.h"

#ifdef AZ_IOT_CONTRACT_WITH_MANAGED
#include "az_iot_certificate_provider_managed.h"
#endif

#define BOOT_CRT "az_iot_contract_test_boot.pem"
#define BOOT_KEY "az_iot_contract_test_boot_key.pem"
#define OP_KEY "az_iot_contract_test_op_key.pem"
#define OP_CERT "az_iot_contract_test_op_cert.pem"

/** @brief Write a self-signed bootstrap certificate and its key. */
static void write_bootstrap_identity(void)
{
  EVP_PKEY* k = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  X509* x = X509_new();
  if (!k || !x)
  {
    fail_msg("out of memory");
  }
  assert_int_equal(1, ASN1_INTEGER_set(X509_get_serialNumber(x), 1));
  assert_non_null(X509_gmtime_adj(X509_getm_notBefore(x), 0));
  assert_non_null(X509_gmtime_adj(X509_getm_notAfter(x), 3600));
  assert_int_equal(1, X509_set_pubkey(x, k));
  assert_int_equal(1, test_x509_set_self_signed_name(x, "az-iot-contract-boot"));
  assert_true(X509_sign(x, k, EVP_sha256()) > 0);

  BIO* b = BIO_new_file(BOOT_CRT, "wb");
  assert_non_null(b);
  assert_int_equal(1, PEM_write_bio_X509(b, x));
  BIO_free(b);
  b = BIO_new_file(BOOT_KEY, "wb");
  assert_non_null(b);
  assert_int_equal(1, PEM_write_bio_PrivateKey(b, k, NULL, NULL, 0, NULL, NULL));
  BIO_free(b);
  X509_free(x);
  EVP_PKEY_free(k);
}

static void remove_operational_files(void* ctx)
{
  (void)ctx;
  (void)remove(OP_KEY);
  (void)remove(OP_CERT);
}

static void reset_nothing(void* ctx) { (void)ctx; }

static az_iot_certificate_provider_pem g_pem;

static az_iot_certificate_provider* create_pem(void* ctx)
{
  (void)ctx;
  az_iot_certificate_provider_pem_options o = az_iot_certificate_provider_pem_options_default();
  o.client_cert_pem_path = BOOT_CRT;
  o.client_key_pem_path = BOOT_KEY;
  return az_iot_certificate_provider_pem_init(&g_pem, &o) == AZ_IOT_OK ? &g_pem.base : NULL;
}

static sample_cert_provider g_sample;

static az_iot_certificate_provider* create_sample(void* ctx)
{
  (void)ctx;
  sample_cert_provider_options o = {
    .bootstrap_cert_path = BOOT_CRT,
    .bootstrap_key_path = BOOT_KEY,
    .trusted_ca_path = NULL,
    .operational_key_path = OP_KEY,
    .operational_cert_path = OP_CERT,
  };
  return sample_cert_provider_init(&g_sample, &o) == AZ_IOT_OK ? &g_sample.base : NULL;
}

#ifdef AZ_IOT_CONTRACT_WITH_MANAGED
static az_iot_certificate_provider_managed g_managed;

/** @brief @p ctx non-NULL: X.509 bootstrap identity; NULL: SAS onboarding (none). */
static az_iot_certificate_provider* create_managed(void* ctx)
{
  az_iot_certificate_provider_managed_options o
      = az_iot_certificate_provider_managed_options_default();
  if (ctx)
  {
    o.bootstrap_cert_pem_path = BOOT_CRT;
    o.bootstrap_key_pem_path = BOOT_KEY;
  }
  o.operational_key_pem_path = OP_KEY;
  o.operational_cert_pem_path = OP_CERT;
  return az_iot_certificate_provider_managed_init(&g_managed, &o) == AZ_IOT_OK ? &g_managed.base
                                                                               : NULL;
}
#endif

int main(void)
{
  write_bootstrap_identity();

  static int x509_bootstrap;
  const cert_provider_contract_fixture fixtures[] = {
    { .name = "pem", .reset = reset_nothing, .create = create_pem, .persists = false },
#ifdef AZ_IOT_CONTRACT_WITH_MANAGED
    { .name = "managed",
      .ctx = &x509_bootstrap,
      .reset = remove_operational_files,
      .create = create_managed,
      .persists = true },
    { .name = "managed (SAS onboarding)",
      .ctx = NULL,
      .reset = remove_operational_files,
      .create = create_managed,
      .persists = true },
#endif
    { .name = "sample",
      .reset = remove_operational_files,
      .create = create_sample,
      .persists = true },
  };

  int failed = 0;
  for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i)
  {
    failed += cert_provider_contract_run(&fixtures[i]);
  }

  remove_operational_files(NULL);
  (void)remove(BOOT_CRT);
  (void)remove(BOOT_KEY);
  return failed;
}
