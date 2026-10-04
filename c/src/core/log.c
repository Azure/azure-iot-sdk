// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* This file IS the logging facade every other translation unit is required to
 * route through, so it is the one place that legitimately formats with the C
 * library and writes to a stream. Everything downstream of az_iot_log_emit is
 * the application's choice of sink.
 *
 * az-iot-allow: vsnprintf -- builds the message emitf hands to the sink */
#include "azure/iot/az_iot_log.h"
#include "internal/log_format.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define LOG_HOSTED 1
#elif defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#define LOG_HOSTED 1
#else
#define LOG_HOSTED 0
#endif

/** @brief Marker that ends a truncated message. */
#define LOG_ELLIPSIS "..."
#define LOG_ELLIPSIS_LEN (sizeof(LOG_ELLIPSIS) - 1u)

/* C99 static assertion: truncation needs room for LOG_ELLIPSIS and some text. */
typedef char log_message_max_is_at_least_16[(AZ_IOT_LOG_MESSAGE_MAX) >= 16 ? 1 : -1];

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

/* Sinks are handed to plain "%s" formatting far more often than not, so a NULL
 * reaching one is undefined behaviour in code the SDK does not own. Substitute
 * here rather than trusting every caller and every sink. */
/** @brief @p value, or @p fallback when NULL. */
static const char* log_text_or(const char* value, const char* fallback)
{
  return value != NULL ? value : fallback;
}

/** @brief Overwrite the tail of a full buffer with LOG_ELLIPSIS. */
static void mark_truncated(char* buf, size_t cap)
{
  if (cap > LOG_ELLIPSIS_LEN)
  {
    memcpy(buf + cap - 1u - LOG_ELLIPSIS_LEN, LOG_ELLIPSIS, LOG_ELLIPSIS_LEN);
    buf[cap - 1u] = '\0';
  }
}

void az_iot_log_emit(
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* msg)
{
  if (!az_iot_log_is_enabled(level))
  {
    return;
  }
  msg = log_text_or(msg, "");
  /* Bound as emitf() does, so every sink sees the same limit. Copied only when
   * it does not fit. */
  char cut[AZ_IOT_LOG_MESSAGE_MAX];
  size_t n = 0u;
  while (n < sizeof(cut) && msg[n] != '\0')
  {
    ++n;
  }
  if (n == sizeof(cut))
  {
    memcpy(cut, msg, sizeof(cut) - 1u);
    mark_truncated(cut, sizeof(cut));
    msg = cut;
  }
  s_global_sink.sink(
      s_global_sink.user_ctx,
      level,
      log_text_or(component, "?"),
      log_text_or(file, "?"),
      line,
      msg);
}

void az_iot_log_emitf(
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* fmt,
    ...)
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

  /* A negative count is a genuine encoding failure and there is nothing to
   * report. */
  if (written < 0)
  {
    return;
  }
  if ((size_t)written >= sizeof(msg))
  {
    mark_truncated(msg, sizeof(msg));
  }

  s_global_sink.sink(
      s_global_sink.user_ctx,
      level,
      log_text_or(component, "?"),
      log_text_or(file, "?"),
      line,
      msg);
}

/* ------------------------------------------------------------------------- */
/* Line formatting shared by the built-in sinks                               */
/* ------------------------------------------------------------------------- */

/** @brief UTC wall-clock time as seconds since the Unix epoch plus milliseconds. */
static void wall_clock(int64_t* out_sec, uint32_t* out_ms)
{
#if defined(_WIN32)
  FILETIME ft;
  ULARGE_INTEGER t;
  GetSystemTimeAsFileTime(&ft);
  t.LowPart = ft.dwLowDateTime;
  t.HighPart = ft.dwHighDateTime;
  /* 100 ns ticks since 1601-01-01; 11644473600 s separate it from 1970. */
  *out_sec = (int64_t)(t.QuadPart / 10000000ull) - 11644473600ll;
  *out_ms = (uint32_t)((t.QuadPart / 10000ull) % 1000ull);
#elif LOG_HOSTED
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
  {
    ts.tv_sec = time(NULL);
    ts.tv_nsec = 0;
  }
  *out_sec = (int64_t)ts.tv_sec;
  *out_ms = (uint32_t)(ts.tv_nsec / 1000000L);
#else
  *out_sec = (int64_t)time(NULL);
  *out_ms = 0;
#endif
}

/** @brief Append `YYYY-MM-DDTHH:MM:SS.mmmZ` for @p sec/@p ms (proleptic Gregorian, UTC). */
static void append_timestamp(az_iot_span_writer* w, int64_t sec, uint32_t ms)
{
  int64_t days = sec / 86400;
  int64_t rem = sec % 86400;
  if (rem < 0)
  {
    rem += 86400;
    days -= 1;
  }
  /* H. Hinnant's days-to-civil. */
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1;
  int64_t m = mp < 10 ? mp + 3 : mp - 9;
  int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  if (y < 0 || y > 9999)
  {
    y = 0;
  }

  az_iot_span_writer_append_u32_padded(w, (uint32_t)y, 4);
  az_iot_span_writer_append_str(w, "-");
  az_iot_span_writer_append_u32_padded(w, (uint32_t)m, 2);
  az_iot_span_writer_append_str(w, "-");
  az_iot_span_writer_append_u32_padded(w, (uint32_t)d, 2);
  az_iot_span_writer_append_str(w, "T");
  az_iot_span_writer_append_u32_padded(w, (uint32_t)(rem / 3600), 2);
  az_iot_span_writer_append_str(w, ":");
  az_iot_span_writer_append_u32_padded(w, (uint32_t)((rem / 60) % 60), 2);
  az_iot_span_writer_append_str(w, ":");
  az_iot_span_writer_append_u32_padded(w, (uint32_t)(rem % 60), 2);
  az_iot_span_writer_append_str(w, ".");
  az_iot_span_writer_append_u32_padded(w, ms, 3);
  az_iot_span_writer_append_str(w, "Z");
}

/** @brief OS thread id, or 0 where the platform has none. */
static uint32_t thread_id(void)
{
#if defined(_WIN32)
  return (uint32_t)GetCurrentThreadId();
#elif defined(__linux__)
  return (uint32_t)syscall(SYS_gettid);
#else
  return 0;
#endif
}

/** @brief Final path component of @p file, so build paths stay out of logs. */
static const char* base_name(const char* file)
{
  const char* base = file;
  for (const char* p = file; *p != '\0'; ++p)
  {
    if (*p == '/' || *p == '\\')
    {
      base = p + 1;
    }
  }
  return base;
}

/** @brief Fixed-width level name. */
static const char* level_name(az_iot_log_level level)
{
  static const char* const names[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR" };
  return (level >= AZ_IOT_LOG_LEVEL_TRACE && level <= AZ_IOT_LOG_LEVEL_ERROR) ? names[level]
                                                                              : "?    ";
}

/** @brief Bytes @p c takes once escaped: control characters other than tab are escaped. */
static size_t escaped_size(unsigned char c)
{
  if (c == '\n' || c == '\r')
  {
    return 2u;
  }
  return ((c < 0x20u && c != '\t') || c == 0x7fu) ? 4u : 1u;
}

/** @brief Append @p c escaped as `\n`, `\r`, `\xNN`, or as is. */
static void append_escaped(az_iot_span_writer* w, unsigned char c)
{
  switch (escaped_size(c))
  {
    case 1u:
      az_iot_span_writer_append_u8(w, (uint8_t)c);
      break;
    case 2u:
      az_iot_span_writer_append_str(w, c == '\n' ? "\\n" : "\\r");
      break;
    default:
      az_iot_span_writer_append_str(w, "\\x");
      az_iot_span_writer_append_hex32(w, c, 2u);
      break;
  }
}

/**
 * @brief Append @p s escaped (see escaped_size()), at most @p max bytes; when
 * cut, the last bytes are LOG_ELLIPSIS. An escape is never split.
 *
 * @p max must be larger than LOG_ELLIPSIS_LEN.
 */
static void append_bounded(az_iot_span_writer* w, const char* s, size_t max)
{
  const unsigned char* p;
  size_t total = 0u;
  size_t used = 0u;
  for (p = (const unsigned char*)s; *p != 0u; ++p)
  {
    total += escaped_size(*p);
  }
  size_t budget = total <= max ? max : max - LOG_ELLIPSIS_LEN;
  for (p = (const unsigned char*)s; *p != 0u; ++p)
  {
    size_t k = escaped_size(*p);
    if (used + k > budget)
    {
      break;
    }
    append_escaped(w, *p);
    used += k;
  }
  if (total > max)
  {
    az_iot_span_writer_append_str(w, LOG_ELLIPSIS);
  }
}

size_t az_iot_log__format_line(
    char* buf,
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* msg)
{
  az_iot_span_writer w;
  size_t len = 0;
  int64_t sec = 0;
  uint32_t ms = 0;
  uint32_t tid = thread_id();

  wall_clock(&sec, &ms);
  az_iot_span_writer_init(&w, az_span_create((uint8_t*)buf, (int32_t)AZ_IOT_LOG__LINE_MAX));
  append_timestamp(&w, sec, ms);
  az_iot_span_writer_append_str(&w, " [");
  az_iot_span_writer_append_str(&w, level_name(level));
  az_iot_span_writer_append_str(&w, "] [");
  append_bounded(&w, component, AZ_IOT_LOG__COMPONENT_MAX);
  az_iot_span_writer_append_str(&w, "] ");
  if (tid != 0u)
  {
    az_iot_span_writer_append_str(&w, "[t:");
    az_iot_span_writer_append_u32(&w, tid);
    az_iot_span_writer_append_str(&w, "] ");
  }
  append_bounded(&w, base_name(file), AZ_IOT_LOG__FILE_NAME_MAX);
  az_iot_span_writer_append_str(&w, ":");
  az_iot_span_writer_append_u32(&w, line > 0 ? (uint32_t)line : 0u);
  az_iot_span_writer_append_str(&w, ": ");
  append_bounded(&w, msg, (size_t)AZ_IOT_LOG_MESSAGE_MAX - 1u);
  az_iot_span_writer_append_str(&w, "\n");
  return az_iot_span_writer_end_str(&w, &len) == AZ_IOT_OK ? len : 0u;
}

/* ------------------------------------------------------------------------- */
/* stderr sink                                                               */
/* ------------------------------------------------------------------------- */

/** @brief az_iot_log_sink_callback for az_iot_log_stderr_sink(). */
static void stderr_sink_fn(
    void* user_ctx,
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* msg)
{
  char buf[AZ_IOT_LOG__LINE_MAX];
  size_t n = az_iot_log__format_line(buf, level, component, file, line, msg);
  (void)user_ctx;
  /* One write per line, so lines from different threads do not interleave. */
  (void)fwrite(buf, 1u, n, stderr);
}

az_iot_log_sink az_iot_log_stderr_sink(az_iot_log_level min_level)
{
  az_iot_log_sink sink;
  sink.sink = stderr_sink_fn;
  sink.user_ctx = NULL;
  sink.min_level = min_level;
  return sink;
}
