// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/*
 * A real MQTT 3.1.1 transport for the end-to-end suites: an az_iot_mqtt_iface
 * implementation that talks to a live service.
 *
 * Why this exists beside the Paho adapter:
 *
 *  - It can reach a service through an HTTP CONNECT proxy. Paho resolves and
 *    connects itself, so it cannot be used from a network where the only route
 *    out is a proxy.
 *  - It can present a SAS password. The shipping core is X.509-only: it never
 *    fills connect_options.password, and az_iot_mqtt_iface deliberately leaves
 *    credentials to the adapter. Supplying one here is what an adapter for a
 *    symmetric-key device would do, and it lets the e2e suite run against an
 *    environment enrolled that way without changing the SDK.
 *
 * Everything above the iface -- connection client, DPS flow, feature clients --
 * is the shipping SDK. Only the bytes on the socket are ours.
 *
 * Not for production: no reconnect, no QoS 2, no flow control, and a fixed
 * receive bound. It is a test transport, kept small enough to be obviously
 * correct.
 */
#ifndef AZ_IOT_E2E_MQTT_TRANSPORT_H
#define AZ_IOT_E2E_MQTT_TRANSPORT_H

#include <stddef.h>

#include "azure/iot/az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Asks the host for the password to present for @p username.
   *
   * Called on every CONNECT, so a SAS token is minted fresh rather than being
   * captured once and left to expire mid-run. Write at most @p out_size bytes
   * including the terminator; write nothing to connect with no password. */
  typedef void (
      *az_iot_e2e_mqtt_password_cb)(const char* username, char* out, size_t out_size, void* ctx);

  typedef struct az_iot_e2e_mqtt_config
  {
    /* "host:port" of an HTTP CONNECT proxy, or NULL to connect directly. */
    const char* proxy;
    /* PEM bundle used to verify the service. NULL selects the system store. */
    const char* ca_file;
    az_iot_e2e_mqtt_password_cb password_cb;
    void* password_ctx;
  } az_iot_e2e_mqtt_config;

  /* Create a factory the connection client can be handed.
   *
   * The config struct is copied, but only shallowly: every client the factory
   * creates copies those same pointers. So the strings must outlive every
   * connection client and session built from this factory, not merely the
   * factory object -- pointing them at storage that goes out of scope leaves a
   * live client dereferencing it. String literals or process-lifetime buffers
   * are the intended use.
   *
   * Returns NULL on failure. */
  az_iot_mqtt_factory* az_iot_e2e_mqtt_factory_create(const az_iot_e2e_mqtt_config* config);

  /* Destroy a factory that was never adopted by a connection client. */
  void az_iot_e2e_mqtt_factory_destroy(az_iot_mqtt_factory* factory);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_E2E_MQTT_TRANSPORT_H */
