# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# pkg-config files, one per installed component: azure-iot-sdk-<component>.pc.
#
#   cc app.c $(pkg-config --static --cflags --libs azure-iot-sdk-mqttv3 azure-iot-sdk-adapter_paho)
#
# The libraries are static, so --static is required whenever an adapter is
# used. Libs lists this package's own libraries (first-party and the bundled
# azure-sdk-for-c ones); Requires.private and Libs.private list third-party ones.
#
# prefix= is the absolute install prefix, written at install time so that
# `cmake --install --prefix` is honoured. It is not ${pcfiledir}-relative:
# pkg-config prefixes -I/-L with PKG_CONFIG_SYSROOT_DIR, which a path already
# inside the sysroot would then get twice.
#
# Included by az_iot_install.cmake, which defines the lists used here. Not
# generated for MSVC, which pkg-config does not serve.

if(MSVC)
    return()
endif()

# Converts the link items of <tgt> to pkg-config fields in the caller's
# _az_iot_pc_requires, _az_iot_pc_requires_private and _az_iot_pc_libs_private.
# A target without a mapping is an error, so a new dependency cannot silently
# go missing from the .pc files.
function(_az_iot_pc_deps tgt openssl_req)
    set(_requires "")
    set(_requires_private "")
    set(_libs_private "")
    get_target_property(_queue ${tgt} LINK_LIBRARIES)
    if(NOT _queue)
        set(_queue "")
    endif()
    list(LENGTH _queue _left)
    while(_left GREATER 0)
        list(POP_FRONT _queue _item)
        if(_item MATCHES "^\\$<LINK_ONLY:(.+)>$")
            set(_item "${CMAKE_MATCH_1}")
        endif()
        if(_item STREQUAL "az_iot_core")
            list(APPEND _requires azure-iot-sdk-core)
        elseif(_item STREQUAL "OpenSSL::SSL")
            list(APPEND _requires_private libssl)
        elseif(_item STREQUAL "OpenSSL::Crypto")
            list(APPEND _requires_private "libcrypto${openssl_req}")
        elseif(_item MATCHES "^MbedTLS::(mbedcrypto|tfpsacrypto)$")
            # mbedcrypto.pc links tfpsacrypto on mbedTLS 4.x.
            list(APPEND _requires_private mbedcrypto)
        elseif(_item MATCHES "^paho-mqtt3as?-static$")
            # Paho installs no .pc file: link its archive and its own dependencies.
            get_target_property(_name ${_item} OUTPUT_NAME)
            if(NOT _name)
                set(_name ${_item})
            endif()
            list(APPEND _libs_private "-l${_name}")
            get_target_property(_paho_deps ${_item} INTERFACE_LINK_LIBRARIES)
            if(_paho_deps)
                list(PREPEND _queue ${_paho_deps})
            endif()
        elseif(TARGET "${_item}" OR IS_ABSOLUTE "${_item}" OR _item MATCHES "[$<>]")
            message(FATAL_ERROR "pkg-config: no mapping for ${tgt} link item '${_item}'")
        elseif(_item MATCHES "^-")
            list(APPEND _libs_private "${_item}")
        else()
            list(APPEND _libs_private "-l${_item}")
        endif()
        list(LENGTH _queue _left)
    endwhile()
    foreach(_var IN ITEMS _requires _requires_private _libs_private)
        list(REMOVE_DUPLICATES ${_var})
        list(JOIN ${_var} " " _joined)
        set(_az_iot_pc${_var} "${_joined}" PARENT_SCOPE)
    endforeach()
endfunction()

foreach(_dir IN ITEMS LIBDIR INCLUDEDIR)
    if(IS_ABSOLUTE "${CMAKE_INSTALL_${_dir}}")
        set(AZ_IOT_PC_${_dir} "${CMAKE_INSTALL_${_dir}}")
    else()
        set(AZ_IOT_PC_${_dir} "\${prefix}/${CMAKE_INSTALL_${_dir}}")
    endif()
endforeach()
file(RELATIVE_PATH _az_iot_pc_sdk_for_c_inc "/${CMAKE_INSTALL_INCLUDEDIR}" "/${_az_iot_sdk_for_c_inc}")
file(RELATIVE_PATH _az_iot_pc_adapter_inc "/${CMAKE_INSTALL_INCLUDEDIR}" "/${_az_iot_adapter_inc}")

# Bundled azure-sdk-for-c libraries, dependents first. az_posix and az_core
# reference each other; any az_core use pulls in what az_posix needs from it.
set(_az_iot_pc_sdk_for_c_libs "")
foreach(_tgt IN ITEMS az_iot_provisioning az_iot_hub az_iot_common az_core az_posix az_win32 az_noplatform)
    if(_tgt IN_LIST _az_iot_sdk_for_c_targets)
        string(APPEND _az_iot_pc_sdk_for_c_libs " -l${_tgt}")
    endif()
endforeach()

set(_az_iot_pc_descriptions
    core                          "core: connection client and software updates"
    mqttv3                        "IoT Hub mqttv3 clients"
    mqttv5                        "IoT Hub mqttv5 clients"
    adapter_paho                  "Eclipse Paho MQTT adapter"
    adapter_rust_mqtt             "Rust MQTT adapter"
    su_crypto_openssl             "OpenSSL software updates crypto adapter"
    su_crypto_mbedtls             "mbedTLS software updates crypto adapter"
    certificate_provider_managed  "OpenSSL managed certificate provider"
)

set(_az_iot_pc_dir "${PROJECT_BINARY_DIR}/pkgconfig")
list(LENGTH _az_iot_export_names _n)
math(EXPR _last "${_n} - 1")
foreach(_i RANGE 0 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _az_iot_export_names ${_i} _tgt)
    list(GET _az_iot_export_names ${_j} AZ_IOT_PC_COMPONENT)
    if(NOT TARGET ${_tgt})
        continue()
    endif()
    list(FIND _az_iot_pc_descriptions ${AZ_IOT_PC_COMPONENT} _k)
    math(EXPR _k "${_k} + 1")
    list(GET _az_iot_pc_descriptions ${_k} _desc)
    set(AZ_IOT_PC_DESCRIPTION "Azure IoT C SDK ${_desc}")

    if(_tgt STREQUAL "az_iot_core")
        set(AZ_IOT_PC_REQUIRES "")
        set(AZ_IOT_PC_REQUIRES_PRIVATE "")
        set(AZ_IOT_PC_CFLAGS "-I\${includedir} -I\${includedir}/${_az_iot_pc_sdk_for_c_inc}")
        set(AZ_IOT_PC_LIBS "-l${_tgt}${_az_iot_pc_sdk_for_c_libs}")
        set(AZ_IOT_PC_LIBS_PRIVATE "")
    else()
        set(_openssl_req "")
        if(_tgt MATCHES "^az_iot_(su_crypto_openssl|certificate_provider_managed)$")
            set(_openssl_req " >= 3.0")
        endif()
        _az_iot_pc_deps(${_tgt} "${_openssl_req}")
        set(AZ_IOT_PC_REQUIRES "${_az_iot_pc_requires}")
        set(AZ_IOT_PC_REQUIRES_PRIVATE "${_az_iot_pc_requires_private}")
        set(AZ_IOT_PC_CFLAGS "")
        if(_tgt IN_LIST _az_iot_adapter_headers)
            set(AZ_IOT_PC_CFLAGS "-I\${includedir}/${_az_iot_pc_adapter_inc}")
        endif()
        set(AZ_IOT_PC_LIBS "-l${_tgt}")
        set(AZ_IOT_PC_LIBS_PRIVATE "${_az_iot_pc_libs_private}")
    endif()

    # Two passes: everything now except the prefix, which install time fills in.
    set(_pc "${_az_iot_pc_dir}/${_az_iot_pkg}-${AZ_IOT_PC_COMPONENT}.pc")
    set(AZ_IOT_PC_PREFIX "@AZ_IOT_PC_PREFIX@")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/${_az_iot_pkg}.pc.in" "${_pc}.in" @ONLY)
    install(CODE "set(AZ_IOT_PC_PREFIX \"\${CMAKE_INSTALL_PREFIX}\")
configure_file(\"${_pc}.in\" \"${_pc}\" @ONLY)")
    install(FILES "${_pc}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endforeach()

unset(_az_iot_pc_dir)
foreach(_var IN ITEMS PREFIX LIBDIR INCLUDEDIR COMPONENT DESCRIPTION REQUIRES REQUIRES_PRIVATE CFLAGS LIBS LIBS_PRIVATE)
    unset(AZ_IOT_PC_${_var})
endforeach()
unset(_var)
unset(_az_iot_pc_sdk_for_c_inc)
unset(_az_iot_pc_adapter_inc)
unset(_az_iot_pc_sdk_for_c_libs)
unset(_az_iot_pc_descriptions)
unset(_az_iot_pc_requires)
unset(_az_iot_pc_requires_private)
unset(_az_iot_pc_libs_private)
unset(_openssl_req)
unset(_desc)
unset(_k)
unset(_pc)
