// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_LOG_H
#define AZ_IOT_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

  typedef enum az_iot_log_level
  {
    AZ_IOT_LOG_LEVEL_TRACE = 0,
    AZ_IOT_LOG_LEVEL_DEBUG,
    AZ_IOT_LOG_LEVEL_INFO,
    AZ_IOT_LOG_LEVEL_WARN,
    AZ_IOT_LOG_LEVEL_ERROR,
    AZ_IOT_LOG_LEVEL_OFF
  } az_iot_log_level;

  typedef void (*az_iot_log_sink_callback)(
      void* user_ctx,
      az_iot_log_level level,
      const char* file,
      int line,
      const char* msg);

  typedef struct az_iot_log_sink
  {
    az_iot_log_sink_callback sink;
    void* user_ctx;
    az_iot_log_level min_level;
  } az_iot_log_sink;

  /**
   * @brief Register the process-wide log sink. NULL disables logging (default).
   *
   * Not thread-safe: set it before creating any client and do not change it
   * while a client or MQTT adapter thread may log.
   *
   * @param[in] sink Copied; NULL to disable.
   */
  void az_iot_log_set_global_sink(const az_iot_log_sink* sink);

  /**
   * @brief Built-in sink that writes one line per message to stderr.
   *
   * Line format, shared with the file sink (az_iot_log_file.h):
   * `<UTC ISO 8601 time> [<LEVEL>] [t:<thread id>] <file>:<line>: <message>`.
   * The thread id is omitted where the platform has none.
   *
   * @param[in] min_level Lowest level written.
   * @return Sink to pass to az_iot_log_set_global_sink().
   */
  az_iot_log_sink az_iot_log_stderr_sink(az_iot_log_level min_level);

/* Maximum length, terminator included, of a message built by
 * az_iot_log_emitf(). Longer messages are truncated rather than dropped, and end
 * in "..." so the cut is visible. Override to trade stack footprint for
 * detail; at least 16. */
#ifndef AZ_IOT_LOG_MESSAGE_MAX
#define AZ_IOT_LOG_MESSAGE_MAX 384
#endif

  /* True when a sink is registered that would accept @p level. The emit
   * functions test this themselves; call it directly only to skip work that
   * would be wasted when logging is off. */
  bool az_iot_log_is_enabled(az_iot_log_level level);

  /* Route a ready-made message to the registered sink. A NULL @p msg or @p file
   * is replaced with a placeholder before the sink sees it, so a sink may format
   * both with "%s" unconditionally. */
  void az_iot_log_emit(az_iot_log_level level, const char* file, int line, const char* msg);

  /* Route a printf-formatted message to the registered sink. Nothing is
   * formatted when no sink would accept @p level, and a NULL @p fmt emits
   * nothing at all. */
  void az_iot_log_emitf(az_iot_log_level level, const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
      __attribute__((format(printf, 4, 5)))
#endif
      ;

/* Tracing entry points for SDK code and for MQTT adapters, which have no other
 * way to reach the application's sink. Writing to stdout or stderr directly is
 * not an alternative: the application chooses where diagnostics go.
 *
 * The severity constants are spelled AZ_IOT_LOG_LEVEL_* precisely so that these
 * macro names stay free; nothing here shadows an enumerator.
 *
 * WHY TWO FAMILIES. AZ_IOT_LOG_X takes a ready-made string; AZ_IOT_LOG_XF takes
 * a printf format and arguments. A single variadic family would be shorter to
 * declare, and would be wrong for two reasons.
 *
 * The first is safety, and it is the one that matters everywhere. In a variadic
 * family the first argument is a format string, so AZ_IOT_LOG_ERROR(msg) would
 * hand a runtime value to printf as a format. Any percent sign travelling in
 * that value -- a topic filter, a service response, anything reflected back
 * from the network -- is then interpreted as a conversion with no argument
 * behind it, which is undefined behaviour and historically a way to read the
 * stack. Splitting the families makes the safe form the default and the shorter
 * name, so reaching for the dangerous one has to be deliberate. The F variants
 * carry __attribute__((format(printf))) on GCC and clang, so their arguments
 * are checked at compile time.
 *
 * The second is footprint, and it is conditional. az_iot_log_emit() never calls
 * vsnprintf, so a build that uses only the non-F macros does not pull the
 * formatting machinery onto the link line. That is worth real bytes on an
 * embedded libc where stdio is opt-in -- newlib-nano and similar -- and worth
 * nothing on a hosted glibc build, which links printf regardless of what this
 * SDK calls. Claim the saving only for the former. */
#define AZ_IOT_LOG_TRACE(msg) az_iot_log_emit(AZ_IOT_LOG_LEVEL_TRACE, __FILE__, __LINE__, (msg))
#define AZ_IOT_LOG_DEBUG(msg) az_iot_log_emit(AZ_IOT_LOG_LEVEL_DEBUG, __FILE__, __LINE__, (msg))
#define AZ_IOT_LOG_INFO(msg) az_iot_log_emit(AZ_IOT_LOG_LEVEL_INFO, __FILE__, __LINE__, (msg))
#define AZ_IOT_LOG_WARN(msg) az_iot_log_emit(AZ_IOT_LOG_LEVEL_WARN, __FILE__, __LINE__, (msg))
#define AZ_IOT_LOG_ERROR(msg) az_iot_log_emit(AZ_IOT_LOG_LEVEL_ERROR, __FILE__, __LINE__, (msg))

#define AZ_IOT_LOG_TRACEF(...) \
  az_iot_log_emitf(AZ_IOT_LOG_LEVEL_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define AZ_IOT_LOG_DEBUGF(...) \
  az_iot_log_emitf(AZ_IOT_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define AZ_IOT_LOG_INFOF(...) \
  az_iot_log_emitf(AZ_IOT_LOG_LEVEL_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define AZ_IOT_LOG_WARNF(...) \
  az_iot_log_emitf(AZ_IOT_LOG_LEVEL_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define AZ_IOT_LOG_ERRORF(...) \
  az_iot_log_emitf(AZ_IOT_LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_LOG_H */
