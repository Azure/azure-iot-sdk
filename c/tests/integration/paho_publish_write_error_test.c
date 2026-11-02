// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/*
 * Paho adapter: a QoS 1 publish whose write fails.
 *
 * The test is the server: it accepts the adapter's connection on loopback, answers CONNECT with
 * CONNACK and reads nothing more. It then shuts down the sending side of the adapter's socket.
 * Paho does not notice (reads still work, and the server does not answer the FIN), so its next
 * write -- the PUBLISH -- fails with a socket error, as when a broker closes the connection
 * mid-write (IoT Hub refusing a packet over its Maximum Packet Size).
 *
 * Paho 1.3.16 then freed the topic and payload it had stored for retry, and freed them again when
 * it cleaned the session or the client was destroyed (patches/0003). It also reports that
 * failure with token 0, so the adapter must name the publish from its own context.
 *
 * POSIX only: it finds the adapter's socket by its peer port and uses shutdown(SHUT_WR).
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmocka.h>

#include "az_iot_test_mqtt_server.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "support/test_env.h"

#define PUMP_SLICE_MS 10u
#define PAYLOAD_SIZE 4096

typedef struct
{
  int connected;
  int disconnected;
  int publish_acks;
  uint16_t publish_ack_packet_id;
  az_iot_result publish_ack_status;
} write_error_log;

/* Runs on the test thread only: process_loop() dispatches the adapter's queue. */
static void on_event(const az_iot_mqtt_event* evt, void* ctx)
{
  write_error_log* log = (write_error_log*)ctx;
  switch (evt->kind)
  {
    case AZ_IOT_MQTT_EVT_CONNECTED:
      log->connected += (evt->status == AZ_IOT_OK);
      break;
    case AZ_IOT_MQTT_EVT_DISCONNECTED:
      log->disconnected++;
      break;
    case AZ_IOT_MQTT_EVT_PUBLISH_ACK:
      log->publish_acks++;
      log->publish_ack_packet_id = evt->packet_id;
      log->publish_ack_status = evt->status;
      break;
    case AZ_IOT_MQTT_EVT_MESSAGE:
    case AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK:
    case AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK:
    case AZ_IOT_MQTT_EVT_ERROR:
      break;
  }
}

/* Pumps until *counter is non-zero, within 5 s times the timeout scale. */
static bool pump_until(az_iot_mqtt_client* c, const int* counter)
{
  unsigned const slices = (5000u / PUMP_SLICE_MS) * az_iot_test_timeout_scale();
  for (unsigned i = 0; i < slices && *counter == 0; ++i)
  {
    assert_int_equal(c->iface->process_loop(c, PUMP_SLICE_MS), AZ_IOT_OK);
  }
  return *counter != 0;
}

/* Listens on an ephemeral loopback port; returns the socket and sets *port. */
static int listen_on_loopback(uint16_t* port)
{
  int const listener = socket(AF_INET, SOCK_STREAM, 0);
  assert_true(listener >= 0);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t addr_len = sizeof(addr);
  assert_int_equal(bind(listener, (struct sockaddr*)&addr, sizeof(addr)), 0);
  assert_int_equal(listen(listener, 1), 0);
  assert_int_equal(getsockname(listener, (struct sockaddr*)&addr, &addr_len), 0);
  *port = ntohs(addr.sin_port);
  return listener;
}

static void read_exactly(int fd, uint8_t* buffer, size_t length)
{
  size_t done = 0;
  while (done < length)
  {
    ssize_t const n = recv(fd, buffer + done, length - done, 0);
    assert_true(n > 0);
    done += (size_t)n;
  }
}

/* Accepts the adapter's connection, reads its CONNECT and answers CONNACK. */
static int accept_and_acknowledge_connect(int listener, az_iot_test_mqtt_version version)
{
  int const fd = accept(listener, NULL, NULL);
  assert_true(fd >= 0);
  uint8_t byte = 0;
  read_exactly(fd, &byte, 1);
  assert_int_equal(byte, 0x10); /* CONNECT */
  size_t remaining_length = 0;
  unsigned shift = 0;
  do
  {
    read_exactly(fd, &byte, 1);
    remaining_length |= (size_t)(byte & 0x7Fu) << shift;
    shift += 7;
  } while ((byte & 0x80u) != 0 && shift < 28);
  uint8_t* connect = (uint8_t*)malloc(remaining_length > 0 ? remaining_length : 1);
  assert_non_null(connect);
  read_exactly(fd, connect, remaining_length);
  free(connect);

  az_iot_test_mqtt_packet const connack
      = az_iot_test_mqtt_connack(version, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  assert_int_equal(send(fd, connack.bytes, connack.len, 0), (ssize_t)connack.len);
  return fd;
}

/* The adapter's socket: the only one in this process whose peer is @p server_port. */
static int find_socket_to_server(uint16_t server_port)
{
  long const open_max = sysconf(_SC_OPEN_MAX);
  int const max = (open_max <= 0 || open_max > 65536) ? 65536 : (int)open_max;
  for (int fd = 0; fd < max; ++fd)
  {
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr*)&peer, &peer_len) == 0 && peer.sin_family == AF_INET
        && ntohs(peer.sin_port) == server_port)
    {
      return fd;
    }
  }
  return -1;
}

/* Connects, makes the PUBLISH write fail, and checks the adapter reports that publish by its
 * packet id. Paho then frees what it stored for retry: at the session end when the session ends
 * with the connection (@p clean_start), else at destroy. */
static void publish_whose_write_fails(az_iot_mqtt_version version, bool clean_start)
{
  uint16_t port = 0;
  int const listener = listen_on_loopback(&port);

  static const az_iot_mqtt_limits k_limits = { 8192, 4096, 8, 0, 1024 };
  size_t const workspace_size = az_iot_paho_get_workspace_size(&k_limits);
  uint8_t* workspace = (uint8_t*)malloc(workspace_size);
  assert_non_null(workspace);
  az_iot_mqtt_client* c = NULL;
  assert_int_equal(
      az_iot_paho_get_adapter()->init(version, &k_limits, workspace, workspace_size, &c),
      AZ_IOT_OK);
  write_error_log log = { 0 };
  c->iface->set_inbound_cb(c, on_event, &log);

  az_iot_mqtt_connect_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "127.0.0.1";
  opts.port = port;
  opts.client_id = "az-publish-write-error";
  opts.keep_alive_seconds = 30;
  opts.connect_timeout_seconds = 5u * az_iot_test_timeout_scale();
  opts.clean_start = clean_start;
  assert_int_equal(c->iface->connect(c, &opts), AZ_IOT_OK);
  int const server_fd = accept_and_acknowledge_connect(listener, AZ_IOT_TEST_MQTT_V3_1_1);
  assert_true(pump_until(c, &log.connected));

  int const client_fd = find_socket_to_server(port);
  assert_true(client_fd >= 0);
  assert_int_equal(shutdown(client_fd, SHUT_WR), 0);

  static uint8_t payload[PAYLOAD_SIZE];
  memset(payload, 'p', sizeof(payload));
  az_iot_mqtt_message msg;
  memset(&msg, 0, sizeof(msg));
  msg.topic = "az/publish-write-error";
  msg.payload = payload;
  msg.payload_len = sizeof(payload);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t packet_id = 0;
  assert_int_equal(c->iface->publish(c, &msg, &packet_id), AZ_IOT_OK);
  assert_int_not_equal(packet_id, 0);

  assert_true(pump_until(c, &log.publish_acks));
  assert_int_equal(log.publish_acks, 1);
  assert_int_equal(log.publish_ack_packet_id, packet_id);
  assert_int_equal(log.publish_ack_status, AZ_IOT_ERR_MQTT);
  /* Paho ends the session after a write error. */
  assert_true(pump_until(c, &log.disconnected));

  c->iface->destroy(c);
  free(workspace);
  close(server_fd);
  close(listener);
}

static void a_v3_publish_whose_write_fails_is_reported_and_freed_once(void** state)
{
  (void)state;
  publish_whose_write_fails(AZ_IOT_MQTT_VERSION_3_1_1, true);
}

static void a_v3_publish_whose_write_fails_is_reported_and_freed_once_at_destroy(void** state)
{
  (void)state;
  /* What the SDK's hub session does: its session outlives the connection. */
  publish_whose_write_fails(AZ_IOT_MQTT_VERSION_3_1_1, false);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_v3_publish_whose_write_fails_is_reported_and_freed_once),
    cmocka_unit_test(a_v3_publish_whose_write_fails_is_reported_and_freed_once_at_destroy),
  };
  return cmocka_run_group_tests_name("paho_publish_write_error", tests, NULL, NULL);
}
