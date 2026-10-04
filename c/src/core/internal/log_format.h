// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file log_format.h
 * @brief Internal line formatter shared by the built-in log sinks. */
#ifndef AZ_IOT_LOG_FORMAT_INTERNAL_H
#define AZ_IOT_LOG_FORMAT_INTERNAL_H

#include <stddef.h>

#include "azure/iot/az_iot_log.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Longest source file name written; longer ones are cut. */
#define AZ_IOT_LOG__FILE_NAME_MAX 64u

/** @brief Longest component written; longer ones are cut. */
#define AZ_IOT_LOG__COMPONENT_MAX 32u

/** @brief Bound on everything but the message: time (24), level (8), component
 * (AZ_IOT_LOG__COMPONENT_MAX + 3), thread (15), file (AZ_IOT_LOG__FILE_NAME_MAX),
 * line (11), separators (4), newline and NUL. */
#define AZ_IOT_LOG__LINE_PREFIX_MAX \
  (64u + AZ_IOT_LOG__COMPONENT_MAX + 3u + AZ_IOT_LOG__FILE_NAME_MAX)

/** @brief Capacity of one formatted line. A message expands up to 4x when
 * escaped, but is cut to AZ_IOT_LOG_MESSAGE_MAX - 1 bytes after escaping. */
#define AZ_IOT_LOG__LINE_MAX ((size_t)AZ_IOT_LOG_MESSAGE_MAX + AZ_IOT_LOG__LINE_PREFIX_MAX)

  /**
   * @brief Build one newline-terminated line:
   * `<UTC time> [<LEVEL>] [<component>] [t:<tid>] <file>:<line>: <message>`.
   *
   * Control characters in @p component and @p msg are escaped (`\n`, `\r`, `\xNN`) so a message
   * is always one line; the escaped message is cut to AZ_IOT_LOG_MESSAGE_MAX - 1
   * bytes, ending in "..." when cut.
   *
   * @param[out] buf AZ_IOT_LOG__LINE_MAX bytes.
   * @param[in] level Level.
   * @param[in] component Component; cut to AZ_IOT_LOG__COMPONENT_MAX bytes.
   * @param[in] file Source file; only its final path component is written.
   * @param[in] line Source line.
   * @param[in] msg Message.
   * @return Line length, newline included, excluding the terminator; 0 on failure.
   */
  size_t az_iot_log__format_line(
      char* buf,
      az_iot_log_level level,
      const char* component,
      const char* file,
      int line,
      const char* msg);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_LOG_FORMAT_INTERNAL_H */
