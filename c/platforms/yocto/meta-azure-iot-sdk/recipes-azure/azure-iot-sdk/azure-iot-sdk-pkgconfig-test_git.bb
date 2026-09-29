# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

SUMMARY = "Azure IoT SDK C library pkg-config consumer test"
DESCRIPTION = "Builds the library's installed-package tests (c/tests/install) against \
the sysroot with pkg-config alone, one per installed component."
SECTION = "devel"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=383e8100f32a37cb8ab324979b03d266"

require azure-iot-sdk-src.inc

S = "${WORKDIR}/git/c/tests/install"
B = "${WORKDIR}/build"

inherit pkgconfig

DEPENDS = "azure-iot-sdk openssl"

do_configure[noexec] = "1"

# Every component azure-iot-sdk installed, found through its .pc file.
do_compile() {
    comps=""
    for pc in ${STAGING_LIBDIR}/pkgconfig/azure-iot-sdk-*.pc; do
        [ -f "$pc" ] || bbfatal "no azure-iot-sdk .pc files in ${STAGING_LIBDIR}/pkgconfig"
        c="${pc##*/azure-iot-sdk-}"
        comps="$comps ${c%.pc}"
    done
    CC="${CC}" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" \
        bash ${S}/pkg-config-test.sh ${B}/bin $comps
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/bin/pkg_config_test_* ${D}${bindir}/
}
