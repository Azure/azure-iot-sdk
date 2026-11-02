// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file fixed_sizes_test.c
 * @brief Includes every header with a fixed feature-client size, twice.
 *
 * Built as is, it must compile: ordinary and repeated includes are accepted.
 * Built with one of the fixed sizes defined (check_fixed_sizes.cmake), it
 * must fail on that header's guard.
 */
#include "azure/iot/az_iot_log_file.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/mqttv5/az_iot_direct_method_client.h"

#include "azure/iot/az_iot_log_file.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/mqttv5/az_iot_direct_method_client.h"

/** @brief Uses each fixed size, so a missing definition fails too. */
int az_iot_test_fixed_sizes_sum(void);
int az_iot_test_fixed_sizes_sum(void)
{
  return AZ_IOT_DM_METHOD_NAME_MAX + AZ_IOT_DM_RID_MAX + AZ_IOT_DM_CORR_DATA_MAX
      + AZ_IOT_MQTTV5_DM_TOPIC_MAX + AZ_IOT_LOG_FILE_PATH_MAX;
}
