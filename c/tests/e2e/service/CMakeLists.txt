# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ---------------------------------------------------------------------------
# az_iot_e2e_service
#
# The service-side (cloud) half of the in-process end-to-end suite. Presents a
# plain-C facade (az_iot_e2e_service.h) to the tests and CONFINES every use of
# the vendored AMQP client (az_amqp) and the HTTPS/SAS machinery to its own
# translation units. The az_amqp / az_amqp_transport dependencies are linked
# PRIVATE on purpose: a test that includes only the public header cannot reach
# an <azure/amqp/...> type, so the SDK's MQTT-only device charter is preserved.
# ---------------------------------------------------------------------------
add_library(az_iot_e2e_service STATIC
    az_iot_e2e_service.c
    e2e_sas.c
    e2e_amqp.c
    e2e_notify_time.c
    e2e_http.c
)

# Only the public facade header directory is exported to consumers.
target_include_directories(az_iot_e2e_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Transport + protocol: kept PRIVATE so az_amqp headers never leak to tests.
target_link_libraries(az_iot_e2e_service PRIVATE az_amqp az_amqp_transport)

# SAS token crypto (HMAC-SHA256 + base64). OpenSSL is available on both Linux and
# Windows in the e2e build (Paho already requires it), so we use it on all
# platforms and avoid a second, platform-specific crypto path.
find_package(OpenSSL REQUIRED)
target_link_libraries(az_iot_e2e_service PRIVATE OpenSSL::Crypto)

# First-party code: build warning-clean under the project's -Werror policy.
az_iot_apply_warnings(az_iot_e2e_service)
