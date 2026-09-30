// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_server.h
 * @brief In-process TCP/TLS peer for transport tests (POSIX + OpenSSL, test-only).
 *
 * Mints its own CA and server certificate per instance, so certificate
 * validation can be exercised without a broker. Answers an MQTT CONNECT with a
 * success CONNACK of the protocol level the client asked for.
 */
#ifndef AZ_MQTT_TEST_SERVER_H
#define AZ_MQTT_TEST_SERVER_H

#include <stdbool.h>
#include <stdint.h>

/** @brief What the server does once a client connects. */
typedef enum
{
  /** @brief Complete TLS (if enabled), answer CONNECT with CONNACK, then idle. */
  TEST_SERVER_MQTT = 0,
  /** @brief Accept TCP and never read or write (a peer that stops answering). */
  TEST_SERVER_SILENT,
  /** @brief Answer CONNECT, then close the connection. */
  TEST_SERVER_CLOSE_AFTER_CONNACK,
  /** @brief Answer CONNECT, then write only the start of a TLS record (TLS only). */
  TEST_SERVER_PARTIAL_TLS_RECORD,
  /** @brief Answer CONNECT, then never read again (the client's sends back up). */
  TEST_SERVER_STOP_READING,
  /** @brief Answer CONNECT, then send DISCONNECT (0x8B) and close (MQTT 5; 3.1.1 just closes). */
  TEST_SERVER_DISCONNECT_AFTER_CONNACK,
} test_server_behavior;

typedef struct
{
  bool tls;
  /** @brief subjectAltName of the server certificate, e.g. "DNS:localhost,IP:127.0.0.1". */
  char const* san;
  /** @brief Sign the server certificate with a CA the client is not given. */
  bool untrusted_ca;
  /** @brief Server certificate validity ended before the test ran. */
  bool expired;
  /** @brief Require and verify a client certificate issued by the test CA. */
  bool require_client_cert;
  test_server_behavior behavior;
  /** @brief Never answer PINGREQ. */
  bool no_pingresp;
  /** @brief Server Keep Alive to put in an MQTT 5 CONNACK (see server_keep_alive_present). */
  uint16_t server_keep_alive;
  /** @brief Send server_keep_alive even when 0; otherwise 0 means "no property". */
  bool server_keep_alive_present;
  /** @brief QoS 0 PUBLISHes written in one burst right after the CONNACK. */
  int burst_publishes;
  /** @brief CONNACK reason (MQTT 5) or return code (MQTT 3.1.1); 0 accepts. */
  uint8_t connack_code;
  /** @brief Reason codes per SUBACK; 0 answers SUBSCRIBE with one granted QoS 0. */
  int suback_codes;
  /** @brief MQTT 5: after CONNACK, send one PUBLISH with this many user properties and subscription identifiers (max 12). */
  int publish_properties;
  /** @brief After CONNACK, send an empty AUTH packet (valid in MQTT 5, reserved type in 3.1.1). */
  bool send_auth;
  /** @brief After CONNACK, send an empty DISCONNECT (valid in MQTT 5, client-only in 3.1.1). */
  bool send_empty_disconnect;
} test_server_options;

typedef struct test_server test_server;

/** @brief Defaults: TLS on, SAN "DNS:localhost,IP:127.0.0.1", MQTT behavior. */
test_server_options test_server_options_default(void);

/**
 * @brief Start a server on 127.0.0.1 with an ephemeral port.
 * @return NULL on failure.
 */
test_server* test_server_start(test_server_options const* options);

void test_server_stop(test_server* server);

uint16_t test_server_port(test_server const* server);

/** @brief PEM file with the CA the client should trust (always the good CA). */
char const* test_server_ca_path(test_server const* server);

/** @brief PEM files of a client identity issued by the trusted CA. */
char const* test_server_client_cert_path(test_server const* server);
char const* test_server_client_key_path(test_server const* server);

/** @brief TCP connections accepted so far. */
int test_server_accepted(test_server* server);

/** @brief TLS handshakes completed so far (server side). */
int test_server_handshakes(test_server* server);

/** @brief Whether the last completed handshake carried a client certificate. */
bool test_server_saw_client_cert(test_server* server);

/** @brief PINGREQs received so far. */
int test_server_pingreqs(test_server* server);

/** @brief Whether the client closed the last connection (orderly or not). */
bool test_server_client_closed(test_server* server);

#endif // AZ_MQTT_TEST_SERVER_H
