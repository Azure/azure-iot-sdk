// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_TWIN_CLIENT_INTERNAL_H
#define AZ_IOT_TWIN_CLIENT_INTERNAL_H

#include "azure/iot/az_iot_twin_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register a FEATURE-CLIENT subscriber (e.g. the ADU client) for
 * desired-property patches. Feature-client subscribers occupy a separate pool
 * and are dispatched BEFORE application subscribers, so that by the time an
 * application callback runs, feature clients have already consumed their slice
 * of the patch.
 *
 * This entry point is internal — applications MUST use the public
 * az_iot_twin_client_subscribe_desired(), which can only register into the
 * application pool. Returns AZ_IOT_ERR_NOT_SUPPORTED when the feature pool is
 * full, AZ_IOT_ERR_BUSY if called from within a dispatch. */
az_iot_result_t az_iot_twin_client__subscribe_desired(
    az_iot_twin_client_t* twin,
    az_iot_twin_desired_cb cb,
    void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_TWIN_CLIENT_INTERNAL_H */
