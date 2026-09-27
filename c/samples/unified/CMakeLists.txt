# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Each serves a Classic or an AEG hub, whichever DPS assigns, so each links
# both generations.
foreach(sample
        telemetry
        connect_first
        twin_get_patch
        direct_method_responder
        direct_method_slow_responder
        c2d_receiver
        # unified/telemetry over the two egress options, so the diff against it
        # is exactly the feature: MQTT over WebSockets, and an HTTP CONNECT proxy.
        websockets
        proxy)
    az_iot_add_sample(unified ${sample} az_iot_mqttv3 az_iot_mqttv5)
endforeach()

# File upload exists on Classic only; on an AEG hub the sample says so.
az_iot_add_sample(unified file_upload az_iot_mqttv3)
# File upload is HTTPS-only and the SDK ships no HTTP client, so the sample
# uses libcurl (when available) to perform the SAS-URI request, the blob PUT
# to Azure Storage, and the completion notification. Optional: without curl
# the sample still provisions, connects, and prints the request it built.
find_package(CURL QUIET)
if(CURL_FOUND)
    target_compile_definitions(az_iot_sample_unified_file_upload PRIVATE AZ_IOT_SAMPLE_WITH_CURL)
    target_link_libraries(az_iot_sample_unified_file_upload PRIVATE CURL::libcurl)
else()
    message(STATUS "az_iot_sample_unified_file_upload: libcurl not found; building without the "
                   "HTTPS upload path. Install libcurl (e.g. 'vcpkg install curl') to enable it.")
endif()
