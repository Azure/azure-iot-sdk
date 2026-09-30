// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "test_common.h"

#include <string.h>

#if AZ_MQTT_TEST_VERSION == 5
#define AZ_MQTT5_SPAN_FROM_ARRAY(ARRAY) \
  AZ_SPAN_FROM_BUFFER(*(uint8_t(*)[sizeof(ARRAY)])(ARRAY))
#endif

void az_mqtt_e2e_fixture_reset(az_mqtt_e2e_fixture* fixture)
{
  memset(fixture, 0, sizeof(*fixture));
}

az_result az_mqtt_e2e_init_client(
    az_mqtt_e2e_fixture* fixture,
    AZ_MQTT_T(client)* client,
    az_mqtt_e2e_client_params const* params)
{
  if (az_mqtt_transport_sizeof() > E2E_TRANSPORT_BUF_SIZE)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }

  az_mqtt_transport* transport = (az_mqtt_transport*)fixture->transport_buf.bytes;
  az_result rc = az_mqtt_transport_init(transport);
  if (az_result_failed(rc))
  {
    return rc;
  }

  AZ_MQTT_T(connect_options) connect_opts = AZ_MQTT_T(connect_options_default)();
  connect_opts.client_id = params->client_id;
  connect_opts.keep_alive_seconds = params->keep_alive_seconds;
  connect_opts.AZ_MQTT_TEST_CLEAN = params->clean_start;

  AZ_MQTT_T(client_options) opts;
  memset(&opts, 0, sizeof(opts));
  opts.transport = transport;
  opts.send_buffer = AZ_SPAN_FROM_BUFFER(fixture->send_buf);
  opts.receive_buffer = AZ_SPAN_FROM_BUFFER(fixture->recv_buf);
  opts.connect_options = connect_opts;
  opts.hostname = params->hostname;
  opts.port = params->port;
  opts.tls_options = params->tls_options;

  opts.on_connack = params->on_connack;
  opts.on_publish = params->on_publish;
  opts.on_suback = params->on_suback;
  opts.on_unsuback = params->on_unsuback;
  opts.on_puback = params->on_puback;
  opts.on_pubcomp = params->on_pubcomp;
#if AZ_MQTT_TEST_VERSION == 5
  opts.on_disconnect = params->on_disconnect;

  opts.buffers.connack_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(fixture->connack_props);
  opts.buffers.publish_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(fixture->publish_props);
  opts.buffers.publish_subscription_identifiers = AZ_MQTT5_SPAN_FROM_ARRAY(fixture->publish_subscription_ids);
  opts.buffers.suback_reason_codes = AZ_MQTT5_SPAN_FROM_ARRAY(fixture->suback_reasons);
  opts.buffers.suback_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(fixture->suback_props);
  opts.buffers.ack_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(fixture->ack_props);
  opts.buffers.disconnect_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(fixture->disconnect_props);
#endif

  return AZ_MQTT_T(client_init)(client, &opts);
}

az_result az_mqtt_e2e_wait_until(
    AZ_MQTT_T(client)* client,
    int32_t max_iterations,
    int32_t process_loop_timeout_ms,
    bool (*condition)(void))
{
  for (int32_t i = 0; i < max_iterations; ++i)
  {
    if (condition())
    {
      return AZ_OK;
    }

    az_result rc = AZ_MQTT_T(client_process_loop)(client, process_loop_timeout_ms);
    if (az_result_failed(rc))
    {
      return rc;
    }
  }

  return condition() ? AZ_OK : AZ_MQTT_ERROR_TIMEOUT;
}

void az_mqtt_e2e_copy_span_to_cstr(az_span src, char* dest, size_t capacity)
{
  int32_t src_len = az_span_size(src);
  size_t to_copy = (src_len < (int32_t)(capacity - 1)) ? (size_t)src_len : (capacity - 1);
  if (to_copy > 0)
  {
    memcpy(dest, az_span_ptr(src), to_copy);
  }
  dest[to_copy] = '\0';
}
