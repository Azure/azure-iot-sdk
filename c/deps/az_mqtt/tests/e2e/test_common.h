// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef AZ_MQTT_E2E_TEST_COMMON_H
#define AZ_MQTT_E2E_TEST_COMMON_H


#include "az_mqtt_test_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define E2E_SEND_BUF_SIZE 2048
#define E2E_RECV_BUF_SIZE 2048
#define E2E_MAX_USER_PROPS 4
#define E2E_MAX_REASON_CODES 4
#define E2E_TRANSPORT_BUF_SIZE (256 * 1024)

typedef struct
{
  uint8_t send_buf[E2E_SEND_BUF_SIZE];
  uint8_t recv_buf[E2E_RECV_BUF_SIZE];
  union
  {
    uint8_t bytes[E2E_TRANSPORT_BUF_SIZE];
    void* align_pointer;
    int64_t align_int64;
    double align_double;
  } transport_buf; /**< Aligned for the transport's members. */

#if AZ_MQTT_TEST_VERSION == 5
  AZ_MQTT_T(user_property) connack_props[E2E_MAX_USER_PROPS];
  AZ_MQTT_T(user_property) publish_props[E2E_MAX_USER_PROPS];
  AZ_MQTT_T(user_property) suback_props[E2E_MAX_USER_PROPS];
  AZ_MQTT_T(user_property) ack_props[E2E_MAX_USER_PROPS];
  AZ_MQTT_T(user_property) disconnect_props[E2E_MAX_USER_PROPS];
  AZ_MQTT_T(reason_code) suback_reasons[E2E_MAX_REASON_CODES];
  int32_t publish_subscription_ids[E2E_MAX_USER_PROPS];
#endif
} az_mqtt_e2e_fixture;

typedef struct
{
  az_span client_id;
  az_span hostname;
  uint16_t port;
  uint16_t keep_alive_seconds;
  /** @brief Clean start (MQTT 5) / clean session (MQTT 3.1.1). */
  bool clean_start;
  az_mqtt_tls_options const* tls_options;

  AZ_MQTT_T(on_connack_fn) on_connack;
  AZ_MQTT_T(on_publish_received_fn) on_publish;
  AZ_MQTT_T(on_suback_fn) on_suback;
  AZ_MQTT_T(on_unsuback_fn) on_unsuback;
  AZ_MQTT_T(on_puback_fn) on_puback;
  AZ_MQTT_T(on_pubcomp_fn) on_pubcomp;
#if AZ_MQTT_TEST_VERSION == 5
  AZ_MQTT_T(on_disconnect_fn) on_disconnect;
#endif
} az_mqtt_e2e_client_params;

void az_mqtt_e2e_fixture_reset(az_mqtt_e2e_fixture* fixture);

az_result az_mqtt_e2e_init_client(
    az_mqtt_e2e_fixture* fixture,
    AZ_MQTT_T(client)* client,
    az_mqtt_e2e_client_params const* params);

az_result az_mqtt_e2e_wait_until(
    AZ_MQTT_T(client)* client,
    int32_t max_iterations,
    int32_t process_loop_timeout_ms,
    bool (*condition)(void));

void az_mqtt_e2e_copy_span_to_cstr(az_span src, char* dest, size_t capacity);

#endif // AZ_MQTT_E2E_TEST_COMMON_H
