// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_LOG_H
#define AZ_IOT_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum az_iot_log_level_tag
{
    AZ_IOT_LOG_TRACE = 0,
    AZ_IOT_LOG_DEBUG,
    AZ_IOT_LOG_INFO,
    AZ_IOT_LOG_WARN,
    AZ_IOT_LOG_ERROR,
    AZ_IOT_LOG_OFF
} az_iot_log_level_t;

typedef void (*az_iot_log_sink_fn)(
    void* user_ctx,
    az_iot_log_level_t level,
    const char* file,
    int line,
    const char* msg);

typedef struct az_iot_log_sink_tag
{
    az_iot_log_sink_fn sink;
    void* user_ctx;
    az_iot_log_level_t min_level;
} az_iot_log_sink_t;

/* Register a process-wide log sink. Pass NULL to disable logging (default). */
void az_iot_log_set_global_sink(const az_iot_log_sink_t* sink);

/* Built-in sink that writes to stderr. Usage:
 *   az_iot_log_sink_t sink = az_iot_log_stderr_sink(AZ_IOT_LOG_ERROR);
 *   az_iot_log_set_global_sink(&sink); */
az_iot_log_sink_t az_iot_log_stderr_sink(az_iot_log_level_t min_level);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_LOG_H */
