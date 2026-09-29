// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* esp-mqtt adapter: public factory builders.
 *
 * Implements the azure-iot-sdk az_iot_mqtt_iface vtable on top of ESP-IDF's
 * native esp-mqtt client (the `mqtt` component). Register one or both factories
 * with the connection client; the SDK selects v3.1.1 for DPS/MQTTv3 and v5 for
 * MQTTv5 at connection time.
 */
#ifndef AZ_IOT_MQTT_ESP_H
#define AZ_IOT_MQTT_ESP_H

#include "azure/iot/az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Build a factory that produces MQTT v3.1.1 esp-mqtt clients. */
  az_iot_mqtt_factory* az_iot_esp_mqtt_factory_create_v3_1_1(void);

  /* Build a factory that produces MQTT v5 esp-mqtt clients. */
  az_iot_mqtt_factory* az_iot_esp_mqtt_factory_create_v5(void);

  /* Destroy a factory produced by either constructor above. Does not destroy
   * clients the factory has handed out. */
  void az_iot_esp_mqtt_factory_destroy(az_iot_mqtt_factory* factory);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTT_ESP_H */
