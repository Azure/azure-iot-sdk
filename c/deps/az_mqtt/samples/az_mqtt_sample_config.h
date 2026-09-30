// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef AZ_MQTT_SAMPLE_CONFIG_H
#define AZ_MQTT_SAMPLE_CONFIG_H

#include <az_mqtt/az_mqtt_transport.h>

#include <stdint.h>

// Buffer sizes used by the sample.
#define SEND_BUFFER_SIZE 4096
#define RECV_BUFFER_SIZE 4096
// Must be >= az_mqtt_transport_sizeof() at runtime.
// Schannel transport on Windows is large due to TLS I/O buffers.
#define TRANSPORT_BUFFER_SIZE (256 * 1024)

// Static buffers (zero allocation).
static uint8_t s_send_buffer[SEND_BUFFER_SIZE];
static uint8_t s_recv_buffer[RECV_BUFFER_SIZE];
// Aligned for the transport's pointer and 64-bit members.
static union
{
  uint8_t bytes[TRANSPORT_BUFFER_SIZE];
  void* align_pointer;
  int64_t align_int64;
  double align_double;
} s_transport_buffer;

#endif // AZ_MQTT_SAMPLE_CONFIG_H
