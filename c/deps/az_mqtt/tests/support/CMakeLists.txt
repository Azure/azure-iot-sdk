# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# In-process peer (POSIX + OpenSSL) for tests that need no broker.
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

add_library(az_mqtt_test_server STATIC test_server.c)
target_include_directories(az_mqtt_test_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(az_mqtt_test_server PRIVATE _DEFAULT_SOURCE)
target_link_libraries(az_mqtt_test_server PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
az_mqtt_apply_warnings(az_mqtt_test_server)

string(TOUPPER "${AZ_MQTT_TLS_BACKEND_RESOLVED}" _backend)
set(AZ_MQTT_TEST_BACKEND_DEFINE "AZ_MQTT_TEST_BACKEND_${_backend}" PARENT_SCOPE)

# The core again, with 1 s transport bounds, so timeout paths run in seconds.
list(TRANSFORM AZ_MQTT_CORE_SOURCES PREPEND "${PROJECT_SOURCE_DIR}/" OUTPUT_VARIABLE _sources)
add_library(az_mqtt_short_timeouts STATIC ${_sources})
foreach(_prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS)
  get_target_property(_value az_mqtt_core ${_prop})
  if(_value)
    set_property(TARGET az_mqtt_short_timeouts PROPERTY ${_prop} ${_value})
  endif()
endforeach()
get_target_property(_libs az_mqtt_core LINK_LIBRARIES)
target_link_libraries(az_mqtt_short_timeouts PUBLIC ${_libs})
target_compile_definitions(az_mqtt_short_timeouts PUBLIC
  AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS=1000 AZ_MQTT_TRANSPORT_CONNECT_TIMEOUT_MS=1000)
target_include_directories(az_mqtt_short_timeouts PUBLIC ${PROJECT_SOURCE_DIR}/inc)
az_mqtt_apply_warnings(az_mqtt_short_timeouts)

# add_az_mqtt_local_test(<target> <source> [LIBRARY <lib>] [PER_VERSION]): a
# cmocka test against the in-process peer, linked to <lib> (default az_mqtt::core).
# PER_VERSION instead builds az_mqttN_<target minus "az_mqtt_"> per enabled
# version N, on az_mqtt::mqttvN with AZ_MQTT_TEST_VERSION=N (az_mqtt_test_api.h).
# Internal headers (src/platform) are visible.
function(add_az_mqtt_local_test target_name source_file)
  cmake_parse_arguments(ARG "PER_VERSION" "LIBRARY" "" ${ARGN})
  if(ARG_PER_VERSION)
    string(REGEX REPLACE "^az_mqtt_" "" _suffix ${target_name})
    foreach(version 3 5)
      if(AZ_MQTT_ENABLE_MQTTV${version})
        _az_mqtt_local_test(az_mqtt${version}_${_suffix} ${source_file} az_mqtt::mqttv${version})
        target_compile_definitions(az_mqtt${version}_${_suffix} PRIVATE AZ_MQTT_TEST_VERSION=${version})
      endif()
    endforeach()
  else()
    if(NOT ARG_LIBRARY)
      set(ARG_LIBRARY az_mqtt::core)
    endif()
    _az_mqtt_local_test(${target_name} ${source_file} ${ARG_LIBRARY})
  endif()
endfunction()

function(_az_mqtt_local_test target_name source_file library)
  add_executable(${target_name} ${source_file})
  target_include_directories(${target_name} PRIVATE
    ${cmocka_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/platform ${PROJECT_SOURCE_DIR}/tests)
  target_compile_definitions(${target_name} PRIVATE ${AZ_MQTT_TEST_BACKEND_DEFINE} _DEFAULT_SOURCE)
  target_link_libraries(${target_name} PRIVATE ${library} az_mqtt_test_server cmocka)
  az_mqtt_apply_warnings(${target_name})
  add_test(NAME ${target_name} COMMAND ${target_name})
  # A regression here tends to hang rather than fail.
  set_tests_properties(${target_name} PROPERTIES TIMEOUT 120)
endfunction()
