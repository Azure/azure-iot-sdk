# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

option(AZ_IOT_WITH_PAHO        "Build the Paho-C MQTT adapter (default)"      ON)
option(AZ_IOT_WITH_RUST_MQTT   "Build the Rust MQTT adapter shell"             OFF)
option(AZ_IOT_WITH_ADU_CRYPTO_OPENSSL "Build the OpenSSL ADU crypto adapter"   ON)
option(AZ_IOT_WITH_CERT_PROVIDER_MANAGED "Build the OpenSSL managed certificate provider" ON)
option(AZ_IOT_WITH_EASY        "Build API B (easy / convenience layer)"        ON)
option(AZ_IOT_BUILD_SAMPLES    "Build sample apps"                              ON)
option(AZ_IOT_BUILD_TESTS      "Build unit tests"                               OFF)
# End-to-end tests talk to a REAL Azure IoT Hub/DPS instance and are off by
# default (they require provisioned cloud resources + the Paho adapter). The
# e2e GitHub Actions workflow enables this alongside AZ_IOT_BUILD_TESTS.
option(AZ_IOT_BUILD_E2E        "Build the end-to-end test device agent"         OFF)
# The conformance suites drive a REAL MQTT broker over the network. The harness
# executables always build when the Paho adapter is on -- customers link the
# conformance library to validate their own adapter -- but they are registered
# as CTest tests only when this is set, and the address comes from
# AZ_IOT_MQTT_BROKER_HOST / _PORT at run time.
#
# The real-stack reconnect integration test is registered by this option too. It
# is not a conformance suite, but it needs exactly the same thing -- a reachable
# broker at the same address -- and a second knob would be one more thing a job
# could forget to set.
#
# Deciding this at build time is deliberate. A suite that inspects the
# environment and quietly excuses itself is a suite nobody notices has stopped
# running, and the coverage it was contributing disappears with it. If a broker
# is not wanted, leave this off and the tests do not exist.
option(AZ_IOT_BUILD_CONFORMANCE_TESTS "Register the tests that need a reachable MQTT broker" OFF)
# The server-certificate-validation cases used to need a TLS listener, which the
# plain mosquitto service container in CI does not provide. They were compiled in
# always and called cmocka's skip() when no TLS port was configured -- so a
# MANDATORY security check sat inside a green suite without ever running.
#
# They now terminate TLS in the in-process test proxy, which mints the
# certificates it presents, so what they need is OpenSSL rather than a separately
# provisioned broker. Configuring with this on and no OpenSSL is a hard error:
# the whole point is that dropping these cases cannot be silent.
option(AZ_IOT_BUILD_CONFORMANCE_TESTS_TLS "Include the TLS certificate-validation conformance tests (needs OpenSSL)" OFF)
# CSR-based DPS enrollment needs an enrollment linked to a signing CA, which
# only the dedicated ci-c-e2e-csr workflow provisions. Same rule as above: the
# test is built when it is going to be run, not built-and-skipped.
option(AZ_IOT_BUILD_E2E_CSR    "Build the CSR enrollment e2e test (needs a CA-linked DPS enrollment)" OFF)
option(AZ_IOT_USE_SYSTEM_DEPS  "Prefer find_package() over fetched deps"       OFF)
option(AZ_IOT_USE_CPM          "Use CPM.cmake to fetch deps from source"       OFF)
option(AZ_IOT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"           ON)
# Code coverage (gcov/gcovr) for first-party targets. Off by default: it forces
# -O0-style instrumentation and roughly doubles test wall time, so it gets its
# own build tree (the linux-gcc-coverage preset) rather than riding along with
# the normal debug build. No-op on MSVC -- Windows coverage is collected
# externally by OpenCppCoverage against an ordinary build.
option(AZ_IOT_ENABLE_COVERAGE  "Instrument first-party targets for gcov"       OFF)
# azure-sdk-for-c (az::core + az::iot::hub + az::iot::provisioning) is a
# MANDATORY dependency. It is how we talk to DPS and IoTHub-Classic. There is
# intentionally no option to disable it.
