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
#include "az_iot_test_proxy.h"

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
  {
    return;
  }
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
      {
        n = CONF_TOPIC_MAX - 1;
      }
      memcpy(r->topics[i], evt->message->topic, n);
      r->topics[i][n] = '\0';
    }
    size_t plen = evt->message->payload_len;
    if (plen > CONF_PAYLOAD_MAX)
    {
      plen = CONF_PAYLOAD_MAX;
    }
    if (plen)
    {
      memcpy(r->payloads[i], evt->message->payload, plen);
    }
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
    {
      return 1;
    }
    conf_sleep_ms(10);
  }
  return p(r);
}

static int saw_connected_ok(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_CONNECTED && r->statuses[i] == AZ_IOT_OK)
    {
      return 1;
    }
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
    {
      return 1;
    }
  }
  return 0;
}

static int saw_message(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_MESSAGE)
    {
      return 1;
    }
  }
  return 0;
}

/* A CONNECTED event carrying an error status (as opposed to a transport-level
 * disconnect): proves a non-zero CONNACK reason code was routed through the
 * adapter's connack mapping rather than swallowed. */
static int saw_connack_error(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_CONNECTED && r->statuses[i] != AZ_IOT_OK)
    {
      return 1;
    }
  }
  return 0;
}

static int saw_disconnected(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_DISCONNECTED)
    {
      return 1;
    }
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
  {
    c->iface->destroy(c);
  }
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

#ifdef AZ_IOT_CONFORMANCE_WITH_TLS
/* Write `pem` to a uniquely-named file in the CWD; returns 1 on success and
 * fills `path_out`. The caller removes it when done.
 *
 * The trust anchor used to be an embedded, pre-generated CA that expired and
 * had to be re-minted by hand. The test proxy now mints one per run, so what
 * gets written here is whatever the proxy just generated -- but the adapter
 * takes a trusted-CA *path*, so it still has to reach the filesystem. */
static int write_temp_pem(const char* pem, char* path_out, size_t cap)
{
  snprintf(path_out, cap, "az_iot_conf_ca_%lu.pem", conf_now_ms());
  FILE* f = fopen(path_out, "wb");
  if (!f)
  {
    return 0;
  }
  size_t n = strlen(pem);
  size_t w = fwrite(pem, 1, n, f);
  fclose(f);
  return w == n;
}
#endif /* AZ_IOT_CONFORMANCE_WITH_TLS */

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

/* --------------------------------------------------------------------------
 * Server-certificate validation.
 *
 * These cases are compiled only when AZ_IOT_CONFORMANCE_WITH_TLS is defined,
 * which the AZ_IOT_BUILD_CONFORMANCE_TESTS_TLS option sets. The original
 * version of this test was compiled unconditionally and called cmocka's skip()
 * when no TLS port was configured -- which is how a mandatory security check
 * sat inside a green suite without ever running. Whether these run is visible
 * in the build configuration, never decided at run time.
 *
 * The TLS peer is the in-process test proxy, which mints its own CA and leaf,
 * so a separately provisioned TLS broker is no longer part of the contract.
 * -------------------------------------------------------------------------- */
#ifdef AZ_IOT_CONFORMANCE_WITH_TLS

/* Positive control for the certificate cases below: a leaf signed by the
 * trusted CA, inside its validity window and matching the connected host, must
 * be ACCEPTED and the client reaches CONNECTED through the TLS-terminating
 * proxy. Without this, a handshake broken for some unrelated reason would make
 * every negative case below pass for the wrong reason. */
static void tls_handshake_succeeds_with_trusted_valid_cert(void** state)
{
  (void)state;

  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  uint16_t proxy_port = 0;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, &proxy_port), 0);

  az_iot_test_proxy_tls_options tls
      = az_iot_test_proxy_tls_options_default(); /* trusted CA, valid window */
  assert_int_equal(az_iot_test_proxy_enable_tls(proxy, &tls), 0);

  char ca_pem[4096];
  assert_true(az_iot_test_proxy_ca_pem(proxy, ca_pem, sizeof(ca_pem)) > 0);
  char ca_path[128];
  assert_true(write_temp_pem(ca_pem, ca_path, sizeof(ca_path)));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-tlsok");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "127.0.0.1";
  copts.port = proxy_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  copts.tls.trusted_ca_path = ca_path;
  copts.tls.verify_server = true;

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
  remove(ca_path);
}

/* Server-side certificate validation is mandatory: with verify_server = true and
 * a leaf the client's trust anchor does NOT sign, the TLS handshake must be
 * rejected and the client must never reach CONNECTED. The proxy terminates TLS
 * with a leaf signed by an UNTRUSTED CA while the client is handed the
 * (different) trusted CA. If validation were disabled the handshake would
 * succeed and a CONNACK would arrive, failing this test. */
static void server_cert_validation_rejects_untrusted(void** state)
{
  (void)state;

  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  uint16_t proxy_port = 0;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, &proxy_port), 0);

  az_iot_test_proxy_tls_options tls = az_iot_test_proxy_tls_options_default();
  tls.sign_with_untrusted_ca = 1; /* leaf signed by a CA the client won't trust */
  assert_int_equal(az_iot_test_proxy_enable_tls(proxy, &tls), 0);

  char ca_pem[4096];
  assert_true(az_iot_test_proxy_ca_pem(proxy, ca_pem, sizeof(ca_pem)) > 0);
  char ca_path[128];
  assert_true(write_temp_pem(ca_pem, ca_path, sizeof(ca_path)));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-tlsuntrusted");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "127.0.0.1"; /* connect to the TLS-terminating proxy */
  copts.port = proxy_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  copts.tls.trusted_ca_path = ca_path; /* trusts the exported CA, not the leaf's signer */
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
  az_iot_test_proxy_stop(proxy);
  remove(ca_path);
}

/* An EXPIRED certificate must be rejected even though it chains to the trusted
 * CA: the proxy presents a leaf the trusted CA signed, whose validity window is
 * entirely in the past. verify_server = true enables chain *and* validity
 * checking, so an adapter that only checks the chain fails here. */
static void server_cert_validation_rejects_expired(void** state)
{
  (void)state;

  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  uint16_t proxy_port = 0;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, &proxy_port), 0);

  az_iot_test_proxy_tls_options tls = az_iot_test_proxy_tls_options_default();
  tls.not_before_offset_sec = -7200; /* valid-from two hours ago */
  tls.not_after_offset_sec = -3600; /* expired one hour ago */
  assert_int_equal(az_iot_test_proxy_enable_tls(proxy, &tls), 0);

  char ca_pem[4096];
  assert_true(az_iot_test_proxy_ca_pem(proxy, ca_pem, sizeof(ca_pem)) > 0);
  char ca_path[128];
  assert_true(write_temp_pem(ca_pem, ca_path, sizeof(ca_path)));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-tlsexpired");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "127.0.0.1";
  copts.port = proxy_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  copts.tls.trusted_ca_path = ca_path;
  copts.tls.verify_server = true;

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  assert_false(saw_connected_ok(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
  remove(ca_path);
}

/* Hostname verification: a certificate that chains to the trusted CA and is
 * inside its validity window must STILL be rejected when its SAN does not name
 * the host the client asked for. The proxy presents a leaf for
 * "DNS:wrong.invalid" while the client connects to 127.0.0.1, so only an
 * adapter that enables the peer-name check -- not merely chain validation --
 * rejects it. This is the impersonation case: a certificate legitimately issued
 * for some other host is otherwise perfectly valid. */
static void server_cert_validation_rejects_hostname_mismatch(void** state)
{
  (void)state;

  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  uint16_t proxy_port = 0;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, &proxy_port), 0);

  az_iot_test_proxy_tls_options tls = az_iot_test_proxy_tls_options_default();
  tls.leaf_san = "DNS:wrong.invalid"; /* trusted + valid, but the wrong host */
  assert_int_equal(az_iot_test_proxy_enable_tls(proxy, &tls), 0);

  char ca_pem[4096];
  assert_true(az_iot_test_proxy_ca_pem(proxy, ca_pem, sizeof(ca_pem)) > 0);
  char ca_path[128];
  assert_true(write_temp_pem(ca_pem, ca_path, sizeof(ca_path)));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-tlshost");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "127.0.0.1"; /* does not match the leaf's SAN */
  copts.port = proxy_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  copts.tls.trusted_ca_path = ca_path;
  copts.tls.verify_server = true;

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  assert_false(saw_connected_ok(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
  remove(ca_path);
}
#endif /* AZ_IOT_CONFORMANCE_WITH_TLS */

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

/* A mid-session network drop must surface as a DISCONNECTED/ERROR event (never
 * be silently swallowed), and the client must reconnect afterwards. An
 * in-process test proxy interposes a raw TCP passthrough between the client and
 * the real broker and resets the connection on demand; because the drop is
 * one-shot, the following reconnect flows through cleanly. Unlike the
 * fake-adapter retry tests, this drives the genuine client + socket stack. */
static void reconnect_after_network_drop(void** state)
{
  (void)state;

  /* Stand up the proxy in front of the real broker. */
  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  uint16_t proxy_port = 0;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, &proxy_port), 0);
  assert_int_not_equal(proxy_port, 0);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-drop");

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "127.0.0.1"; /* connect through the proxy, not the broker */
  copts.port = proxy_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;

  /* First connection, established through the proxy. */
  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  /* Force a mid-session network drop (TCP reset). */
  az_iot_test_proxy_drop_now(proxy);
  assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));

  /* Reconnect through the same (now disarmed) proxy must succeed. Use a fresh
   * recorder so the CONNECTED we observe is the reconnect, not the first one. */
  conf_recorder rec2 = { 0 };
  c->iface->set_inbound_cb(c, on_event, &rec2);
  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec2, saw_connected_ok, k_step_timeout_ms));

  /* The proxy must have accepted two distinct client connections. */
  assert_true(az_iot_test_proxy_connections(proxy) >= 2);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* A CONNACK carrying a non-zero reason code must be routed to a CONNECTED event
 * with an error status (via az_iot_mqtt_connack_result), not swallowed. The proxy
 * acts as a synthetic broker and answers the client's CONNECT with a
 * "not authorized" CONNACK built for the client's MQTT version. */
static void connect_connack_error_is_reported(void** state)
{
  (void)state;

  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  uint16_t proxy_port = 0;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, &proxy_port), 0);

  uint8_t connack[8];
  size_t connack_len;
  if (g_factory->version == AZ_IOT_MQTT_VERSION_5)
  {
    /* session-present=0, reason=0x87 (Not authorized), property-length=0. */
    connack[0] = 0x20;
    connack[1] = 0x03;
    connack[2] = 0x00;
    connack[3] = 0x87;
    connack[4] = 0x00;
    connack_len = 5;
  }
  else
  {
    /* ack flags=0, return code=0x05 (Not authorized). */
    connack[0] = 0x20;
    connack[1] = 0x02;
    connack[2] = 0x00;
    connack[3] = 0x05;
    connack_len = 4;
  }
  assert_int_equal(az_iot_test_proxy_set_synthetic_connack(proxy, connack, connack_len), 0);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-connack");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "127.0.0.1";
  copts.port = proxy_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connack_error, k_step_timeout_ms));
  }
  /* The reason code must reach the app as a failed CONNECTED, never a success. */
  assert_true(saw_connack_error(&rec));
  assert_false(saw_connected_ok(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* With no PINGRESP ever sent, a connected client with a short keep-alive must
 * detect the dead link and surface a DISCONNECTED event. The proxy answers the
 * CONNECT with a success CONNACK, then stays silent. */
static void keep_alive_timeout_is_reported(void** state)
{
  (void)state;

  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  uint16_t proxy_port = 0;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, &proxy_port), 0);

  uint8_t connack[8];
  size_t connack_len;
  if (g_factory->version == AZ_IOT_MQTT_VERSION_5)
  {
    connack[0] = 0x20;
    connack[1] = 0x03;
    connack[2] = 0x00;
    connack[3] = 0x00;
    connack[4] = 0x00;
    connack_len = 5;
  }
  else
  {
    connack[0] = 0x20;
    connack[1] = 0x02;
    connack[2] = 0x00;
    connack[3] = 0x00;
    connack_len = 4;
  }
  assert_int_equal(az_iot_test_proxy_set_synthetic_connack(proxy, connack, connack_len), 0);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-keepalive");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "127.0.0.1";
  copts.port = proxy_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 2; /* short so the timeout fires quickly */
  copts.connect_timeout_seconds = k_step_timeout_seconds;

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  /* No PINGRESP will ever arrive; the keep-alive must trip within a few
   * intervals and surface a disconnect. */
  assert_true(wait_until(c, &rec, saw_disconnected, 12000));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* A v5 server DISCONNECT must be surfaced as a DISCONNECTED event. The proxy
 * sends a success CONNACK, then injects a DISCONNECT with a reason code.
 *
 * MQTT 3.1.1 has no server-sent DISCONNECT packet, so this case belongs only to
 * the v5 suite. It is registered there and nowhere else rather than checking
 * g_factory->version and skipping: the two harness executables are built from
 * separate mains, so the version cannot vary within a run, and a suite that
 * declines part of its job at run time is one nobody notices has shrunk. */
static void server_disconnect_is_reported(void** state)
{
  (void)state;

  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  uint16_t proxy_port = 0;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, &proxy_port), 0);

  uint8_t connack[5] = { 0x20, 0x03, 0x00, 0x00, 0x00 }; /* success */
  assert_int_equal(az_iot_test_proxy_set_synthetic_connack(proxy, connack, sizeof(connack)), 0);
  uint8_t disconnect[3] = { 0xE0, 0x01, 0x8B }; /* reason 0x8B: server shutting down */
  assert_int_equal(
      az_iot_test_proxy_set_synthetic_disconnect(proxy, disconnect, sizeof(disconnect), 300), 0);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-srvdisc");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "127.0.0.1";
  copts.port = proxy_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));
  assert_true(wait_until(c, &rec, saw_disconnected, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* ------------------------------------------------------------------------- */
/* network impairment                                                         */
/* ------------------------------------------------------------------------- */

/* Stand up a proxy in front of the real broker and connect a client through it.
 * Returns the proxy; fills `port_out` with the port the client should dial. */
static az_iot_test_proxy* start_proxy(uint16_t* port_out)
{
  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.upstream_host = g_host;
  popts.upstream_port = g_port;
  az_iot_test_proxy* proxy = NULL;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, port_out), 0);
  assert_int_not_equal(*port_out, 0);
  return proxy;
}

static void connect_via_proxy(
    az_iot_mqtt_client* c,
    conf_recorder* rec,
    const char* client_id,
    uint16_t proxy_port)
{
  c->iface->set_inbound_cb(c, on_event, rec);
  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = "127.0.0.1";
  copts.port = proxy_port;
  copts.client_id = client_id;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, rec, saw_connected_ok, k_step_timeout_ms));
}

/* Find a recorded MESSAGE matching topic+payload exactly. */
static int found_message(const conf_recorder* r, const char* topic, const uint8_t* body, size_t len)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_MESSAGE && strcmp(r->topics[i], topic) == 0
        && r->payload_lens[i] == len && memcmp(r->payloads[i], body, len) == 0)
    {
      return 1;
    }
  }
  return 0;
}

/* A broker that writes a packet in single-byte pieces is indistinguishable, at
 * the socket layer, from a lossy link that dribbles it through. The client must
 * reassemble rather than assume one read yields one packet.
 *
 * This is the direction that matters and the one that used to go unshaped: the
 * proxy documented both directions but only ever fragmented client->broker,
 * where the client's own reassembly code is not involved. */
static void roundtrip_survives_broker_to_client_fragmentation(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  /* One byte per write, from the CONNACK onwards. */
  az_iot_test_proxy_impairment imp = az_iot_test_proxy_impairment_default();
  imp.fragment_max = 1;
  az_iot_test_proxy_set_impairment(proxy, AZ_IOT_TEST_PROXY_B2C, &imp);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-frag");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_via_proxy(c, &rec, cid, proxy_port);

  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

  static const uint8_t body[] = { 'f', 'r', 'a', 'g', 'm', 'e', 'n', 't', 'e', 'd' };
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);

  assert_true(wait_until(c, &rec, saw_message, k_step_timeout_ms));
  assert_true(found_message(&rec, topic, body, sizeof(body)));

  /* Without this the case would pass just as well if the impairment were
   * silently ignored -- which is exactly the bug it exists to catch. An
   * unfragmented session needs a handful of writes to reach this point; one
   * byte at a time needs one per byte. */
  assert_true(az_iot_test_proxy_writes(proxy, AZ_IOT_TEST_PROXY_B2C) > 50);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* Added latency and jitter on both directions must not change any outcome, only
 * its timing: a client that races its own acknowledgements would fail here. The
 * jitter seed is fixed so a failure replays. */
static void roundtrip_survives_latency_and_jitter(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  az_iot_test_proxy_impairment imp = az_iot_test_proxy_impairment_default();
  imp.base_delay_ms = 150;
  imp.jitter_ms = 50;
  imp.seed = 0xC0FFEEu;
  az_iot_test_proxy_set_impairment(proxy, AZ_IOT_TEST_PROXY_C2B, &imp);
  imp.seed = 0xBADCAFEu;
  az_iot_test_proxy_set_impairment(proxy, AZ_IOT_TEST_PROXY_B2C, &imp);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-jitter");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_via_proxy(c, &rec, cid, proxy_port);

  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

  static const uint8_t body[] = { 'j', 'i', 't', 't', 'e', 'r' };
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  unsigned long started = conf_now_ms();
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);

  assert_true(wait_until(c, &rec, saw_message, k_step_timeout_ms));
  unsigned long elapsed = conf_now_ms() - started;
  assert_true(found_message(&rec, topic, body, sizeof(body)));
  /* The echo crosses the proxy twice, so it cannot beat two base delays. The
   * harness pumps in 50 ms steps, so the floor has to clear that quantum to
   * mean anything: unshaped, this round trip lands in the first poll. */
  assert_true(elapsed >= 250);
  /* Slow is not the same as broken: the session must still be up. */
  assert_false(saw_disconnected(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* A bandwidth ceiling must slow a payload down without corrupting it. The
 * ceiling is applied only after the session is established, so the delay
 * measured belongs to the payload and not to the handshake. */
static void bandwidth_ceiling_slows_a_payload_without_corrupting_it(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-bw");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_via_proxy(c, &rec, cid, proxy_port);

  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

  /* 1 KiB at 2 KiB/s cannot arrive in under about half a second. */
  az_iot_test_proxy_impairment imp = az_iot_test_proxy_impairment_default();
  imp.bytes_per_sec = 2048;
  az_iot_test_proxy_set_impairment(proxy, AZ_IOT_TEST_PROXY_B2C, &imp);

  static uint8_t body[1024];
  for (size_t i = 0; i < sizeof(body); ++i)
  {
    body[i] = (uint8_t)(i & 0xFFu);
  }
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  unsigned long started = conf_now_ms();
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);

  assert_true(wait_until(c, &rec, saw_message, 15000));
  unsigned long elapsed = conf_now_ms() - started;

  /* Every byte arrives, in order: rate limiting must never truncate or
   * reorder the stream. */
  assert_true(found_message(&rec, topic, body, sizeof(body)));
  /* Unshaped this round trip is a few milliseconds, so a floor well under the
   * ~500 ms theoretical minimum still proves the ceiling was enforced. */
  assert_true(elapsed >= 300);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* A stall is not a drop: the link goes quiet, buffers fill, and then everything
 * arrives intact. This is what a client sees across a radio gap or a suspended
 * VM, and it must not be mistaken for a dead connection. */
static void a_stalled_link_resumes_without_losing_the_session(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-stall");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_via_proxy(c, &rec, cid, proxy_port);

  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

  /* Hold the broker's traffic for well under the 30 s keep-alive, so a
   * disconnect here would be the client giving up early rather than a
   * legitimate keep-alive expiry. */
  az_iot_test_proxy_stall(proxy, AZ_IOT_TEST_PROXY_B2C, 1500);

  static const uint8_t body[] = { 's', 't', 'a', 'l', 'l', 'e', 'd' };
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);

  /* Nothing may arrive while the link is held. */
  assert_false(wait_until(c, &rec, saw_message, 700));
  /* The bytes are buffered in the proxy, not lost. */
  assert_true(az_iot_test_proxy_queued_bytes(proxy, AZ_IOT_TEST_PROXY_B2C) > 0);

  /* Once the stall lifts, the backlog is delivered intact. */
  assert_true(wait_until(c, &rec, saw_message, k_step_timeout_ms));
  assert_true(found_message(&rec, topic, body, sizeof(body)));
  assert_false(saw_disconnected(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* ------------------------------------------------------------------------- */
/* entry point                                                                */
/* ------------------------------------------------------------------------- */

/* The cases both suites run. Kept in a macro so the v3 and v5 lists cannot
 * drift apart by editing one and forgetting the other: the only intended
 * difference between them is the v5-only server-DISCONNECT case, which is
 * appended to the v5 list below. */
#define AZ_IOT_CONFORMANCE_COMMON_TESTS                                                          \
  cmocka_unit_test(connect_disconnect_roundtrip), cmocka_unit_test(publish_subscribe_roundtrip), \
      cmocka_unit_test(disconnect_without_connect_is_rejected),                                  \
      cmocka_unit_test(connect_after_disconnect_reuses_the_client),                              \
      cmocka_unit_test(connect_to_a_closed_port_is_rejected),                                    \
      cmocka_unit_test(connect_to_an_unresolvable_host_is_rejected),                             \
      cmocka_unit_test(connect_to_a_black_holed_address_never_reports_connected),                \
      cmocka_unit_test(idle_session_survives_the_keep_alive_interval),                           \
      TLS_TESTS cmocka_unit_test(reconnect_after_network_drop),                                  \
      cmocka_unit_test(connect_connack_error_is_reported),                                       \
      cmocka_unit_test(keep_alive_timeout_is_reported),                                          \
      cmocka_unit_test(roundtrip_survives_broker_to_client_fragmentation),                       \
      cmocka_unit_test(roundtrip_survives_latency_and_jitter),                                   \
      cmocka_unit_test(bandwidth_ceiling_slows_a_payload_without_corrupting_it),                 \
      cmocka_unit_test(a_stalled_link_resumes_without_losing_the_session)

/* Expands to nothing when the certificate cases were compiled out, so the two
 * lists above stay a single expression either way. */
#ifdef AZ_IOT_CONFORMANCE_WITH_TLS
#define TLS_TESTS                                                   \
  cmocka_unit_test(tls_handshake_succeeds_with_trusted_valid_cert), \
      cmocka_unit_test(server_cert_validation_rejects_untrusted),   \
      cmocka_unit_test(server_cert_validation_rejects_expired),     \
      cmocka_unit_test(server_cert_validation_rejects_hostname_mismatch),
#else
#define TLS_TESTS
#endif

int az_iot_conformance_run(az_iot_conformance_suite suite_kind, az_iot_mqtt_factory* factory)
{
  if (!factory)
  {
    return 1;
  }

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

  if (!host || !*host)
  {
    /* A failure, not a skip. Whether this suite runs is decided at build time
     * by AZ_IOT_BUILD_CONFORMANCE_TESTS; if it was built and registered, the
     * broker address is a promise the caller has already made. Excusing
     * ourselves here would hide a misconfigured job that silently stopped
     * exercising the adapter. */
    fprintf(
        stderr,
        "conformance: AZ_IOT_MQTT_BROKER_HOST is unset or empty, but this suite was built with "
        "AZ_IOT_BUILD_CONFORMANCE_TESTS=ON. Point it at a reachable broker, or configure with "
        "AZ_IOT_BUILD_CONFORMANCE_TESTS=OFF so the suite is not registered.\n");
    return 1;
  }

  g_factory = factory;
  g_host = host;
  g_port = port ? (uint16_t)atoi(port) : (uint16_t)1883;

  /* The certificate-validation cases used to need a separately provisioned TLS
   * broker (AZ_IOT_MQTT_BROKER_TLS_HOST/_PORT). They now terminate TLS in the
   * in-process test proxy against this same plaintext broker, so there is no
   * second endpoint to configure -- and no way for that configuration to go
   * missing and take a security check with it. */

  fprintf(
      stderr,
      "conformance: running %s suite against %s:%u\n",
      (suite_kind == AZ_IOT_CONFORMANCE_SUITE_V5) ? "MQTTv5" : "MQTTv3.1.1",
      g_host,
      (unsigned)g_port);

  int failed;
  if (suite_kind == AZ_IOT_CONFORMANCE_SUITE_V5)
  {
    const struct CMUnitTest v5_tests[]
        = { AZ_IOT_CONFORMANCE_COMMON_TESTS, cmocka_unit_test(server_disconnect_is_reported) };
    failed = cmocka_run_group_tests(v5_tests, NULL, NULL);
  }
  else
  {
    const struct CMUnitTest v3_tests[] = { AZ_IOT_CONFORMANCE_COMMON_TESTS };
    failed = cmocka_run_group_tests(v3_tests, NULL, NULL);
  }
  return (failed == 0) ? 0 : 1;
}
