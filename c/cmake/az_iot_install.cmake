# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Install rules and the azure-iot-sdk CMake package.
#
# Consumers use:
#   find_package(azure-iot-sdk CONFIG REQUIRED [COMPONENTS <component>...])
#   target_link_libraries(app PRIVATE azure::iot::mqttv3 azure::iot::adapter_paho)
#
# Components are the installed library targets, named without the azure::iot::
# prefix: core, mqttv3, mqttv5 and each adapter the build produced. Each also
# gets a pkg-config file, azure-iot-sdk-<component>.pc (az_iot_pkgconfig.cmake).
#
# azure-sdk-for-c has no install rules and its types are embedded in our public
# structs, so its libraries and headers ship in this package (headers under
# include/azure-sdk-for-c). Paho installs its own eclipse-paho-mqtt-c package
# into the same prefix; the config file loads that one.
#
# Include once, after every target is defined.

if(NOT AZ_IOT_INSTALL)
    return()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(_az_iot_pkg azure-iot-sdk)
set(_az_iot_pkg_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/${_az_iot_pkg}")
set(_az_iot_sdk_for_c_inc "${CMAKE_INSTALL_INCLUDEDIR}/azure-sdk-for-c")

# First-party target -> export name (azure::iot::<name>, matching the aliases).
set(_az_iot_export_names
    az_iot_core                          core
    az_iot_mqttv3                        mqttv3
    az_iot_mqttv5                        mqttv5
    az_iot_adapter_paho                  adapter_paho
    az_iot_adapter_rust_mqtt             adapter_rust_mqtt
    az_iot_su_crypto_openssl             su_crypto_openssl
    az_iot_su_crypto_mbedtls             su_crypto_mbedtls
    az_iot_certificate_provider_managed  certificate_provider_managed
)

set(_az_iot_targets "")
set(AZ_IOT_PACKAGE_COMPONENTS "")
list(LENGTH _az_iot_export_names _n)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE 0 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _az_iot_export_names ${_i} _tgt)
    list(GET _az_iot_export_names ${_j} _name)
    if(TARGET ${_tgt})
        set_target_properties(${_tgt} PROPERTIES EXPORT_NAME ${_name})
        list(APPEND _az_iot_targets ${_tgt})
        list(APPEND AZ_IOT_PACKAGE_COMPONENTS ${_name})
    endif()
endforeach()

# azure-sdk-for-c targets our libraries link. Their include directories are raw
# source paths, which install(EXPORT) rejects: keep them for the build tree only
# and point the installed targets at the bundled headers.
set(_az_iot_sdk_for_c_targets "")
foreach(_tgt IN ITEMS az_core az_iot_common az_iot_hub az_iot_provisioning az_win32 az_posix az_noplatform)
    if(NOT TARGET ${_tgt})
        continue()
    endif()
    foreach(_prop IN ITEMS INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
        get_target_property(_dirs ${_tgt} ${_prop})
        if(NOT _dirs)
            continue()
        endif()
        set(_fixed "")
        foreach(_d IN LISTS _dirs)
            if(_d MATCHES "^\\$<INSTALL_INTERFACE:")
                continue()
            elseif(_d MATCHES "^\\$<BUILD_INTERFACE:")
                list(APPEND _fixed "${_d}")
            else()
                list(APPEND _fixed "$<BUILD_INTERFACE:${_d}>")
            endif()
        endforeach()
        # Imported targets' include directories are already SYSTEM.
        if(_prop STREQUAL "INTERFACE_INCLUDE_DIRECTORIES")
            list(APPEND _fixed "$<INSTALL_INTERFACE:${_az_iot_sdk_for_c_inc}>")
        endif()
        list(REMOVE_DUPLICATES _fixed)
        set_target_properties(${_tgt} PROPERTIES ${_prop} "${_fixed}")
    endforeach()
    list(APPEND _az_iot_sdk_for_c_targets ${_tgt})
endforeach()

install(TARGETS ${_az_iot_targets} ${_az_iot_sdk_for_c_targets}
    EXPORT ${_az_iot_pkg}-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/inc/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY "${azure_sdk_for_c_SOURCE_DIR}/sdk/inc/"
    DESTINATION ${_az_iot_sdk_for_c_inc}
    FILES_MATCHING PATTERN "*.h"
)

# Adapter headers that live next to their sources. Installed together, and the
# directory is on each adapter's installed include path, so consumers keep the
# bare #include "<header>.h" spelling.
set(_az_iot_adapter_inc "${CMAKE_INSTALL_INCLUDEDIR}/azure/iot/adapters")
set(_az_iot_adapter_headers
    az_iot_adapter_rust_mqtt             adapters/rust_mqtt/az_iot_mqtt_rust_ffi.h
    az_iot_su_crypto_openssl             adapters/su/crypto_openssl/az_iot_su_crypto_openssl.h
    az_iot_su_crypto_mbedtls             adapters/su/crypto_mbedtls/az_iot_su_crypto_mbedtls.h
    az_iot_certificate_provider_managed  adapters/cert_openssl/az_iot_certificate_provider_managed.h
)
list(LENGTH _az_iot_adapter_headers _n)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE 0 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _az_iot_adapter_headers ${_i} _tgt)
    list(GET _az_iot_adapter_headers ${_j} _hdr)
    if(TARGET ${_tgt})
        target_include_directories(${_tgt} PUBLIC "$<INSTALL_INTERFACE:${_az_iot_adapter_inc}>")
        install(FILES "${PROJECT_SOURCE_DIR}/${_hdr}" DESTINATION ${_az_iot_adapter_inc})
    endif()
endforeach()

if(EXISTS "${PROJECT_SOURCE_DIR}/../LICENSE")
    install(FILES "${PROJECT_SOURCE_DIR}/../LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${_az_iot_pkg}")
endif()
install(FILES "${azure_sdk_for_c_SOURCE_DIR}/LICENSE" "${azure_sdk_for_c_SOURCE_DIR}/NOTICE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${_az_iot_pkg}/azure-sdk-for-c")

# Dependencies the config file must load before the exported targets.
set(AZ_IOT_PACKAGE_NEEDS_PAHO OFF)
set(AZ_IOT_PACKAGE_NEEDS_OPENSSL OFF)
set(AZ_IOT_PACKAGE_OPENSSL_MIN "")
if(TARGET az_iot_adapter_paho)
    set(AZ_IOT_PACKAGE_NEEDS_PAHO ON)
    if(TARGET paho-mqtt3as-static)
        set(AZ_IOT_PACKAGE_NEEDS_OPENSSL ON)
    endif()
    get_target_property(_defs az_iot_adapter_paho COMPILE_DEFINITIONS)
    if("AZ_IOT_PAHO_KEY_CUSTODY=1" IN_LIST _defs)
        set(AZ_IOT_PACKAGE_OPENSSL_MIN 3.0)
    endif()
    unset(_defs)
endif()
if(TARGET az_iot_su_crypto_openssl OR TARGET az_iot_certificate_provider_managed)
    set(AZ_IOT_PACKAGE_NEEDS_OPENSSL ON)
    set(AZ_IOT_PACKAGE_OPENSSL_MIN 3.0)
endif()
if(TARGET az_iot_su_crypto_mbedtls)
    set(AZ_IOT_PACKAGE_NEEDS_MBEDTLS ON)
else()
    set(AZ_IOT_PACKAGE_NEEDS_MBEDTLS OFF)
endif()

install(EXPORT ${_az_iot_pkg}-targets
    NAMESPACE azure::iot::
    DESTINATION ${_az_iot_pkg_cmake_dir}
)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/${_az_iot_pkg}-config.cmake.in"
    "${PROJECT_BINARY_DIR}/${_az_iot_pkg}-config.cmake"
    INSTALL_DESTINATION ${_az_iot_pkg_cmake_dir}
)
# 0.x: a minor bump may break the API.
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/${_az_iot_pkg}-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
install(FILES
    "${PROJECT_BINARY_DIR}/${_az_iot_pkg}-config.cmake"
    "${PROJECT_BINARY_DIR}/${_az_iot_pkg}-config-version.cmake"
    DESTINATION ${_az_iot_pkg_cmake_dir}
)

include(az_iot_pkgconfig)

unset(_n)
unset(_last)
unset(_i)
unset(_j)
unset(_tgt)
unset(_hdr)
unset(_name)
unset(_prop)
unset(_dirs)
unset(_fixed)
unset(_d)
