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
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
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

/* A username with no password is still an identity Paho can send; the password
 * is simply omitted rather than the whole credential being dropped. */
static void a_proxy_username_without_a_password_is_kept(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mqtt_connect_options opts;
  base_options(&opts);
  opts.proxy.host = "proxy.corp.example";
  opts.proxy.port = 3128;
  opts.proxy.username = "device";

  assert_int_equal(do_connect(fx, &opts), AZ_IOT_OK);
  assert_string_equal(mock_paho_last_http_proxy(), "device@proxy.corp.example:3128");
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

int main(void)
{
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
    cmocka_unit_test_setup_teardown(a_proxy_username_without_a_password_is_kept, setup, teardown),
    cmocka_unit_test_setup_teardown(an_empty_proxy_host_means_no_proxy, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_later_connect_does_not_inherit_the_previous_proxy, setup, teardown),
    cmocka_unit_test_setup_teardown(websockets_and_a_proxy_combine, setup, teardown),
  };
  return cmocka_run_group_tests_name("paho_transport_proxy", tests, NULL, NULL);
}
