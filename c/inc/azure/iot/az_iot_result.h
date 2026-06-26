// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_RESULT_H
#define AZ_IOT_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum az_iot_result_tag
{
    AZ_IOT_OK = 0,
    AZ_IOT_ERR_INVALID_ARG,
    AZ_IOT_ERR_OUT_OF_MEMORY,
    AZ_IOT_ERR_NOT_INITIALIZED,
    AZ_IOT_ERR_ALREADY_INITIALIZED,
    AZ_IOT_ERR_NOT_CONNECTED,
    AZ_IOT_ERR_TIMEOUT,
    AZ_IOT_ERR_TLS,
    AZ_IOT_ERR_AUTH,
    AZ_IOT_ERR_PROTOCOL,
    AZ_IOT_ERR_MQTT,
    AZ_IOT_ERR_DPS,
    AZ_IOT_ERR_NOT_SUPPORTED,
    AZ_IOT_ERR_BUSY,
    AZ_IOT_ERR_NOT_ENOUGH_SPACE,
    AZ_IOT_ERR_DETACHED,
    AZ_IOT_ERR_INTERNAL,
    AZ_IOT_ERR_NOT_FOUND
} az_iot_result_t;

const char* az_iot_result_to_string(az_iot_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_RESULT_H */
