// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* A stand-in for the Eclipse Paho MQTTAsync API.
 *
 * az_iot_mqtt_paho.c is compiled straight into the connect-cleanup test and
 * linked against this instead of the real library. The reason is that the
 * failures under test -- MQTTAsync_createWithOptions, MQTTAsync_setCallbacks,
 * MQTTAsync_setDisconnected or MQTTAsync_connect returning an error after the
 * adapter has already taken custody of a private key -- cannot be provoked
 * through the real library from the adapter's own connect(): it validates its
 * inputs before it gets there. Choosing which call fails is the only thing this
 * mock exists for; it models no MQTT behaviour whatsoever.
 *
 * It also keeps the suite hermetic: the real MQTTAsync_connect starts a
 * background thread and a socket, neither of which a leak test wants. */
#ifndef AZ_IOT_TEST_MOCK_PAHO_ASYNC_H
#define AZ_IOT_TEST_MOCK_PAHO_ASYNC_H

#include <MQTTAsync.h>

#include <stdbool.h>

/* Back to "every call succeeds", counters zeroed. */
void mock_paho_reset(void);

/* Result of each hook the adapter calls, in the order connect() calls them.
 * MQTTASYNC_SUCCESS -- the reset default -- lets the call through. */
void mock_paho_set_create_rc(int rc);
void mock_paho_set_set_callbacks_rc(int rc);
void mock_paho_set_set_disconnected_rc(int rc);
void mock_paho_set_connect_rc(int rc);

/* Result of MQTTAsync_disconnect, and what it does with the completion
 * callbacks the adapter supplies.
 *
 * MQTTASYNC_SUCCESS (the reset default) accepts the call and invokes
 * onSuccess, which is what the real client does for a disconnect it completes.
 * mock_paho_set_disconnect_completion(false) accepts the call but invokes
 * onFailure instead. A non-success rc from mock_paho_set_disconnect_rc()
 * refuses the call outright and invokes neither, which is the one case the
 * adapter has to report for itself. */
void mock_paho_set_disconnect_rc(int rc);
void mock_paho_set_disconnect_completion(bool success);

int mock_paho_disconnect_calls(void);

/* Did the adapter supply completion callbacks and a context at all? Without
 * them the real client reports a client-initiated disconnect to nobody. */
bool mock_paho_disconnect_had_callbacks(void);

int mock_paho_create_calls(void);
int mock_paho_connect_calls(void);
int mock_paho_destroy_calls(void);

/* What the adapter asked for on the last MQTTAsync_connect. Valid once
 * mock_paho_connect_calls() is non-zero. */

/* The serverURI the adapter built, so a test can tell ssl:// from tcp://. */
const char* mock_paho_last_server_uri(void);

/* Were SSL options attached to the connect at all? */
bool mock_paho_last_connect_had_ssl(void);

/* MQTTAsync_SSLOptions::enableServerCertAuth (chain) and ::verify (hostname)
 * as the adapter set them. -1 when no SSL options were attached. */
int mock_paho_last_enable_server_cert_auth(void);
int mock_paho_last_verify(void);

/* MQTTAsync_SSLOptions::privateKey, i.e. the path handed to the TLS stack. */
const char* mock_paho_last_private_key(void);

#endif /* AZ_IOT_TEST_MOCK_PAHO_ASYNC_H */
