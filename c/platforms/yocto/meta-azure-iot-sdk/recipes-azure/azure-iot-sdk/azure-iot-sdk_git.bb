# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

SUMMARY = "Azure IoT SDK C library"
DESCRIPTION = "C99 client library for Azure IoT Hub (mqttv3, mqttv5) and DPS: static \
libraries, headers, CMake package and pkg-config files."
SECTION = "libs"

# The library is MIT. It bundles azure-sdk-for-c (MIT) and Eclipse Paho MQTT C
# (EPL-2.0 or EDL-1.0).
LICENSE = "MIT & (EPL-2.0 | EDL-1.0)"
LIC_FILES_CHKSUM = " \
    file://../LICENSE;md5=383e8100f32a37cb8ab324979b03d266 \
    file://${WORKDIR}/deps/azure-sdk-for-c/LICENSE;md5=2fad5f5ca21999718a06030e56ab52c8 \
    file://${WORKDIR}/deps/paho.mqtt.c/epl-v20;md5=d9fc0efef5228704e7f5b37f27192723 \
    file://${WORKDIR}/deps/paho.mqtt.c/edl-v10;md5=3adfcc70f5aeb7a44f3f9b495aa1fbf3 \
"

require azure-iot-sdk-src.inc

# CMake fetches these at configure time; Yocto fetches them here instead. Keep
# the revisions in step with AZ_SDK_C_TAG (c/CMakeLists.txt) and PAHO_C_TAG
# (c/adapters/paho/CMakeLists.txt).
SRC_URI += " \
    git://github.com/Azure/azure-sdk-for-c.git;protocol=https;nobranch=1;name=azure-sdk-for-c;destsuffix=deps/azure-sdk-for-c \
    git://github.com/eclipse/paho.mqtt.c.git;protocol=https;nobranch=1;name=paho;destsuffix=deps/paho.mqtt.c \
"
# 1.5.0
SRCREV_azure-sdk-for-c = "6d6e634a3bb7f4d6bb5666ec8e1005ecd248e635"
# v1.3.13
SRCREV_paho = "07a875788d8cc6f5833b12581d6e3e349b34d719"
SRCREV_FORMAT = "sdk_azure-sdk-for-c_paho"

S = "${WORKDIR}/git/c"

inherit cmake pkgconfig

DEPENDS = "openssl"

PACKAGECONFIG ??= "paho su-crypto-openssl certificate-provider-managed"
PACKAGECONFIG[paho] = "-DAZ_IOT_WITH_PAHO=ON,-DAZ_IOT_WITH_PAHO=OFF"
PACKAGECONFIG[rust-mqtt] = "-DAZ_IOT_WITH_RUST_MQTT=ON,-DAZ_IOT_WITH_RUST_MQTT=OFF"
PACKAGECONFIG[su-crypto-openssl] = "-DAZ_IOT_WITH_SU_CRYPTO_OPENSSL=ON,-DAZ_IOT_WITH_SU_CRYPTO_OPENSSL=OFF"
# mbedTLS 3.6 LTS or 4.1+ (meta-oe).
PACKAGECONFIG[mbedtls] = "-DAZ_IOT_WITH_SU_CRYPTO_MBEDTLS=ON,-DAZ_IOT_WITH_SU_CRYPTO_MBEDTLS=OFF,mbedtls"
PACKAGECONFIG[certificate-provider-managed] = "-DAZ_IOT_WITH_CERT_PROVIDER_MANAGED=ON,-DAZ_IOT_WITH_CERT_PROVIDER_MANAGED=OFF"

EXTRA_OECMAKE = " \
    -DAZ_IOT_INSTALL=ON \
    -DAZ_IOT_BUILD_TESTS=OFF \
    -DAZ_IOT_BUILD_SAMPLES=OFF \
    -DAZ_IOT_WARNINGS_AS_ERRORS=OFF \
    -DFETCHCONTENT_SOURCE_DIR_AZURE_SDK_FOR_C=${WORKDIR}/deps/azure-sdk-for-c \
    -DFETCHCONTENT_SOURCE_DIR_PAHO_MQTT_C=${WORKDIR}/deps/paho.mqtt.c \
"
# azure-sdk-for-c otherwise clones vcpkg when no toolchain file is given.
export AZURE_SDK_DISABLE_AUTO_VCPKG = "1"

# The bundled sources are outside ${S}: keep their build paths out of the output.
DEBUG_PREFIX_MAP:append = " \
    -fmacro-prefix-map=${WORKDIR}/deps=${TARGET_DBGSRC_DIR}/deps \
    -fdebug-prefix-map=${WORKDIR}/deps=${TARGET_DBGSRC_DIR}/deps \
"

# Components the selected PACKAGECONFIG must produce. An adapter whose
# dependency is not found is otherwise skipped by CMake without an error.
AZ_IOT_SDK_COMPONENTS = "core mqttv3 mqttv5 \
    ${@bb.utils.contains('PACKAGECONFIG', 'paho', 'adapter_paho', '', d)} \
    ${@bb.utils.contains('PACKAGECONFIG', 'rust-mqtt', 'adapter_rust_mqtt', '', d)} \
    ${@bb.utils.contains('PACKAGECONFIG', 'su-crypto-openssl', 'su_crypto_openssl', '', d)} \
    ${@bb.utils.contains('PACKAGECONFIG', 'mbedtls', 'su_crypto_mbedtls', '', d)} \
    ${@bb.utils.contains('PACKAGECONFIG', 'certificate-provider-managed', 'certificate_provider_managed', '', d)} \
"

do_install:append() {
    for c in ${AZ_IOT_SDK_COMPONENTS}; do
        if [ ! -f ${D}${libdir}/pkgconfig/azure-iot-sdk-$c.pc ]; then
            bbfatal "azure-iot-sdk component '$c' was not built"
        fi
    done
}

# Static libraries only: the runtime package is empty, and the libraries,
# headers, CMake package and .pc files go to -staticdev and -dev.
ALLOW_EMPTY:${PN} = "1"
# Static consumers link these too; pkg-config Requires.private is not scanned.
RDEPENDS:${PN}-dev += "openssl-dev ${@bb.utils.contains('PACKAGECONFIG', 'mbedtls', 'mbedtls-dev', '', d)}"
