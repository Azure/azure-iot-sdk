// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file az_iot_log_file.h
 * @brief Rotating log file sink for the logging facade (az_iot_log.h). */
#ifndef AZ_IOT_LOG_FILE_H
#define AZ_IOT_LOG_FILE_H

#include <stdbool.h>
#include <stdint.h>

#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Recommended log file name, so support can ask for it by name. */
#define AZ_IOT_LOG_FILE_DEFAULT_NAME "azure-iot-sdk-c.log"

/** @brief Default size at which the active log file is rotated. Overridable; non-zero. */
#ifndef AZ_IOT_LOG_FILE_DEFAULT_MAX_BYTES
#define AZ_IOT_LOG_FILE_DEFAULT_MAX_BYTES (1024u * 1024u)
#endif

/** @brief Default number of rotated files kept: `<path>.1` (newest) to `<path>.N`.
 * Overridable; 1 to 99. */
#ifndef AZ_IOT_LOG_FILE_DEFAULT_MAX_FILES
#define AZ_IOT_LOG_FILE_DEFAULT_MAX_FILES 3u
#endif

/** @brief Capacity, terminator included, of the stored log file path. Overridable; at least 8. */
#ifndef AZ_IOT_LOG_FILE_PATH_MAX
#define AZ_IOT_LOG_FILE_PATH_MAX 256
#endif

  /** @brief File sink options. Zero selects the default for each field. */
  typedef struct az_iot_log_file_sink_options
  {
    /** @brief Rotate before a line would take the active file past this size.
     * A single line longer than this is still written, to an empty file. */
    uint32_t max_file_bytes;
    /** @brief Rotated files kept, at most 99. */
    uint32_t max_files;
  } az_iot_log_file_sink_options;

  /** @brief File sink state. Caller-allocated; fields are private. */
  typedef struct az_iot_log_file_sink
  {
    struct
    {
      void* stream;
      bool open;
      bool recover_staged;
      uint32_t bytes;
      az_iot_log_file_sink_options options;
      long lock;
      char path[AZ_IOT_LOG_FILE_PATH_MAX];
    } _internal;
  } az_iot_log_file_sink;

  /**
   * @brief Default file sink options.
   *
   * @return AZ_IOT_LOG_FILE_DEFAULT_MAX_BYTES and AZ_IOT_LOG_FILE_DEFAULT_MAX_FILES.
   */
  AZ_NODISCARD az_iot_log_file_sink_options az_iot_log_file_sink_options_default(void);

  /**
   * @brief Open a rotating log file and build a sink that writes to it.
   *
   * Appends to an existing file. Each line is flushed as it is written, so the
   * file is complete up to a crash. On POSIX a new file is created owner-only
   * (0600). Safe to call from several threads: writes and rotation are
   * serialized. Hosted platforms only (Windows, Linux, macOS).
   *
   * @param[out] file_sink State; must outlive every use of @p out_sink.
   * @param[in] path File path, copied. Rotated files are `<path>.1` .. `<path>.N`.
   * @param[in] options NULL for defaults.
   * @param[in] min_level Lowest level written.
   * @param[out] out_sink Sink to pass to az_iot_log_set_global_sink().
   * @retval AZ_IOT_OK Opened.
   * @retval AZ_IOT_ERR_INVALID_ARG A NULL argument, an empty path, or max_files > 99.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE @p path plus a rotation suffix does not fit
   * AZ_IOT_LOG_FILE_PATH_MAX.
   * @retval AZ_IOT_ERR_NOT_FOUND The file could not be opened.
   * @retval AZ_IOT_ERR_NOT_SUPPORTED Not a hosted platform.
   */
  AZ_NODISCARD az_iot_result az_iot_log_file_sink_open(
      az_iot_log_file_sink* file_sink,
      const char* path,
      const az_iot_log_file_sink_options* options,
      az_iot_log_level min_level,
      az_iot_log_sink* out_sink);

  /**
   * @brief Close the file. Unregister the sink first if it is the global one.
   *
   * @param[in,out] file_sink State from az_iot_log_file_sink_open(); NULL is ignored.
   */
  void az_iot_log_file_sink_close(az_iot_log_file_sink* file_sink);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_LOG_FILE_H */
