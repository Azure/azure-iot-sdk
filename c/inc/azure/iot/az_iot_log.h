// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_LOG_H
#define AZ_IOT_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
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

/* Register a process-wide log sink. Pass NULL to disable logging (default). */
void az_iot_log_set_global_sink(const az_iot_log_sink* sink);

/* Built-in sink that writes to stderr. Usage:
 *   az_iot_log_sink sink = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_ERROR);
 *   az_iot_log_set_global_sink(&sink); */
az_iot_log_sink az_iot_log_stderr_sink(az_iot_log_level min_level);

/* Maximum length, terminator included, of a message built by
 * az_iot_log_emitf(). Longer messages are truncated rather than dropped: a
 * shortened diagnostic is more useful than none. Override to trade stack
 * footprint for detail. */
#ifndef AZ_IOT_LOG_MESSAGE_MAX
#define AZ_IOT_LOG_MESSAGE_MAX 384
#endif

/* True when a sink is registered that would accept @p level. The emit
 * functions test this themselves; call it directly only to skip work that
 * would be wasted when logging is off. */
bool az_iot_log_is_enabled(az_iot_log_level level);

/* Route a ready-made message to the registered sink. */
void az_iot_log_emit(az_iot_log_level level, const char* file, int line, const char* msg);

/* Route a printf-formatted message to the registered sink. Nothing is
 * formatted when no sink would accept @p level. */
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
 * macro names stay free; nothing here shadows an enumerator. */
#define AZ_IOT_LOG_TRACE(msg) az_iot_log_emit(AZ_IOT_LOG_LEVEL_TRACE, __FILE__, __LINE__, (msg))
#define AZ_IOT_LOG_DEBUG(msg) az_iot_log_emit(AZ_IOT_LOG_LEVEL_DEBUG, __FILE__, __LINE__, (msg))
#define AZ_IOT_LOG_INFO(msg)  az_iot_log_emit(AZ_IOT_LOG_LEVEL_INFO,  __FILE__, __LINE__, (msg))
#define AZ_IOT_LOG_WARN(msg)  az_iot_log_emit(AZ_IOT_LOG_LEVEL_WARN,  __FILE__, __LINE__, (msg))
#define AZ_IOT_LOG_ERROR(msg) az_iot_log_emit(AZ_IOT_LOG_LEVEL_ERROR, __FILE__, __LINE__, (msg))

#define AZ_IOT_LOG_TRACEF(...) az_iot_log_emitf(AZ_IOT_LOG_LEVEL_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define AZ_IOT_LOG_DEBUGF(...) az_iot_log_emitf(AZ_IOT_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define AZ_IOT_LOG_INFOF(...)  az_iot_log_emitf(AZ_IOT_LOG_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define AZ_IOT_LOG_WARNF(...)  az_iot_log_emitf(AZ_IOT_LOG_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define AZ_IOT_LOG_ERRORF(...) az_iot_log_emitf(AZ_IOT_LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_LOG_H */
