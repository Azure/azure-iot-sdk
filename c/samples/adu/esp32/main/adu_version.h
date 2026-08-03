// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The firmware's own ADU update version. This is the version the device reports
 * as its installedUpdateId and the version ADU matches a deployment against.
 *
 * New-AduEsp32Image.ps1 REWRITES this file before each build so the firmware
 * baked into the update image and the version in the ADU import manifest stay
 * in lock-step. Edit the default here only for the very first (factory) flash. */
#ifndef ADU_VERSION_H
#define ADU_VERSION_H

#define ADU_UPDATE_PROVIDER "Contoso"
#define ADU_UPDATE_NAME "ESP32-ADU"
#define ADU_UPDATE_VERSION "1.0.0"

#endif /* ADU_VERSION_H */
