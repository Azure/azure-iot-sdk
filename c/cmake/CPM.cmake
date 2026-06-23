# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# CPM.cmake placeholder.
#
# Vendor the official single-file CPM.cmake here when AZ_IOT_USE_CPM=ON is exercised.
# Download from: https://github.com/cpm-cmake/CPM.cmake/releases (latest tag)
# and commit the file in-tree so reproducibility does not depend on network at configure time.
#
# Usage in CMakeLists.txt:
#   if(AZ_IOT_USE_CPM)
#       include(CPM)
#       CPMAddPackage("gh:eclipse/paho.mqtt.c#v1.3.13")
#   endif()
