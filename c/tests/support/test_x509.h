// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_TEST_X509_H
#define AZ_IOT_TEST_X509_H

/* X.509 helpers for tests, limited to calls that are neither removed nor deprecated
 * from OpenSSL 3.0 through 4.x. */

#include <openssl/asn1.h>
#include <openssl/objects.h>
#include <openssl/x509.h>

/**
 * @brief Sets the subject and issuer of @p certificate to CN=@p common_name.
 *
 * @return 1 on success, 0 on failure.
 */
static inline int test_x509_set_self_signed_name(X509* certificate, const char* common_name)
{
  X509_NAME* name = X509_NAME_new();
  int ok = name != NULL
      && X509_NAME_add_entry_by_txt(
               name, "CN", MBSTRING_UTF8, (const unsigned char*)common_name, -1, -1, 0)
      && X509_set_subject_name(certificate, name) && X509_set_issuer_name(certificate, name);
  X509_NAME_free(name);
  return ok;
}

/**
 * @brief Returns the subject common name of @p request as UTF-8.
 *
 * @return The name, released with OPENSSL_free(); NULL when absent or on failure.
 */
static inline char* test_x509_request_common_name(const X509_REQ* request)
{
  const X509_NAME* subject = X509_REQ_get_subject_name(request);
  int index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
  unsigned char* common_name = NULL;
  if (index < 0
      || ASN1_STRING_to_UTF8(
             &common_name, X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, index)))
          < 0)
  {
    return NULL;
  }
  return (char*)common_name;
}

#endif /* AZ_IOT_TEST_X509_H */
