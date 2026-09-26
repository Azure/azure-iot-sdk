// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The firmware's own software update version. This is the version the device reports
 * as its installedUpdateId and the version software updates matches a deployment against.
 *
 * New-SuEsp32Image.ps1 REWRITES this file before each build so the firmware
 * baked into the update image and the version in the software updates import manifest stay
 * in lock-step. Edit the default here only for the very first (factory) flash. */
#ifndef SU_VERSION_H
#define SU_VERSION_H

#define SU_UPDATE_PROVIDER "Contoso"
#define SU_UPDATE_NAME "ESP32-SU"
#define SU_UPDATE_VERSION "1.0.0"

#endif /* SU_VERSION_H */
