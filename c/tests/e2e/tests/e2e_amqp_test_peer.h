// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file e2e_amqp_test_peer.h
 * @brief An in-memory AMQP transport for unit tests: the test plays the peer by queuing frames
 * to the client and reading the bytes the client wrote. No network is used.
 */
#ifndef E2E_AMQP_TEST_PEER_H
#define E2E_AMQP_TEST_PEER_H

#include <stddef.h>
#include <stdint.h>

#include <azure/az_amqp.h>

#define E2E_AMQP_TEST_PEER_FRAME_TYPE_AMQP 0
#define E2E_AMQP_TEST_PEER_FRAME_TYPE_SASL 1
#define E2E_AMQP_TEST_PEER_PROTOCOL_ID_AMQP 0
#define E2E_AMQP_TEST_PEER_PROTOCOL_ID_SASL 3

/** @brief The bytes exchanged with the client, and the peer's hooks. */
typedef struct e2e_amqp_test_peer
{
  uint8_t to_client[65536];
  size_t to_client_length;
  size_t to_client_position;
  uint8_t from_client[65536];
  size_t from_client_length;
  /** @brief Optional: called before each client read, so the peer can answer what it wrote. */
  void (*answer)(struct e2e_amqp_test_peer* peer);
  /** @brief Optional: called when the client closes the transport. */
  void (*on_close)(struct e2e_amqp_test_peer* peer);
} e2e_amqp_test_peer;

/** @brief Transport callbacks over an #e2e_amqp_test_peer, the transport's impl pointer. */
extern const az_amqp_transport_vtable e2e_amqp_test_peer_transport_vtable;

/** @brief Queues an 8-byte protocol header ("AMQP", @p protocol_id, 1, 0, 0) to the client. */
void e2e_amqp_test_peer_send_header(e2e_amqp_test_peer* peer, uint8_t protocol_id);

/** @brief Queues one frame on channel 0: an encoded performative and an optional payload. */
void e2e_amqp_test_peer_send_frame(
    e2e_amqp_test_peer* peer,
    uint8_t frame_type,
    az_span performative,
    az_span payload);

#endif /* E2E_AMQP_TEST_PEER_H */
