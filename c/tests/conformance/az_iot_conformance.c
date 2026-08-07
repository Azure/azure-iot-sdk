// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Shared implementation of the azure-iot-sdk MQTT iface conformance suite.
 *
 * This translation unit knows nothing about any specific MQTT adapter. It
 * exercises the public iface vtable end-to-end against a real broker and is
 * therefore the basis for both:
 *   - CI verification of the bundled Paho adapter
 *   - Customer self-validation of any MQTT client+adapter they want to plug in
 */
#include "az_iot_conformance.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#if defined(_WIN32)
#include <windows.h>
static void conf_sleep_ms(unsigned ms) { Sleep(ms); }
static unsigned long conf_now_ms(void) { return (unsigned long)GetTickCount64(); }
#else
#include <time.h>
static void conf_sleep_ms(unsigned ms)
{
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}
static unsigned long conf_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
}
#endif

/* ------------------------------------------------------------------------- */
/* shared state for the suite (set by az_iot_conformance_run)            */
/* ------------------------------------------------------------------------- */

static az_iot_mqtt_factory* g_factory = NULL;
static const char* g_host = "localhost";
static uint16_t g_port = 1883;
/* Optional TLS endpoint for the server-certificate-validation test. When
 * g_tls_port == 0 that test is skipped (no TLS broker configured). */
static const char* g_tls_host = "localhost";
static uint16_t g_tls_port = 0;
static const unsigned k_step_timeout_ms = 5000;
/* Same budget as k_step_timeout_ms, on the scale the connect option uses. */
static const unsigned k_step_timeout_seconds = 5;

/* ------------------------------------------------------------------------- */
/* event recorder used by every test                                         */
/* ------------------------------------------------------------------------- */

#define CONF_TOPIC_MAX 256
#define CONF_PAYLOAD_MAX 1024
#define CONF_EVENTS_MAX 16

typedef struct conf_recorder
{
  size_t count;
  az_iot_mqtt_event_kind kinds[CONF_EVENTS_MAX];
  az_iot_result statuses[CONF_EVENTS_MAX];
  uint16_t packet_ids[CONF_EVENTS_MAX];
  char topics[CONF_EVENTS_MAX][CONF_TOPIC_MAX];
  uint8_t payloads[CONF_EVENTS_MAX][CONF_PAYLOAD_MAX];
  size_t payload_lens[CONF_EVENTS_MAX];
} conf_recorder;

static void on_event(const az_iot_mqtt_event* evt, void* ctx)
{
  conf_recorder* r = (conf_recorder*)ctx;
  if (r->count >= CONF_EVENTS_MAX)
    return;
  size_t i = r->count++;
  r->kinds[i] = evt->kind;
  r->statuses[i] = evt->status;
  r->packet_ids[i] = evt->packet_id;
  if (evt->message)
  {
    if (evt->message->topic)
    {
      size_t n = strlen(evt->message->topic);
      if (n >= CONF_TOPIC_MAX)
        n = CONF_TOPIC_MAX - 1;
      memcpy(r->topics[i], evt->message->topic, n);
      r->topics[i][n] = '\0';
    }
    size_t plen = evt->message->payload_len;
    if (plen > CONF_PAYLOAD_MAX)
      plen = CONF_PAYLOAD_MAX;
    if (plen)
      memcpy(r->payloads[i], evt->message->payload, plen);
    r->payload_lens[i] = plen;
  }
}

/* Pump process_loop() until either `predicate(recorder)` is true or the
 * timeout elapses. Returns true if the predicate became true. */
typedef int (*conf_predicate_callback)(const conf_recorder*);

static int wait_until(
    az_iot_mqtt_client* c,
    const conf_recorder* r,
    conf_predicate_callback p,
    unsigned timeout_ms)
{
  unsigned long deadline = conf_now_ms() + timeout_ms;
  while (conf_now_ms() < deadline)
  {
    c->iface->process_loop(c, 50);
    if (p(r))
      return 1;
    conf_sleep_ms(10);
  }
  return p(r);
}

static int saw_connected_ok(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_CONNECTED && r->statuses[i] == AZ_IOT_OK)
      return 1;
  }
  return 0;
}

/* A failed/aborted connection: a CONNECTED event carrying an error status, or a
 * DISCONNECTED/ERROR event. Used by the server-cert-validation test to confirm
 * the TLS handshake was actively rejected. */
static int saw_connect_failure(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    az_iot_mqtt_event_kind k = r->kinds[i];
    if ((k == AZ_IOT_MQTT_EVT_CONNECTED && r->statuses[i] != AZ_IOT_OK)
        || k == AZ_IOT_MQTT_EVT_DISCONNECTED || k == AZ_IOT_MQTT_EVT_ERROR)
    {
      return 1;
    }
  }
  return 0;
}

static int saw_subscribe_ack_ok(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK && r->statuses[i] == AZ_IOT_OK)
      return 1;
  }
  return 0;
}

static int saw_message(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_MESSAGE)
      return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */
/* ------------------------------------------------------------------------- */

static void unique_client_id(char* buf, size_t cap, const char* prefix)
{
  /* Cheap uniqueness: prefix + monotonic ticks + pid-ish. */
  snprintf(buf, cap, "%s-%lu", prefix, conf_now_ms());
}

static az_iot_mqtt_client* make_client(void)
{
  az_iot_mqtt_client* c = g_factory->create(g_factory->factory_ctx);
  assert_non_null(c);
  assert_non_null(c->iface);
  return c;
}

static void destroy_client(az_iot_mqtt_client* c)
{
  if (c && c->iface && c->iface->destroy)
    c->iface->destroy(c);
}

static void connect_client(az_iot_mqtt_client* c, conf_recorder* rec, const char* client_id)
{
  c->iface->set_inbound_cb(c, on_event, rec);
  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_host;
  copts.port = g_port;
  copts.client_id = client_id;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, rec, saw_connected_ok, k_step_timeout_ms));
}

/* A self-signed CA that signs nothing the broker presents. Embedded so the
 * server-cert-validation test is self-contained: a correctly-validating client
 * given only this trust anchor MUST reject the broker's real server cert. */
static const char* const k_bogus_ca_pem
    = "-----BEGIN CERTIFICATE-----\n"
      "MIIDGzCCAgOgAwIBAgIUcY/tipk6Ba3njjZHvp/DevhtRDwwDQYJKoZIhvcNAQEL\n"
      "BQAwHTEbMBkGA1UEAwwSYm9ndXMtdW50cnVzdGVkLWNhMB4XDTI2MDcyNzE4MTkz\n"
      "NFoXDTI2MDcyOTE4MTkzNFowHTEbMBkGA1UEAwwSYm9ndXMtdW50cnVzdGVkLWNh\n"
      "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAxld2x/G0sjUSx/4EF352\n"
      "aX4lE5KR73P8ZqX8o7H77H7BDpCZkP08QBmlw16YByEkxjiOXjdgthv8u3stl03v\n"
      "utK0uFaCu8z6DwtHOgrnXiBOlGAbZGDWs34qq1miriMBNPQDEWErVPGJxhyhoad3\n"
      "1TP2yMN5XsUw0YRoXjXSWHQUBAlOytjrYpVfHmzWBgwjFbwzeWkvKlDHMfm6Ciz+\n"
      "LJ0D4wXUc1WbbNl8iJUibBevQ+7wTtAGL0nRBqUfCkfHM+ULa5W+/ikReRy2OTcW\n"
      "40faNCriW+e8DqJ6zV1GobU5di3fgYeJHZ8ad/jGcC+CVt2yD7rJFyVvGvYJAII7\n"
      "jwIDAQABo1MwUTAdBgNVHQ4EFgQUfqVCmOkcsFlXWiMujLCzJfT0+NEwHwYDVR0j\n"
      "BBgwFoAUfqVCmOkcsFlXWiMujLCzJfT0+NEwDwYDVR0TAQH/BAUwAwEB/zANBgkq\n"
      "hkiG9w0BAQsFAAOCAQEAjXnW4IVBfm128GFwYVFHrh/i+Qh+9LKDBcXwm3nj7iTG\n"
      "Re77I+PqzzISg18V/3AwIGBz7gPI2XYJMMnQqmjtHRXSAeDfd/t8iLTmLNg4YShJ\n"
      "nsr+cfyzmQrFoSWqhQp028R9wi+v3w8ybEIBIWNjOXq1U84eTIpR0H8jqOTOk2zP\n"
      "+Svl8I6WTbKnOYnS/XxjlFbWlG0JGfvde4r6XIkvMDb6WZLBTRjZdiduxxYi6gum\n"
      "CruHIV/vz46VyWtvDQq8NKZeGzzF6Kx7WF7quwbQkWGOR5qQDSUmc4j+Uchc0RG6\n"
      "lhlEJdpwfD5tV8wtBqfBebpBF1e4ziC3o3tHjp00vQ==\n"
      "-----END CERTIFICATE-----\n";

/* Write `pem` to a uniquely-named file in the CWD; returns 1 on success and
 * fills `path_out`. The caller removes it when done. */
static int write_temp_pem(const char* pem, char* path_out, size_t cap)
{
  snprintf(path_out, cap, "az_iot_conf_bogus_ca_%lu.pem", conf_now_ms());
  FILE* f = fopen(path_out, "wb");
  if (!f)
    return 0;
  size_t n = strlen(pem);
  size_t w = fwrite(pem, 1, n, f);
  fclose(f);
  return w == n;
}

/* ------------------------------------------------------------------------- */
/* test cases                                                                 */
/* ------------------------------------------------------------------------- */

static void connect_disconnect_roundtrip(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-conn");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_client(c, &rec, cid);

  assert_int_equal(c->iface->disconnect(c), AZ_IOT_OK);
  /* Disconnect ack arrives as DISCONNECTED event; allow up to timeout but
   * don't fail if the broker tears down silently. */
  (void)wait_until(c, &rec, saw_message /* unrelated; just pumps */, 200);
  destroy_client(c);
}

static void publish_subscribe_roundtrip(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-pubsub");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_client(c, &rec, cid);

  /* subscribe + wait for SUBSCRIBE_ACK */
  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_int_not_equal(sub_pid, 0);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

  /* publish */
  static const uint8_t body[] = { 'p', 'i', 'n', 'g' };
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);
  assert_int_not_equal(pub_pid, 0);

  /* expect the message to come back through our own subscription */
  assert_true(wait_until(c, &rec, saw_message, k_step_timeout_ms));

  /* find the matching message and verify topic+payload */
  int found = 0;
  for (size_t i = 0; i < rec.count; ++i)
  {
    if (rec.kinds[i] == AZ_IOT_MQTT_EVT_MESSAGE && strcmp(rec.topics[i], topic) == 0
        && rec.payload_lens[i] == sizeof(body) && memcmp(rec.payloads[i], body, sizeof(body)) == 0)
    {
      found = 1;
      break;
    }
  }
  assert_true(found);

  /* tidy up */
  uint16_t unsub_pid = 0;
  assert_int_equal(c->iface->unsubscribe(c, topic, &unsub_pid), AZ_IOT_OK);
  (void)c->iface->disconnect(c);
  destroy_client(c);
}

static void disconnect_without_connect_is_rejected(void** state)
{
  (void)state;
  az_iot_mqtt_client* c = make_client();
  az_iot_result r = c->iface->disconnect(c);
  /* Either NOT_CONNECTED (preferred) or some adapter-specific MQTT error,
   * but never AZ_IOT_OK on a never-connected client. */
  assert_int_not_equal(r, AZ_IOT_OK);
  destroy_client(c);
}

/* Server-side certificate validation is mandatory: with verify_server = true and
 * a trust anchor that does NOT sign the broker's certificate, the TLS handshake
 * must be rejected and the client must never reach CONNECTED. If validation were
 * disabled, the handshake would succeed and a CONNACK would arrive (CONNECTED
 * ok), failing this test. Requires a TLS endpoint (AZ_IOT_MQTT_BROKER_TLS_PORT);
 * skipped otherwise. */
static void server_cert_validation_rejects_untrusted(void** state)
{
  (void)state;
  if (g_tls_port == 0)
  {
    fprintf(
        stderr,
        "conformance: server-cert-validation test skipped "
        "(set AZ_IOT_MQTT_BROKER_TLS_PORT to a TLS broker)\n");
    skip();
  }

  char ca_path[128];
  assert_true(write_temp_pem(k_bogus_ca_pem, ca_path, sizeof(ca_path)));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-tlsverify");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_tls_host;
  copts.port = g_tls_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  copts.tls.trusted_ca_path = ca_path; /* wrong CA: cannot sign the server cert */
  copts.tls.verify_server = true; /* must validate the server certificate */

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    /* connect() is async; pump until the handshake is rejected. */
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  /* The decisive assertion: an untrusted server cert must NOT yield a
   * successful connection. */
  assert_false(saw_connected_ok(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  remove(ca_path);
}

/* ------------------------------------------------------------------------- */
/* transport failure paths                                                    */
/* ------------------------------------------------------------------------- */

/* Connect to a port nothing is listening on. The adapter must surface the
 * refusal (or at minimum never claim success) rather than hanging: a device
 * that believes it is connected to a closed port never retries. */
static void connect_to_a_closed_port_is_rejected(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-refused");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_host;
  /* Port 1 (tcpmux) is reserved and effectively never bound on CI images. */
  copts.port = 1;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  assert_false(saw_connected_ok(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

/* A name that cannot resolve must fail the same way -- promptly and visibly.
 * .invalid is reserved by RFC 2606 precisely so it can never resolve. */
static void connect_to_an_unresolvable_host_is_rejected(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-dns");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "no-such-broker.invalid";
  copts.port = g_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  assert_false(saw_connected_ok(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

/* A black-holed address (RFC 5737 TEST-NET-3, guaranteed unrouted) never
 * answers. The decisive property is that the adapter does not report a
 * connection it does not have; connect_timeout_seconds bounds how long the
 * caller waits. */
static void connect_to_a_black_holed_address_never_reports_connected(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-timeout");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "203.0.113.1";
  copts.port = 8883;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = 2;

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    /* Give the adapter its own timeout plus slack, then check the invariant. */
    (void)wait_until(c, &rec, saw_connect_failure, 4000);
  }
  assert_false(saw_connected_ok(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

/* ------------------------------------------------------------------------- */
/* session longevity                                                          */
/* ------------------------------------------------------------------------- */

/* With no application traffic for longer than the keep-alive interval, the
 * adapter must keep the session alive on its own (PINGREQ/PINGRESP). If it did
 * not, the broker would drop the client and the round trip below would fail --
 * which is exactly how an idle device silently stops receiving C2D messages. */
static void idle_session_survives_the_keep_alive_interval(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-keepalive");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  char topic[128];
  snprintf(topic, sizeof(topic), "az-iot-conf/%s/keepalive", cid);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_host;
  copts.port = g_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 2;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

  /* Idle well past keep_alive_seconds while still pumping (the adapter needs
   * process_loop() to emit its PINGREQ -- that is the contract). */
  unsigned long idle_deadline = conf_now_ms() + 5000;
  while (conf_now_ms() < idle_deadline)
  {
    c->iface->process_loop(c, 100);
    conf_sleep_ms(50);
  }
  assert_false(saw_connect_failure(&rec));

  /* The session must still carry traffic. */
  rec.count = 0;
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = (const uint8_t*)"alive";
  msg.payload_len = 5;
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_message, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

/* A client instance must be reusable after a clean disconnect: the SDK's own
 * reconnect path builds a fresh adapter, but a BYO adapter that leaks state
 * across sessions would break any caller that reuses one. */
static void connect_after_disconnect_reuses_the_client(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-reuse");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();

  connect_client(c, &rec, cid);
  assert_int_equal(c->iface->disconnect(c), AZ_IOT_OK);
  conf_sleep_ms(200);
  (void)c->iface->process_loop(c, 100);

  rec.count = 0;
  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_host;
  copts.port = g_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

/* ------------------------------------------------------------------------- */
/* entry point                                                                */
/* ------------------------------------------------------------------------- */

static int env_truthy(const char* v)
{
  if (!v || !*v)
    return 0;
  return (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y');
}

int az_iot_conformance_run(az_iot_conformance_suite suite_kind, az_iot_mqtt_factory* factory)
{
  if (!factory)
    return 1;

  /* Validate the factory's advertised version matches the requested suite. */
  az_iot_mqtt_version want = (suite_kind == AZ_IOT_CONFORMANCE_SUITE_V5)
      ? AZ_IOT_MQTT_VERSION_5
      : AZ_IOT_MQTT_VERSION_3_1_1;
  if (factory->version != want)
  {
    fprintf(
        stderr,
        "conformance: factory version mismatch (got %d, want %d)\n",
        (int)factory->version,
        (int)want);
    return 1;
  }

  /* Resolve broker config from env. */
  const char* host = getenv("AZ_IOT_MQTT_BROKER_HOST");
  const char* port = getenv("AZ_IOT_MQTT_BROKER_PORT");
  const char* skip = getenv("AZ_IOT_MQTT_BROKER_SKIP");

  if (env_truthy(skip) || !host || !*host)
  {
    fprintf(
        stderr,
        "conformance: skipped (set AZ_IOT_MQTT_BROKER_HOST to a reachable broker; "
        "current: host=%s skip=%s)\n",
        host ? host : "(unset)",
        skip ? skip : "(unset)");
    return 77; /* CTest SKIP_RETURN_CODE */
  }

  g_factory = factory;
  g_host = host;
  g_port = port ? (uint16_t)atoi(port) : (uint16_t)1883;

  /* Optional TLS endpoint for the server-cert-validation test. Defaults its
   * host to the plaintext broker host; the test is skipped when no TLS port
   * is provided. */
  {
    const char* tls_host = getenv("AZ_IOT_MQTT_BROKER_TLS_HOST");
    const char* tls_port = getenv("AZ_IOT_MQTT_BROKER_TLS_PORT");
    g_tls_host = (tls_host && *tls_host) ? tls_host : g_host;
    g_tls_port = (tls_port && *tls_port) ? (uint16_t)atoi(tls_port) : 0;
  }

  fprintf(
      stderr,
      "conformance: running %s suite against %s:%u\n",
      (suite_kind == AZ_IOT_CONFORMANCE_SUITE_V5) ? "MQTTv5" : "MQTTv3.1.1",
      g_host,
      (unsigned)g_port);

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(connect_disconnect_roundtrip),
    cmocka_unit_test(publish_subscribe_roundtrip),
    cmocka_unit_test(disconnect_without_connect_is_rejected),
    cmocka_unit_test(connect_after_disconnect_reuses_the_client),
    cmocka_unit_test(connect_to_a_closed_port_is_rejected),
    cmocka_unit_test(connect_to_an_unresolvable_host_is_rejected),
    cmocka_unit_test(connect_to_a_black_holed_address_never_reports_connected),
    cmocka_unit_test(idle_session_survives_the_keep_alive_interval),
    cmocka_unit_test(server_cert_validation_rejects_untrusted),
  };
  int failed = cmocka_run_group_tests(tests, NULL, NULL);
  return (failed == 0) ? 0 : 1;
}
