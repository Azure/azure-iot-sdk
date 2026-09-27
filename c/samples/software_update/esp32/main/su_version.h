// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The firmware's own software update version. This is the version the device reports
 * as its installedUpdateId and the version software updates matches a deployment against.
 *
 * Raise SU_UPDATE_VERSION for each update image, and import the image under the
 * same version, so the device reports what it runs. */
#ifndef SU_VERSION_H
#define SU_VERSION_H

#define SU_UPDATE_PROVIDER "Contoso"
#define SU_UPDATE_NAME "ESP32-SU"
#define SU_UPDATE_VERSION "1.0.0"

#endif /* SU_VERSION_H */
