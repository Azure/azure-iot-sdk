# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Azure IoT SDK C library source, shared by azure-iot-sdk and its pkg-config consumer test.
#
# To build another revision, bump SRCREV_sdk to a commit on main. Both can be
# overridden from local.conf, e.g. to build a local checkout:
#   AZ_IOT_SDK_GIT_URI = "git:///path/to/azure-iot-sdk;protocol=file;nobranch=1"
#   SRCREV_sdk = "<commit>"

HOMEPAGE = "https://github.com/Azure/azure-iot-sdk"

AZ_IOT_SDK_GIT_URI ?= "git://github.com/Azure/azure-iot-sdk.git;protocol=https;branch=main"
SRCREV_sdk ?= "92a3a80baefae800e73d0e7faab28f7ccf416e06"

SRC_URI = "${AZ_IOT_SDK_GIT_URI};name=sdk"
SRCREV_FORMAT = "sdk"

# Matches project(azure-iot-sdk VERSION ...) in c/CMakeLists.txt.
PV = "0.0.1+git"
