// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* authentication/custom_provider_template
 *
 * A self-contained template for implementing your own certificate provider
 * (D8 non-extractable keys, D9 app-owned material). Use this as a starting
 * point to integrate a TPM / HSM / secure element / OS keystore that the
 * shipped OpenSSL "managed" provider does not cover.
 *
 * It builds with no external crypto dependency and does not connect to a hub;
 * main() simply exercises the vtable so the wiring compiles and round-trips.
 * Replace the placeholder bodies with real platform crypto:
 *   - get_csr():  produce a PKCS#10 CSR (base64 DER) over your device key.
 *   - sign():     sign a digest with a NON-extractable key (HSM/TPM). When you
 *                 implement this, leave client_key_pem/path NULL in load() and
 *                 set client_key_uri/crypto_engine_id so the TLS adapter calls
 *                 back into sign() during the handshake instead of reading a key.
 *   - store_issued_certificate(): persist the issued chain wherever you like.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_result.h"

/* Your provider's state. The az_iot_certificate_provider base MUST be first
 * so a pointer to it can be cast to/from your struct. */
typedef struct
{
  az_iot_certificate_provider base;
  const char* bootstrap_cert_path;
  const char* bootstrap_key_path;
  const char* trusted_ca_path;
  int has_operational;
} my_provider;

static char* dup_cstr(const char* s)
{
  if (!s)
  {
    return NULL;
  }
  size_t n = strlen(s) + 1;
  char* out = (char*)malloc(n);
  if (out)
  {
    memcpy(out, s, n);
  }
  return out;
}

static az_iot_result my_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  my_provider* m = (my_provider*)self;
  if (!m || !out)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(out, 0, sizeof(*out));
  out->trusted_ca_path = m->trusted_ca_path;

  if (role == AZ_IOT_CRED_OPERATIONAL)
  {
    if (!m->has_operational)
    {
      return AZ_IOT_ERR_NOT_FOUND;
    }
    /* Point the adapter at your issued operational cert. For a
     * non-extractable key, leave client_key_* NULL and set client_key_uri
     * (e.g. "pkcs11:token=...;object=...") + crypto_engine_id so the TLS
     * adapter calls sign() instead of reading a private key. */
    out->client_cert_path = "operational_cert.pem";
    out->client_key_uri = NULL; /* e.g. "pkcs11:object=device-key" */
    out->crypto_engine_id = NULL; /* e.g. "pkcs11" */
  }
  else
  {
    out->client_cert_path = m->bootstrap_cert_path;
    out->client_key_path = m->bootstrap_key_path;
  }
  return AZ_IOT_OK;
}

static void my_release(az_iot_certificate_provider* self, az_iot_certificate_material* material)
{
  (void)self;
  (void)material; /* nothing heap-allocated in load() above */
}

static az_iot_result my_get_csr(
    az_iot_certificate_provider* self,
    const char* subject_common_name,
    az_iot_certificate_signing_request* out_csr)
{
  (void)self;
  if (!out_csr)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* REPLACE ME: build a real PKCS#10 CSR (base64 DER, no PEM headers) whose
   * subject CN is `subject_common_name`, signed by your device key. The
   * placeholder below only demonstrates the ownership contract: the returned
   * string is heap-owned and freed via release_csr(). */
  (void)subject_common_name;
  out_csr->csr_base64 = dup_cstr("PLACEHOLDER-BASE64-DER-CSR");
  return out_csr->csr_base64 ? AZ_IOT_OK : AZ_IOT_ERR_OUT_OF_MEMORY;
}

static void my_release_csr(
    az_iot_certificate_provider* self,
    az_iot_certificate_signing_request* csr)
{
  (void)self;
  if (csr && csr->csr_base64)
  {
    free((void*)csr->csr_base64);
    csr->csr_base64 = NULL;
  }
}

static az_iot_result my_store_issued_certificate(
    az_iot_certificate_provider* self,
    const az_iot_issued_certificate* issued)
{
  my_provider* m = (my_provider*)self;
  if (!m || !issued)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* REPLACE ME: persist issued->certificates[0..count) - each entry is a
   * base64 DER cert (az_span); PEM-wrap and write it wherever your platform
   * keeps certificates. */
  fprintf(stderr, "[custom] persisting issued chain: %zu cert(s)\n", issued->count);
  m->has_operational = 1;
  return AZ_IOT_OK;
}

static az_iot_result my_sign(
    az_iot_certificate_provider* self,
    const uint8_t* digest,
    size_t digest_len,
    uint8_t* out_sig,
    size_t out_sig_cap,
    size_t* out_sig_len)
{
  (void)self;
  (void)digest;
  (void)digest_len;
  (void)out_sig;
  (void)out_sig_cap;
  (void)out_sig_len;
  /* REPLACE ME (optional, D8): sign `digest` with your non-extractable key,
   * writing up to out_sig_cap bytes and setting *out_sig_len. Return
   * AZ_IOT_ERR_NOT_SUPPORTED if your key is a normal PEM file instead. */
  return AZ_IOT_ERR_NOT_SUPPORTED;
}

static void my_destroy(az_iot_certificate_provider* self)
{
  (void)self; /* nothing owned in this template */
}

static const az_iot_certificate_provider_vtable s_my_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = my_load,
  .release = my_release,
  .deinit = my_destroy,
  .get_csr = my_get_csr,
  .release_csr = my_release_csr,
  .store_issued_certificate = my_store_issued_certificate,
  .sign = my_sign,
};

int main(void)
{
  my_provider provider = { 0 };
  provider.base.vtable = &s_my_vtable;
  provider.bootstrap_cert_path = "bootstrap_cert.pem";
  provider.bootstrap_key_path = "bootstrap_key.pem";
  provider.trusted_ca_path = "trusted_ca.pem";

  /* Pass &provider.base wherever an az_iot_certificate_provider* is expected
   * (assign to az_iot_connection_client_options.certificate_provider, etc.).
   * Here we just exercise the vtable to prove the wiring. */
  az_iot_certificate_signing_request csr = { 0 };
  if (provider.base.vtable->get_csr(&provider.base, "my-device-id", &csr) != AZ_IOT_OK)
  {
    return 1;
  }
  fprintf(stderr, "[custom] get_csr produced: %s\n", csr.csr_base64);
  provider.base.vtable->release_csr(&provider.base, &csr);

  az_span chain[1] = { AZ_SPAN_FROM_STR("MIIBase64DERcertGoesHere==") };
  az_iot_issued_certificate issued = { .certificates = chain, .count = 1 };
  (void)provider.base.vtable->store_issued_certificate(&provider.base, &issued);

  provider.base.vtable->deinit(&provider.base);
  return 0;
}
