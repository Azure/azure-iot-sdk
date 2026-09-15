// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Shared device-side fixture for the in-process end-to-end suites.
 *
 * Both e2e executables (the fast scenario suite and the ADU suite) need the same
 * "device half": load the DPS/X.509 configuration from the environment, provision
 * via a DPS X.509 individual enrollment, and connect to the assigned IoT Hub over
 * the Paho MQTT adapter. This module owns that boilerplate so each suite only adds
 * its scenario-specific code.
 *
 * The device id targeted by the service side is the DPS registration id (the SDK's
 * default: device id == registration id for individual enrollments).
 *
 * Configuration (set by the e2e CI job; device material is materialized to files):
 *   AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID,
 *   AZ_IOT_CLIENT_CERT, AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA,
 *   AZ_IOT_DPS_GLOBAL_ENDPOINT (optional)
 *
 * Two provisioning modes, chosen by which variables are present:
 *   - DPS (the default): AZ_IOT_DPS_ID_SCOPE + AZ_IOT_DPS_REGISTRATION_ID.
 *   - Direct hub connect: AZ_IOT_HUB_HOSTNAME + AZ_IOT_DEVICE_ID, for an
 *     environment whose DPS instance cannot hand out an assignment. The hub
 *     half of every scenario is identical either way.
 *
 * Egress (optional, both modes, applied to the DPS and the hub connect alike):
 *   AZ_IOT_MQTT_TRANSPORT   "tcp" (default) or "websocket"
 *   AZ_IOT_MQTT_WEBSOCKET_PATH  overrides the Azure default path
 *   AZ_IOT_PROXY_HOST, AZ_IOT_PROXY_PORT,
 *   AZ_IOT_PROXY_USERNAME, AZ_IOT_PROXY_PASSWORD
 */
#ifndef E2E_DEVICE_H
#define E2E_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "azure/iot/az_iot.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Device-side fixture. All fields are owned by the module; drive it only through
   * the functions below. */
  typedef struct e2e_device
  {
    /* Configuration copied from the environment (heap-owned). */
    char* id_scope;
    char* reg_id;
    char* cert;
    char* key;
    char* ca;
    char* global_endpoint; /* optional */
    /* Direct-hub mode (no DPS). Both set, or both NULL. */
    char* hub_hostname;
    char* hub_device_id;
    /* Egress configuration read from the environment. */
    az_iot_mqtt_transport transport;
    char* websocket_path; /* optional */
    char* proxy_host; /* NULL = direct connection */
    uint16_t proxy_port;
    char* proxy_username;
    char* proxy_password;

    /* Live device client. */
    az_iot_certificate_provider_pem certs;
    az_iot_connection_client conn;
    az_iot_connection_state conn_state;
    bool certs_ok;
    bool conn_ok;

    /* Convenience alias == reg_id; the id the service side targets. */
    const char* device_id;
  } e2e_device;

  /* Load the device configuration from the environment, initialize the certificate
   * provider, provision via DPS, and block (pumping) until the device reaches the
   * CONNECTED state. Returns 0 on success, non-zero on failure (a message is written
   * to stderr). On failure the partially-initialized device is left safe to pass to
   * e2e_device_disconnect(). */
  int e2e_device_connect(e2e_device* dev);

  /* Advance the device MQTT stack for a single slice of up to @p ms milliseconds. */
  void e2e_device_do_work(e2e_device* dev, int ms);

  /* Close the connection (pumping until idle), deinitialize the client and the
   * certificate provider, and release the configuration. Safe to call on a device
   * that never fully connected. */
  void e2e_device_disconnect(e2e_device* dev);

#ifdef __cplusplus
}
#endif

#endif /* E2E_DEVICE_H */
