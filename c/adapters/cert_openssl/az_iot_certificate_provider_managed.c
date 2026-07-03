// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL 3.0+ implementation of the "managed" certificate provider (D5).
 *
 * Uses only public OpenSSL 3.0 APIs: EVP_PKEY_Q_keygen for key generation,
 * the X509_REQ_* family for PKCS#10, and BIO for file I/O (so this file needs
 * no CRT fopen and stays clean under MSVC /W4 /WX). */
#include "az_iot_certificate_provider_managed.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

/* --------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static char* dup_str(const char* s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

static EVP_PKEY* generate_key(int key_type)
{
    if (key_type == AZ_IOT_MANAGED_KEY_RSA_2048)
    {
        return EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)2048);
    }
    return EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
}

static EVP_PKEY* load_key_file(const char* path)
{
    BIO* b = BIO_new_file(path, "rb");
    if (!b) { ERR_clear_error(); return NULL; }
    EVP_PKEY* k = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
    BIO_free(b);
    return k;
}

static az_iot_result_t write_key_file(const char* path, EVP_PKEY* key)
{
    BIO* b = BIO_new_file(path, "wb");
    if (!b) return AZ_IOT_ERR_INTERNAL;
    int ok = PEM_write_bio_PrivateKey(b, key, NULL, NULL, 0, NULL, NULL);
    BIO_free(b);
    return (ok == 1) ? AZ_IOT_OK : AZ_IOT_ERR_INTERNAL;
}

static bool file_exists(const char* path)
{
    BIO* b = BIO_new_file(path, "rb");
    if (!b) { ERR_clear_error(); return false; }
    BIO_free(b);
    return true;
}

/* --------------------------------------------------------------------------
 * vtable hooks
 * ------------------------------------------------------------------------ */

static az_iot_result_t managed_load(
    az_iot_certificate_provider_t* self,
    az_iot_cert_role_t role,
    az_iot_certificate_material_t* out)
{
    az_iot_certificate_provider_managed_t* m = (az_iot_certificate_provider_managed_t*)self;
    if (!m || !out) return AZ_IOT_ERR_INVALID_ARG;
    if (!m->loaded) return AZ_IOT_ERR_NOT_INITIALIZED;

    memset(out, 0, sizeof(*out));
    out->trusted_ca_path = m->trusted_ca_path;

    if (role == AZ_IOT_CRED_OPERATIONAL)
    {
        if (!m->has_operational) return AZ_IOT_ERR_NOT_FOUND;
        out->client_cert_path = m->operational_cert_path;
        out->client_key_path  = m->operational_key_path;
    }
    else
    {
        out->client_cert_path = m->bootstrap_cert_path;
        out->client_key_path  = m->bootstrap_key_path;
    }
    return AZ_IOT_OK;
}

static void managed_release(
    az_iot_certificate_provider_t* self, az_iot_certificate_material_t* material)
{
    (void)self; (void)material; /* paths are owned by the provider struct */
}

static az_iot_result_t managed_get_csr(
    az_iot_certificate_provider_t* self,
    const char* subject_common_name,
    az_iot_certificate_signing_request_t* out_csr)
{
    az_iot_certificate_provider_managed_t* m = (az_iot_certificate_provider_managed_t*)self;
    if (!m || !out_csr) return AZ_IOT_ERR_INVALID_ARG;
    if (!m->loaded || !m->operational_key) return AZ_IOT_ERR_NOT_INITIALIZED;

    out_csr->csr_base64 = NULL;

    EVP_PKEY* key = (EVP_PKEY*)m->operational_key;
    X509_REQ* req = NULL;
    X509_NAME* name = NULL;
    unsigned char* der = NULL;
    char* b64 = NULL;
    az_iot_result_t rc = AZ_IOT_ERR_INTERNAL;

    req = X509_REQ_new();
    if (!req) goto done;
    if (X509_REQ_set_version(req, 0L) != 1) goto done; /* PKCS#10 v1 */

    name = X509_NAME_new();
    if (!name) goto done;
    {
        const char* cn = (subject_common_name && subject_common_name[0])
            ? subject_common_name : "azure-iot-device";
        if (X509_NAME_add_entry_by_txt(
                name, "CN", MBSTRING_UTF8, (const unsigned char*)cn, -1, -1, 0) != 1)
            goto done;
    }
    if (X509_REQ_set_subject_name(req, name) != 1) goto done;
    if (X509_REQ_set_pubkey(req, key) != 1) goto done;
    if (X509_REQ_sign(req, key, EVP_sha256()) == 0) goto done;

    {
        int der_len = i2d_X509_REQ(req, &der);
        if (der_len <= 0 || der == NULL) goto done;

        size_t b64_cap = (size_t)(((der_len + 2) / 3) * 4) + 1;
        b64 = (char*)malloc(b64_cap);
        if (!b64) { rc = AZ_IOT_ERR_OUT_OF_MEMORY; goto done; }
        int b64_len = EVP_EncodeBlock((unsigned char*)b64, der, der_len);
        if (b64_len <= 0) goto done;
        b64[b64_len] = '\0';
    }

    out_csr->csr_base64 = b64;
    b64 = NULL; /* ownership transferred to caller (freed via release_csr) */
    rc = AZ_IOT_OK;

done:
    if (der) OPENSSL_free(der);
    if (b64) free(b64);
    if (name) X509_NAME_free(name);
    if (req) X509_REQ_free(req);
    return rc;
}

static void managed_release_csr(
    az_iot_certificate_provider_t* self, az_iot_certificate_signing_request_t* csr)
{
    (void)self;
    if (csr && csr->csr_base64)
    {
        free((void*)csr->csr_base64);
        csr->csr_base64 = NULL;
    }
}

static az_iot_result_t managed_store(
    az_iot_certificate_provider_t* self, const az_iot_issued_certificate_t* issued)
{
    az_iot_certificate_provider_managed_t* m = (az_iot_certificate_provider_managed_t*)self;
    if (!m || !issued) return AZ_IOT_ERR_INVALID_ARG;
    if (!m->loaded) return AZ_IOT_ERR_NOT_INITIALIZED;
    if (!issued->client_cert_chain_pem || issued->count == 0) return AZ_IOT_ERR_INVALID_ARG;

    BIO* b = BIO_new_file(m->operational_cert_path, "wb");
    if (!b) return AZ_IOT_ERR_INTERNAL;

    az_iot_result_t rc = AZ_IOT_OK;
    for (size_t i = 0; i < issued->count; ++i)
    {
        const char* pem = issued->client_cert_chain_pem[i];
        if (!pem) continue;
        int n = (int)strlen(pem);
        if (n > 0)
        {
            if (BIO_write(b, pem, n) != n) { rc = AZ_IOT_ERR_INTERNAL; break; }
            if (pem[n - 1] != '\n') BIO_write(b, "\n", 1); /* separate PEM blocks */
        }
    }
    BIO_free(b);

    if (rc == AZ_IOT_OK) m->has_operational = true;
    return rc;
}

static void managed_deinit_vtable(az_iot_certificate_provider_t* self)
{
    az_iot_certificate_provider_managed_deinit((az_iot_certificate_provider_managed_t*)self);
}

static const az_iot_certificate_provider_vtable_t s_managed_vtable = {
    .version                  = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
    .load                     = managed_load,
    .release                  = managed_release,
    .deinit                   = managed_deinit_vtable,
    .get_csr                  = managed_get_csr,
    .release_csr              = managed_release_csr,
    .store_issued_certificate = managed_store,
};

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

void az_iot_certificate_provider_managed_deinit(
    az_iot_certificate_provider_managed_t* provider)
{
    if (!provider) return;
    if (provider->operational_key) EVP_PKEY_free((EVP_PKEY*)provider->operational_key);
    free(provider->bootstrap_cert_path);
    free(provider->bootstrap_key_path);
    free(provider->trusted_ca_path);
    free(provider->operational_key_path);
    free(provider->operational_cert_path);
    memset(provider, 0, sizeof(*provider));
}

az_iot_result_t az_iot_certificate_provider_managed_init(
    az_iot_certificate_provider_managed_t* provider,
    const az_iot_certificate_provider_managed_options_t* opts)
{
    if (!provider || !opts) return AZ_IOT_ERR_INVALID_ARG;
    if (!opts->bootstrap_cert_pem_path   || !opts->bootstrap_cert_pem_path[0] ||
        !opts->bootstrap_key_pem_path    || !opts->bootstrap_key_pem_path[0] ||
        !opts->operational_key_pem_path  || !opts->operational_key_pem_path[0] ||
        !opts->operational_cert_pem_path || !opts->operational_cert_pem_path[0])
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }

    memset(provider, 0, sizeof(*provider));
    provider->base.vtable = &s_managed_vtable;
    provider->key_type = (int)opts->key_type;

    provider->bootstrap_cert_path   = dup_str(opts->bootstrap_cert_pem_path);
    provider->bootstrap_key_path    = dup_str(opts->bootstrap_key_pem_path);
    provider->operational_key_path  = dup_str(opts->operational_key_pem_path);
    provider->operational_cert_path = dup_str(opts->operational_cert_pem_path);
    if (!provider->bootstrap_cert_path || !provider->bootstrap_key_path ||
        !provider->operational_key_path || !provider->operational_cert_path)
    {
        az_iot_certificate_provider_managed_deinit(provider);
        return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
    if (opts->trusted_ca_pem_path && opts->trusted_ca_pem_path[0])
    {
        provider->trusted_ca_path = dup_str(opts->trusted_ca_pem_path);
        if (!provider->trusted_ca_path)
        {
            az_iot_certificate_provider_managed_deinit(provider);
            return AZ_IOT_ERR_OUT_OF_MEMORY;
        }
    }

    /* Operational key: load if present on disk, else generate and persist. */
    EVP_PKEY* key = load_key_file(provider->operational_key_path);
    if (!key)
    {
        key = generate_key(provider->key_type);
        if (!key)
        {
            az_iot_certificate_provider_managed_deinit(provider);
            return AZ_IOT_ERR_INTERNAL;
        }
        az_iot_result_t wr = write_key_file(provider->operational_key_path, key);
        if (wr != AZ_IOT_OK)
        {
            EVP_PKEY_free(key);
            az_iot_certificate_provider_managed_deinit(provider);
            return wr;
        }
    }
    provider->operational_key = key;

    /* An operational cert persisted by a previous run means we can connect
     * with the OPERATIONAL identity immediately (no re-enrollment needed). */
    provider->has_operational = file_exists(provider->operational_cert_path);

    provider->loaded = true;
    return AZ_IOT_OK;
}
