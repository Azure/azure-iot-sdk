// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file az_iot_log_components.h
 * @brief Every component this SDK passes to the logging facade (az_iot_log.h),
 * in one place. Kept out of az_iot_log.h so the facade carries no SDK-specific
 * identities. */
#ifndef AZ_IOT_LOG_COMPONENTS_H
#define AZ_IOT_LOG_COMPONENTS_H

/**
 * @name Log components
 * @brief Values of the @p component argument, one per SDK area. Compare them
 * with strcmp() to filter or route; AZ_IOT_LOG_COMPONENT_APP is for application
 * messages and is never used by the SDK.
 * @{
 */
#define AZ_IOT_LOG_COMPONENT_APP "app"
#define AZ_IOT_LOG_COMPONENT_CONNECTION "connection"
#define AZ_IOT_LOG_COMPONENT_DPS "dps"
#define AZ_IOT_LOG_COMPONENT_CERT "cert"
#define AZ_IOT_LOG_COMPONENT_CERT_PEM "cert_pem"
#define AZ_IOT_LOG_COMPONENT_SU "su"
#define AZ_IOT_LOG_COMPONENT_PAHO "paho"
#define AZ_IOT_LOG_COMPONENT_AZ_MQTT "az_mqtt"
#define AZ_IOT_LOG_COMPONENT_C2D "c2d"
#define AZ_IOT_LOG_COMPONENT_MQTTV3_TELEMETRY "mqttv3_telemetry"
#define AZ_IOT_LOG_COMPONENT_MQTTV3_TWIN "mqttv3_twin"
#define AZ_IOT_LOG_COMPONENT_MQTTV3_DIRECT_METHOD "mqttv3_direct_method"
#define AZ_IOT_LOG_COMPONENT_MQTTV3_FILE_UPLOAD "mqttv3_file_upload"
#define AZ_IOT_LOG_COMPONENT_MQTTV5_TELEMETRY "mqttv5_telemetry"
#define AZ_IOT_LOG_COMPONENT_MQTTV5_TWIN "mqttv5_twin"
#define AZ_IOT_LOG_COMPONENT_MQTTV5_DIRECT_METHOD "mqttv5_direct_method"
/** @} */

#endif /* AZ_IOT_LOG_COMPONENTS_H */
