// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Conformance harness: validates the az_mqtt adapter as an MQTT 3.1.1 client
 * (AZ_IOT_CONFORMANCE_AZ_MQTT_V=3) or an MQTT 5 client (5). Declares the proxy capability, the
 * WebSocket capability when AZ_IOT_CONFORMANCE_WS_PORT names a listener, and, in an OpenSSL build
 * (AZ_IOT_AZ_MQTT_OPENSSL), the key-reference custody route with the material from
 * AZ_IOT_CONFORMANCE_KEY_URI, _KEY_ENGINE and _CLIENT_CERT -- as the Paho harnesses do. */
#include "../conformance/az_iot_conformance.h"
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
#if AZ_IOT_CONFORMANCE_AZ_MQTT_V == 5
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v5();
  az_iot_conformance_suite const suite = AZ_IOT_CONFORMANCE_SUITE_V5;
#else
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v3_1_1();
  az_iot_conformance_suite const suite = AZ_IOT_CONFORMANCE_SUITE_V3_1_1;
#endif
  if (f == NULL)
  {
    return 1;
  }
  az_iot_conformance_options opts = { 0 };
#if defined(AZ_IOT_AZ_MQTT_OPENSSL)
  char key_uri[512];
  char key_engine[128];
  char client_cert[512];
  /* The URI route only: the adapter refuses a sign callback, which the baseline checks. Absent
   * material, the declared capability is never exercised and the run fails (see
   * AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN). */
  opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI;
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
    az_iot_az_mqtt_factory_destroy(f);
    return 1;
  }
#endif
  char ws_port_buf[16];
  char ws_path_buf[256];
  const char* ws_port_str = NULL;
  if (env_or_null("AZ_IOT_CONFORMANCE_WS_PORT", ws_port_buf, sizeof(ws_port_buf), &ws_port_str)
      != 0)
  {
    az_iot_az_mqtt_factory_destroy(f);
    return 1;
  }
  if (ws_port_str != NULL)
  {
    /* Set but not a port: fail rather than run without the WebSocket cases. */
    char* end = NULL;
    errno = 0;
    unsigned long p = strtoul(ws_port_str, &end, 10);
    if (errno != 0 || end == ws_port_str || *end != '\0' || ws_port_str[0] == '-' || p == 0
        || p > 65535)
    {
      fprintf(stderr, "conformance: AZ_IOT_CONFORMANCE_WS_PORT is not a port: '%s'\n", ws_port_str);
      az_iot_az_mqtt_factory_destroy(f);
      return 1;
    }
    {
      opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_WEBSOCKETS;
      opts.websocket_port = (uint16_t)p;
      if (env_or_null(
              "AZ_IOT_CONFORMANCE_WS_PATH", ws_path_buf, sizeof(ws_path_buf), &opts.websocket_path)
          != 0)
      {
        az_iot_az_mqtt_factory_destroy(f);
        return 1;
      }
    }
  }
  opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_PROXY;
  int rc = az_iot_conformance_run_with_options(suite, f, &opts);
  az_iot_az_mqtt_factory_destroy(f);
  return rc;
}
