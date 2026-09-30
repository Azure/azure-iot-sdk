// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_socket_posix.h
 * @brief Internal: non-blocking POSIX TCP helpers shared by the POSIX transports.
 *
 * Sockets are non-blocking from creation, never raise SIGPIPE, and every wait is
 * bounded by the caller's timeout.
 */
#ifndef AZ_MQTT_SOCKET_POSIX_H
#define AZ_MQTT_SOCKET_POSIX_H

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

struct addrinfo;

/** @brief TCP connect in progress; walks every resolved address in turn. */
typedef struct
{
  int fd;
  struct addrinfo* addresses;
  struct addrinfo* next;
  /** @brief addresses came from getaddrinfo() and are freed here. */
  bool owns_addresses;
  /** @brief When the current address was tried. */
  int64_t attempt_start_ms;
} _az_mqtt_tcp_connect;

/** @brief Readiness to wait for. */
typedef enum
{
  _AZ_MQTT_WAIT_READ = 1,
  _AZ_MQTT_WAIT_WRITE = 2,
} _az_mqtt_wait;

/** @brief Monotonic milliseconds. */
int64_t _az_mqtt_now_ms(void);

/**
 * @brief Milliseconds left until @p deadline_ms; -1 when @p deadline_ms is negative (no limit).
 */
int32_t _az_mqtt_remaining_ms(int64_t deadline_ms);

/** @brief Deadline @p timeout_ms from now; negative @p timeout_ms means no limit. */
int64_t _az_mqtt_deadline(int32_t timeout_ms);

void _az_mqtt_tcp_connect_init(_az_mqtt_tcp_connect* c);

/**
 * @brief Resolve @p host and start a non-blocking connect to the first address.
 *
 * Name resolution is the one step that blocks (getaddrinfo has no portable
 * non-blocking form).
 */
AZ_NODISCARD az_result
_az_mqtt_tcp_connect_start(_az_mqtt_tcp_connect* c, az_span host, uint16_t port);

/**
 * @brief Start a non-blocking connect over a caller-owned address list (tests;
 * _az_mqtt_tcp_connect_start() uses it after name resolution).
 */
AZ_NODISCARD az_result
_az_mqtt_tcp_connect_start_addresses(_az_mqtt_tcp_connect* c, struct addrinfo* addresses);

/**
 * @brief Wait up to @p timeout_ms for the connect to complete.
 *
 * An address that neither connects nor fails within
 * AZ_MQTT_TRANSPORT_ADDRESS_ATTEMPT_MS is abandoned for the next one, so a
 * black-holed first address (e.g. broken IPv6) does not stall the connect.
 *
 * @retval AZ_OK Connected; c->fd is the socket and now belongs to the caller.
 * @retval AZ_MQTT_ERROR_TIMEOUT Still connecting; call again.
 * @retval AZ_MQTT_ERROR_TRANSPORT Every address failed.
 */
AZ_NODISCARD az_result _az_mqtt_tcp_connect_poll(_az_mqtt_tcp_connect* c, int32_t timeout_ms);

/** @brief Abandon the connect; closes the socket unless it was handed over. */
void _az_mqtt_tcp_connect_cancel(_az_mqtt_tcp_connect* c);

/**
 * @brief Wait until @p fd is ready for @p what.
 * @return 1 ready, 0 timed out, -1 error.
 */
int _az_mqtt_wait_fd(int fd, _az_mqtt_wait what, int32_t timeout_ms);

/**
 * @brief send() that never raises SIGPIPE.
 * @return Bytes sent, 0 if it would block, -1 on error.
 */
int32_t _az_mqtt_send_nosignal(int fd, uint8_t const* data, int32_t size);

/**
 * @brief recv() on a non-blocking socket.
 * @return Bytes read, 0 if it would block, -1 on error or orderly close.
 */
int32_t _az_mqtt_recv_nonblocking(int fd, uint8_t* buffer, int32_t size);

/** @brief Whether the last socket call failed only because it would block. */
bool _az_mqtt_would_block(void);

#endif // AZ_MQTT_SOCKET_POSIX_H
