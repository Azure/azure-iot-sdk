# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# One sample per protocol version; each links only its own library.
foreach(version 3 5)
  if(AZ_MQTT_ENABLE_MQTTV${version})
    add_executable(az_mqtt${version}_sample_connect az_mqtt${version}_sample_connect.c)
    target_link_libraries(az_mqtt${version}_sample_connect PRIVATE az_mqtt::mqttv${version})
    az_mqtt_apply_warnings(az_mqtt${version}_sample_connect)
  endif()
endforeach()
