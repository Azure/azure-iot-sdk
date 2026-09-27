// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "azure/iot/az_iot_abi.h"
#include "azure/iot/az_iot_version.h"

const char* az_iot_version_string(void) { return AZ_IOT_VERSION_STRING; }

uint32_t az_iot_abi_fingerprint(void) { return AZ_IOT_ABI_FINGERPRINT; }
