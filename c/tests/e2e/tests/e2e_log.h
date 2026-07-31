// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Shared log-sink setup for the e2e suites.
 *
 * The SDK and the MQTT adapters route every trace through the registered sink
 * rather than writing to stderr, so the level chosen here decides how much
 * connect / DPS / adapter detail a CI log carries. It defaults to ERROR to keep
 * a passing run quiet, and AZ_IOT_E2E_LOG_LEVEL raises it when a failure needs
 * diagnosing. Every e2e executable installs the same sink so that raising the
 * variable has the same effect whichever suite is being chased.
 */
#ifndef AZ_IOT_E2E_LOG_H
#define AZ_IOT_E2E_LOG_H

#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_log.h"

/* Reads AZ_IOT_E2E_LOG_LEVEL (TRACE|DEBUG|INFO|WARN); anything else, including
 * an unset or empty variable, leaves the default of ERROR. */
static inline az_iot_log_level e2e_log_level_from_env(void)
{
    az_iot_log_level level = AZ_IOT_LOG_LEVEL_ERROR;

#ifdef _WIN32
    /* getenv is deprecated under MSVC and the e2e targets build with /WX. */
    char* value = NULL;
    size_t value_len = 0;
    if (_dupenv_s(&value, &value_len, "AZ_IOT_E2E_LOG_LEVEL") != 0)
    {
        value = NULL;
    }
#else
    const char* value = getenv("AZ_IOT_E2E_LOG_LEVEL");
#endif

    if (value != NULL && value[0] != '\0')
    {
        if      (strcmp(value, "TRACE") == 0) level = AZ_IOT_LOG_LEVEL_TRACE;
        else if (strcmp(value, "DEBUG") == 0) level = AZ_IOT_LOG_LEVEL_DEBUG;
        else if (strcmp(value, "INFO")  == 0) level = AZ_IOT_LOG_LEVEL_INFO;
        else if (strcmp(value, "WARN")  == 0) level = AZ_IOT_LOG_LEVEL_WARN;
    }

#ifdef _WIN32
    free(value);
#endif

    return level;
}

/* Installs the stderr sink at the env-selected level. The sink struct is copied
 * by az_iot_log_set_global_sink, so a local is enough. */
static inline void e2e_install_log_sink(void)
{
    az_iot_log_sink sink = az_iot_log_stderr_sink(e2e_log_level_from_env());
    az_iot_log_set_global_sink(&sink);
}

#endif /* AZ_IOT_E2E_LOG_H */
