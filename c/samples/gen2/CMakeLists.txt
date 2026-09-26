# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# AEG (MQTT v5) hubs only; each pins gen2 before open().
foreach(sample
        telemetry
        twin_get_patch
        direct_method_responder
        direct_method_slow_responder
        c2d_receiver)
    az_iot_add_sample(gen2 ${sample} az_iot_gen2)
endforeach()
