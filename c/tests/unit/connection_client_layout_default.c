// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* Built with the SDK's capacity macros: sizes as the SDK library sees them. */

#include <stddef.h>

#include "azure/iot/az_iot_connection_client.h"

size_t az_iot_test_connection_client_default_size(void);

size_t az_iot_test_connection_client_default_size(void) { return sizeof(az_iot_connection_client); }
