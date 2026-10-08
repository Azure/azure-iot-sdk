// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Paho adapter: what a transport selection and a proxy turn into on the wire
 * to Paho.
 *
 * Everything the feature does in this adapter is decided before any socket is
 * opened: which scheme and path the serverURI carries, which port it defaults
 * to, and whether MQTTAsync_connectOptions names a proxy. That makes it exactly
 * assertable against the MQTTAsync mock -- and only against the mock, since the
 * real library would answer these questions by trying to connect.
 *
 * The connect path is driven through the real adapter (az_iot_mqtt_paho.c is
 * compiled into this executable) with the custody module faked out, as in
 * paho_connect_cleanup_test.c.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "fake_paho_key_custody.h"
#include "mock_paho_async.h"

typedef struct
{
  az_iot_mqtt_factory* factory;
  az_iot_mqtt_client* client;
} fixture;

static int setup(void** state)
{
  mock_paho_reset();
  fake_custody_reset();

  fixture* fx = (fixture*)calloc(1, sizeof(fixture));
  assert_non_null(fx);
  fx->factory = az_iot_paho_factory_create_v3_1_1();
  assert_non_null(fx->factory);
  fx->client = fx->factory->create(fx->factory->factory_ctx);
  assert_non_null(fx->client);
  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    if (fx->client)
    {
      fx->client->iface->destroy(fx->client);
    }
    az_iot_paho_factory_destroy(fx->factory);
    free(fx);
  }
  fake_custody_reset();
  return 0;
}

/* A TLS connect with no key custody involved: a CA path is enough to select
 * TLS, and it keeps the fake custody module out of the picture entirely. */
static void base_options(az_iot_mqtt_connect_options* opts)
{
  memset(opts, 0, sizeof(*opts));
  opts->host = "example-hub.azure-devices.net";
  opts->client_id = "ut-device";
  opts->tls.trusted_ca_path = "/dev/null/ca.pem";
}

static az_iot_result do_connect(fixture* fx, az_iot_mqtt_connect_options* opts)
{
  return fx->client->iface->connect(fx->client, opts);
}

/* ------------------------------------------------------------------------- */
/* serverURI: scheme, port and path                                           */
/* ------------------------------------------------------------------------- */

/* The default is the behaviour that predates the transport field, port and
 * all: a zeroed options struct still reaches 8883 over TLS. */
static void tcp_is_the_default_and_keeps_8883(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_server_uri(), "ssl://example-hub.azure-devices.net:8883");
  assert_null(mock_paho_last_http_proxy());
  assert_null(mock_paho_last_https_proxy());
}

/* WebSockets: wss, 443 by default, and the Azure resource path -- a WebSocket
 * connect to the bare host would be answered with an HTTP error, not a
 * CONNACK. */
static void websockets_select_wss_443_and_the_default_path(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(
      mock_paho_last_server_uri(), "wss://example-hub.azure-devices.net:443/$iothub/websocket");
}

static void an_explicit_websocket_path_is_used_verbatim(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  opts.websocket_path = "/mqtt";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_server_uri(), "wss://example-hub.azure-devices.net:443/mqtt");
}

/* An explicit port wins over the transport default, which is what a gateway or
 * a test broker on a non-standard port needs. */
static void an_explicit_port_overrides_the_transport_default(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  opts.port = 8443;

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(
      mock_paho_last_server_uri(), "wss://example-hub.azure-devices.net:8443/$iothub/websocket");
}

/* Without TLS material the scheme drops to ws:// and the default port to 80,
 * matching what tcp:// / 1883 already did for the TCP transport. */
static void a_plaintext_websocket_connect_uses_ws_and_80(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "localhost";
  opts.client_id = "ut-device";
  opts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_server_uri(), "ws://localhost:80/$iothub/websocket");
}

/* ------------------------------------------------------------------------- */
/* proxy                                                                      */
/* ------------------------------------------------------------------------- */

/* Both slots, because Paho reads httpProxy for tcp:// and ws:// and httpsProxy
 * for ssl:// and wss://; setting only one would silently skip the proxy for
 * half the transport matrix. struct_version has to be >= 8 or Paho ignores the
 * fields altogether. */
static void a_proxy_is_set_on_both_slots(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_http_proxy(), "proxy.corp.example:3128");
  assert_string_equal(mock_paho_last_https_proxy(), "proxy.corp.example:3128");
  assert_true(mock_paho_last_connect_struct_version() >= 8);
  /* The proxy is a transport detail: it must not change the broker the session
   * targets, nor disable TLS to it. */
  assert_string_equal(mock_paho_last_server_uri(), "ssl://example-hub.azure-devices.net:8883");
  assert_true(mock_paho_last_connect_had_ssl());
  assert_int_equal(mock_paho_last_enable_server_cert_auth(), 1);
  assert_int_equal(mock_paho_last_verify(), 1);
}

static void a_proxy_without_a_port_defaults_to_8080(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_http_proxy(), "proxy.corp.example:8080");
}

static void proxy_credentials_are_rendered_for_basic_auth(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.username = "device";
  opts.proxy.password = "s3cret";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_http_proxy(), "device:s3cret@proxy.corp.example:3128");
}

/* A username with no password still has to carry the colon: HTTP Basic encodes
 * "user:", and Paho Base64-encodes whatever sits before the '@' verbatim. A bare
 * "user" would authenticate as a user name with no password field at all. */
static void a_proxy_username_without_a_password_keeps_the_colon(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.username = "device";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_http_proxy(), "device:@proxy.corp.example:3128");
}

/* Paho splits the proxy string at the FIRST '@' and percent-decodes what
 * precedes it. An '@' in a password would therefore hand Paho a different host
 * entirely -- the credential must arrive percent-encoded so that what Paho
 * decodes is what was configured. */
static void an_at_sign_in_the_password_is_encoded(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.username = "device";
  opts.proxy.password = "p@ss";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_http_proxy(), "device:p%40ss@proxy.corp.example:3128");
}

/* A literal '%' must be escaped too: Paho decodes %XX, so an unescaped '%'
 * would either be swallowed or -- when the next two characters are not hex
 * digits -- leave its decoder making no progress. */
static void a_percent_in_a_credential_is_encoded(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.username = "de%vice";
  opts.proxy.password = "100%";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_http_proxy(), "de%25vice:100%25@proxy.corp.example:3128");
}

/* An '@' in the user name is encoded on the same rule, so a domain-qualified
 * proxy account works. */
static void an_at_sign_in_the_username_is_encoded(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.username = "device@corp";
  opts.proxy.password = "pw";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_http_proxy(), "device%40corp:pw@proxy.corp.example:3128");
}

/* A colon in the user name has no representation: Basic splits the DECODED
 * credential at its first colon, so percent-encoding would not help. Refuse the
 * connect rather than authenticate as a silently truncated user. */
static void a_colon_in_the_proxy_username_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.username = "dev:ice";
  opts.proxy.password = "pw";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_ERR_INVALID_ARG);
  /* Fail closed: nothing was handed to Paho, so there is no session that could
   * have reached the broker without the proxy. */
  assert_int_equal(mock_paho_connect_calls(), 0);
}

/* A password with no user name is not a credential Basic can carry, and is
 * treated as no credential rather than as an anonymous one. */
static void a_password_without_a_username_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.password = "pw";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_http_proxy(), "proxy.corp.example:3128");
}

/* A transport value this adapter does not implement must be refused, not
 * quietly downgraded to TCP: the caller asked for something else precisely
 * because a direct 8883 session is not available to it. */
static void an_unknown_transport_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.transport = (az_iot_mqtt_transport)99;

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_int_equal(mock_paho_create_calls(), 0);
  assert_int_equal(mock_paho_connect_calls(), 0);
}

/* An empty host is "no proxy", not a proxy called "". The distinction matters
 * because it is what an unset environment variable copied into the options
 * looks like. */
static void an_empty_proxy_host_means_no_proxy(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "";
  opts.proxy.port = 3128;

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_null(mock_paho_last_http_proxy());
  assert_null(mock_paho_last_https_proxy());
}

/* Reconnecting without a proxy must not inherit the previous connect's one,
 * and vice versa: the adapter rebuilds the string on every connect. */
static void a_later_connect_does_not_inherit_the_previous_proxy(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_non_null(mock_paho_last_http_proxy());

  base_options(&opts);
  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_null(mock_paho_last_http_proxy());
  assert_null(mock_paho_last_https_proxy());
}

/* WebSockets and a proxy together -- the case the whole feature exists for:
 * a device that may only talk to 443, and only through the proxy. */
static void websockets_and_a_proxy_combine(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(
      mock_paho_last_server_uri(), "wss://example-hub.azure-devices.net:443/$iothub/websocket");
  assert_string_equal(mock_paho_last_https_proxy(), "proxy.corp.example:3128");
}

/* ------------------------------------------------------------------------- */
/* credential redaction on the trace path                                    */
/* ------------------------------------------------------------------------- */

/* Paho logs the Basic proxy credential -- Base64, therefore reversible -- at
 * TRACE_PROTOCOL, and this adapter forwards Paho's trace messages to the SDK
 * log. Anyone who set AZ_IOT_PAHO_TRACE to debug a connection would otherwise
 * find the proxy user name and password in their application log. */

typedef struct
{
  char last[512];
  int count;
} log_capture;

static log_capture g_log_capture;

static void capturing_sink(
    void* user_ctx,
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* msg)
{
  (void)component;
  (void)user_ctx;
  (void)level;
  (void)file;
  (void)line;
  snprintf(g_log_capture.last, sizeof(g_log_capture.last), "%s", msg ? msg : "");
  g_log_capture.count++;
}

static void the_proxy_auth_trace_line_is_redacted(void** state)
{
  fixture* fx = (fixture*)*state;

  /* The adapter installs its trace callback on the first connect. */
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.username = "device";
  opts.proxy.password = "s3cret";
  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);

  MQTTAsync_traceCallback* trace = mock_paho_trace_callback();
  assert_non_null(trace);

  az_iot_log_sink sink = { capturing_sink, NULL, AZ_IOT_LOG_LEVEL_TRACE };
  az_iot_log_set_global_sink(&sink);

  memset(&g_log_capture, 0, sizeof(g_log_capture));
  /* The exact text Paho 1.3.13 emits, with a Base64 credential. */
  char message[] = "Setting http proxy auth to ZGV2aWNlOnMzY3JldA==";
  trace(MQTTASYNC_TRACE_PROTOCOL, message);

  az_iot_log_set_global_sink(NULL);

  assert_int_equal(g_log_capture.count, 1);
  assert_null(strstr(g_log_capture.last, "ZGV2aWNlOnMzY3JldA=="));
  assert_non_null(strstr(g_log_capture.last, "<redacted>"));
  /* The line is still recognisable, so the trace remains useful. */
  assert_non_null(strstr(g_log_capture.last, "proxy auth to "));
}

/* An ordinary trace line is forwarded unchanged -- redaction must not blank the
 * diagnostics the trace level exists for. */
static void an_unrelated_trace_line_is_forwarded_verbatim(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);

  MQTTAsync_traceCallback* trace = mock_paho_trace_callback();
  assert_non_null(trace);

  az_iot_log_sink sink = { capturing_sink, NULL, AZ_IOT_LOG_LEVEL_TRACE };
  az_iot_log_set_global_sink(&sink);

  memset(&g_log_capture, 0, sizeof(g_log_capture));
  char message[] = "Setting http proxy to proxy.corp.example:3128";
  trace(MQTTASYNC_TRACE_PROTOCOL, message);

  az_iot_log_set_global_sink(NULL);

  assert_int_equal(g_log_capture.count, 1);
  assert_non_null(strstr(g_log_capture.last, "proxy.corp.example:3128"));
  assert_null(strstr(g_log_capture.last, "<redacted>"));
}

int main(void)
{
  /* Before any connect: the adapter installs Paho's trace callback once per
   * process, and only when this is set. The redaction cases need that callback,
   * and the rest are unaffected by a trace level nothing reads. */
#ifdef _WIN32
  _putenv_s("AZ_IOT_PAHO_TRACE", "protocol");
#else
  setenv("AZ_IOT_PAHO_TRACE", "protocol", 1);
#endif
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(tcp_is_the_default_and_keeps_8883, setup, teardown),
    cmocka_unit_test_setup_teardown(
        websockets_select_wss_443_and_the_default_path, setup, teardown),
    cmocka_unit_test_setup_teardown(an_explicit_websocket_path_is_used_verbatim, setup, teardown),
    cmocka_unit_test_setup_teardown(
        an_explicit_port_overrides_the_transport_default, setup, teardown),
    cmocka_unit_test_setup_teardown(a_plaintext_websocket_connect_uses_ws_and_80, setup, teardown),
    cmocka_unit_test_setup_teardown(a_proxy_is_set_on_both_slots, setup, teardown),
    cmocka_unit_test_setup_teardown(a_proxy_without_a_port_defaults_to_8080, setup, teardown),
    cmocka_unit_test_setup_teardown(proxy_credentials_are_rendered_for_basic_auth, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_proxy_username_without_a_password_keeps_the_colon, setup, teardown),
    cmocka_unit_test_setup_teardown(an_at_sign_in_the_password_is_encoded, setup, teardown),
    cmocka_unit_test_setup_teardown(a_percent_in_a_credential_is_encoded, setup, teardown),
    cmocka_unit_test_setup_teardown(an_at_sign_in_the_username_is_encoded, setup, teardown),
    cmocka_unit_test_setup_teardown(a_colon_in_the_proxy_username_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(a_password_without_a_username_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(an_unknown_transport_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(an_empty_proxy_host_means_no_proxy, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_later_connect_does_not_inherit_the_previous_proxy, setup, teardown),
    cmocka_unit_test_setup_teardown(websockets_and_a_proxy_combine, setup, teardown),
    cmocka_unit_test_setup_teardown(the_proxy_auth_trace_line_is_redacted, setup, teardown),
    cmocka_unit_test_setup_teardown(an_unrelated_trace_line_is_forwarded_verbatim, setup, teardown),
  };
  return cmocka_run_group_tests_name("paho_transport_proxy", tests, NULL, NULL);
}
