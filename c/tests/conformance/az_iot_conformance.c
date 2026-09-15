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
#include "az_iot_test_mqtt_server.h"
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
/* Adapter capabilities and end-to-end custody material, from
 * az_iot_conformance_run_with_options(). */
static uint32_t g_capabilities = 0;
static const char* g_key_uri = NULL;
static const char* g_key_engine = NULL;
static const char* g_client_cert_path = NULL;
static az_iot_mqtt_sign_callback g_sign = NULL;
static void* g_sign_ctx = NULL;
static uint16_t g_websocket_port = 0;
static const char* g_websocket_path = NULL;

static bool adapter_claims_websockets(void)
{
  return (g_capabilities & (uint32_t)AZ_IOT_CONFORMANCE_CAP_WEBSOCKETS) != 0u;
}

static bool adapter_claims_proxy(void)
{
  return (g_capabilities & (uint32_t)AZ_IOT_CONFORMANCE_CAP_PROXY) != 0u;
}

static bool adapter_claims_key_custody_uri(void)
{
  return (g_capabilities & (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI) != 0u;
}

static bool adapter_claims_key_custody_sign(void)
{
  return (g_capabilities & (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_SIGN) != 0u;
}
/* Which custody routes have complete material.
 *
 * Judged per route and on ALL of a route's fields, never on key_uri alone. An
 * earlier version tested only key_uri, so material supplied without it was
 * ignored in silence -- and the run then reported "no key was supplied", which
 * the opt-out downgrades to a notice. Material that was supplied and ignored is
 * a misconfiguration, and it must not be able to look like a machine that
 * deliberately has no token. That is also why a certificate with no route to
 * use it is PARTIAL rather than NONE. */
int az_iot_conformance_custody_material_state(
    const char* key_uri,
    const char* crypto_engine_id,
    const char* client_cert_path,
    bool has_sign,
    bool has_sign_ctx)
{
  /* An empty string is missing, not supplied. The bundled harnesses already
   * map an empty environment variable to NULL, so accepting "" here would hold
   * a direct caller of this API to a weaker rule than the harnesses that ship
   * with it -- and "" as a key URI reaches the TLS case as a complete set. */
  const bool has_uri = (key_uri != NULL) && (key_uri[0] != '\0');
  const bool has_engine = (crypto_engine_id != NULL) && (crypto_engine_id[0] != '\0');
  const bool has_cert = (client_cert_path != NULL) && (client_cert_path[0] != '\0');

  /* Either half of the pair means the URI route was intended, so a missing
   * counterpart is reported rather than treated as "route not requested". */
  const bool uri_requested = has_uri || has_engine;

  /* has_sign_ctx counts here too: without it a lone sign_ctx would be reported
   * as "nothing supplied" and the check below could never be reached. */
  if (!uri_requested && !has_sign && !has_sign_ctx && !has_cert)
  {
    return AZ_IOT_CONFORMANCE_CUSTODY_NONE;
  }
  if (uri_requested && !(has_uri && has_engine && has_cert))
  {
    return AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL;
  }
  if (has_sign && !has_cert)
  {
    return AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL;
  }
  /* A context with no callback to hand it to. sign_ctx is legitimately NULL
   * WITH a callback -- it is opaque and many adapters need none -- so it is
   * only ever evidence of intent in this direction. */
  if (has_sign_ctx && !has_sign)
  {
    return AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL;
  }
  if (!uri_requested && !has_sign)
  {
    /* A certificate and/or a sign context, with no route to use either. */
    return AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL;
  }

  return (uri_requested ? AZ_IOT_CONFORMANCE_CUSTODY_URI : 0)
      | (has_sign ? AZ_IOT_CONFORMANCE_CUSTODY_SIGN : 0);
}

/* Read an environment variable without tripping MSVC's C4996 on getenv.
 *
 * Returns `buf` when the variable is set, NULL when it is not. A value that
 * does not fit is reported as the empty string rather than truncated: this
 * feeds the opt-out comparison below, and a truncation that happened to leave
 * "1" would opt out on the strength of a value nobody wrote. Erring towards
 * "not set" can only make the suite stricter. */
static const char* read_env(const char* name, char* buf, size_t cap)
{
  if (cap == 0)
  {
    return NULL;
  }
  buf[0] = '\0';
#ifdef _WIN32
  size_t needed = 0;
  if (getenv_s(&needed, buf, cap, name) != 0)
  {
    /* Set but too long for `buf`; anything but a faithful copy must not match. */
    return (needed > 0) ? "" : NULL;
  }
  return (needed == 0) ? NULL : buf;
#else
  const char* value = getenv(name);
  if (value == NULL)
  {
    return NULL;
  }
  if (strlen(value) >= cap)
  {
    return "";
  }
  memcpy(buf, value, strlen(value) + 1);
  return buf;
#endif
}

/* A declared capability that was never exercised.
 *
 * The suite exists so that a third party can bring their own MQTT layer and
 * have a pass mean something. A capability is the adapter's own claim to
 * implement an optional feature, so a run that prints a warning and still
 * exits 0 is the one outcome that must not happen: the claim ends up published
 * as "conformant" having never been checked.
 *
 * So this FAILS the run. An environment that genuinely cannot exercise it --
 * no token on the machine, a build without TLS -- must say so deliberately by
 * setting AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN=1, which downgrades it to a
 * notice. That keeps "could not run it here" a decision someone made, not an
 * accident of the environment, and it is never the default.
 *
 * Returns the number of failures to add to the run's total. */
int az_iot_conformance_report_unproven_capability(
    const char* capability,
    const char* why,
    const char* allow_value)
{
  /* Taken as an argument rather than read here so the policy is testable
   * without touching the environment: setenv() is POSIX and absent on MSVC,
   * and this contract has to be checked on every leg, not just the ones with a
   * POSIX libc. */
  const bool allowed = (allow_value != NULL) && (allow_value[0] == '1') && (allow_value[1] == '\0');

  fprintf(
      stderr,
      "conformance: %s: %s is declared but its contract was NOT exercised: %s\n",
      allowed ? "NOTICE" : "FAILED",
      capability,
      why);

  if (allowed)
  {
    fprintf(
        stderr,
        "conformance: allowed by AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN=1. This run does NOT"
        " demonstrate %s and must not be reported as conformant for it.\n",
        capability);
    return 0;
  }

  fprintf(
      stderr,
      "conformance: declare the capability only in a run that can prove it, or set"
      " AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN=1 to accept an unproven run.\n");
  return 1;
}

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
  int32_t protocol_codes[CONF_EVENTS_MAX];
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
  r->protocol_codes[i] = evt->protocol_code;
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

/* A SUBSCRIBE_ACK carrying an error status. The broker granting nothing is not
 * a transport failure, so it has to arrive as a failed ack rather than as a
 * disconnect or as silence. */
static int saw_subscribe_ack_error(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK && r->statuses[i] != AZ_IOT_OK)
    {
      return 1;
    }
  }
  return 0;
}

/* A PUBLISH_ACK carrying an error status. A publish the broker refuses is not a
 * transport failure either: it has to reach the application as a failed ack. */
static int saw_publish_ack_error(const conf_recorder* r)
{
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->kinds[i] == AZ_IOT_MQTT_EVT_PUBLISH_ACK && r->statuses[i] != AZ_IOT_OK)
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

/* ------------------------------------------------------------------------- */
/* broker packets                                                             */
/* ------------------------------------------------------------------------- */

/* Bind the version under test once, so the cases below read as intent rather
 * than repeating a v3/v5 branch each time they need a refusal. */
static az_iot_test_mqtt_version conf_mqtt_version(void)
{
  return (g_factory->version == AZ_IOT_MQTT_VERSION_5) ? AZ_IOT_TEST_MQTT_V5
                                                       : AZ_IOT_TEST_MQTT_V3_1_1;
}

static az_iot_test_mqtt_packet conf_connack(az_iot_test_mqtt_reason reason)
{
  return az_iot_test_mqtt_connack(conf_mqtt_version(), reason);
}

static az_iot_test_mqtt_packet conf_suback(az_iot_test_mqtt_reason reason)
{
  return az_iot_test_mqtt_suback(conf_mqtt_version(), reason);
}

static az_iot_test_mqtt_packet conf_puback(az_iot_test_mqtt_reason reason)
{
  return az_iot_test_mqtt_puback(conf_mqtt_version(), reason);
}

static az_iot_test_mqtt_packet conf_disconnect(az_iot_test_mqtt_reason reason)
{
  return az_iot_test_mqtt_disconnect(conf_mqtt_version(), reason);
}

/* Point an INJECT rule at a built packet. Deliberately here and not in the
 * proxy's header: the proxy takes opaque bytes and must not learn what an MQTT
 * packet is. */
static void inject_packet(
    az_iot_test_proxy_rule* rule,
    az_iot_test_proxy_direction dir,
    const az_iot_test_mqtt_packet* packet)
{
  rule->action = AZ_IOT_TEST_PROXY_ACTION_INJECT;
  rule->inject_dir = dir;
  rule->bytes = packet->bytes;
  rule->bytes_len = packet->len;
  rule->echo_packet_id = packet->echo_packet_id;
  rule->packet_id_offset = packet->packet_id_offset;
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
  /* A millisecond stamp alone is not unique: writing a CA and a client
   * certificate back to back can land both in the same millisecond, and the
   * second silently overwrites the first -- which presents as a handshake that
   * fails for no visible reason. */
  static unsigned long seq = 0;
  int written = snprintf(path_out, cap, "az_iot_conf_pem_%lu_%lu.pem", conf_now_ms(), seq++);
  if (written < 0 || (size_t)written >= cap)
  {
    return 0; /* a truncated path is how the collision above happens again */
  }
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

/* A disconnect the APPLICATION asked for is reported, like one the peer
 * caused.
 *
 * The suite asserted DISCONNECTED only for peer-initiated teardowns -- a
 * dropped link, a server DISCONNECT -- and the roundtrip case above
 * deliberately does not fail when nothing arrives. That left the
 * client-initiated path unasserted, and an adapter could pass the whole suite
 * while never reporting it: the disconnect call itself still returns OK.
 *
 * It matters because the connection client moves to DISCONNECTING on close()
 * and waits for this event to settle the session to IDLE. Without it, any
 * caller that waits for a close to finish waits forever. */
static void a_client_initiated_disconnect_is_reported(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-discrep");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_client(c, &rec, cid);

  assert_int_equal(c->iface->disconnect(c), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_disconnected, k_step_timeout_ms));

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
  copts.tls.use_tls = true;

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
  remove(ca_path);
}

/* Server-side certificate validation is mandatory and unconditional: against a
 * leaf the client's trust anchor does NOT sign, the TLS handshake must be
 * rejected and the client must never reach CONNECTED. The proxy terminates TLS
 * with a leaf signed by an UNTRUSTED CA while the client is handed the
 * (different) trusted CA. If validation were disabled the handshake would
 * succeed and a CONNACK would arrive, failing this test.
 *
 * Nothing here asks for validation -- the TLS options carry no flag that could
 * request or refuse it. That is the point: the adapter must validate because it
 * always validates, not because this test opted in. */
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
  /* Nothing asks for validation: the CA alone selects TLS, and no field can
   * request or refuse verification. Rejection below is therefore proof that the
   * adapter validates unconditionally. */

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
 * entirely in the past. Validation covers chain *and* validity, so an adapter
 * that only checks the chain fails here. */
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
  /* No flag opts in to verification; see the untrusted-cert case above. */

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
  /* No flag opts in to verification; see the untrusted-cert case above. */

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

  az_iot_test_mqtt_packet connack = conf_connack(AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED);
  assert_int_equal(az_iot_test_proxy_set_synthetic_connack(proxy, connack.bytes, connack.len), 0);

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

  /* And the code that produced that status must still be readable. CONNACK and
   * SUBACK share the rule, so covering only one leaves the other free to start
   * flattening again without any suite noticing. */
  int ack = -1;
  for (size_t i = 0; i < rec.count; ++i)
  {
    if (rec.kinds[i] == AZ_IOT_MQTT_EVT_CONNECTED && rec.statuses[i] != AZ_IOT_OK)
    {
      ack = (int)i;
      break;
    }
  }
  assert_true(ack >= 0);
  assert_int_equal(
      rec.protocol_codes[ack], (g_factory->version == AZ_IOT_MQTT_VERSION_5) ? 0x87 : 0x05);

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

  az_iot_test_mqtt_packet connack = conf_connack(AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  assert_int_equal(az_iot_test_proxy_set_synthetic_connack(proxy, connack.bytes, connack.len), 0);

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

  az_iot_test_mqtt_packet connack = conf_connack(AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  assert_int_equal(az_iot_test_proxy_set_synthetic_connack(proxy, connack.bytes, connack.len), 0);
  az_iot_test_mqtt_packet bye = conf_disconnect(AZ_IOT_TEST_MQTT_REASON_SERVER_SHUTTING_DOWN);
  assert_int_equal(az_iot_test_proxy_set_synthetic_disconnect(proxy, bye.bytes, bye.len, 300), 0);

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
/* scripted broker faults                                                     */
/* ------------------------------------------------------------------------- */

/* A broker that refuses a subscription answers with a SUBACK whose reason code
 * says so. No real broker will do that on demand, so the proxy swallows the
 * SUBSCRIBE and answers it itself, echoing the client's packet id -- an ack
 * carrying the wrong id would be discarded before the adapter's failure path
 * ever ran.
 *
 * Everything else on the connection is still the real broker; only this one
 * exchange is scripted. */
static void a_refused_subscribe_is_reported(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  az_iot_test_mqtt_packet suback = conf_suback(AZ_IOT_TEST_MQTT_REASON_REFUSED);

  az_iot_test_proxy_rule swallow = { 0 };
  swallow.dir = AZ_IOT_TEST_PROXY_C2B;
  swallow.on_packet = AZ_IOT_TEST_PROXY_PKT_SUBSCRIBE;
  swallow.action = AZ_IOT_TEST_PROXY_ACTION_SUPPRESS;
  assert_true(az_iot_test_proxy_add_rule(proxy, &swallow) >= 0);

  az_iot_test_proxy_rule refuse = { 0 };
  refuse.dir = AZ_IOT_TEST_PROXY_C2B;
  refuse.on_packet = AZ_IOT_TEST_PROXY_PKT_SUBSCRIBE;
  inject_packet(&refuse, AZ_IOT_TEST_PROXY_B2C, &suback);
  int refuse_id = az_iot_test_proxy_add_rule(proxy, &refuse);
  assert_true(refuse_id >= 0);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-subrefuse");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_via_proxy(c, &rec, cid, proxy_port);

  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);

  /* The refusal must arrive as a failed ack, not as silence and not as a
   * successful one. */
  assert_true(wait_until(c, &rec, saw_subscribe_ack_error, k_step_timeout_ms));
  assert_false(saw_subscribe_ack_ok(&rec));

  /* And not merely "an error": the broker will refuse this filter every time it
   * is asked, so the status has to say so, and the code that justified it has to
   * still be readable. An adapter that reports a blanket AZ_IOT_ERR_MQTT here
   * leaves the core reconnecting forever against a filter that can never be
   * granted, which is precisely what this assertion exists to catch. */
  int ack = -1;
  for (size_t i = 0; i < rec.count; ++i)
  {
    if (rec.kinds[i] == AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK)
    {
      ack = (int)i;
      break;
    }
  }
  assert_true(ack >= 0);
  assert_int_equal(rec.statuses[ack], AZ_IOT_ERR_SUBSCRIPTION_REFUSED);
  assert_int_equal(
      rec.protocol_codes[ack], (g_factory->version == AZ_IOT_MQTT_VERSION_5) ? 0x87 : 0x80);
  assert_int_equal(az_iot_test_proxy_rule_hits(proxy, (size_t)refuse_id), 1);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* A PUBLISH cut short mid-flight must never reach the application. Half a
 * packet is not a message, and a client that surfaced one would hand the
 * application a truncated payload it has no way to detect. */
static void a_truncated_publish_is_never_surfaced_as_a_message(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  /* Keep only the first four bytes of the broker's copy, so the topic and
   * payload never arrive. */
  az_iot_test_proxy_rule cut = { 0 };
  cut.dir = AZ_IOT_TEST_PROXY_B2C;
  cut.on_packet = AZ_IOT_TEST_PROXY_PKT_PUBLISH;
  cut.action = AZ_IOT_TEST_PROXY_ACTION_TRUNCATE;
  cut.truncate_to = 4;
  int cut_id = az_iot_test_proxy_add_rule(proxy, &cut);
  assert_true(cut_id >= 0);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-trunc");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_via_proxy(c, &rec, cid, proxy_port);

  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

  static const uint8_t body[] = { 't', 'r', 'u', 'n', 'c', 'a', 't', 'e', 'd' };
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);

  assert_false(wait_until(c, &rec, saw_message, 1500));
  /* Without this the case would pass on a proxy that never truncated anything. */
  assert_true(az_iot_test_proxy_rule_hits(proxy, (size_t)cut_id) >= 1);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* A PUBACK for a packet id the client never used is the sort of thing a
 * confused or malicious peer sends. It must be ignored: not matched against
 * some unrelated in-flight publish, and not treated as a protocol error that
 * tears down a working session. */
static void an_acknowledgement_for_an_unknown_packet_id_is_ignored(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  /* Ride along with the SUBSCRIBE so the injection lands in the middle of a
   * live session rather than before it is up. The SUBSCRIBE itself is still
   * forwarded; this rule only adds traffic. */
  az_iot_test_mqtt_packet stray_puback = conf_puback(AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  /* Pin an id the client cannot have used, and stop the proxy echoing the real
   * one over it -- the wrong id is the whole point of the case. */
  az_iot_test_mqtt_set_packet_id(&stray_puback, 0xBEEF);

  az_iot_test_proxy_rule stray = { 0 };
  stray.dir = AZ_IOT_TEST_PROXY_C2B;
  stray.on_packet = AZ_IOT_TEST_PROXY_PKT_SUBSCRIBE;
  inject_packet(&stray, AZ_IOT_TEST_PROXY_B2C, &stray_puback);
  int stray_id = az_iot_test_proxy_add_rule(proxy, &stray);
  assert_true(stray_id >= 0);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-strayack");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_via_proxy(c, &rec, cid, proxy_port);

  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));
  assert_int_equal(az_iot_test_proxy_rule_hits(proxy, (size_t)stray_id), 1);

  /* The session has to still work afterwards: a stray ack must not wedge the
   * client or be mistaken for the answer to a later publish. */
  static const uint8_t body[] = { 'a', 'f', 't', 'e', 'r' };
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);

  assert_true(wait_until(c, &rec, saw_message, k_step_timeout_ms));
  assert_true(found_message(&rec, topic, body, sizeof(body)));
  assert_false(saw_disconnected(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

#ifdef AZ_IOT_CONFORMANCE_WITH_TLS
/* Stand up a proxy that terminates TLS and demands a client certificate, and
 * write out the CA the client must trust. Returns the proxy. */
static az_iot_test_proxy* start_mutual_tls_proxy(uint16_t* port_out, char* ca_path, size_t ca_cap)
{
  az_iot_test_proxy* proxy = start_proxy(port_out);

  az_iot_test_proxy_tls_options tls = az_iot_test_proxy_tls_options_default();
  tls.require_client_cert = 1;
  assert_int_equal(az_iot_test_proxy_enable_tls(proxy, &tls), 0);

  char ca_pem[4096];
  assert_true(az_iot_test_proxy_ca_pem(proxy, ca_pem, sizeof(ca_pem)) > 0);
  assert_true(write_temp_pem(ca_pem, ca_path, ca_cap));
  return proxy;
}

/* Mint a client certificate with the given validity offsets and write the pair
 * to disk, since the adapter takes paths rather than PEM. */
static void issue_client_pem(
    az_iot_test_proxy* proxy,
    long not_before_offset_sec,
    long not_after_offset_sec,
    char* cert_path,
    size_t cert_path_cap,
    char* key_path,
    size_t key_path_cap)
{
  az_iot_test_proxy_client_cert_options opt = az_iot_test_proxy_client_cert_options_default();
  opt.not_before_offset_sec = not_before_offset_sec;
  opt.not_after_offset_sec = not_after_offset_sec;

  char cert_pem[4096];
  char key_pem[4096];
  assert_int_equal(
      az_iot_test_proxy_issue_client_cert(
          proxy, &opt, cert_pem, sizeof(cert_pem), key_pem, sizeof(key_pem)),
      0);
  assert_true(write_temp_pem(cert_pem, cert_path, cert_path_cap));
  assert_true(write_temp_pem(key_pem, key_path, key_path_cap));
}

/* The positive control for the mutual-TLS cases: a client certificate the proxy
 * issued, inside its validity window, must be ACCEPTED. Without this a
 * handshake broken for some unrelated reason would make the rejection below
 * pass for the wrong reason. */
static void mutual_tls_succeeds_with_a_valid_client_cert(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  char ca_path[128];
  az_iot_test_proxy* proxy = start_mutual_tls_proxy(&proxy_port, ca_path, sizeof(ca_path));

  char cert_path[128];
  char key_path[128];
  issue_client_pem(
      proxy, -3600, 24L * 3600L, cert_path, sizeof(cert_path), key_path, sizeof(key_path));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-mtlsok");
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
  copts.tls.client_cert_path = cert_path;
  copts.tls.client_key_path = key_path;
  copts.tls.use_tls = true;

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
  remove(ca_path);
  remove(cert_path);
  remove(key_path);
}

/* An expired client certificate must be refused by the peer. The assertion
 * belongs to the peer's verification, which is why this needed a proxy that
 * asks for a client certificate at all -- one that never asks cannot refuse.
 *
 * The certificate's validity window is placed relative to now, so it is expired
 * by construction rather than by a checked-in fixture that has to be re-minted
 * every time it ages out. */
static void expired_client_cert_is_rejected(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  char ca_path[128];
  az_iot_test_proxy* proxy = start_mutual_tls_proxy(&proxy_port, ca_path, sizeof(ca_path));

  /* Valid from two days ago until one day ago. */
  char cert_path[128];
  char key_path[128];
  issue_client_pem(
      proxy,
      -2L * 24 * 3600,
      -24L * 3600,
      cert_path,
      sizeof(cert_path),
      key_path,
      sizeof(key_path));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-mtlsexp");
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
  copts.tls.client_cert_path = cert_path;
  copts.tls.client_key_path = key_path;
  copts.tls.use_tls = true;

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    (void)wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms);
  }
  /* However it is reported, it must never be reported as connected. */
  assert_false(saw_connected_ok(&rec));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
  remove(ca_path);
  remove(cert_path);
  remove(key_path);
}

/* Presenting no certificate at all to a peer that requires one must fail in the
 * same way. This is the case that distinguishes "the proxy asks" from "the
 * proxy checks": without it, a proxy that requested a certificate but accepted
 * its absence would still pass the expired case for the wrong reason. */
static void a_missing_client_cert_is_rejected(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  char ca_path[128];
  az_iot_test_proxy* proxy = start_mutual_tls_proxy(&proxy_port, ca_path, sizeof(ca_path));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-mtlsnone");
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
  copts.tls.use_tls = true; /* no client certificate offered */

  /* Either connect() refused it outright, or a failure event must actually have
   * been observed. Discarding the wait would let a client that hangs pending
   * forever pass once the timeout expired. */
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

/* A publish the broker refuses must surface as a failed PUBLISH_ACK. The proxy
 * swallows the PUBLISH and answers it with a PUBACK carrying a failure reason,
 * echoing the client's packet id so the ack is matched to the publish rather
 * than discarded as unsolicited.
 *
 * MQTT 3.1.1 has no reason code in a PUBACK, so a v3 publish can only fail by
 * losing the connection -- a different path with a different meaning. The case
 * is therefore registered in the v5 suite alone rather than checking the
 * version and skipping at run time.
 *
 * This closes the last documented gap in the adapter's publish-failure
 * callbacks. */
static void a_refused_publish_is_reported(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  az_iot_test_mqtt_packet puback = conf_puback(AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED);

  az_iot_test_proxy_rule swallow = { 0 };
  swallow.dir = AZ_IOT_TEST_PROXY_C2B;
  swallow.on_packet = AZ_IOT_TEST_PROXY_PKT_PUBLISH;
  swallow.action = AZ_IOT_TEST_PROXY_ACTION_SUPPRESS;
  assert_true(az_iot_test_proxy_add_rule(proxy, &swallow) >= 0);

  az_iot_test_proxy_rule refuse = { 0 };
  refuse.dir = AZ_IOT_TEST_PROXY_C2B;
  refuse.on_packet = AZ_IOT_TEST_PROXY_PKT_PUBLISH;
  inject_packet(&refuse, AZ_IOT_TEST_PROXY_B2C, &puback);
  int refuse_id = az_iot_test_proxy_add_rule(proxy, &refuse);
  assert_true(refuse_id >= 0);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-pubrefuse");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  connect_via_proxy(c, &rec, cid, proxy_port);

  static const uint8_t body[] = { 'r', 'e', 'f', 'u', 's', 'e', 'd' };
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);

  assert_true(wait_until(c, &rec, saw_publish_ack_error, k_step_timeout_ms));
  assert_int_equal(az_iot_test_proxy_rule_hits(proxy, (size_t)refuse_id), 1);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* ------------------------------------------------------------------------- */
/* entry point                                                                */
/* ------------------------------------------------------------------------- */

/* The cases both suites run. Kept in a macro so the v3 and v5 lists cannot
 * drift apart by editing one and forgetting the other. The intended differences
 * are the cases appended to the v5 list below, each of which depends on
 * something MQTT 3.1.1 does not have: a server-sent DISCONNECT packet, and a
 * reason code in a PUBACK. */
/* ------------------------------------------------------------------------- */
/* non-extractable key custody (D8)                                          */
/*                                                                           */
/* The first two cases are BASELINE: they run against every adapter, whatever */
/* it declares. An adapter that does not implement custody is not asked to    */
/* implement it -- it is asked to say so, because the alternative is a client */
/* that connects with no client key at all while the caller believes a token  */
/* is protecting it. Silence is the failure mode being ruled out here.        */
/* ------------------------------------------------------------------------- */

/* A key reference the adapter cannot possibly resolve: the engine names no
 * installed provider. The connect must fail. It must NEVER succeed, because
 * succeeding means the session was established without the key the caller
 * asked to authenticate with.
 *
 * An adapter that does not implement custody has a stronger obligation still:
 * az_iot_mqtt_tls_options documents AZ_IOT_ERR_NOT_SUPPORTED as the answer,
 * so the caller can tell "I cannot do this" from "I tried and it failed". */
static void a_key_reference_is_never_silently_ignored(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-custody-uri");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_host;
  copts.port = g_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  copts.tls.client_key_uri = "pkcs11:object=az-iot-conformance-absent;type=private";
  copts.tls.crypto_engine_id = "az-iot-conformance-no-such-provider";

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    (void)wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms);
  }
  assert_int_not_equal(r, AZ_IOT_OK);
  assert_false(saw_connected_ok(&rec));

  if (!adapter_claims_key_custody_uri())
  {
    assert_int_equal(r, AZ_IOT_ERR_NOT_SUPPORTED);
  }

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

/* The other half of the same contract, for the sign() hook. An adapter with no
 * way to route a signature through a caller-supplied callback -- Paho has no
 * TLS key callback, so it is one -- must refuse rather than connect without a
 * client key. */
static az_iot_result conformance_sign_stub(
    void* ctx,
    const uint8_t* digest,
    size_t digest_len,
    uint8_t* out_sig,
    size_t out_sig_cap,
    size_t* out_sig_len)
{
  (void)ctx;
  (void)digest;
  (void)digest_len;
  (void)out_sig;
  (void)out_sig_cap;
  (void)out_sig_len;
  /* Never reached on an adapter that refuses the credential, which is the
   * point: reaching it would mean the hook was accepted. */
  return AZ_IOT_ERR_NOT_SUPPORTED;
}

static void a_sign_hook_is_never_silently_ignored(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-custody-sign");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_host;
  copts.port = g_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  copts.tls.client_cert_path = NULL;
  copts.tls.sign = conformance_sign_stub;
  copts.tls.sign_ctx = NULL;

  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    (void)wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms);
  }
  assert_int_not_equal(r, AZ_IOT_OK);
  assert_false(saw_connected_ok(&rec));

  if (!adapter_claims_key_custody_sign())
  {
    assert_int_equal(r, AZ_IOT_ERR_NOT_SUPPORTED);
  }

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

#ifdef AZ_IOT_CONFORMANCE_WITH_TLS
/* The end-to-end case, and the only one that proves the feature actually works:
 * complete a real TLS handshake using a private key the adapter cannot read.
 *
 * The proxy asks for a client certificate and accepts whichever one arrives.
 * That is deliberate -- a key sealed in a token cannot be handed to the proxy's
 * CA to be certified, so requiring its issuer would make the property
 * untestable. What proves possession is the CertificateVerify signature, which
 * TLS makes the client produce with the private key. If the adapter cannot sign
 * through the token, the handshake does not complete and this fails.
 *
 * Runs only when the harness supplied a key; see az_iot_conformance_options. */
static void key_custody_completes_a_tls_handshake(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  az_iot_test_proxy_tls_options tls = az_iot_test_proxy_tls_options_default();
  tls.accept_any_client_cert = 1;
  assert_int_equal(az_iot_test_proxy_enable_tls(proxy, &tls), 0);

  char ca_pem[4096];
  char ca_path[128];
  assert_true(az_iot_test_proxy_ca_pem(proxy, ca_pem, sizeof(ca_pem)) > 0);
  assert_true(write_temp_pem(ca_pem, ca_path, sizeof(ca_path)));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-custody-e2e");
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
  copts.tls.client_cert_path = g_client_cert_path;
  copts.tls.client_key_uri = g_key_uri;
  copts.tls.crypto_engine_id = g_key_engine;

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
  remove(ca_path);
}

/* The same proof for the OTHER custody route: the adapter has no engine or
 * provider abstraction and drives the CertificateVerify signature through the
 * caller's callback instead.
 *
 * A separate case because the two routes are separate capabilities. Holding a
 * sign-hook-only adapter to the URI route would fail it for a feature it never
 * claimed, and -- since an unexercised claim fails the run -- one shared
 * capability left such an adapter unable to obtain a conformant result at all.
 *
 * Runs only when the harness supplied a sign hook; see
 * az_iot_conformance_options. */
static void key_custody_sign_hook_completes_a_tls_handshake(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_proxy(&proxy_port);

  az_iot_test_proxy_tls_options tls = az_iot_test_proxy_tls_options_default();
  tls.accept_any_client_cert = 1;
  assert_int_equal(az_iot_test_proxy_enable_tls(proxy, &tls), 0);

  char ca_pem[4096];
  char ca_path[128];
  assert_true(az_iot_test_proxy_ca_pem(proxy, ca_pem, sizeof(ca_pem)) > 0);
  assert_true(write_temp_pem(ca_pem, ca_path, sizeof(ca_path)));

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-custody-sign-e2e");
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
  copts.tls.client_cert_path = g_client_cert_path;
  /* No key URI or engine: this route exists precisely for the stacks that have
   * neither, so supplying them would prove the wrong thing. */
  copts.tls.sign = g_sign;
  copts.tls.sign_ctx = g_sign_ctx;

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
  remove(ca_path);
}
#endif /* AZ_IOT_CONFORMANCE_WITH_TLS */

/* ------------------------------------------------------------------------- */
/* transport: WebSockets and HTTP proxy                                       */
/* ------------------------------------------------------------------------- */

/* Baseline, every adapter: a transport the adapter does not implement must be
 * REFUSED, never quietly downgraded to TCP.
 *
 * The caller that sets WEBSOCKET does it because a direct 8883 session is not
 * available to it, so an adapter that ignores the field and dials TCP produces
 * a connection the caller's network was supposed to prevent -- or, at best, a
 * failure whose reported cause is wrong.
 *
 * Aimed at the ordinary broker port, which is not a WebSocket listener: a
 * declaring adapter must fail the handshake there, and a non-declaring one must
 * refuse before opening anything. Neither may report CONNECTED. */
static void a_websocket_request_is_never_silently_downgraded(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-ws-refuse");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_host;
  copts.port = g_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  copts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;

  /* Either connect() refused it outright, or a failure event must actually have
   * been observed. Discarding the wait would let a client that hangs pending
   * forever pass once the timeout expired. */
  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  assert_false(saw_connected_ok(&rec));

  /* Refusing is correct, and so is trying and failing -- what is not correct is
   * a TCP session reported as success. An adapter that does refuse must do it
   * with the code the interface documents, so the caller can tell "this stack
   * cannot do WebSockets" from "the network refused". */
  if (r != AZ_IOT_OK)
  {
    assert_int_equal(r, AZ_IOT_ERR_NOT_SUPPORTED);
  }

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

/* Baseline, every adapter: a configured proxy that cannot be reached must end
 * as a FAILED connect, never as a direct session to the broker.
 *
 * This is the whole point of a proxy setting on a device: it is an egress
 * control. An adapter that silently connects around it defeats the control
 * while reporting success, which is strictly worse than refusing. The proxy
 * here is a closed port, so the only way to reach CONNECTED is to have ignored
 * it and dialed the broker directly -- exactly the bug being excluded. */
static void an_unreachable_proxy_is_never_bypassed(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-proxy-refuse");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_host;
  copts.port = g_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  /* Port 1 (tcpmux) is reserved and effectively never bound, as in
   * connect_to_a_closed_port_is_rejected above. */
  copts.proxy.host = g_host;
  copts.proxy.port = 1;

  /* Either connect() refused it outright, or a failure event must actually have
   * been observed. Discarding the wait would let a client that hangs pending
   * forever pass once the timeout expired. */
  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  assert_false(saw_connected_ok(&rec));

  /* As above: refusing outright and failing the tunnel are both acceptable;
   * reaching the broker is not, because the only route to it here bypasses the
   * proxy that was configured. */
  if (r != AZ_IOT_OK)
  {
    assert_int_equal(r, AZ_IOT_ERR_NOT_SUPPORTED);
  }

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

/* AZ_IOT_CONFORMANCE_CAP_WEBSOCKETS: a full session over a real WebSocket
 * listener. Registered only when the capability is declared AND a listener was
 * supplied; an undeclared capability is covered by the baseline above. */
static void a_websocket_session_completes_a_roundtrip(void** state)
{
  (void)state;
  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-ws");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = g_host;
  copts.port = g_websocket_port;
  copts.client_id = cid;
  copts.keep_alive_seconds = 30;
  copts.connect_timeout_seconds = k_step_timeout_seconds;
  copts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  copts.websocket_path = g_websocket_path;

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  /* Carry traffic, not just a CONNACK: a WebSocket transport that framed only
   * the handshake correctly would pass a connect-only check. */
  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

  static const uint8_t body[] = { 'w', 's' };
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_message, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
}

/* ------------------------------------------------------------------------- */
/* HTTP CONNECT proxy: positive and negative                                  */
/*                                                                            */
/* These use the in-process test proxy in CONNECT mode rather than an external */
/* one, so they are self-contained: every leg that can reach the broker can    */
/* run them, on Windows as well as Linux, with no service to provision. That   */
/* also lets each case assert on the PROXY's own view -- how many tunnels were */
/* opened, which authority was asked for, whether a credential was refused --  */
/* which is the only way to tell "connected through the tunnel" apart from     */
/* "connected despite it".                                                     */
/* ------------------------------------------------------------------------- */

static az_iot_test_proxy* start_connect_proxy(
    uint16_t* port_out,
    const char* required_username,
    const char* required_password)
{
  az_iot_test_proxy_options popts = az_iot_test_proxy_options_default();
  popts.http_connect = true;
  popts.required_username = required_username;
  popts.required_password = required_password;
  az_iot_test_proxy* proxy = NULL;
  assert_int_equal(az_iot_test_proxy_start(&popts, &proxy, port_out), 0);
  assert_int_not_equal(*port_out, 0);
  return proxy;
}

/* Fill a connect that reaches the broker THROUGH the proxy: the destination is
 * the real broker, and only opts.proxy names the loopback fixture. */
static void proxied_connect_options(
    az_iot_mqtt_connect_options* copts,
    const char* client_id,
    uint16_t proxy_port)
{
  memset(copts, 0, sizeof(*copts));
  copts->host = g_host;
  copts->port = g_port;
  copts->client_id = client_id;
  copts->keep_alive_seconds = 30;
  copts->connect_timeout_seconds = k_step_timeout_seconds;
  copts->proxy.host = "127.0.0.1";
  copts->proxy.port = proxy_port;
}

/* The positive case: a full session, with traffic carried through the tunnel
 * rather than only a CONNACK. A transport that framed the handshake correctly
 * and then mispumped would pass a connect-only check. */
static void a_proxied_session_completes_a_roundtrip(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_connect_proxy(&proxy_port, NULL, NULL);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-proxy");
  char topic[128];
  snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts;
  proxied_connect_options(&copts, cid, proxy_port);

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));

  uint16_t sub_pid = 0;
  assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

  static const uint8_t body[] = { 'v', 'i', 'a' };
  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = sizeof(body);
  msg.qos = AZ_IOT_MQTT_QOS_1;
  uint16_t pub_pid = 0;
  assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_message, k_step_timeout_ms));

  /* The session really went through the tunnel: the proxy opened exactly one,
   * and the authority it was asked for is the broker -- not the proxy itself,
   * which is the mistake a naive implementation makes. */
  assert_int_equal(az_iot_test_proxy_tunnels_opened(proxy), 1);
  assert_int_equal(az_iot_test_proxy_auth_failures(proxy), 0);
  const char* target = az_iot_test_proxy_last_connect_target(proxy);
  assert_non_null(target);
  char expected[300];
  snprintf(expected, sizeof(expected), "%s:%u", g_host, (unsigned)g_port);
  assert_string_equal(target, expected);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* Authenticated proxy, correct credentials. The proxy checks the DECODED
 * user:password it was configured with, so this is what proves the adapter's
 * credential encoding round-trips -- a unit test can only assert the string the
 * adapter built, not that a proxy accepts it. */
static void a_proxy_accepts_correct_credentials(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_connect_proxy(&proxy_port, "device", "s3cret");

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-proxy-auth");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts;
  proxied_connect_options(&copts, cid, proxy_port);
  copts.proxy.username = "device";
  copts.proxy.password = "s3cret";

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));
  assert_int_equal(az_iot_test_proxy_tunnels_opened(proxy), 1);
  assert_int_equal(az_iot_test_proxy_auth_failures(proxy), 0);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* Credentials containing the characters the proxy syntax itself uses. Paho
 * splits its proxy string at the first '@' and percent-decodes what precedes
 * it, so an unescaped '@' silently moves the host and a literal '%' is eaten.
 * The proxy compares against the decoded credential, so anything that does not
 * round-trip fails to authenticate here rather than passing quietly. */
static void a_proxy_accepts_credentials_containing_delimiters(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_connect_proxy(&proxy_port, "dev@corp", "p@ss%77rd");

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-proxy-esc");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts;
  proxied_connect_options(&copts, cid, proxy_port);
  copts.proxy.username = "dev@corp";
  copts.proxy.password = "p@ss%77rd";

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));
  assert_int_equal(az_iot_test_proxy_tunnels_opened(proxy), 1);
  assert_int_equal(az_iot_test_proxy_auth_failures(proxy), 0);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* Wrong password: the proxy answers 407 and never opens a tunnel. The client
 * must end disconnected -- NOT connected by some other route. */
static void a_proxy_rejects_wrong_credentials(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_connect_proxy(&proxy_port, "device", "s3cret");

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-proxy-bad");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts;
  proxied_connect_options(&copts, cid, proxy_port);
  copts.proxy.username = "device";
  copts.proxy.password = "wrong";

  /* Either connect() refused it outright, or a failure event must actually have
   * been observed. Discarding the wait would let a client that hangs pending
   * forever pass once the timeout expired. */
  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  assert_false(saw_connected_ok(&rec));
  assert_int_equal(az_iot_test_proxy_tunnels_opened(proxy), 0);
  assert_true(az_iot_test_proxy_auth_failures(proxy) >= 1);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* A proxy that demands authentication, and a client that offers none. Same
 * outcome, different cause: this separates "sent the wrong credential" from
 * "sent no credential at all", and an adapter that skipped the header entirely
 * would pass the first test while failing real deployments. */
static void a_proxy_rejects_a_missing_credential(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_connect_proxy(&proxy_port, "device", "s3cret");

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-proxy-anon");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts;
  proxied_connect_options(&copts, cid, proxy_port);
  /* No proxy.username / proxy.password. */

  /* Either connect() refused it outright, or a failure event must actually have
   * been observed. Discarding the wait would let a client that hangs pending
   * forever pass once the timeout expired. */
  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  assert_false(saw_connected_ok(&rec));
  assert_int_equal(az_iot_test_proxy_tunnels_opened(proxy), 0);
  assert_true(az_iot_test_proxy_auth_failures(proxy) >= 1);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* The proxy accepts the connection, opens the tunnel, then the link dies
 * mid-session. A tunnelled session has to report that like any other transport
 * failure; silently hanging is the failure mode worth excluding, since the
 * proxy adds a hop that a client might not be watching. */
static void a_tunnel_dropped_mid_session_is_reported(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_connect_proxy(&proxy_port, NULL, NULL);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-proxy-drop");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts;
  proxied_connect_options(&copts, cid, proxy_port);

  assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
  assert_true(wait_until(c, &rec, saw_connected_ok, k_step_timeout_ms));
  assert_int_equal(az_iot_test_proxy_tunnels_opened(proxy), 1);

  az_iot_test_proxy_drop_now(proxy);
  assert_true(wait_until(c, &rec, saw_disconnected, k_step_timeout_ms));

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

/* A proxy that accepts the TCP connection and then refuses the tunnel, which is
 * what a filtering proxy does for a destination it does not allow. The client
 * must fail rather than fall back, and must not mistake the refusal for a
 * broker that answered. Driven by pointing the fixture's CONNECT at a port
 * nothing listens on, so the proxy answers 502. */
static void a_proxy_that_refuses_the_tunnel_fails_the_connect(void** state)
{
  (void)state;
  uint16_t proxy_port = 0;
  az_iot_test_proxy* proxy = start_connect_proxy(&proxy_port, NULL, NULL);

  char cid[64];
  unique_client_id(cid, sizeof(cid), "az-iot-conf-proxy-502");
  conf_recorder rec = { 0 };
  az_iot_mqtt_client* c = make_client();
  c->iface->set_inbound_cb(c, on_event, &rec);

  az_iot_mqtt_connect_options copts;
  proxied_connect_options(&copts, cid, proxy_port);
  /* A name that can never resolve, so the proxy's own upstream dial fails and
   * it answers 502. RFC 6761 reserves ".invalid" for exactly this, which makes
   * the case deterministic -- unlike aiming at a port assumed to be closed,
   * which depends on what happens to be listening on the machine. */
  copts.host = "az-iot-conformance.invalid";

  /* Either connect() refused it outright, or a failure event must actually have
   * been observed. Discarding the wait would let a client that hangs pending
   * forever pass once the timeout expired. */
  az_iot_result r = c->iface->connect(c, &copts);
  if (r == AZ_IOT_OK)
  {
    assert_true(wait_until(c, &rec, saw_connect_failure, k_step_timeout_ms));
  }
  assert_false(saw_connected_ok(&rec));
  assert_int_equal(az_iot_test_proxy_tunnels_opened(proxy), 0);

  (void)c->iface->disconnect(c);
  destroy_client(c);
  az_iot_test_proxy_stop(proxy);
}

#define AZ_IOT_CONFORMANCE_COMMON_TESTS                                                          \
  cmocka_unit_test(connect_disconnect_roundtrip), cmocka_unit_test(publish_subscribe_roundtrip), \
      cmocka_unit_test(a_client_initiated_disconnect_is_reported),                               \
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
      cmocka_unit_test(a_stalled_link_resumes_without_losing_the_session),                       \
      cmocka_unit_test(a_refused_subscribe_is_reported),                                         \
      cmocka_unit_test(a_truncated_publish_is_never_surfaced_as_a_message),                      \
      cmocka_unit_test(an_acknowledgement_for_an_unknown_packet_id_is_ignored),                  \
      cmocka_unit_test(a_key_reference_is_never_silently_ignored),                               \
      cmocka_unit_test(a_sign_hook_is_never_silently_ignored),                                   \
      cmocka_unit_test(a_websocket_request_is_never_silently_downgraded),                        \
      cmocka_unit_test(an_unreachable_proxy_is_never_bypassed)

/* Expands to nothing when the certificate cases were compiled out, so the two
 * lists above stay a single expression either way. */
#ifdef AZ_IOT_CONFORMANCE_WITH_TLS
#define TLS_TESTS                                                         \
  cmocka_unit_test(tls_handshake_succeeds_with_trusted_valid_cert),       \
      cmocka_unit_test(server_cert_validation_rejects_untrusted),         \
      cmocka_unit_test(server_cert_validation_rejects_expired),           \
      cmocka_unit_test(server_cert_validation_rejects_hostname_mismatch), \
      cmocka_unit_test(mutual_tls_succeeds_with_a_valid_client_cert),     \
      cmocka_unit_test(expired_client_cert_is_rejected),                  \
      cmocka_unit_test(a_missing_client_cert_is_rejected),
#else
#define TLS_TESTS
#endif

int az_iot_conformance_run(az_iot_conformance_suite suite_kind, az_iot_mqtt_factory* factory)
{
  return az_iot_conformance_run_with_options(suite_kind, factory, NULL);
}

int az_iot_conformance_run_with_options(
    az_iot_conformance_suite suite_kind,
    az_iot_mqtt_factory* factory,
    const az_iot_conformance_options* options)
{
  if (!factory)
  {
    return 1;
  }

  g_capabilities = options ? options->capabilities : 0u;
  g_key_uri = options ? options->key_uri : NULL;
  g_key_engine = options ? options->crypto_engine_id : NULL;
  g_client_cert_path = options ? options->client_cert_path : NULL;
  g_sign = options ? options->sign : NULL;
  g_sign_ctx = options ? options->sign_ctx : NULL;
  g_websocket_port = options ? options->websocket_port : 0;
  g_websocket_path = options ? options->websocket_path : NULL;

  /* Transport material supplied for a capability that was NOT declared is a
   * mistake, for the same reason it is on the custody routes: it would be
   * ignored in silence, and a run that ignores the thing it was given is
   * indistinguishable from one that never had it. */
  if (!adapter_claims_websockets() && g_websocket_port != 0)
  {
    fprintf(
        stderr,
        "conformance: websocket_port is set but AZ_IOT_CONFORMANCE_CAP_WEBSOCKETS was not "
        "declared\n");
    return 1;
  }

  /* Custody material is all-or-none PER ROUTE. A half-configured token is a
   * mistake, not an opt-out: the end-to-end case would be dropped and the run
   * would still say PASS.
   *
   *   URI route   key_uri + crypto_engine_id + client_cert_path
   *   sign route  sign (+ sign_ctx) + client_cert_path
   *
   * client_cert_path is shared, so one certificate serves both routes.
   *
   * Judged on EVERY field of a route, not on key_uri alone. Judging by key_uri
   * let the commonest misconfiguration through -- a typo in the variable that
   * carries the URI, with the engine and certificate set correctly -- and that
   * run would report "no key was supplied", which AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN
   * then downgrades to a notice. Material that was supplied and ignored would
   * have looked exactly like a machine that deliberately has no token. Do not
   * collapse this back to an aggregate test: a route's material must not be
   * able to hide behind the other route being complete. */
  const int custody_material = az_iot_conformance_custody_material_state(
      g_key_uri, g_key_engine, g_client_cert_path, g_sign != NULL, g_sign_ctx != NULL);

  if (custody_material == AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL)
  {
    fprintf(
        stderr,
        "conformance: key custody material is incomplete (key_uri=%s, crypto_engine_id=%s, "
        "sign=%s, sign_ctx=%s, client_cert_path=%s). The URI route needs key_uri + "
        "crypto_engine_id + client_cert_path; the sign route needs sign + client_cert_path, "
        "and sign_ctx is meaningful only alongside sign\n",
        (g_key_uri != NULL && g_key_uri[0] != '\0') ? "set" : "MISSING",
        (g_key_engine != NULL && g_key_engine[0] != '\0') ? "set" : "MISSING",
        (g_sign != NULL) ? "set" : "MISSING",
        (g_sign_ctx != NULL) ? "set" : "MISSING",
        (g_client_cert_path != NULL && g_client_cert_path[0] != '\0') ? "set" : "MISSING");
    return 1;
  }

  /* Material for a route that was not declared is rejected per route, not in
   * aggregate: declaring one route and supplying the other's material would
   * otherwise pass this check and leave that material silently unused. */
  if ((custody_material & AZ_IOT_CONFORMANCE_CUSTODY_URI) != 0 && !adapter_claims_key_custody_uri())
  {
    fprintf(
        stderr,
        "conformance: key URI custody material was supplied but "
        "AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI was not declared\n");
    return 1;
  }
  if ((custody_material & AZ_IOT_CONFORMANCE_CUSTODY_SIGN) != 0
      && !adapter_claims_key_custody_sign())
  {
    fprintf(
        stderr,
        "conformance: a sign hook was supplied but "
        "AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_SIGN was not declared\n");
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
    const struct CMUnitTest v5_tests[] = { AZ_IOT_CONFORMANCE_COMMON_TESTS,
                                           cmocka_unit_test(server_disconnect_is_reported),
                                           cmocka_unit_test(a_refused_publish_is_reported) };
    failed = cmocka_run_group_tests(v5_tests, NULL, NULL);
  }
  else
  {
    const struct CMUnitTest v3_tests[] = { AZ_IOT_CONFORMANCE_COMMON_TESTS };
    failed = cmocka_run_group_tests(v3_tests, NULL, NULL);
  }

  /* The end-to-end key custody case is a separate group because whether it runs
   * is a run-time fact -- it needs a real key -- and a cmocka test list is a
   * fixed array. */
  /* Each declared route is proved on its own. A shared capability would hold an
   * adapter to a route it never claimed, and -- because an unexercised claim
   * fails -- would leave a one-route adapter no way to be conformant at all. */
  if (adapter_claims_key_custody_uri())
  {
    const char* unproven = NULL;
#ifdef AZ_IOT_CONFORMANCE_WITH_TLS
    /* Gated on the classification, not on g_key_uri: an empty URI counts as
     * missing there, and testing the pointer here would send "" into the
     * handshake as though a key had been supplied. */
    if ((custody_material & AZ_IOT_CONFORMANCE_CUSTODY_URI) != 0)
    {
      const struct CMUnitTest custody_tests[]
          = { cmocka_unit_test(key_custody_completes_a_tls_handshake) };
      failed += cmocka_run_group_tests(custody_tests, NULL, NULL);
    }
    else
    {
      unproven = "no key was supplied. Set az_iot_conformance_options key_uri + crypto_engine_id +"
                 " client_cert_path to a key this adapter can reach and a certificate carrying its"
                 " public key.";
    }
#else
    unproven = "this build has no TLS support. Configure with"
               " -DAZ_IOT_BUILD_CONFORMANCE_TESTS_TLS=ON.";
#endif

    if (unproven != NULL)
    {
      char allow[16];
      failed += az_iot_conformance_report_unproven_capability(
          "AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI",
          unproven,
          read_env("AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN", allow, sizeof(allow)));
    }
  }

  if (adapter_claims_key_custody_sign())
  {
    const char* unproven = NULL;
#ifdef AZ_IOT_CONFORMANCE_WITH_TLS
    if ((custody_material & AZ_IOT_CONFORMANCE_CUSTODY_SIGN) != 0)
    {
      const struct CMUnitTest sign_tests[]
          = { cmocka_unit_test(key_custody_sign_hook_completes_a_tls_handshake) };
      failed += cmocka_run_group_tests(sign_tests, NULL, NULL);
    }
    else
    {
      unproven = "no sign hook was supplied. Set az_iot_conformance_options sign (+ sign_ctx) and"
                 " client_cert_path to a callback that signs with the key and a certificate"
                 " carrying its public key.";
    }
#else
    unproven = "this build has no TLS support. Configure with"
               " -DAZ_IOT_BUILD_CONFORMANCE_TESTS_TLS=ON.";
#endif

    if (unproven != NULL)
    {
      char allow[16];
      failed += az_iot_conformance_report_unproven_capability(
          "AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_SIGN",
          unproven,
          read_env("AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN", allow, sizeof(allow)));
    }
  }

  /* The transport capabilities, on the same terms: each is proved by a real
   * session over that transport, and a declaration the environment could not
   * exercise fails the run unless it was deliberately allowed. The baseline
   * refusal cases above ran for every adapter and do NOT count as proof -- they
   * show the adapter does not bypass the setting, not that it implements it. */
  if (adapter_claims_websockets())
  {
    if (g_websocket_port != 0)
    {
      const struct CMUnitTest ws_tests[]
          = { cmocka_unit_test(a_websocket_session_completes_a_roundtrip) };
      failed += cmocka_run_group_tests(ws_tests, NULL, NULL);
    }
    else
    {
      char allow[16];
      failed += az_iot_conformance_report_unproven_capability(
          "AZ_IOT_CONFORMANCE_CAP_WEBSOCKETS",
          "no WebSocket listener was supplied. Set az_iot_conformance_options websocket_port"
          " (and websocket_path when the broker does not serve the Azure default).",
          read_env("AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN", allow, sizeof(allow)));
    }
  }

  if (adapter_claims_proxy())
  {
    /* No environment needed: the proxy is the in-process fixture, so a declared
     * capability is always proved rather than sometimes reported unproven. The
     * negative cases run here too -- they are only meaningful against an
     * adapter that claims to implement the feature, since the baseline already
     * covers what a non-implementing adapter must do. */
    const struct CMUnitTest proxy_tests[]
        = { cmocka_unit_test(a_proxied_session_completes_a_roundtrip),
            cmocka_unit_test(a_proxy_accepts_correct_credentials),
            cmocka_unit_test(a_proxy_accepts_credentials_containing_delimiters),
            cmocka_unit_test(a_proxy_rejects_wrong_credentials),
            cmocka_unit_test(a_proxy_rejects_a_missing_credential),
            cmocka_unit_test(a_tunnel_dropped_mid_session_is_reported),
            cmocka_unit_test(a_proxy_that_refuses_the_tunnel_fails_the_connect) };
    failed += cmocka_run_group_tests(proxy_tests, NULL, NULL);
  }

  return (failed == 0) ? 0 : 1;
}
