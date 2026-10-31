#ifndef SAMPLE_UTILS_H
#define SAMPLE_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_log_file.h"

typedef struct sample_config
{
  char* id_scope;
  char* reg_id;
  char* cert;
  char* key;
  char* ca;
  /* AZ_IOT_DPS_GLOBAL_ENDPOINT (optional). NULL selects the SDK default,
   * global.azure-devices-provisioning.net. Set it to run a sample against a
   * regional, private-link or sovereign-cloud provisioning endpoint, which
   * otherwise required editing the sample. */
  char* dps_global_endpoint;
} sample_config;

/**
 * @brief Log sink for samples. With AZ_IOT_SAMPLE_LOG_TO_FILE set (any
 * non-empty value), a rotating file sink writing AZ_IOT_LOG_FILE_DEFAULT_NAME in
 * the working directory (see az_iot_log_file_sink_open()); otherwise stderr.
 * Falls back to stderr, with a message, if the file cannot be opened.
 *
 * @param[in] min_level Lowest level written.
 * @return Sink to pass to az_iot_log_set_global_sink().
 */
az_iot_log_sink sample_log_sink(az_iot_log_level min_level);

// Reads DPS configuration from environment variables.
// Returns 0 on success, non-zero if any required variable is missing.
int sample_config_load(sample_config* config);

// Releases memory allocated by sample_config_load (Windows only; no-op on Linux).
void sample_config_release(sample_config* config);

// Returns a heap copy of environment variable `name`, or a heap copy of
// `fallback` when the variable is unset/empty (fallback may be NULL). Caller
// frees with free(). Avoids getenv()/strdup() to stay clean under MSVC /WX.
char* sample_env_copy(const char* name, const char* fallback);

/**
 * @brief Copy @p src into @p dst, NUL included. Portable stand-in for C11
 * strcpy_s(), which glibc does not provide.
 *
 * @param[out] dst      Destination; unchanged when @p src does not fit.
 * @param[in] dst_size  Capacity of @p dst in bytes.
 * @param[in] src       NUL-terminated source.
 * @return true if copied; false if @p src does not fit.
 */
bool sample_string_copy(char* dst, size_t dst_size, const char* src);

/**
 * @brief Read environment variable @p name, or @p fallback when unset/empty,
 * into @p dst. Prints a line to stderr when the value does not fit.
 *
 * @param[in] name      Variable name.
 * @param[in] fallback  Value when unset/empty; may be NULL.
 * @param[out] dst      Destination buffer.
 * @param[in] dst_size  Capacity of @p dst in bytes.
 * @return true if copied; false when unset with no fallback, out of memory or
 * too long.
 */
bool sample_env_copy_to_buffer(const char* name, const char* fallback, char* dst, size_t dst_size);

/**
 * @brief Whether @p name, a decoded manifest file id, can name a file in a directory without
 * leaving it: not empty, `.` or `..`, and free of `/`, `\\`, `:` and NUL.
 *
 * @param[in] name Name to check.
 * @return true when @p name is a single, plain file name.
 */
bool sample_is_plain_file_name(az_span name);

/**
 * @brief Open @p path for binary writing with owner-only access (0600) on
 * POSIX rather than fopen()'s 0666 less umask. On Windows the file keeps the
 * ACL inherited from its directory.
 *
 * @param[in] path File to write.
 * @param[in] replace false: create or truncate @p path. true: remove @p path,
 * then create it exclusively, so a file or link planted there is never opened.
 * @return Stream to close with fclose(), or NULL on failure (with @p replace,
 * including when @p path cannot be removed).
 */
FILE* sample_file_open_private(const char* path, bool replace);

/**
 * @brief Create @p path for binary writing with owner-only access (0600) on
 * POSIX. Fails when @p path exists, so nothing already there is replaced or
 * written through.
 *
 * @param[in] path File to create.
 * @return Stream to close with fclose(), or NULL on failure.
 */
FILE* sample_file_create_private(const char* path);

/**
 * @brief Create a new file, named @p path plus a unique suffix, for binary
 * writing with owner-only access (0600) on POSIX. Nothing already there is
 * replaced or written through. Being in @p path's directory, it can then
 * replace @p path through sample_file_replace().
 *
 * @param[in]  path             Name the new file's name starts with.
 * @param[out] out_unique_path  The new file's name, to free(); NULL on failure.
 * @return Stream to close with fclose(), or NULL on failure.
 */
FILE* sample_file_create_private_unique(const char* path, char** out_unique_path);

/**
 * @brief Open @p path for binary reading; on POSIX only a regular file, not
 * through a symbolic link, is opened.
 *
 * @param[in] path File to read.
 * @return Stream to close with fclose(), or NULL on failure.
 */
FILE* sample_file_open_read_no_follow(const char* path);

/**
 * @brief Flush @p file's buffered writes and the operating system's cache to
 * storage, so they survive a power loss.
 *
 * @param[in] file Stream open for writing.
 * @return true on success.
 */
bool sample_file_flush_to_storage(FILE* file);

/**
 * @brief Replace @p destination_path with @p source_path in one step, durably:
 * a reader sees the old file or the new one, never a partial one, and the
 * replacement survives a power loss. Flush @p source_path to storage first.
 *
 * On POSIX the directory is synced after the rename; if that fails, a line is
 * written to stderr and true is still returned, since the destination is
 * already replaced.
 *
 * @param[in] source_path      Complete file; renamed, so it is gone on success.
 * @param[in] destination_path File to replace; created when absent. Same file
 *                             system as @p source_path.
 * @return true when the destination was replaced; false when it was left as it
 * was.
 */
bool sample_file_replace(const char* source_path, const char* destination_path);

/**
 * @brief Record SIGINT (Ctrl+C) and SIGTERM instead of ending the process, so a
 * sample's loop can stop and release its clients. Call once, from main().
 */
void sample_install_stop_handler(void);

/**
 * @brief Whether SIGINT or SIGTERM arrived since sample_install_stop_handler().
 *
 * @return true once a stop was requested.
 */
bool sample_is_stop_requested(void);

// Milliseconds from an unspecified origin, never moving backwards. Only
// differences are meaningful. The SDK keeps its clock internal, so a sample
// that has to measure elapsed time brings its own.
uint64_t sample_now_ms(void);

// Blocks the calling thread for `ms` milliseconds; returns immediately for
// ms <= 0. Portable across Windows and POSIX, which every polling sample needs
// for its do_work loop.
void sample_sleep_ms(long ms);

/**
 * @brief Sleep for what is left of a @p tick_ms tick that began at
 * @p tick_start_ms (a sample_now_ms() value), so a do_work() loop cannot spin
 * when do_work() returns early. Returns at once when the tick is over.
 *
 * @param[in] tick_start_ms sample_now_ms() when the tick began.
 * @param[in] tick_ms       Tick length.
 */
void sample_sleep_until_tick_end(uint64_t tick_start_ms, uint32_t tick_ms);

#endif // SAMPLE_UTILS_H
