# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Finds tpm2-tss (ESAPI, marshalling and the TCTI loader) through pkg-config.
# Defines Tss2_FOUND and the imported target Tss2::Esys.

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(TSS2 QUIET IMPORTED_TARGET tss2-esys tss2-mu tss2-tctildr)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Tss2 REQUIRED_VARS TSS2_LINK_LIBRARIES)

if(Tss2_FOUND AND NOT TARGET Tss2::Esys)
    add_library(Tss2::Esys INTERFACE IMPORTED)
    target_link_libraries(Tss2::Esys INTERFACE PkgConfig::TSS2)
endif()
