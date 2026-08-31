// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Conformance harness: validates the bundled Paho adapter as an MQTTv3.1.1 client.
 * The same harness pattern is what customers will use to validate their own
 * adapter against the suite. */
#include "../conformance/az_iot_conformance.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(AZ_IOT_PAHO_KEY_CUSTODY)
/* Read an environment variable into `buf`, returning it when the variable is
 * set and non-empty and NULL otherwise. getenv_s on Windows because plain
 * getenv is deprecated there and this build treats warnings as errors. */
static const char* env_or_null(const char* name, char* buf, size_t cap)
{
  if (cap == 0)
  {
    return NULL;
  }
  buf[0] = '\0';
#if defined(_WIN32)
  size_t needed = 0;
  if (getenv_s(&needed, buf, cap, name) != 0 || needed == 0)
  {
    buf[0] = '\0';
    return NULL;
  }
#else
  const char* value = getenv(name);
  if (value == NULL)
  {
    return NULL;
  }
  snprintf(buf, cap, "%s", value);
#endif
  return buf[0] != '\0' ? buf : NULL;
}
#endif

int main(void)
{
  az_iot_mqtt_factory* f = az_iot_paho_factory_create_v3_1_1();

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
   * Absent these the suite runs the custody cases that need no token and says
   * on stderr that the handshake was not exercised. */
  opts.key_uri = env_or_null("AZ_IOT_CONFORMANCE_KEY_URI", key_uri, sizeof(key_uri));
  opts.crypto_engine_id
      = env_or_null("AZ_IOT_CONFORMANCE_KEY_ENGINE", key_engine, sizeof(key_engine));
  opts.client_cert_path
      = env_or_null("AZ_IOT_CONFORMANCE_CLIENT_CERT", client_cert, sizeof(client_cert));
#endif

  int rc = az_iot_conformance_run_with_options(AZ_IOT_CONFORMANCE_SUITE_V3_1_1, f, &opts);
  az_iot_paho_factory_destroy(f);
  return rc;
}
