// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief The OS-wait half of the sample event loop.
 *
 * @details The AMQP client is a non-blocking pump: the samples call #az_amqp_connection_process
 * directly, then call this helper to block until the transport is ready again. This function does
 * NOT call `process`; it only performs the wait the operating system requires (a `select`/`WSAPoll`
 * on the sample transport's socket, bounded by the connection's suggested deadline). Keeping the
 * two steps separate is deliberate — it keeps #az_amqp_connection_process visible as the primary
 * call in every sample loop, and shows exactly where a real integration would substitute its own
 * event loop (epoll, libuv, the ESP-IDF task loop, ...).
 *
 * Without a wait step, a loop that only called #az_amqp_connection_process would spin at 100% CPU,
 * because `process` returns immediately whenever there is no work to do.
 */

#ifndef AZ_AMQP_SAMPLE_WAIT_H
#define AZ_AMQP_SAMPLE_WAIT_H

#include "az_amqp_sample_transport.h"

#include <azure/amqp/az_amqp_common.h>

#include <stdint.h>

/**
 * @brief Blocks until the sample transport is ready for the requested I/O, or until @p
 * timeout_milliseconds elapses.
 *
 * @param[in] transport The sample transport whose socket to wait on.
 * @param[in] io_interest What readiness to wait for (from #az_amqp_connection_process_result).
 * @param[in] timeout_milliseconds The maximum time to block; `-1` waits indefinitely, `0` returns
 * immediately. Pass the connection result's `next_activity_milliseconds`.
 */
void az_amqp_sample_wait_for_io(
    az_amqp_sample_transport* transport,
    az_amqp_io_interest io_interest,
    int32_t timeout_milliseconds);

#endif // AZ_AMQP_SAMPLE_WAIT_H
