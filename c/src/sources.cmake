# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Library source lists, relative to this directory. Shared by src/CMakeLists.txt
# and the ESP-IDF component (platforms/esp32/components/azure-iot-sdk), so both
# builds compile the same files.

set(AZ_IOT_CORE_SOURCES
    core/log.c
    core/log_file.c
    core/result.c
    core/version.c
    core/mqtt_iface.c
    core/connection_client.c
    core/cert_util.c
    core/mono_time.c
    core/retry_policy.c
    core/defensive_timeout.c
    core/deadline.c
    core/delivery_tracker.c
    core/env.c
    core/dispatch.c
    core/proto3.c
    core/span_writer.c
    core/json_string.c
    core/base64.c
    core/message.c
    core/certificate_provider_pem.c
    core/crypto.c
    core/sas_token.c
    core/dps_tpm_client.c
    features/su/su_client.c
    features/su/su_device_properties.c
    features/su/su_root_keys_microsoft.c
    features/su/su_report.c
    features/su/su_channel_dps.c
    features/su/su_protocol.c
)

set(AZ_IOT_MQTTV3_SOURCES
    mqttv3/telemetry_client.c
    mqttv3/c2d_client.c
    mqttv3/direct_method_client.c
    mqttv3/twin_client.c
    mqttv3/file_upload_client.c
)

set(AZ_IOT_MQTTV5_SOURCES
    mqttv5/telemetry_client.c
    mqttv5/direct_method_client.c
    mqttv5/direct_method_budget.c
    mqttv5/direct_method_delivery.c
    mqttv5/direct_method_codec.c
    mqttv5/twin_client.c
    mqttv5/custom_topic_client.c
)
