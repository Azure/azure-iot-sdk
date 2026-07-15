// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Conformance harness: validates the bundled Paho adapter as an MQTTv5 client. */
#include "../conformance/az_iot_conformance.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

int main(void)
{
    az_iot_mqtt_factory* f = az_iot_paho_factory_create_v5();
    int rc = az_iot_conformance_run(AZ_IOT_CONFORMANCE_SUITE_V5, f);
    az_iot_paho_factory_destroy(f);
    return rc;
}
