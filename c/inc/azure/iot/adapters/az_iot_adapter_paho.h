// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_adapter_paho.h
 * @brief Public entry points for the Paho-C MQTT adapter.
 *
 * Construct one or both factories and register them with the connection
 * client. The SDK selects the appropriate factory by MQTT version at
 * connection time (v3.1.1 for DPS and mqttv3, v5 for mqttv5). Each factory
 * creates a fresh Paho client per session.
 *
 * Factory ownership:
 * - The application owns a created factory until
 *   az_iot_connection_client_register_mqtt_factory() returns AZ_IOT_OK. If
 *   setup stops before that, or registration fails, release it with
 *   az_iot_paho_factory_destroy().
 * - Once registered, the connection client owns it and
 *   az_iot_connection_client_deinit() destroys it. Calling
 *   az_iot_paho_factory_destroy() on it as well is a double free.
 *
 * A factory does not own the clients it produced; those follow the
 * az_iot_mqtt_iface destroy() contract.
 *
 * @code
 * az_iot_mqtt_factory* v3 = az_iot_paho_factory_create_v3_1_1();
 * if (az_iot_connection_client_register_mqtt_factory(&client, v3) != AZ_IOT_OK)
 * {
 *   az_iot_paho_factory_destroy(v3); // not registered: still owned here (NULL: no-op)
 *   // handle the error
 * }
 * // ... use the client ...
 * az_iot_connection_client_deinit(&client); // destroys every registered factory
 * // no az_iot_paho_factory_destroy(v3) here
 * @endcode
 */
#ifndef AZ_IOT_ADAPTER_PAHO_H
#define AZ_IOT_ADAPTER_PAHO_H

#include "../az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Create a factory of MQTT 3.1.1 Paho clients.
   * @return The factory, or NULL when out of memory. Owned by the caller until registered.
   */
  az_iot_mqtt_factory* az_iot_paho_factory_create_v3_1_1(void);

  /**
   * @brief Create a factory of MQTT 5 Paho clients.
   * @return The factory, or NULL when out of memory. Owned by the caller until registered.
   */
  az_iot_mqtt_factory* az_iot_paho_factory_create_v5(void);

  /**
   * @brief Destroy a factory from either function above (NULL: no-op). Clients it created are
   * not destroyed.
   *
   * Only for a factory not registered with a connection client: a registered factory is owned by
   * the connection client and destroyed by az_iot_connection_client_deinit().
   */
  void az_iot_paho_factory_destroy(az_iot_mqtt_factory* factory);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADAPTER_PAHO_H */
