// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file
 * @brief SDK version (https://semver.org).
 *
 * Single source of truth for the version: CMake, packaging and the release tag
 * (`c/<AZ_IOT_VERSION_STRING>`) derive from it; eng/check-version.sh checks the copies.
 */
#ifndef AZ_IOT_VERSION_H
#define AZ_IOT_VERSION_H

/** @brief Full version: `MAJOR.MINOR.PATCH[-PRERELEASE]`. */
#define AZ_IOT_VERSION_STRING "1.0.0-preview"

/** @brief Major version. */
#define AZ_IOT_VERSION_MAJOR 1

/** @brief Minor version. */
#define AZ_IOT_VERSION_MINOR 0

/** @brief Patch version. */
#define AZ_IOT_VERSION_PATCH 0

/** @brief Pre-release label. Defined only for a pre-release; undefined for a stable release. */
#define AZ_IOT_VERSION_PRERELEASE "preview"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Version of the linked library.
   *
   * @return #AZ_IOT_VERSION_STRING as compiled into the library.
   */
  const char* az_iot_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_VERSION_H */
