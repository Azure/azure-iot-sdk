// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter, MQTT 5 client, compiled in: a publish az_mqtt drops itself is reported to
 * on_puback with a failed status and a code of its own (az_mqtt5_client.c _publish_dropped). Only
 * a resumed session drops one, and the adapter's connect() never resumes, so the callback is
 * called directly. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "az_iot_mqtt_az_mqtt_v5.c"

#include <cmocka.h>

typedef struct
{
  int acks;
  uint16_t packet_id;
  az_iot_result status;
  int32_t protocol_code;
} ack_recorder;

static void record_ack(const az_iot_mqtt_event* evt, void* ctx)
{
  ack_recorder* r = (ack_recorder*)ctx;
  if (evt->kind == AZ_IOT_MQTT_EVT_PUBLISH_ACK)
  {
    r->acks++;
    r->packet_id = evt->packet_id;
    r->status = evt->status;
    r->protocol_code = evt->protocol_code;
  }
}

/** @brief Reports a drop as az_mqtt does: @p status with @p reason_code; returns the event. */
static ack_recorder drop(az_result status, az_mqtt5_reason_code reason_code)
{
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v5();
  assert_non_null(f);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  ack_recorder r = { 0 };
  c->iface->set_inbound_cb(c, record_ack, &r);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "broker.example";
  o.client_id = "c";
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_OK); // Sets the az_mqtt user context.

  az_mqtt5_ack_data ack;
  memset(&ack, 0, sizeof(ack));
  ack.packet_id = 7;
  ack.reason_code = reason_code;
  ack.status = status;
  _azm_on_puback(&_azm_self(c)->client, &ack);
  assert_int_equal(r.acks, 0); // Held outside process_loop().
  _azm_self(c)->due_count = _azm_self(c)->pending_count; // As process_loop() would, without I/O.
  (void)_azm_deliver_pending(_azm_self(c));
  assert_int_equal(r.acks, 1);

  c->iface->set_inbound_cb(c, NULL, NULL);
  (void)c->iface->disconnect(c);
  c->iface->destroy(c);
  az_iot_az_mqtt_factory_destroy(f);
  return r;
}

static void a_publish_dropped_as_too_large_is_refused(void** state)
{
  (void)state;
  ack_recorder const r = drop(AZ_MQTT_ERROR_PACKET_TOO_LARGE, AZ_MQTT5_REASON_PACKET_TOO_LARGE);
  assert_int_equal(r.packet_id, 7);
  assert_int_equal(r.status, AZ_IOT_ERR_PUBLISH_REFUSED);
  assert_int_equal(r.protocol_code, 0x95);
}

static void a_publish_dropped_otherwise_is_an_mqtt_error(void** state)
{
  (void)state;
  ack_recorder const r = drop(AZ_MQTT_ERROR_SESSION_NOT_RESUMED, AZ_MQTT5_REASON_UNSPECIFIED_ERROR);
  assert_int_equal(r.status, AZ_IOT_ERR_MQTT);
  assert_int_equal(r.protocol_code, 0x80);
}

static void a_failed_status_with_a_success_code_is_an_mqtt_error(void** state)
{
  (void)state;
  ack_recorder const r = drop(AZ_MQTT_ERROR_SESSION_NOT_RESUMED, AZ_MQTT5_REASON_SUCCESS);
  assert_int_equal(r.status, AZ_IOT_ERR_MQTT);
  assert_int_equal(r.protocol_code, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_publish_dropped_as_too_large_is_refused),
    cmocka_unit_test(a_publish_dropped_otherwise_is_an_mqtt_error),
    cmocka_unit_test(a_failed_status_with_a_success_code_is_an_mqtt_error),
  };
  return cmocka_run_group_tests_name("az_mqtt_adapter_ack", tests, NULL, NULL);
}
