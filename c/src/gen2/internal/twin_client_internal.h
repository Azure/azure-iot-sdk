// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_GEN2_TWIN_CLIENT_INTERNAL_H
#define AZ_IOT_GEN2_TWIN_CLIENT_INTERNAL_H

#include "azure/iot/gen2/az_iot_twin_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Test seam: expire every armed defensive timeout so the next
   * az_iot_gen2_twin_client_do_work() acts on it. Lets unit tests exercise the
   * recovery paths without waiting out the real multi-minute schedule. */
  void az_iot_gen2_twin_client__force_timeouts(az_iot_gen2_twin_client* twin);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_GEN2_TWIN_CLIENT_INTERNAL_H */
