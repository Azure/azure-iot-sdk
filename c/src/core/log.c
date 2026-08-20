// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* This file IS the logging facade every other translation unit is required to
 * route through, so it is the one place that legitimately formats with the C
 * library and writes to a stream. Everything downstream of az_iot_log_emit is
 * the application's choice of sink.
 *
 * az-iot-allow: vsnprintf -- builds the message emitf hands to the sink
 * az-iot-allow: fprintf -- the built-in stderr sink itself */
#include "azure/iot/az_iot_log.h"
#include "internal/log_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static az_iot_log_sink s_global_sink;
static int s_sink_active;

void az_iot_log_set_global_sink(const az_iot_log_sink* sink)
{
  if (sink)
  {
    s_global_sink = *sink;
    s_sink_active = 1;
  }
  else
  {
    memset(&s_global_sink, 0, sizeof(s_global_sink));
    s_sink_active = 0;
  }
}

bool az_iot_log_is_enabled(az_iot_log_level level)
{
  return s_sink_active && s_global_sink.sink != NULL && level >= s_global_sink.min_level;
}

/* Sinks are handed to plain "%s" formatting far more often than not -- the
 * built-in stderr sink does exactly that -- so a NULL reaching one is
 * undefined behaviour in code the SDK does not own. Now that emit is public,
 * substitute here rather than trusting every caller and every sink. */
static const char* log_text_or(const char* value, const char* fallback)
{
  return value != NULL ? value : fallback;
}

void az_iot_log_emit(az_iot_log_level level, const char* file, int line, const char* msg)
{
  if (!az_iot_log_is_enabled(level))
  {
    return;
  }
  s_global_sink.sink(
      s_global_sink.user_ctx, level, log_text_or(file, "?"), line, log_text_or(msg, ""));
}

void az_iot_log_emitf(az_iot_log_level level, const char* file, int line, const char* fmt, ...)
{
  /* Test the sink before formatting so a disabled level costs one comparison
   * rather than a full vsnprintf. */
  if (fmt == NULL || !az_iot_log_is_enabled(level))
  {
    return;
  }

  char msg[AZ_IOT_LOG_MESSAGE_MAX];
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  /* vsnprintf truncates on its own when the text does not fit; a negative
   * count is a genuine encoding failure and there is nothing to report. */
  if (written < 0)
  {
    return;
  }

  s_global_sink.sink(s_global_sink.user_ctx, level, log_text_or(file, "?"), line, msg);
}

/* Built-in stderr sink implementation. */
static void stderr_sink_fn(
    void* user_ctx,
    az_iot_log_level level,
    const char* file,
    int line,
    const char* msg)
{
  (void)user_ctx;
  static const char* level_names[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR" };
  const char* lvl = (level >= 0 && level <= AZ_IOT_LOG_LEVEL_ERROR) ? level_names[level] : "?";
  fprintf(stderr, "[%s] %s:%d: %s\n", lvl, file, line, msg);
}

az_iot_log_sink az_iot_log_stderr_sink(az_iot_log_level min_level)
{
  az_iot_log_sink sink;
  sink.sink = stderr_sink_fn;
  sink.user_ctx = NULL;
  sink.min_level = min_level;
  return sink;
}
