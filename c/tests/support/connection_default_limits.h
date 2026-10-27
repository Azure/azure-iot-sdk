// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* The default az_iot_connection_client_limits, for tests that fill a table. The connection
 * client test checks these against the defaults init() applies. */
#ifndef AZ_IOT_TEST_CONNECTION_DEFAULT_LIMITS_H
#define AZ_IOT_TEST_CONNECTION_DEFAULT_LIMITS_H

#define AZ_IOT_TEST_DEFAULT_PENDING_PUBLISHES 16
#define AZ_IOT_TEST_DEFAULT_PUBLISH_RESERVATIONS 2
#define AZ_IOT_TEST_DEFAULT_PERSISTENT_SUBSCRIPTIONS 8
#define AZ_IOT_TEST_DEFAULT_SESSION_HANDLERS 4
#define AZ_IOT_TEST_DEFAULT_FEATURE_STATE_OBSERVERS 6
#define AZ_IOT_TEST_DEFAULT_APPLICATION_STATE_OBSERVERS 4
#define AZ_IOT_TEST_DEFAULT_FEATURE_CLIENTS 8
#define AZ_IOT_TEST_DEFAULT_INBOUND_HANDLERS 8

#endif /* AZ_IOT_TEST_CONNECTION_DEFAULT_LIMITS_H */
