# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

option(AZ_IOT_WITH_PAHO        "Build the Paho-C MQTT adapter (default)"      ON)
option(AZ_IOT_WITH_RUST_MQTT   "Build the Rust MQTT adapter shell"             OFF)
# Non-extractable key custody (D8) in the Paho adapter: resolve a
# "pkcs11:"/"tpm2:" key reference through an OpenSSL ENGINE or provider so the
# TLS handshake signs inside the HSM. Needs the OSSL_STORE/OSSL_ENCODER APIs,
# so it is compiled in only when OpenSSL 3.0+ is present; the adapter builds
# either way and refuses a key reference with AZ_IOT_ERR_NOT_SUPPORTED when the
# support is absent, rather than connecting without a client key.
option(AZ_IOT_PAHO_KEY_CUSTODY "Honour non-extractable key references in the Paho adapter (needs OpenSSL 3.0+)" ON)
option(AZ_IOT_WITH_SU_CRYPTO_OPENSSL "Build the OpenSSL software updates crypto adapter"   ON)
option(AZ_IOT_WITH_SU_CRYPTO_MBEDTLS "Build the mbedTLS software updates crypto adapter when mbedTLS 3.6 LTS or 4.1+ is found" ON)
option(AZ_IOT_WITH_CERT_PROVIDER_MANAGED "Build the OpenSSL managed certificate provider" ON)
option(AZ_IOT_BUILD_SAMPLES    "Build sample apps"                              ON)
option(AZ_IOT_BUILD_TESTS      "Build unit tests"                               OFF)
# End-to-end tests talk to a REAL Azure IoT Hub/DPS instance and are off by
# default (they require provisioned cloud resources + the Paho adapter). The
# e2e GitHub Actions workflow enables this alongside AZ_IOT_BUILD_TESTS.
option(AZ_IOT_BUILD_E2E        "Build the end-to-end test device agent"         OFF)
# The software updates device-update e2e suite. Selects that suite WITHIN the e2e tests;
# AZ_IOT_BUILD_E2E and AZ_IOT_WITH_PAHO are still required, and this option on
# its own creates no target. Separate because the suite needs a Device Update
# environment and an X.509 enrollment that the other e2e jobs do not provision.
option(AZ_IOT_BUILD_E2E_SU "Build the software updates device-update end-to-end suite (needs AZ_IOT_BUILD_E2E + AZ_IOT_WITH_PAHO)" OFF)
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
# The PKCS#11 custody tests drive a REAL token (SoftHSM2 in CI) through an
# OpenSSL 3.x pkcs11 provider. Same rule as the conformance suites: the test is
# built when it is going to be run, not built-and-skipped. The token URI comes
# from AZ_IOT_TEST_PKCS11_KEY_URI at run time and the suite fails if it is unset.
option(AZ_IOT_BUILD_PKCS11_TESTS "Register the Paho key-custody tests that need a PKCS#11 token" OFF)
# The custody e2e suite runs DPS issuance and hub traffic with a device key that
# lives inside a PKCS#11 token. It needs BOTH a provisioned token and the
# standard e2e Azure resources, which only the e2e workflow has, so it follows
# the same build-it-when-it-will-run rule as AZ_IOT_BUILD_E2E_CSR.
option(AZ_IOT_BUILD_E2E_PKCS11 "Build the PKCS#11 custody e2e test (needs a token + e2e resources)" OFF)
# Install rules and the azure-iot-sdk CMake package (find_package). On by default
# only for a top-level build, so a parent project that adds this tree with
# add_subdirectory() or FetchContent does not install it as a side effect.
option(AZ_IOT_INSTALL          "Generate install rules and the azure-iot-sdk CMake package" ${PROJECT_IS_TOP_LEVEL})
option(AZ_IOT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"           ON)
# Compiler and linker hardening; see cmake/az_iot_hardening.cmake.
option(AZ_IOT_ENABLE_HARDENING "Harden builds: GCC/Clang (stack protector, FORTIFY, PIE, RELRO), MSVC (/guard:cf, /CETCOMPAT, /sdl)" ON)
# MSVC code analysis (/analyze) on first-party targets; findings are errors under
# AZ_IOT_WARNINGS_AS_ERRORS. Off by default: it multiplies build time.
option(AZ_IOT_ENABLE_MSVC_ANALYZE "Run the MSVC code analyzer (/analyze) on first-party targets" OFF)
# Code coverage (gcov/gcovr) for first-party targets. Off by default: it forces
# -O0-style instrumentation and roughly doubles test wall time, so it gets its
# own build tree (the linux-gcc-coverage preset) rather than riding along with
# the normal debug build. No-op on MSVC -- Windows coverage is collected
# externally by OpenCppCoverage against an ordinary build.
option(AZ_IOT_ENABLE_COVERAGE  "Instrument first-party targets for gcov"       OFF)
# azure-sdk-for-c (az::core + az::iot::hub + az::iot::provisioning) is a
# MANDATORY dependency. It is how we talk to DPS and the MQTTv3 hub. There is
# intentionally no option to disable it.
