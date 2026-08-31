// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Conformance harness: validates the bundled Paho adapter as an MQTTv5 client. */
#include "../conformance/az_iot_conformance.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include <stdint.h>
#include <stdlib.h>

int main(void)
{
  az_iot_mqtt_factory* f = az_iot_paho_factory_create_v5();

  az_iot_conformance_options opts = { 0 };
#if defined(AZ_IOT_PAHO_KEY_CUSTODY)
  /* The adapter honours a non-extractable key reference in this build, so the
   * suite is told to hold it to that contract. Without the declaration the
   * baseline would only check that it REFUSES custody cleanly, which is the
   * wrong bar for an adapter that implements it. */
  opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY;
  /* The end-to-end handshake needs a key this machine can actually reach.
   * Absent these the suite runs the custody cases that need no token and says
   * on stderr that the handshake was not exercised. */
  opts.key_uri = getenv("AZ_IOT_CONFORMANCE_KEY_URI");
  opts.crypto_engine_id = getenv("AZ_IOT_CONFORMANCE_KEY_ENGINE");
  opts.client_cert_path = getenv("AZ_IOT_CONFORMANCE_CLIENT_CERT");
#endif

  int rc = az_iot_conformance_run_with_options(AZ_IOT_CONFORMANCE_SUITE_V5, f, &opts);
  az_iot_paho_factory_destroy(f);
  return rc;
}
