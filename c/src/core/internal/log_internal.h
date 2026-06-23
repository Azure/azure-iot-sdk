// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_LOG_INTERNAL_H
#define AZ_IOT_LOG_INTERNAL_H

#include "azure/iot/az_iot_log.h"

/* Internal dispatch — call through the registered global sink. */
void az_iot_log_emit(az_iot_log_level_t level, const char* file, int line, const char* msg);

#define AZ_IOT_LOG_ERROR(msg) \
    az_iot_log_emit(AZ_IOT_LOG_ERROR, __FILE__, __LINE__, (msg))

#define AZ_IOT_LOG_WARN(msg) \
    az_iot_log_emit(AZ_IOT_LOG_WARN, __FILE__, __LINE__, (msg))

#define AZ_IOT_LOG_INFO(msg) \
    az_iot_log_emit(AZ_IOT_LOG_INFO, __FILE__, __LINE__, (msg))

#endif /* AZ_IOT_LOG_INTERNAL_H */
