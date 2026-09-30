// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file test_client_session.c
 * @brief Session lifecycle: keep-alive, teardown and callbacks, against an in-process peer.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>


#include "az_mqtt_test_api.h"

#include "test_server.h"

/** @brief Deliberately discard a result (gcc ignores a (void) cast on warn_unused_result). */
static void _ignore(az_result rc) { (void)rc; }

#define ARRAY_SPAN(a) az_span_create((uint8_t*)(a), (int32_t)sizeof(a))

typedef struct
{
  AZ_MQTT_T(client) client;
  az_mqtt_transport* transport;
  test_server* server;
  uint8_t send_buf[1024];
  uint8_t recv_buf[1024];
#if AZ_MQTT_TEST_VERSION == 5
  AZ_MQTT_T(user_property) props[4][4];
  int32_t sub_ids[4];
  AZ_MQTT_T(reason_code) reasons[4];
#endif
} fixture;

static struct
{
  int connacks;
  int connack_reason;
  int subacks;
  int32_t suback_count;
  int publishes;
  int32_t publish_user_properties;
  int32_t publish_subscription_identifiers;
  int disconnects;
  int closed;
  az_result closed_reason;
  int reconnects_left;
  az_result reconnect_rc;
} g;

static void _on_connack(AZ_MQTT_T(client)* c, AZ_MQTT_T(connack_data) const* d)
{
  (void)c;
  g.connacks++;
  g.connack_reason = AZ_MQTT_TEST_CONNACK_CODE(d);
}

static void _on_suback(AZ_MQTT_T(client)* c, AZ_MQTT_T(suback_data) const* d)
{
  (void)c;
  g.subacks++;
#if AZ_MQTT_TEST_VERSION == 5
  g.suback_count = d->reason_code_count;
  for (int32_t i = 0; i < d->reason_code_count; i++)
  {
    assert_int_equal(d->reason_codes[i], 0);
  }
#else
  g.suback_count = az_span_size(d->return_codes);
  for (int32_t i = 0; i < g.suback_count; i++)
  {
    assert_int_equal(az_span_ptr(d->return_codes)[i], AZ_MQTT3_SUBACK_GRANTED_QOS_0);
  }
#endif
}

static void _on_publish(AZ_MQTT_T(client)* c, AZ_MQTT_T(publish_data) const* p)
{
  (void)c;
  g.publishes++;
#if AZ_MQTT_TEST_VERSION == 5
  g.publish_user_properties = p->user_property_count;
  g.publish_subscription_identifiers = p->subscription_identifier_count;
#else
  (void)p;
#endif
}

#if AZ_MQTT_TEST_VERSION == 5
static void _on_disconnect(AZ_MQTT_T(client)* c, AZ_MQTT_T(disconnect_data) const* d)
{
  (void)c;
  (void)d;
  g.disconnects++;
}
#endif

static void _on_closed(AZ_MQTT_T(client)* c, az_result reason)
{
  // The transport is already closed: the client must look disconnected.
  assert_int_equal(AZ_MQTT_T(client_get_state)(c), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  g.closed++;
  g.closed_reason = reason;
  if (g.reconnects_left > 0)
  {
    g.reconnects_left--;
    g.reconnect_rc = AZ_MQTT_T(client_connect)(c, 3000);
  }
}

static int64_t _now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void _sleep_ms(int ms)
{
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

static void _setup(fixture* f, test_server_options const* so, uint16_t keep_alive_s)
{
  memset(f, 0, sizeof(*f));
  memset(&g, 0, sizeof(g));
  f->server = test_server_start(so);
  assert_non_null(f->server);
  f->transport = (az_mqtt_transport*)calloc(1, (size_t)az_mqtt_transport_sizeof());
  assert_non_null(f->transport);
  assert_int_equal(az_mqtt_transport_init(f->transport), AZ_OK);

  AZ_MQTT_T(client_options) o;
  memset(&o, 0, sizeof(o));
  o.transport = f->transport;
  o.send_buffer = ARRAY_SPAN(f->send_buf);
  o.receive_buffer = ARRAY_SPAN(f->recv_buf);
  o.connect_options = AZ_MQTT_T(connect_options_default)();
  o.connect_options.client_id = AZ_SPAN_FROM_STR("session-test");
  o.connect_options.keep_alive_seconds = keep_alive_s;
  o.hostname = AZ_SPAN_FROM_STR("127.0.0.1");
  o.port = test_server_port(f->server);
  o.on_connack = _on_connack;
  o.on_publish = _on_publish;
  o.on_suback = _on_suback;
  o.on_connection_closed = _on_closed;
#if AZ_MQTT_TEST_VERSION == 5
  o.on_disconnect = _on_disconnect;
  o.buffers.connack_user_properties = ARRAY_SPAN(f->props[0]);
  o.buffers.publish_user_properties = ARRAY_SPAN(f->props[1]);
  o.buffers.publish_subscription_identifiers = ARRAY_SPAN(f->sub_ids);
  o.buffers.suback_reason_codes = ARRAY_SPAN(f->reasons);
  o.buffers.suback_user_properties = ARRAY_SPAN(f->props[2]);
  o.buffers.ack_user_properties = ARRAY_SPAN(f->props[3]);
  o.buffers.disconnect_user_properties = ARRAY_SPAN(f->props[3]);
#endif
  assert_int_equal(AZ_MQTT_T(client_init)(&f->client, &o), AZ_OK);
}

static void _teardown(fixture* f)
{
  _ignore(AZ_MQTT_TEST_DISCONNECT(&f->client));
  free(f->transport);
  test_server_stop(f->server);
}

static test_server_options _plain(void)
{
  test_server_options o = test_server_options_default();
  o.tls = false;
  return o;
}

/** @brief Pump until disconnected or @p budget_ms; returns the last process_loop result. */
static az_result _pump_until_closed(fixture* f, int budget_ms)
{
  az_result rc = AZ_OK;
  int64_t const end = _now_ms() + budget_ms;
  while (AZ_MQTT_T(client_get_state)(&f->client) != AZ_MQTT_CLIENT_STATE_DISCONNECTED
         && _now_ms() < end)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f->client, 100);
  }
  return rc;
}

static void a_long_process_loop_wait_still_pings_on_time(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);

  // One call asked to wait 5 s must return at the 1 s keep-alive, having pinged.
  int64_t t0 = _now_ms();
  assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 5000), AZ_OK);
  int64_t elapsed = _now_ms() - t0;
  assert_true(elapsed >= 900 && elapsed < 2500);
  for (int i = 0; i < 20 && test_server_pingreqs(f.server) == 0; i++)
  {
    _ignore(AZ_MQTT_T(client_process_loop)(&f.client, 50));
  }
  assert_int_equal(test_server_pingreqs(f.server), 1);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
  _teardown(&f);
}

static void an_idle_session_with_answered_pings_stays_up(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  int64_t const end = _now_ms() + 3500;
  while (_now_ms() < end)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 100), AZ_OK);
  }
  assert_true(test_server_pingreqs(f.server) >= 2);
  assert_int_equal(g.closed, 0);
  _teardown(&f);
}

static void a_missing_pingresp_ends_the_session(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.no_pingresp = true;
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);

  int64_t t0 = _now_ms();
  assert_int_equal(_pump_until_closed(&f, 5000), AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT);
  // Ping at 1 s, give up 1 s later.
  assert_true(_now_ms() - t0 < 3000);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT);
  assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 0), AZ_MQTT_ERROR_NOT_CONNECTED);
  _teardown(&f);
}

static void server_keep_alive_overrides_the_clients(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.server_keep_alive = 1;
  fixture f;
  _setup(&f, &so, 60);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  int64_t const end = _now_ms() + 2500;
  while (_now_ms() < end)
  {
    _ignore(AZ_MQTT_T(client_process_loop)(&f.client, 100));
  }
#if AZ_MQTT_TEST_VERSION == 5
  assert_true(test_server_pingreqs(f.server) >= 1);
  assert_int_equal(f.client._internal.core._internal.keep_alive_seconds, 1);
#else
  // MQTT 3.1.1 has no Server Keep Alive: the client's 60 s stands.
  assert_int_equal(test_server_pingreqs(f.server), 0);
  assert_int_equal(f.client._internal.core._internal.keep_alive_seconds, 60);
#endif
  _teardown(&f);
}

static void a_server_disconnect_closes_the_connection(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.behavior = TEST_SERVER_DISCONNECT_AFTER_CONNACK;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  (void)_pump_until_closed(&f, 3000);

  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  assert_int_equal(g.closed, 1);
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(g.disconnects, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_SERVER_DISCONNECTED);
#else
  assert_true(az_result_failed(g.closed_reason));
#endif
  _teardown(&f);
}

static void one_process_loop_drains_a_burst(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.burst_publishes = 10;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  _sleep_ms(200); // Let the burst arrive; one call must then handle all of it.
  assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 1000), AZ_OK);
  assert_int_equal(g.publishes, 10);
  _teardown(&f);
}

static void a_local_disconnect_reports_closed_once(void** state)
{
  (void)state;
  test_server_options so = _plain();
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(
      AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  assert_int_equal(
      AZ_MQTT_TEST_DISCONNECT(&f.client), AZ_OK);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_OK);
  for (int i = 0; i < 40 && !test_server_client_closed(f.server); i++)
  {
    _sleep_ms(25);
  }
  assert_true(test_server_client_closed(f.server));
  _teardown(&f);
}

static void connect_to_a_silent_peer_times_out(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.behavior = TEST_SERVER_SILENT;
  fixture f;
  _setup(&f, &so, 30);
  int64_t t0 = _now_ms();
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 500), AZ_MQTT_ERROR_TIMEOUT);
  assert_true(_now_ms() - t0 < 1500);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_TIMEOUT);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_DISCONNECTED);
  _teardown(&f);
}

static void reconnect_after_a_lost_session_works(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.no_pingresp = true;
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(_pump_until_closed(&f, 5000), AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  assert_int_equal(g.connacks, 2);
  _teardown(&f);
}

static void a_refused_connack_code_is_reported_verbatim(void** state)
{
  (void)state;
  test_server_options so = _plain();
  // "Not authorized": MQTT 5 reason 0x87, MQTT 3.1.1 return code 5.
  so.connack_code = AZ_MQTT_TEST_VERSION == 5 ? 0x87 : 0x05;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_MQTT_ERROR_NOT_CONNECTED);
  assert_int_equal(g.connacks, 1);
  assert_int_equal(g.connack_reason, so.connack_code);
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_NOT_CONNECTED);
  _teardown(&f);
}

static void suback_reason_codes_never_exceed_the_buffer(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.suback_codes = 10; // The fixture holds 4.
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  AZ_MQTT_T(subscription) sub;
  memset(&sub, 0, sizeof(sub));
  sub.topic_filter = AZ_SPAN_FROM_STR("t");
  assert_int_equal(AZ_MQTT_T(client_subscribe)(&f.client, &sub, 1, NULL), AZ_OK);
  for (int i = 0; i < 20 && g.subacks == 0; i++)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 50), AZ_OK);
  }
  assert_int_equal(g.subacks, 1);
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(g.suback_count, 4);
#else
  assert_int_equal(g.suback_count, 10); // A view of the packet: no buffer to overflow.
#endif
  _teardown(&f);
}

#if AZ_MQTT_TEST_VERSION == 5
static void publish_properties_never_exceed_the_buffers(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.publish_properties = 10; // The fixture holds 4 of each.
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  for (int i = 0; i < 20 && g.publishes == 0; i++)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 50), AZ_OK);
  }
  assert_int_equal(g.publishes, 1);
  assert_int_equal(g.publish_user_properties, 4);
  assert_int_equal(g.publish_subscription_identifiers, 4);
  assert_int_equal(f.sub_ids[3], 4);
  // The arrays that follow the publish buffers are untouched.
  assert_null(az_span_ptr(f.props[2][0].key));
  assert_int_equal(f.reasons[0], 0);
  _teardown(&f);
}
#endif

static void an_auth_packet_is_a_protocol_error_only_in_mqttv3(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.send_auth = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  az_result rc = AZ_OK;
  for (int i = 0; i < 10 && rc == AZ_OK; i++)
  {
    rc = AZ_MQTT_T(client_process_loop)(&f.client, 50);
  }
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(rc, AZ_OK);
  assert_int_equal(AZ_MQTT_T(client_get_state)(&f.client), AZ_MQTT_CLIENT_STATE_CONNECTED);
#else
  assert_int_equal(rc, AZ_MQTT_ERROR_PROTOCOL);
#endif
  _teardown(&f);
}

static void a_server_disconnect_is_a_protocol_error_only_in_mqttv3(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.send_empty_disconnect = true;
  fixture f;
  _setup(&f, &so, 30);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  (void)_pump_until_closed(&f, 3000);
  assert_int_equal(g.closed, 1);
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_SERVER_DISCONNECTED);
#else
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_PROTOCOL);
#endif
  _teardown(&f);
}

static void an_explicit_server_keep_alive_of_zero_disables_pings(void** state)
{
  (void)state;
  test_server_options so = _plain();
  so.server_keep_alive = 0;
  so.server_keep_alive_present = true;
  fixture f;
  _setup(&f, &so, 1);
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);
  int64_t const end = _now_ms() + 2500;
  while (_now_ms() < end)
  {
    assert_int_equal(AZ_MQTT_T(client_process_loop)(&f.client, 100), AZ_OK);
  }
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(f.client._internal.core._internal.keep_alive_seconds, 0);
  assert_int_equal(test_server_pingreqs(f.server), 0);
#else
  // No such property in MQTT 3.1.1: the client's 1 s stands.
  assert_int_equal(f.client._internal.core._internal.keep_alive_seconds, 1);
  assert_true(test_server_pingreqs(f.server) >= 1);
#endif
  _teardown(&f);
}

static void reconnecting_from_on_connection_closed_is_safe(void** state)
{
  (void)state;
  // Each session gets CONNACK then DISCONNECT (MQTT 5) or a close (3.1.1).
  test_server_options so = _plain();
  so.behavior = TEST_SERVER_DISCONNECT_AFTER_CONNACK;
  fixture f;
  _setup(&f, &so, 30);
  g.reconnects_left = 1;
  assert_int_equal(AZ_MQTT_T(client_connect)(&f.client, 3000), AZ_OK);

  // First end: the callback reconnects synchronously.
  for (int i = 0; i < 30 && g.closed == 0; i++)
  {
    _ignore(AZ_MQTT_T(client_process_loop)(&f.client, 100));
  }
  assert_int_equal(g.closed, 1);
  assert_int_equal(g.reconnect_rc, AZ_OK);
  assert_int_equal(g.connacks, 2);

  // The new session must still see its own end, intact.
  (void)_pump_until_closed(&f, 3000);
  assert_int_equal(g.closed, 2);
#if AZ_MQTT_TEST_VERSION == 5
  assert_int_equal(g.disconnects, 2);
  assert_int_equal(g.closed_reason, AZ_MQTT_ERROR_SERVER_DISCONNECTED);
#endif
  _teardown(&f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_long_process_loop_wait_still_pings_on_time),
    cmocka_unit_test(an_idle_session_with_answered_pings_stays_up),
    cmocka_unit_test(a_missing_pingresp_ends_the_session),
    cmocka_unit_test(server_keep_alive_overrides_the_clients),
    cmocka_unit_test(a_server_disconnect_closes_the_connection),
    cmocka_unit_test(one_process_loop_drains_a_burst),
    cmocka_unit_test(a_local_disconnect_reports_closed_once),
    cmocka_unit_test(connect_to_a_silent_peer_times_out),
    cmocka_unit_test(reconnect_after_a_lost_session_works),
    cmocka_unit_test(a_refused_connack_code_is_reported_verbatim),
    cmocka_unit_test(suback_reason_codes_never_exceed_the_buffer),
#if AZ_MQTT_TEST_VERSION == 5
    cmocka_unit_test(publish_properties_never_exceed_the_buffers),
#endif
    cmocka_unit_test(an_auth_packet_is_a_protocol_error_only_in_mqttv3),
    cmocka_unit_test(a_server_disconnect_is_a_protocol_error_only_in_mqttv3),
    cmocka_unit_test(an_explicit_server_keep_alive_of_zero_disables_pings),
    cmocka_unit_test(reconnecting_from_on_connection_closed_is_safe),
  };
  return cmocka_run_group_tests_name("client_session", tests, NULL, NULL);
}
