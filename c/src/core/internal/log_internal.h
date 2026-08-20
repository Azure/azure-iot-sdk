// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_LOG_INTERNAL_H
#define AZ_IOT_LOG_INTERNAL_H

/* The emit functions and the AZ_IOT_LOG_* macros moved to the public header so
 * that MQTT adapters, which cannot see internal headers, can reach the
 * application's sink instead of writing to stderr. This header stays as the
 * include SDK sources already use. */
#include "azure/iot/az_iot_log.h"

#endif /* AZ_IOT_LOG_INTERNAL_H */
