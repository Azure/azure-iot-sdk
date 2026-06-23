// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Public entry points for the Paho-C MQTT adapter.
 *
 * Construct one or both factories and register them with the connection
 * client. The SDK selects the appropriate factory by MQTT version at
 * connection time (v3.1.1 for DPS/Classic, v5 for Hub-Next). Each factory
 * creates a fresh Paho client per session.
 *
 * Lifetime: the returned factory pointer remains valid until
 * az_iot_paho_factory_destroy() is called by the application. The
 * factory does NOT own any clients it produced - those follow the iface
 * destroy() contract.
 */
#ifndef AZ_IOT_ADAPTER_PAHO_H
#define AZ_IOT_ADAPTER_PAHO_H

#include "../az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build a factory that produces MQTTv3.1.1 Paho clients. */
az_iot_mqtt_factory_t* az_iot_paho_factory_create_v3_1_1(void);

/* Build a factory that produces MQTTv5 Paho clients. */
az_iot_mqtt_factory_t* az_iot_paho_factory_create_v5(void);

/* Destroy a factory produced by either constructor above. Does not destroy
 * clients the factory has handed out. */
void az_iot_paho_factory_destroy(az_iot_mqtt_factory_t* factory);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADAPTER_PAHO_H */
