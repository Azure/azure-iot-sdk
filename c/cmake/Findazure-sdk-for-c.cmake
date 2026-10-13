# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Finds an installed azure-sdk-for-c. Upstream ships no CMake package, so this
# looks for its headers and static libraries:
#
#   <prefix>/include[/azure-sdk-for-c]/azure/core/az_version.h
#   <prefix>/lib/<az_core, az_iot_common, az_iot_hub, az_iot_provisioning,
#                  and one platform library: az_posix, az_win32 or az_noplatform>
#
# Search with CMAKE_PREFIX_PATH or azure-sdk-for-c_ROOT. The cache variables
# AZURE_SDK_FOR_C_INCLUDE_DIR and AZURE_SDK_FOR_C_<NAME>_LIBRARY override it.
#
# Debug libraries of the same names are looked for in <prefix>/debug/lib (vcpkg
# layout) or set with AZURE_SDK_FOR_C_<NAME>_LIBRARY_DEBUG; the Debug
# configuration links them. Otherwise every configuration links the ones above.
#
# Defines imported targets az::core, az::iot::common, az::iot::hub,
# az::iot::provisioning and az::platform, and azure-sdk-for-c_VERSION.

include(FindPackageHandleStandardArgs)

find_path(AZURE_SDK_FOR_C_INCLUDE_DIR
    NAMES azure/core/az_version.h
    PATH_SUFFIXES azure-sdk-for-c
)

set(_az_sdk_for_c_version "")
if(AZURE_SDK_FOR_C_INCLUDE_DIR)
    file(STRINGS "${AZURE_SDK_FOR_C_INCLUDE_DIR}/azure/core/az_version.h" _az_sdk_for_c_ver_line
        REGEX "^#define AZ_SDK_VERSION_STRING \"[^\"]+\"")
    if(_az_sdk_for_c_ver_line MATCHES "\"([^\"]+)\"")
        set(_az_sdk_for_c_version "${CMAKE_MATCH_1}")
    endif()
    unset(_az_sdk_for_c_ver_line)
endif()
set(azure-sdk-for-c_VERSION "${_az_sdk_for_c_version}")

if(WIN32)
    set(_az_sdk_for_c_platform_names az_win32 az_noplatform)
elseif(UNIX)
    set(_az_sdk_for_c_platform_names az_posix az_noplatform)
else()
    set(_az_sdk_for_c_platform_names az_noplatform)
endif()

set(_az_sdk_for_c_libs CORE az_core IOT_COMMON az_iot_common IOT_HUB az_iot_hub
    IOT_PROVISIONING az_iot_provisioning)
set(_az_sdk_for_c_lib_vars "")
list(LENGTH _az_sdk_for_c_libs _n)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE 0 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _az_sdk_for_c_libs ${_i} _var)
    list(GET _az_sdk_for_c_libs ${_j} _lib)
    find_library(AZURE_SDK_FOR_C_${_var}_LIBRARY NAMES ${_lib})
    list(APPEND _az_sdk_for_c_lib_vars AZURE_SDK_FOR_C_${_var}_LIBRARY)
endforeach()
find_library(AZURE_SDK_FOR_C_PLATFORM_LIBRARY NAMES ${_az_sdk_for_c_platform_names} NAMES_PER_DIR)
list(APPEND _az_sdk_for_c_lib_vars AZURE_SDK_FOR_C_PLATFORM_LIBRARY)

# The Debug counterpart of each library found: same file name, in ../debug/lib.
set(_az_sdk_for_c_debug_vars "")
foreach(_v IN LISTS _az_sdk_for_c_lib_vars)
    if(${_v})
        get_filename_component(_dir "${${_v}}" DIRECTORY)
        get_filename_component(_file "${${_v}}" NAME)
        find_library(${_v}_DEBUG NAMES "${_file}" PATHS "${_dir}/../debug/lib" NO_DEFAULT_PATH)
    endif()
    list(APPEND _az_sdk_for_c_debug_vars ${_v}_DEBUG)
endforeach()
mark_as_advanced(AZURE_SDK_FOR_C_INCLUDE_DIR ${_az_sdk_for_c_lib_vars} ${_az_sdk_for_c_debug_vars})

find_package_handle_standard_args(azure-sdk-for-c
    REQUIRED_VARS AZURE_SDK_FOR_C_INCLUDE_DIR ${_az_sdk_for_c_lib_vars}
    VERSION_VAR azure-sdk-for-c_VERSION
)

if(azure-sdk-for-c_FOUND AND NOT TARGET az::core)
    set(_az_sdk_for_c_targets core CORE iot::common IOT_COMMON iot::hub IOT_HUB
        iot::provisioning IOT_PROVISIONING platform PLATFORM)
    list(LENGTH _az_sdk_for_c_targets _n)
    math(EXPR _last "${_n} - 1")
    foreach(_i RANGE 0 ${_last} 2)
        math(EXPR _j "${_i} + 1")
        list(GET _az_sdk_for_c_targets ${_i} _name)
        list(GET _az_sdk_for_c_targets ${_j} _var)
        add_library(az::${_name} STATIC IMPORTED)
        set_target_properties(az::${_name} PROPERTIES
            IMPORTED_LOCATION "${AZURE_SDK_FOR_C_${_var}_LIBRARY}"
            IMPORTED_LINK_INTERFACE_LANGUAGES C)
        if(AZURE_SDK_FOR_C_${_var}_LIBRARY_DEBUG)
            set_target_properties(az::${_name} PROPERTIES
                IMPORTED_LOCATION_DEBUG "${AZURE_SDK_FOR_C_${_var}_LIBRARY_DEBUG}")
        endif()
    endforeach()
    unset(_az_sdk_for_c_targets)
    # az_core and the platform library call each other.
    set_target_properties(az::core PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${AZURE_SDK_FOR_C_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES az::platform
        IMPORTED_LINK_INTERFACE_MULTIPLICITY 2)
    set_target_properties(az::platform PROPERTIES INTERFACE_LINK_LIBRARIES az::core)
    set_target_properties(az::iot::common PROPERTIES INTERFACE_LINK_LIBRARIES az::core)
    set_target_properties(az::iot::hub PROPERTIES INTERFACE_LINK_LIBRARIES "az::core;az::iot::common")
    set_target_properties(az::iot::provisioning PROPERTIES INTERFACE_LINK_LIBRARIES "az::core;az::iot::common")
endif()

unset(_az_sdk_for_c_version)
unset(_az_sdk_for_c_platform_names)
unset(_az_sdk_for_c_libs)
unset(_az_sdk_for_c_lib_vars)
unset(_az_sdk_for_c_debug_vars)
unset(_v)
unset(_dir)
unset(_file)
unset(_n)
unset(_last)
unset(_i)
unset(_j)
unset(_var)
unset(_lib)
unset(_name)
