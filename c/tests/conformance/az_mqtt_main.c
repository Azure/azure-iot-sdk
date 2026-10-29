// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Conformance harness: validates the az_mqtt adapter as an MQTT 3.1.1 client
 * (AZ_IOT_CONFORMANCE_AZ_MQTT_V=3) or an MQTT 5 client (5). Declares the proxy capability, the
 * WebSocket capability when AZ_IOT_CONFORMANCE_WS_PORT names a listener, and, in an OpenSSL build
 * (AZ_IOT_AZ_MQTT_OPENSSL), the key-reference custody route with the material from
 * AZ_IOT_CONFORMANCE_KEY_URI, _KEY_ENGINE and _CLIENT_CERT -- as the Paho harnesses do. In an
 * mbedTLS build with PSA keys (AZ_IOT_AZ_MQTT_MBEDTLS), the PSA custody route with the PEM key at
 * AZ_IOT_CONFORMANCE_PSA_KEY, imported into PSA as non-exportable, and _CLIENT_CERT. */
#include "../conformance/az_iot_conformance.h"
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AZ_IOT_AZ_MQTT_MBEDTLS)
#include <mbedtls/build_info.h>
#if MBEDTLS_VERSION_MAJOR >= 4 || defined(MBEDTLS_USE_PSA_CRYPTO)
#define AZ_IOT_CONFORMANCE_PSA_KEYS
#include <mbedtls/pk.h>
#include <mbedtls/psa_util.h>
#include <psa/crypto.h>
#endif
#endif

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

#if defined(AZ_IOT_CONFORMANCE_PSA_KEYS)
/** @brief Imports the PEM private key at @p path into PSA, signing only and not exportable;
 * returns its id, 0 on failure. */
static uint32_t import_psa_key(const char* path)
{
  static unsigned char pem[16384];
  FILE* file = fopen(path, "rb");
  size_t const length = file != NULL ? fread(pem, 1, sizeof(pem) - 1, file) : 0;
  if (file != NULL)
  {
    (void)fclose(file);
  }
  if (length == 0 || length == sizeof(pem) - 1 || psa_crypto_init() != PSA_SUCCESS)
  {
    fprintf(stderr, "conformance: cannot read the PSA key from '%s'\n", path);
    return 0;
  }
  pem[length] = '\0';
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
#if MBEDTLS_VERSION_MAJOR >= 4
  int rc = mbedtls_pk_parse_key(&pk, pem, length + 1, NULL, 0);
#else
  int rc = mbedtls_pk_parse_key(
      &pk, pem, length + 1, NULL, 0, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
#endif
  memset(pem, 0, sizeof(pem));
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
  rc = rc == 0 ? mbedtls_pk_get_psa_attributes(&pk, PSA_KEY_USAGE_SIGN_HASH, &attributes) : rc;
  // Those attributes also allow export and copy: an adapter could then read the key.
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
  rc = rc == 0 ? mbedtls_pk_import_into_psa(&pk, &attributes, &id) : rc;
  mbedtls_pk_free(&pk);
  uint8_t exported[1];
  size_t exported_length = 0;
  if (rc != 0
      || psa_export_key(id, exported, sizeof(exported), &exported_length)
          != PSA_ERROR_NOT_PERMITTED)
  {
    fprintf(stderr, "conformance: cannot import '%s' as a non-exportable PSA key (%d)\n", path, rc);
    (void)psa_destroy_key(id);
    return 0;
  }
  return (uint32_t)id;
}
#endif

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
#if defined(AZ_IOT_CONFORMANCE_PSA_KEYS)
  char psa_key_path[512];
  char client_cert[512];
  const char* psa_key = NULL;
  /* Absent material, the declared capability is never exercised and the run fails (see
   * AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN). */
  opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_PSA;
  if (env_or_null("AZ_IOT_CONFORMANCE_PSA_KEY", psa_key_path, sizeof(psa_key_path), &psa_key) != 0
      || env_or_null(
             "AZ_IOT_CONFORMANCE_CLIENT_CERT",
             client_cert,
             sizeof(client_cert),
             &opts.client_cert_path)
          != 0
      || (psa_key != NULL && (opts.client_key_psa_id = import_psa_key(psa_key)) == 0))
  {
    az_iot_az_mqtt_factory_destroy(f);
    return 1;
  }
#endif
  opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_PROXY;
  opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_TLS_PEM;
  int rc = az_iot_conformance_run_with_options(suite, f, &opts);
  az_iot_az_mqtt_factory_destroy(f);
#if defined(AZ_IOT_CONFORMANCE_PSA_KEYS)
  if (opts.client_key_psa_id != 0)
  {
    (void)psa_destroy_key((mbedtls_svc_key_id_t)opts.client_key_psa_id);
  }
#endif
  return rc;
}
