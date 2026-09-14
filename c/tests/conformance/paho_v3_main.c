// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Conformance harness: validates the bundled Paho adapter as an MQTTv3.1.1 client.
 * The same harness pattern is what customers will use to validate their own
 * adapter against the suite. */
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
  az_iot_mqtt_factory* f = az_iot_paho_factory_create_v3_1_1();

  az_iot_conformance_options opts = { 0 };
#if defined(AZ_IOT_PAHO_KEY_CUSTODY)
  char key_uri[512];
  char key_engine[128];
  char client_cert[512];
  /* The adapter honours a non-extractable key reference in this build, so the
   * suite is told to hold it to that contract. Without the declaration the
   * baseline would only check that it REFUSES custody cleanly, which is the
   * wrong bar for an adapter that implements it.
   *
   * The URI route ONLY. Paho exposes no TLS key callback, so this adapter
   * cannot honour tls.sign and must not claim _SIGN: the baseline already
   * requires it to refuse that route cleanly, which it does. */
  opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI;
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

  /* The adapter implements MQTT over WebSockets and the HTTP CONNECT proxy, so
   * the suite is told to hold it to both contracts rather than only to the
   * baseline (which checks it does not bypass either setting).
   *
   * Each capability is declared only when this machine can supply the endpoint
   * that PROVES it, because a declared-but-unexercised claim fails the run and a
   * leg with no proxy or no WebSocket listener is an ordinary environment, not
   * an error. Nothing is skipped by leaving them undeclared: the baseline cases
   * run for every adapter on every leg and are what pin the rule that matters --
   * neither setting may be bypassed into a plain TCP session to the broker. */
  char ws_port_buf[16];
  char ws_path_buf[256];
  const char* ws_port_str = NULL;
  if (env_or_null("AZ_IOT_CONFORMANCE_WS_PORT", ws_port_buf, sizeof(ws_port_buf), &ws_port_str) == 0
      && ws_port_str != NULL)
  {
    unsigned long p = strtoul(ws_port_str, NULL, 10);
    if (p > 0 && p <= 65535)
    {
      opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_WEBSOCKETS;
      opts.websocket_port = (uint16_t)p;
      (void)env_or_null(
          "AZ_IOT_CONFORMANCE_WS_PATH", ws_path_buf, sizeof(ws_path_buf), &opts.websocket_path);
    }
  }

  char proxy_host_buf[256];
  char proxy_port_buf[16];
  const char* proxy_host = NULL;
  const char* proxy_port_str = NULL;
  if (env_or_null(
          "AZ_IOT_CONFORMANCE_PROXY_HOST", proxy_host_buf, sizeof(proxy_host_buf), &proxy_host)
          == 0
      && proxy_host != NULL
      && env_or_null(
             "AZ_IOT_CONFORMANCE_PROXY_PORT",
             proxy_port_buf,
             sizeof(proxy_port_buf),
             &proxy_port_str)
          == 0
      && proxy_port_str != NULL)
  {
    unsigned long p = strtoul(proxy_port_str, NULL, 10);
    if (p > 0 && p <= 65535)
    {
      opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_PROXY;
      opts.proxy_host = proxy_host;
      opts.proxy_port = (uint16_t)p;
    }
  }

  int rc = az_iot_conformance_run_with_options(AZ_IOT_CONFORMANCE_SUITE_V3_1_1, f, &opts);
  az_iot_paho_factory_destroy(f);
  return rc;
}
