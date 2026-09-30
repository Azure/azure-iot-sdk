# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

add_az_mqtt_local_test(az_mqtt_test_transport_tls test_transport_tls.c)
add_az_mqtt_local_test(az_mqtt_test_transport_robustness test_transport_robustness.c)
add_az_mqtt_local_test(az_mqtt_test_transport_timeouts test_transport_timeouts.c
  LIBRARY az_mqtt_short_timeouts)
add_az_mqtt_local_test(az_mqtt_test_transport_credentials test_transport_credentials.c)
# The test drives the backend directly (configure hook, PSA import).
if(TARGET MbedTLS::mbedtls)
  target_link_libraries(az_mqtt_test_transport_credentials PRIVATE MbedTLS::mbedtls)
elseif(MBEDTLS_INCLUDE_DIR)
  target_include_directories(az_mqtt_test_transport_credentials PRIVATE ${MBEDTLS_INCLUDE_DIR})
  target_link_libraries(az_mqtt_test_transport_credentials PRIVATE
    ${MBEDTLS_SSL_LIB} ${MBEDTLS_X509_LIB} ${MBEDTLS_CRYPTO_LIB})
endif()
