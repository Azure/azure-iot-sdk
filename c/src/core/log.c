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
#include <share.h>
#include <windows.h>
#define LOG_HOSTED 1
#elif defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#define LOG_HOSTED 1
#else
#define LOG_HOSTED 0
#endif

/** @brief Longest source file name written; longer ones are cut. */
#define LOG_FILE_NAME_MAX 64u
/** @brief Bound on everything but the message: time (24), level (8), thread
 * (15), file (LOG_FILE_NAME_MAX), line (11), separators (4), newline and NUL. */
#define LOG_LINE_PREFIX_MAX (64u + LOG_FILE_NAME_MAX)
/** @brief Capacity of one formatted line. */
#define LOG_LINE_MAX ((size_t)AZ_IOT_LOG_MESSAGE_MAX + LOG_LINE_PREFIX_MAX)
/** @brief Marker that ends a truncated message. */
#define LOG_ELLIPSIS "..."
#define LOG_ELLIPSIS_LEN (sizeof(LOG_ELLIPSIS) - 1u)
#define LOG_FILE_MAX_FILES_LIMIT 99u

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

  s_global_sink.sink(s_global_sink.user_ctx, level, log_text_or(file, "?"), line, msg);
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

/**
 * @brief Append at most @p max bytes of @p s; when cut, the last bytes are LOG_ELLIPSIS.
 *
 * @p max must be larger than LOG_ELLIPSIS_LEN.
 */
static void append_bounded(az_iot_span_writer* w, const char* s, size_t max)
{
  size_t n = strlen(s);
  if (n <= max)
  {
    az_iot_span_writer_append_span(w, az_span_create((uint8_t*)(uintptr_t)s, (int32_t)n));
    return;
  }
  az_iot_span_writer_append_span(
      w, az_span_create((uint8_t*)(uintptr_t)s, (int32_t)(max - LOG_ELLIPSIS_LEN)));
  az_iot_span_writer_append_str(w, LOG_ELLIPSIS);
}

/**
 * @brief Build one newline-terminated line in @p buf (LOG_LINE_MAX bytes).
 *
 * The message is cut to AZ_IOT_LOG_MESSAGE_MAX - 1 bytes, as emitf() cuts it,
 * so emit() and emitf() honour the same bound.
 *
 * @return Line length, newline included, excluding the terminator; 0 on failure.
 */
static size_t format_line(
    char* buf,
    az_iot_log_level level,
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
  az_iot_span_writer_init(&w, az_span_create((uint8_t*)buf, (int32_t)LOG_LINE_MAX));
  append_timestamp(&w, sec, ms);
  az_iot_span_writer_append_str(&w, " [");
  az_iot_span_writer_append_str(&w, level_name(level));
  az_iot_span_writer_append_str(&w, "] ");
  if (tid != 0u)
  {
    az_iot_span_writer_append_str(&w, "[t:");
    az_iot_span_writer_append_u32(&w, tid);
    az_iot_span_writer_append_str(&w, "] ");
  }
  append_bounded(&w, base_name(file), LOG_FILE_NAME_MAX);
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
    const char* file,
    int line,
    const char* msg)
{
  char buf[LOG_LINE_MAX];
  size_t n = format_line(buf, level, file, line, msg);
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

/* ------------------------------------------------------------------------- */
/* Rotating file sink                                                        */
/* ------------------------------------------------------------------------- */

AZ_NODISCARD az_iot_log_file_sink_options az_iot_log_file_sink_options_default(void)
{
  az_iot_log_file_sink_options o;
  o.max_file_bytes = AZ_IOT_LOG_FILE_DEFAULT_MAX_BYTES;
  o.max_files = AZ_IOT_LOG_FILE_DEFAULT_MAX_FILES;
  return o;
}

#if LOG_HOSTED

/* Spin lock: the SDK links no thread library, and adapter threads log too. */
/** @brief Acquire @p lock. */
static void file_lock(long* lock)
{
#if defined(_MSC_VER)
  while (InterlockedExchange((volatile LONG*)lock, 1) != 0)
  {
    (void)SwitchToThread();
  }
#elif defined(_WIN32)
  while (__atomic_exchange_n(lock, 1L, __ATOMIC_ACQUIRE) != 0L)
  {
    (void)SwitchToThread();
  }
#else
  while (__atomic_exchange_n(lock, 1L, __ATOMIC_ACQUIRE) != 0L)
  {
    (void)sched_yield();
  }
#endif
}

/** @brief Release @p lock. */
static void file_unlock(long* lock)
{
#if defined(_MSC_VER)
  (void)InterlockedExchange((volatile LONG*)lock, 0);
#else
  __atomic_store_n(lock, 0L, __ATOMIC_RELEASE);
#endif
}

/** @brief Open @p path for appending; owner-only on POSIX when created. */
static FILE* open_append(const char* path)
{
#if defined(_WIN32)
  /* Deny other writers, allow readers: the file can be inspected while in use. */
  return _fsopen(path, "ab", _SH_DENYWR);
#else
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
  FILE* f;
  if (fd < 0)
  {
    return NULL;
  }
  f = fdopen(fd, "ab");
  if (f == NULL)
  {
    (void)close(fd);
  }
  return f;
#endif
}

/**
 * @brief `<base>.<index>` into @p out; @p out holds AZ_IOT_LOG_FILE_PATH_MAX.
 *
 * @return false, with @p out empty, if it does not fit. open() checked that it does.
 */
static bool rotated_name(char* out, const char* base, uint32_t index)
{
  az_iot_span_writer w;
  az_iot_span_writer_init(&w, az_span_create((uint8_t*)out, AZ_IOT_LOG_FILE_PATH_MAX));
  az_iot_span_writer_append_str(&w, base);
  az_iot_span_writer_append_str(&w, ".");
  az_iot_span_writer_append_u32(&w, index);
  return az_iot_span_writer_end_str(&w, NULL) == AZ_IOT_OK;
}

/** @brief Open the active file and resync the byte count from its size. Lock held.
 *
 * @return true if open. */
static bool file_reopen(az_iot_log_file_sink* fs)
{
  FILE* f = open_append(fs->_internal.path);
  fs->_internal.bytes = 0u;
  if (f != NULL && fseek(f, 0L, SEEK_END) == 0)
  {
    long size = ftell(f);
    fs->_internal.bytes = size > 0 ? (uint32_t)size : 0u;
  }
  fs->_internal.stream = f;
  return f != NULL;
}

/**
 * @brief Rotate `<path>` to `<path>.1`, shifting older files up. Lock held.
 *
 * The active file is first moved to `<path>.0`, so a failure leaves the older
 * files untouched. Either way the active file is reopened with its real size,
 * so a failed rotation is retried on the next line and a failed reopen is
 * retried by file_sink_fn().
 */
static void file_rotate(az_iot_log_file_sink* fs)
{
  char from[AZ_IOT_LOG_FILE_PATH_MAX];
  char to[AZ_IOT_LOG_FILE_PATH_MAX];
  char staged[AZ_IOT_LOG_FILE_PATH_MAX];
  const char* path = fs->_internal.path;
  uint32_t n = fs->_internal.options.max_files;

  (void)fclose((FILE*)fs->_internal.stream);
  fs->_internal.stream = NULL;

  bool staged_ok = rotated_name(staged, path, 0u);
  if (staged_ok)
  {
    (void)remove(staged);
    staged_ok = rename(path, staged) == 0;
  }
  if (staged_ok)
  {
    /* max_files is at least 1: open() replaces 0 with the default. */
    (void)rotated_name(to, path, n);
    (void)remove(to);
    for (uint32_t i = n; i > 1u; --i)
    {
      (void)rotated_name(from, path, i - 1u);
      (void)rotated_name(to, path, i);
      (void)rename(from, to);
    }
    (void)rotated_name(to, path, 1u);
    (void)rename(staged, to);
  }

  (void)file_reopen(fs);
}

/** @brief az_iot_log_sink_callback for the file sink; @p user_ctx is the az_iot_log_file_sink. */
static void file_sink_fn(
    void* user_ctx,
    az_iot_log_level level,
    const char* file,
    int line,
    const char* msg)
{
  az_iot_log_file_sink* fs = (az_iot_log_file_sink*)user_ctx;
  char buf[LOG_LINE_MAX];
  size_t n = format_line(buf, level, file, line, msg);
  if (n == 0u)
  {
    return;
  }

  file_lock(&fs->_internal.lock);
  /* A failed reopen (rotation, or the file was removed) is retried here. */
  if (fs->_internal.open && (fs->_internal.stream != NULL || file_reopen(fs)))
  {
    if (fs->_internal.bytes > 0u
        && (uint64_t)fs->_internal.bytes + n > fs->_internal.options.max_file_bytes)
    {
      file_rotate(fs);
    }
    if (fs->_internal.stream != NULL)
    {
      FILE* f = (FILE*)fs->_internal.stream;
      size_t w = fwrite(buf, 1u, n, f);
      (void)fflush(f);
      fs->_internal.bytes += (uint32_t)w;
    }
  }
  file_unlock(&fs->_internal.lock);
}

#endif /* LOG_HOSTED */

AZ_NODISCARD az_iot_result az_iot_log_file_sink_open(
    az_iot_log_file_sink* file_sink,
    const char* path,
    const az_iot_log_file_sink_options* options,
    az_iot_log_level min_level,
    az_iot_log_sink* out_sink)
{
  if (file_sink == NULL || path == NULL || path[0] == '\0' || out_sink == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_log_file_sink_options o
      = options != NULL ? *options : az_iot_log_file_sink_options_default();
  if (o.max_file_bytes == 0u)
  {
    o.max_file_bytes = AZ_IOT_LOG_FILE_DEFAULT_MAX_BYTES;
  }
  if (o.max_files == 0u)
  {
    o.max_files = AZ_IOT_LOG_FILE_DEFAULT_MAX_FILES;
  }
  if (o.max_files > LOG_FILE_MAX_FILES_LIMIT)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Room for ".NN" and the terminator. */
  if (strlen(path) + 4u > (size_t)AZ_IOT_LOG_FILE_PATH_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

#if LOG_HOSTED
  memset(file_sink, 0, sizeof(*file_sink));
  memcpy(file_sink->_internal.path, path, strlen(path) + 1u);
  file_sink->_internal.options = o;

  if (!file_reopen(file_sink))
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  file_sink->_internal.open = true;

  out_sink->sink = file_sink_fn;
  out_sink->user_ctx = file_sink;
  out_sink->min_level = min_level;
  return AZ_IOT_OK;
#else
  (void)min_level;
  return AZ_IOT_ERR_NOT_SUPPORTED;
#endif
}

void az_iot_log_file_sink_close(az_iot_log_file_sink* file_sink)
{
#if LOG_HOSTED
  if (file_sink == NULL)
  {
    return;
  }
  file_lock(&file_sink->_internal.lock);
  if (file_sink->_internal.stream != NULL)
  {
    (void)fclose((FILE*)file_sink->_internal.stream);
    file_sink->_internal.stream = NULL;
  }
  file_sink->_internal.open = false;
  file_unlock(&file_sink->_internal.lock);
#else
  (void)file_sink;
#endif
}
