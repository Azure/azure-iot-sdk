// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Conformance harness: validates the bundled Paho adapter as an MQTTv5 client. */
#include "../conformance/az_iot_conformance.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AZ_IOT_PAHO_KEY_CUSTODY)
/* Read an environment variable into `buf`. Writes the value to *out (NULL when
 * unset or empty) and returns 0; returns -1 if the value does not fit.
 *
 * A value too long is an ERROR, not an empty one. These name a key URI and a
 * certificate path: a truncated URI is a different key, and a truncated path is
 * a different file, so accepting either would produce a conformance result
 * about a credential nobody configured. Reporting it as unset would be almost
 * as bad -- the suite would say the handshake was not exercised when the
 * operator had in fact supplied a key.
 *
 * getenv_s on Windows because plain getenv is deprecated there and this build
 * treats warnings as errors. */
static int env_or_null(const char* name, char* buf, size_t cap, const char** out)
{
  *out = NULL;
  if (cap == 0)
  {
    return -1;
  }
  buf[0] = '\0';
#if defined(_WIN32)
  size_t needed = 0;
  errno_t rc = getenv_s(&needed, buf, cap, name);
  if (rc == ERANGE)
  {
    fprintf(
        stderr,
        "conformance: %s is too long for this harness (%u bytes needed, %u available)\n",
        name,
        (unsigned)needed,
        (unsigned)cap);
    buf[0] = '\0';
    return -1;
  }
  if (rc != 0 || needed == 0)
  {
    buf[0] = '\0';
    return 0;
  }
#else
  const char* value = getenv(name);
  if (value == NULL)
  {
    return 0;
  }
  size_t len = strlen(value);
  if (len >= cap)
  {
    fprintf(
        stderr,
        "conformance: %s is too long for this harness (%u bytes, %u available)\n",
        name,
        (unsigned)len,
        (unsigned)(cap - 1));
    return -1;
  }
  memcpy(buf, value, len + 1);
#endif
  *out = buf[0] != '\0' ? buf : NULL;
  return 0;
}
#endif

int main(void)
{
  az_iot_mqtt_factory* f = az_iot_paho_factory_create_v5();

  az_iot_conformance_options opts = { 0 };
#if defined(AZ_IOT_PAHO_KEY_CUSTODY)
  char key_uri[512];
  char key_engine[128];
  char client_cert[512];
  /* The adapter honours a non-extractable key reference in this build, so the
   * suite is told to hold it to that contract. Without the declaration the
   * baseline would only check that it REFUSES custody cleanly, which is the
   * wrong bar for an adapter that implements it. */
  opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY;
  /* The end-to-end handshake needs a key this machine can actually reach.
   * Absent these the capability is declared but never exercised, which FAILS
   * the run: a pass has to mean the claim was checked. A machine with no token
   * must say so deliberately with AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN=1, which
   * downgrades it to a notice and proves nothing about custody. */
  if (env_or_null("AZ_IOT_CONFORMANCE_KEY_URI", key_uri, sizeof(key_uri), &opts.key_uri) != 0
      || env_or_null(
             "AZ_IOT_CONFORMANCE_KEY_ENGINE",
             key_engine,
             sizeof(key_engine),
             &opts.crypto_engine_id)
          != 0
      || env_or_null(
             "AZ_IOT_CONFORMANCE_CLIENT_CERT",
             client_cert,
             sizeof(client_cert),
             &opts.client_cert_path)
          != 0)
  {
    az_iot_paho_factory_destroy(f);
    return 1;
  }
#endif

  int rc = az_iot_conformance_run_with_options(AZ_IOT_CONFORMANCE_SUITE_V5, f, &opts);
  az_iot_paho_factory_destroy(f);
  return rc;
}
