// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Rotating log file sink (az_iot_log_file.h). Lines are built by the shared
 * formatter in log.c. */
#include "azure/iot/az_iot_log_file.h"
#include "internal/log_format.h"
#include "internal/span_writer.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#define LOG_HOSTED 1
#else
#define LOG_HOSTED 0
#endif

#define LOG_FILE_MAX_FILES_LIMIT 99u

/* C99 static assertions: an overridden default must be usable as is. */
typedef char log_file_default_max_files_in_range
    [(AZ_IOT_LOG_FILE_DEFAULT_MAX_FILES) >= 1u
             && (AZ_IOT_LOG_FILE_DEFAULT_MAX_FILES) <= LOG_FILE_MAX_FILES_LIMIT
         ? 1
         : -1];
typedef char log_file_default_max_bytes_nonzero[(AZ_IOT_LOG_FILE_DEFAULT_MAX_BYTES) > 0u ? 1 : -1];
typedef char log_file_path_max_fits_a_suffix[(AZ_IOT_LOG_FILE_PATH_MAX) >= 8 ? 1 : -1];

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

/** @brief Size of @p f, saturated to UINT32_MAX; UINT32_MAX when unknown, so it rotates. */
static uint32_t file_size(FILE* f)
{
#if defined(_WIN32)
  __int64 size = _fseeki64(f, 0, SEEK_END) == 0 ? _ftelli64(f) : -1;
#else
  off_t size = fseeko(f, 0, SEEK_END) == 0 ? ftello(f) : (off_t)-1;
#endif
  if (size < 0 || (uint64_t)size > UINT32_MAX)
  {
    return UINT32_MAX;
  }
  return (uint32_t)size;
}

/** @brief Open the active file and resync the byte count from its size. Lock held.
 *
 * @return true if open. */
static bool file_reopen(az_iot_log_file_sink* fs)
{
  FILE* f = open_append(fs->_internal.path);
  fs->_internal.bytes = f != NULL ? file_size(f) : 0u;
  fs->_internal.stream = f;
  return f != NULL;
}

/** @brief Whether @p path names an existing regular file. */
static bool file_exists(const char* path)
{
#if defined(_WIN32)
  DWORD attrs = GetFileAttributesA(path);
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u;
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

/** @brief rename(); true when it succeeded or @p from did not exist. */
static bool rename_if_present(const char* from, const char* to)
{
  errno = 0;
  return rename(from, to) == 0 || errno == ENOENT;
}

/**
 * @brief Rotate `<path>` to `<path>.1`, shifting older files up. Lock held.
 *
 * The active file is first staged as `<path>.0`. If any later step fails, the
 * moves already made are undone and the staged file is moved back, so no
 * generation but the oldest (dropped by design) is lost, and rotation is
 * retried on the next line. A `<path>.0` left by a failed move-back or an
 * interrupted process is placed as `<path>.1` instead of the active file,
 * which then stays active.
 * The active file is always reopened with its real size.
 */
static void file_rotate(az_iot_log_file_sink* fs)
{
  char from[AZ_IOT_LOG_FILE_PATH_MAX];
  char to[AZ_IOT_LOG_FILE_PATH_MAX];
  char staged[AZ_IOT_LOG_FILE_PATH_MAX];
  bool moved[LOG_FILE_MAX_FILES_LIMIT + 1u] = { false };
  const char* path = fs->_internal.path;
  /* max_files is 1..99: open() replaces 0 with the default and rejects more. */
  uint32_t n = fs->_internal.options.max_files;
  bool ok = rotated_name(staged, path, 0u);
  bool staged_active = false;
  uint32_t i;

  (void)fclose((FILE*)fs->_internal.stream);
  fs->_internal.stream = NULL;

  if (ok && !file_exists(staged))
  {
    ok = rename(path, staged) == 0;
    staged_active = ok;
  }

  if (ok)
  {
    (void)rotated_name(to, path, n);
    errno = 0;
    ok = remove(to) == 0 || errno == ENOENT;
  }
  for (i = n; ok && i > 1u; --i)
  {
    (void)rotated_name(from, path, i - 1u);
    (void)rotated_name(to, path, i);
    ok = rename_if_present(from, to);
    moved[i] = ok;
  }
  if (ok)
  {
    (void)rotated_name(to, path, 1u);
    ok = rename(staged, to) == 0;
  }

  if (!ok)
  {
    /* Undo, lowest first, so each destination is free again. */
    for (i = 2u; i <= n; ++i)
    {
      if (moved[i])
      {
        (void)rotated_name(from, path, i - 1u);
        (void)rotated_name(to, path, i);
        (void)rename_if_present(to, from);
      }
    }
    if (staged_active)
    {
      (void)rename(staged, path);
    }
  }

  (void)file_reopen(fs);
}

/** @brief az_iot_log_sink_callback for the file sink; @p user_ctx is the az_iot_log_file_sink. */
static void file_sink_fn(
    void* user_ctx,
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* msg)
{
  az_iot_log_file_sink* fs = (az_iot_log_file_sink*)user_ctx;
  char buf[AZ_IOT_LOG__LINE_MAX];
  size_t n = az_iot_log__format_line(buf, level, component, file, line, msg);
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
      fs->_internal.bytes = (uint64_t)fs->_internal.bytes + w > UINT32_MAX
          ? UINT32_MAX
          : fs->_internal.bytes + (uint32_t)w;
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
  /* A "<path>.0" left by an interrupted rotation is placed as "<path>.1" now,
   * not when the active file next fills. */
  char staged[AZ_IOT_LOG_FILE_PATH_MAX];
  if (rotated_name(staged, file_sink->_internal.path, 0u) && file_exists(staged))
  {
    file_rotate(file_sink);
    if (file_sink->_internal.stream == NULL)
    {
      return AZ_IOT_ERR_NOT_FOUND;
    }
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
