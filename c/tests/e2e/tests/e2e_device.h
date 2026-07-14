// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

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
 */
#ifndef E2E_DEVICE_H
#define E2E_DEVICE_H

#include <stdbool.h>

#include "azure/iot/az_iot.h"

#ifdef __cplusplus
extern "C" {
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

    /* Live device client. */
    az_iot_certificate_provider_pem certs;
    az_iot_connection_client        conn;
    az_iot_connection_state         conn_state;
    bool                              certs_ok;
    bool                              conn_ok;

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
