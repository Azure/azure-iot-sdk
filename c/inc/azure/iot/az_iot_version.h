// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_VERSION_H
#define AZ_IOT_VERSION_H

#define AZ_IOT_VERSION_MAJOR 0
#define AZ_IOT_VERSION_MINOR 0
#define AZ_IOT_VERSION_PATCH 1
#define AZ_IOT_VERSION_STRING "0.0.1"

#ifdef __cplusplus
extern "C" {
#endif

const char* az_iot_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_VERSION_H */
