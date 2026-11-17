// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file e2e_service_bound_topic_format.h
 * @brief The mqttv5 service-bound topic format under test, from AZ_IOT_SERVICE_BOUND_TOPIC_FORMAT:
 * "per-device" (the default) or "legacy", so the suites run against IoT hubs that do or do not
 * support the per-device topic.
 */
#ifndef E2E_SERVICE_BOUND_TOPIC_FORMAT_H
#define E2E_SERVICE_BOUND_TOPIC_FORMAT_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_connection_client.h"
#include "support/test_env.h"

/**
 * @brief Sets @p opts->service_bound_topic_format from AZ_IOT_SERVICE_BOUND_TOPIC_FORMAT; unset
 * leaves it as it is.
 *
 * @return false, with @p opts unchanged, when the variable holds anything else.
 */
static inline bool e2e_use_service_bound_topic_format(az_iot_connection_client_options* opts)
{
  char* value = az_iot_test_env_dup("AZ_IOT_SERVICE_BOUND_TOPIC_FORMAT");
  bool ok = true;
  if (value != NULL)
  {
    if (strcmp(value, "per-device") == 0)
    {
      opts->service_bound_topic_format = AZ_IOT_SERVICE_BOUND_TOPIC_FORMAT_PER_DEVICE;
    }
    else if (strcmp(value, "legacy") == 0)
    {
      opts->service_bound_topic_format = AZ_IOT_SERVICE_BOUND_TOPIC_FORMAT_LEGACY;
    }
    else
    {
      fprintf(stderr, "[e2e] AZ_IOT_SERVICE_BOUND_TOPIC_FORMAT must be per-device or legacy\n");
      ok = false;
    }
    free(value);
  }
  return ok;
}

#endif /* E2E_SERVICE_BOUND_TOPIC_FORMAT_H */
