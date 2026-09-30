// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_sample_connect.c
 * @brief Sample: Connect to an MQTT 5.0 broker, subscribe, publish, and receive.
 *
 * Usage:
 *   az_mqtt5_sample_connect [host] [port]
 *
 * Defaults: host = "localhost", port = 1883 (plain TCP).
 * Set port to 8883 and define AZ_MQTT_SAMPLE_USE_TLS to enable TLS.
 *
 * This sample demonstrates the zero-allocation client speaking MQTT 5.0:
 * - All buffers are stack-allocated
 * - No malloc/free calls anywhere
 * - Deterministic memory footprint
 */

#include <az_mqtt5/az_mqtt5_client.h>

#include "az_mqtt_sample_config.h"

#include <azure/core/az_span.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_USER_PROPERTIES 8
#define MAX_SUBACK_REASON_CODES 8

// Reinterpret any fixed-size array as a byte array to use AZ_SPAN_FROM_BUFFER.
#define AZ_MQTT5_SPAN_FROM_ARRAY(ARRAY) AZ_SPAN_FROM_BUFFER(*(uint8_t(*)[sizeof(ARRAY)])(ARRAY))

// Buffers for received MQTT 5.0 properties (zero allocation).
static az_mqtt5_user_property s_connack_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_publish_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_suback_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_ack_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_user_property s_disconnect_user_props[MAX_USER_PROPERTIES];
static az_mqtt5_reason_code s_suback_reason_codes[MAX_SUBACK_REASON_CODES];
static int32_t s_publish_sub_ids[MAX_USER_PROPERTIES];

// ──────────────────────── Callbacks ──────────────────────────

static void on_connack(az_mqtt5_client* client, az_mqtt5_connack_data const* connack)
{
  (void)client;
  printf(
      "[CONNACK] reason=%d session_present=%d\n",
      (int)connack->reason_code,
      connack->session_present);

  if (connack->reason_code == AZ_MQTT5_REASON_SUCCESS)
  {
    printf("  max_qos=%d retain_available=%d\n", connack->maximum_qos, connack->retain_available);
    if (connack->server_keep_alive > 0)
    {
      printf("  server_keep_alive=%d\n", connack->server_keep_alive);
    }
  }
}

static void on_publish(az_mqtt5_client* client, az_mqtt5_publish_data const* publish)
{
  (void)client;
  printf(
      "[PUBLISH received] topic=\"%.*s\" qos=%d payload_len=%d\n",
      az_span_size(publish->topic),
      (char const*)az_span_ptr(publish->topic),
      (int)publish->qos,
      az_span_size(publish->payload));

  if (az_span_size(publish->payload) > 0)
  {
    printf(
        "  payload: \"%.*s\"\n",
        az_span_size(publish->payload),
        (char const*)az_span_ptr(publish->payload));
  }
}

static void on_suback(az_mqtt5_client* client, az_mqtt5_suback_data const* suback)
{
  (void)client;
  printf("[SUBACK] packet_id=%d reason_codes=[", suback->packet_id);
  if (suback->reason_codes != NULL)
  {
    for (int32_t i = 0; i < suback->reason_code_count; i++)
    {
      printf("%s0x%02X", i > 0 ? ", " : "", (unsigned)suback->reason_codes[i]);
    }
  }
  else
  {
    printf("NULL");
  }
  printf("]\n");
}

static void on_puback(az_mqtt5_client* client, az_mqtt5_ack_data const* ack)
{
  (void)client;
  printf("[PUBACK] packet_id=%d reason=%d\n", ack->packet_id, (int)ack->reason_code);
}

static void on_disconnect(az_mqtt5_client* client, az_mqtt5_disconnect_data const* disc)
{
  (void)client;
  printf("[DISCONNECT from broker] reason=%d\n", (int)disc->reason_code);
  if (az_span_size(disc->reason_string) > 0)
  {
    printf(
        "  reason_string: \"%.*s\"\n",
        az_span_size(disc->reason_string),
        (char const*)az_span_ptr(disc->reason_string));
  }
}

// ──────────────────────── Main ───────────────────────────────

int main(int argc, char* argv[])
{
  // Parse arguments
  char const* host = "localhost";
  uint16_t port = 1883;

  if (argc >= 2)
  {
    host = argv[1];
  }
  if (argc >= 3)
  {
    port = (uint16_t)atoi(argv[2]);
  }

  printf("MQTT5 Sample: Connecting to %s:%d\n", host, port);

  int32_t transport_size = az_mqtt_transport_sizeof();
  if (transport_size > (int32_t)sizeof(s_transport_buffer.bytes))
  {
    printf(
        "ERROR: transport buffer too small (need=%d, have=%d). "
        "Increase TRANSPORT_BUFFER_SIZE in az_mqtt_sample_config.h\n",
        transport_size,
        (int)sizeof(s_transport_buffer.bytes));
    return 1;
  }

  // Initialize transport
  az_mqtt_transport* transport = (az_mqtt_transport*)s_transport_buffer.bytes;
  az_result rc = az_mqtt_transport_init(transport);
  if (az_result_failed(rc))
  {
    printf("ERROR: transport init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  // Configure the client for MQTT 5.0
  az_mqtt5_connect_options connect_opts = az_mqtt5_connect_options_default();
  connect_opts.client_id = AZ_SPAN_FROM_STR("mqtt5-c-sample");
  connect_opts.keep_alive_seconds = 30;
  connect_opts.clean_start = true;

  az_mqtt5_client_options client_opts;
  memset(&client_opts, 0, sizeof(client_opts));
  client_opts.transport = transport;
  client_opts.send_buffer = AZ_SPAN_FROM_BUFFER(s_send_buffer);
  client_opts.receive_buffer = AZ_SPAN_FROM_BUFFER(s_recv_buffer);
  client_opts.connect_options = connect_opts;
  client_opts.hostname = az_span_create((uint8_t*)(uintptr_t)host, (int32_t)strlen(host));
  client_opts.port = port;
  client_opts.tls_options = NULL; // Plain TCP for this sample

  // Callbacks
  client_opts.on_connack = on_connack;
  client_opts.on_publish = on_publish;
  client_opts.on_suback = on_suback;
  client_opts.on_puback = on_puback;
  client_opts.on_disconnect = on_disconnect;

  // Pre-allocated buffers for received properties
  client_opts.buffers.connack_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(s_connack_user_props);
  client_opts.buffers.publish_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(s_publish_user_props);
  client_opts.buffers.publish_subscription_identifiers = AZ_MQTT5_SPAN_FROM_ARRAY(s_publish_sub_ids);
  client_opts.buffers.suback_reason_codes = AZ_MQTT5_SPAN_FROM_ARRAY(s_suback_reason_codes);
  client_opts.buffers.suback_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(s_suback_user_props);
  client_opts.buffers.ack_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(s_ack_user_props);
  client_opts.buffers.disconnect_user_properties = AZ_MQTT5_SPAN_FROM_ARRAY(s_disconnect_user_props);

  // Initialize client
  az_mqtt5_client client;
  rc = az_mqtt5_client_init(&client, &client_opts);
  if (az_result_failed(rc))
  {
    printf("ERROR: client init failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }

  // Connect (blocks until CONNACK or timeout)
  printf("Connecting...\n");
  rc = az_mqtt5_client_connect(&client, 10000 /* 10 second timeout */);
  if (az_result_failed(rc))
  {
    printf("ERROR: connect failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }
  printf("Connected!\n");

  // Subscribe to a test topic
  az_mqtt5_subscription sub;
  sub.topic_filter = AZ_SPAN_FROM_STR("mqtt5/test/#");
  sub.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  sub.no_local = false;
  sub.retain_as_published = false;
  sub.retain_handling = AZ_MQTT5_RETAIN_HANDLING_SEND_AT_SUBSCRIBE;

  uint16_t sub_packet_id;
  rc = az_mqtt5_client_subscribe(&client, &sub, 1, &sub_packet_id);
  if (az_result_failed(rc))
  {
    printf("ERROR: subscribe failed: 0x%08X\n", (unsigned)rc);
    if (az_result_failed(az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION)))
    {
      printf("WARNING: disconnect failed\n");
    }
    return 1;
  }
  printf("Subscribe sent (packet_id=%d)\n", sub_packet_id);

  // Publish a test message
  az_mqtt5_publish_options pub_opts = az_mqtt5_publish_options_default();
  pub_opts.topic = AZ_SPAN_FROM_STR("mqtt5/test/hello");
  pub_opts.payload = AZ_SPAN_FROM_STR("Hello from az_mqtt5_client! Zero allocations.");
  pub_opts.qos = AZ_MQTT_QOS_AT_LEAST_ONCE;
  pub_opts.payload_format_indicator = 1; // UTF-8 string

  uint16_t pub_packet_id;
  rc = az_mqtt5_client_publish(&client, &pub_opts, &pub_packet_id);
  if (az_result_failed(rc))
  {
    printf("ERROR: publish failed: 0x%08X\n", (unsigned)rc);
    if (az_result_failed(az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION)))
    {
      printf("WARNING: disconnect failed\n");
    }
    return 1;
  }
  printf("Publish sent (packet_id=%d)\n", pub_packet_id);

  // Process loop: handle SUBACK, PUBACK, incoming messages
  printf("Processing events (10 iterations)...\n");
  for (int i = 0; i < 10; i++)
  {
    rc = az_mqtt5_client_process_loop(&client, 1000 /* 1 second timeout */);
    if (az_result_failed(rc))
    {
      printf("ERROR: process_loop failed: 0x%08X\n", (unsigned)rc);
      break;
    }
  }

  // Disconnect
  printf("Disconnecting...\n");
  rc = az_mqtt5_client_disconnect(&client, AZ_MQTT5_REASON_NORMAL_DISCONNECTION);
  if (az_result_failed(rc))
  {
    printf("ERROR: disconnect failed: 0x%08X\n", (unsigned)rc);
    return 1;
  }
  printf("Done.\n");

  return 0;
}
