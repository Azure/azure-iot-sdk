// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief Provides version information for the AMQP 1.0 client library.
 *
 * @note You MUST NOT use any symbols (macros, functions, structures, enums, etc.)
 * prefixed with an underscore ('_') directly in your application code. These symbols
 * are part of the AMQP client's internal implementation; we do not document these symbols
 * and they are subject to change in future versions of the SDK which would break your code.
 */

#ifndef _az_AMQP_VERSION_H
#define _az_AMQP_VERSION_H

/// Major numeric identifier.
#define AZ_AMQP_VERSION_MAJOR 1

/// Minor numeric identifier.
#define AZ_AMQP_VERSION_MINOR 0

/// Patch numeric identifier.
#define AZ_AMQP_VERSION_PATCH 0

/// Optional pre-release identifier. Leave empty for a stable release.
#define AZ_AMQP_VERSION_PRERELEASE "beta.1"

/// The version, in string format, of the AMQP client library (`SHORTNAME_VERSIONINFO`).
#define AZ_AMQP_VERSION_STRING "1.0.0-beta.1"

/**
 * @brief The AMQP protocol version targeted by this library, as defined by the OASIS AMQP 1.0
 * specification (major.minor.revision == 1.0.0).
 */
#define AZ_AMQP_PROTOCOL_VERSION_STRING "1.0.0"

#endif // _az_AMQP_VERSION_H
