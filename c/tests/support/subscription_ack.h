// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Driving the CONNECTED subscription gate from a test.
 *
 * Kept separate from connection_test_harness.h on purpose: that header pulls in
 * internal/reconnect.h, which is only on the include path for the connection
 * suites. The feature suites (twin, c2d, direct method, software updates) need this helper
 * and nothing else from it.
 *
 * Include AFTER <cmocka.h>.
 */
#ifndef AZ_IOT_TEST_SUBSCRIPTION_ACK_H
#define AZ_IOT_TEST_SUBSCRIPTION_ACK_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"

#include "support/mock_mqtt_iface.h"

/* Acknowledge every SUBSCRIBE the client has issued so far.
 *
 * CONNECTED is gated on the persistent subscriptions being SUBACKed, so a test
 * that injects only a CONNACK never reaches CONNECTED and every subsequent call
 * fails with AZ_IOT_ERR_NOT_CONNECTED.
 *
 * The mock delivers at most one queued event per process_loop, so each ack gets
 * its own do_work rather than queueing them all and pumping once. */
static inline void az_iot_test_ack_subscriptions(
    az_iot_connection_client* client,
    az_iot_mock_mqtt_client* m)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = 0; i < n; ++i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(m, i);
    if (!call || call->kind != AZ_IOT_MOCK_CALL_SUBSCRIBE)
    {
      continue;
    }
    uint16_t packet_id = call->packet_id;
    (void)az_iot_mock_mqtt_client_inject_suback(m, packet_id, AZ_IOT_OK);
    (void)az_iot_connection_client_do_work(client, 0);
  }
}

#endif /* AZ_IOT_TEST_SUBSCRIPTION_ACK_H */
