// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* In-memory az_iot_certificate_provider for ESP32: hands the SDK PEM material
 * that is compiled into the firmware (EMBED_TXTFILES), so the device needs no
 * filesystem. The esp-mqtt adapter consumes the client_cert_pem/client_key_pem/
 * trusted_ca_pem fields it returns. */
#ifndef AZ_IOT_CERT_EMBEDDED_H
#define AZ_IOT_CERT_EMBEDDED_H

#include "azure/iot/az_iot_certificate_provider.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct az_iot_cert_embedded
  {
    az_iot_certificate_provider base; /* MUST be first */
    const char* ca_pem;
    const char* cert_pem;
    const char* key_pem;
  } az_iot_cert_embedded;

  /* Initialize the provider with NUL-terminated PEM strings. @p ca_pem may be NULL
   * to fall back to the platform CA bundle. The strings are referenced, not
   * copied, so they must outlive the provider. */
  void az_iot_cert_embedded_init(
      az_iot_cert_embedded* provider,
      const char* ca_pem,
      const char* cert_pem,
      const char* key_pem);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CERT_EMBEDDED_H */
