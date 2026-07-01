# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ---------------------------------------------------------------------------
# Vendored az_amqp (TEST-ONLY).
#
# Provenance: https://github.com/Azure/az_amqp @ fe7c0c2ed87257a22f0cd6b183374cdc855e5000
# (inc/, src/, and the reference transport from samples/common/).
#
# A small, allocation-free, single-pump C99 AMQP 1.0 client. It is used ONLY by
# the end-to-end test service client (c/tests/e2e/service) to receive device
# telemetry from an IoT Hub's Event Hub-compatible endpoint and to send C2D
# messages over the IoT Hub AMQP service endpoint. It reuses THIS SDK's existing
# az_core + platform (az_win32 / az_posix) target and does NOT re-fetch
# azure-sdk-for-c.
#
# The SHIPPING SDK stays MQTT-only (see c/docs/amqp_vs_mqtt_for_new_sdk_client.md);
# AMQP appears here strictly as a test dependency so the C service half of every
# scenario can be verified in-process, with no C# harness.
#
# NOTE: az_iot_apply_warnings() is intentionally NOT applied to these targets —
# az_amqp is third-party and is not compiled under the SDK's -Werror policy. Its
# public include directories are exported SYSTEM so the strict warnings of the
# consuming (first-party) targets do not flag its headers.
# ---------------------------------------------------------------------------

# --- Protocol engine ---------------------------------------------------------
add_library(az_amqp STATIC
    src/az_amqp_cbs.c
    src/az_amqp_connection.c
    src/az_amqp_link.c
    src/az_amqp_message.c
    src/az_amqp_sasl.c
    src/az_amqp_session.c
    src/az_amqp_transport.c
    src/az_amqp_value.c
)

target_include_directories(az_amqp SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
target_include_directories(az_amqp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# az::core links the platform abstraction (az_win32 / az_posix) transitively,
# which provides az_platform_clock_msec used by the connection idle timer.
target_link_libraries(az_amqp PUBLIC az::core)

# --- Reference transport (native TLS: Winsock+Schannel / BSD sockets+OpenSSL) -
add_library(az_amqp_transport STATIC
    transport/az_amqp_sample_transport.c
    transport/az_amqp_sample_wait.c
)

target_include_directories(az_amqp_transport SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/transport
)
target_link_libraries(az_amqp_transport PUBLIC az_amqp)

if(WIN32)
    # Schannel path. secur32/ws2_32 are also #pragma comment(lib)'d in the source;
    # listed here too so non-MSVC Windows toolchains link them, plus crypt32 for
    # the transient PEM certificate store the Schannel adapter builds.
    target_link_libraries(az_amqp_transport PUBLIC ws2_32 secur32 crypt32)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(az_amqp_transport PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    # AZ_AMQP_SAMPLE_USE_OPENSSL selects the OpenSSL TLS path. The _DEFAULT_SOURCE /
    # _POSIX_C_SOURCE macros expose the POSIX networking API the reference transport
    # relies on (getaddrinfo/struct addrinfo in az_amqp_sample_transport.c; select/
    # fd_set/struct timeval in az_amqp_sample_wait.c). The SDK compiles under a strict
    # -std=c99, which defines __STRICT_ANSI__ and otherwise hides those behind glibc/
    # musl feature-test guards. Defined on the command line so they precede every
    # header included by either transport translation unit.
    target_compile_definitions(az_amqp_transport PRIVATE
        AZ_AMQP_SAMPLE_USE_OPENSSL
        _DEFAULT_SOURCE
        _POSIX_C_SOURCE=200112L
    )
endif()
