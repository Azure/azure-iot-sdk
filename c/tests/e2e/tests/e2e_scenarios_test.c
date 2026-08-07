// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* In-process end-to-end scenarios against a real Azure IoT Hub / DPS instance.
 *
 * ONE process plays both halves of every scenario:
 *   - Device: the shipping SDK over the Paho MQTT adapter (provisioned via DPS
 *     with an X.509 individual enrollment, exactly like a real device).
 *   - Service (cloud): the az_iot_e2e_service facade, which receives telemetry,
 *     sends C2D, invokes direct methods and reads/patches twins. That facade
 *     hides the transport it uses (vendored AMQP + HTTPS/SAS) entirely; this
 *     test never includes an AMQP header, so the SDK's MQTT-only device charter
 *     is preserved at the test boundary.
 *
 * The two halves are driven cooperatively: each wait loop interleaves the
 * device's do_work with the service's poll/pump so neither side blocks the
 * other. Everything is single-threaded and non-blocking.
 *
 * Configuration comes entirely from the environment (set by the e2e CI job):
 *   Device (materialized to files by CI):
 *     AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID,
 *     AZ_IOT_CLIENT_CERT, AZ_IOT_CLIENT_KEY, AZ_IOT_TRUSTED_CA,
 *     AZ_IOT_DPS_GLOBAL_ENDPOINT (optional)
 *   Service:
 *     IOTHUB_CONNECTION_STRING, IOTHUB_EVENTHUB_CONNECTION_STRING,
 *     IOTHUB_EVENTHUB_LISTEN_NAME (optional), IOTHUB_EVENTHUB_PARTITION_COUNT (optional)
 *
 * The device id targeted by the service side is the DPS registration id (the
 * SDK's default: device id == registration id for individual enrollments).
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot.h"

#include "az_iot_e2e_service.h"
#include "e2e_device.h"
#include "e2e_log.h"

/* ---- timing budgets ------------------------------------------------------- */
#define E2E_TELEMETRY_TIMEOUT_S 90 /* publish + EH-side receive               */
#define E2E_C2D_TIMEOUT_S 60
#define E2E_METHOD_TIMEOUT_S 60
#define E2E_TWIN_TIMEOUT_S 60 /* per REST-request drive                  */
#define E2E_TWIN_PROP_TIMEOUT_S 90 /* reported-property propagation poll      */
#define E2E_CONNECT_TIMEOUT_S 45 /* cold service-AMQP connect retry budget  */
#define E2E_PUMP_MS 20 /* per do_work / poll slice                */

/* ---- shared fixture ------------------------------------------------------- */

typedef struct
{
  e2e_device dev;
  az_iot_e2e_service* service;
} e2e_fixture;

static e2e_fixture g_fixture;

/* Advance the device MQTT stack for a single slice. */
static void device_do_work(e2e_fixture* fx, int ms) { e2e_device_do_work(&fx->dev, ms); }

/* ---- unique correlation markers ------------------------------------------- */

/* Build a short, run-unique marker like "tag-1a2b3c4d5e6f". A fresh hub plus a
 * unique marker per scenario means the telemetry watcher never confuses an old
 * message for the one under test. */
static void make_marker(char* out, size_t cap, const char* tag)
{
  static unsigned counter = 0;
  unsigned long long t = (unsigned long long)time(NULL);
  unsigned r = (unsigned)rand();
  snprintf(out, cap, "%s-%08llx%04x%03x", tag, t & 0xffffffffull, r & 0xffff, (counter++) & 0xfff);
}

/* ---- group setup / teardown ----------------------------------------------- */

static int group_setup(void** state)
{
  memset(&g_fixture, 0, sizeof(g_fixture));
  srand((unsigned)time(NULL));

  e2e_install_log_sink();

  const char* svc_err = NULL;
  g_fixture.service = az_iot_e2e_service_create(&svc_err);
  if (g_fixture.service == NULL)
  {
    fprintf(
        stderr,
        "[e2e] service client create failed: %s\n",
        (svc_err != NULL) ? svc_err : "unknown");
    return -1;
  }

  if (e2e_device_connect(&g_fixture.dev) != 0)
  {
    az_iot_e2e_service_destroy(g_fixture.service);
    g_fixture.service = NULL;
    return -1;
  }

  *state = &g_fixture;
  return 0;
}

static int group_teardown(void** state)
{
  e2e_fixture* fx = (e2e_fixture*)*state;
  if (fx == NULL)
  {
    return 0;
  }
  e2e_device_disconnect(&fx->dev);
  if (fx->service != NULL)
  {
    az_iot_e2e_service_destroy(fx->service);
    fx->service = NULL;
  }
  return 0;
}

/* ---- telemetry ------------------------------------------------------------ */

typedef struct
{
  int done;
  az_iot_result status;
} send_ctx;

static void on_send_done(az_iot_result status, void* user_ctx)
{
  send_ctx* c = (send_ctx*)user_ctx;
  c->status = status;
  c->done = 1;
}

static void test_telemetry(void** state)
{
  e2e_fixture* fx = (e2e_fixture*)*state;

  char marker[64];
  make_marker(marker, sizeof(marker), "tele");

  char payload[128];
  snprintf(payload, sizeof(payload), "{\"e2e\":\"telemetry\",\"marker\":\"%s\"}", marker);

  /* Start listening on the Event Hub-compatible endpoint before sending so we
   * never miss the message. The cold AMQP handshake (TLS -> connection -> CBS
   * SAS -> per-partition receivers) against a freshly provisioned hub can fail
   * transiently (observed on the Windows leg: the peer closes the connection
   * during open), so retry with a short backoff up to E2E_CONNECT_TIMEOUT_S,
   * pumping the device in between to keep its MQTT connection warm. Each
   * attempt opens a brand-new connection (a failed one frees itself and
   * releases the transport's single TLS slot). */
  bool watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  for (time_t connect_start = time(NULL);
       !watching && (time(NULL) - connect_start) < E2E_CONNECT_TIMEOUT_S;)
  {
    for (int i = 0; i < 50; i++) /* ~1s backoff, device kept alive */
    {
      device_do_work(fx, E2E_PUMP_MS);
    }
    watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  }
  if (!watching)
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fprintf(stderr, "[e2e] telemetry watch begin failed: %s\n", (err != NULL) ? err : "unknown");
  }
  assert_true(watching);

  az_iot_telemetry_client telemetry_client;
  assert_int_equal(az_iot_telemetry_client_init(&telemetry_client, &fx->dev.conn), AZ_IOT_OK);

  az_iot_telemetry_property props[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
  };
  az_iot_telemetry_message msg = { 0 };
  msg.payload = (const uint8_t*)payload;
  msg.payload_len = strlen(payload);
  msg.properties = props;
  msg.properties_count = sizeof(props) / sizeof(props[0]);

  send_ctx sc = { 0 };
  assert_int_equal(
      az_iot_telemetry_client_send(&telemetry_client, &msg, on_send_done, &sc), AZ_IOT_OK);

  bool seen = false;
  time_t start = time(NULL);
  while ((time(NULL) - start) < E2E_TELEMETRY_TIMEOUT_S)
  {
    device_do_work(fx, E2E_PUMP_MS);
    assert_true(az_iot_e2e_service_do_work(fx->service, E2E_PUMP_MS));
    if (az_iot_e2e_service_telemetry_seen(fx->service, marker))
    {
      seen = true;
      break;
    }
  }

  /* The Event Hub can observe the message before the device's own send
   * acknowledgement (PUBACK) has been pumped in, so breaking on `seen` alone
   * races the send. Drain the device briefly until the send completes. */
  for (time_t ack = time(NULL); !sc.done && (time(NULL) - ack) < 5;)
  {
    device_do_work(fx, E2E_PUMP_MS);
  }

  az_iot_telemetry_client_destroy(&telemetry_client);
  /* Release the AMQP/TLS connection before the next scenario (the Windows
   * reference transport allows only one TLS connection at a time). */
  az_iot_e2e_service_telemetry_watch_end(fx->service);

  assert_true(sc.done);
  assert_int_equal(sc.status, AZ_IOT_OK);
  if (!seen)
  {
    fprintf(
        stderr,
        "[e2e] telemetry marker '%s' not observed within %ds\n",
        marker,
        E2E_TELEMETRY_TIMEOUT_S);
  }
  assert_true(seen);
}

/* ---- cloud-to-device ------------------------------------------------------ */

typedef struct
{
  char expected[64];
  bool received;
  bool matched;
} c2d_ctx;

static void on_c2d(const az_iot_c2d_message* msg, void* user_ctx)
{
  c2d_ctx* c = (c2d_ctx*)user_ctx;
  size_t expected_len = strlen(c->expected);
  c->matched = (msg->payload_len == expected_len)
      && (memcmp(msg->payload, c->expected, expected_len) == 0);
  c->received = true;
}

static void test_c2d(void** state)
{
  e2e_fixture* fx = (e2e_fixture*)*state;

  c2d_ctx cctx = { 0 };
  make_marker(cctx.expected, sizeof(cctx.expected), "c2d");

  az_iot_c2d_client c2d;
  assert_int_equal(az_iot_c2d_client_init(&c2d, &fx->dev.conn), AZ_IOT_OK);
  assert_int_equal(az_iot_c2d_client_set_handler(&c2d, on_c2d, &cctx), AZ_IOT_OK);

  /* Give the subscription a few work slices to settle before the cloud sends. */
  for (int i = 0; i < 20; ++i)
  {
    device_do_work(fx, E2E_PUMP_MS);
  }

  /* The service C2D send opens its own short-lived AMQP connection, whose cold
   * handshake can be refused transiently (same Windows "peer closed the
   * connection" as telemetry). Retry with a short backoff up to
   * E2E_CONNECT_TIMEOUT_S, pumping the device in between; each attempt opens a
   * fresh connection (a failed one tears itself down and releases the TLS slot). */
  bool sent = az_iot_e2e_service_send_c2d(
      fx->service, fx->dev.device_id, (const uint8_t*)cctx.expected, strlen(cctx.expected));
  for (time_t connect_start = time(NULL);
       !sent && (time(NULL) - connect_start) < E2E_CONNECT_TIMEOUT_S;)
  {
    for (int i = 0; i < 50; i++) /* ~1s backoff, device kept alive */
    {
      device_do_work(fx, E2E_PUMP_MS);
    }
    sent = az_iot_e2e_service_send_c2d(
        fx->service, fx->dev.device_id, (const uint8_t*)cctx.expected, strlen(cctx.expected));
  }
  if (!sent)
  {
    fprintf(stderr, "[e2e] c2d send failed: %s\n", az_iot_e2e_service_last_error(fx->service));
  }
  assert_true(sent);

  time_t start = time(NULL);
  while (!cctx.received && (time(NULL) - start) < E2E_C2D_TIMEOUT_S)
  {
    device_do_work(fx, E2E_PUMP_MS);
  }

  az_iot_c2d_client_destroy(&c2d);

  assert_true(cctx.received);
  assert_true(cctx.matched);
}

/* ---- direct method -------------------------------------------------------- */

static void on_method(
    az_iot_direct_method_request* request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  (void)method_name;
  (void)user_ctx;
  /* Echo the request payload back with a 200. */
  az_iot_result rc = az_iot_direct_method_respond(request, 200, payload, payload_len);
  (void)rc;
}

static void test_direct_method(void** state)
{
  e2e_fixture* fx = (e2e_fixture*)*state;

  az_iot_direct_method_client dm;
  assert_int_equal(az_iot_direct_method_client_init(&dm, &fx->dev.conn), AZ_IOT_OK);
  assert_int_equal(az_iot_direct_method_client_set_handler(&dm, on_method, NULL), AZ_IOT_OK);

  for (int i = 0; i < 20; ++i)
  {
    device_do_work(fx, E2E_PUMP_MS);
  }

  assert_true(
      az_iot_e2e_service_method_invoke_begin(fx->service, fx->dev.device_id, "echo", "\"ping\""));

  int status = 0;
  int rc = 0;
  char resp[512] = { 0 };
  time_t start = time(NULL);
  while ((time(NULL) - start) < E2E_METHOD_TIMEOUT_S)
  {
    device_do_work(fx, E2E_PUMP_MS);
    rc = az_iot_e2e_service_request_poll(fx->service, &status, resp, sizeof(resp));
    if (rc != 0)
    {
      break;
    }
  }

  az_iot_direct_method_client_destroy(&dm);

  if (rc != 1)
  {
    fprintf(
        stderr, "[e2e] method poll rc=%d: %s\n", rc, az_iot_e2e_service_last_error(fx->service));
  }
  assert_int_equal(rc, 1);
  assert_int_equal(status, 200);
  assert_non_null(strstr(resp, "ping"));
}

/* ---- twin ----------------------------------------------------------------- */

typedef struct
{
  char expected[64];
  bool received;
  bool matched;
} desired_ctx;

static void on_desired(const uint8_t* patch, size_t patch_len, uint64_t version, void* user_ctx)
{
  (void)version;
  desired_ctx* d = (desired_ctx*)user_ctx;
  /* patch is not NUL-terminated; scan the delivered range for the marker. */
  size_t needle_len = strlen(d->expected);
  if (patch_len >= needle_len)
  {
    for (size_t i = 0; i + needle_len <= patch_len; ++i)
    {
      if (memcmp(patch + i, d->expected, needle_len) == 0)
      {
        d->matched = true;
        break;
      }
    }
  }
  d->received = true;
}

typedef struct
{
  int done;
  az_iot_result status;
} patch_ack_ctx;

static void on_patch_ack(az_iot_result status, void* user_ctx)
{
  patch_ack_ctx* p = (patch_ack_ctx*)user_ctx;
  p->status = status;
  p->done = 1;
}

/* Drive an in-flight REST request to completion while keeping the device serviced. */
static int drive_request(e2e_fixture* fx, int* status, char* resp, size_t resp_size, int timeout_s)
{
  int rc = 0;
  time_t start = time(NULL);
  while ((time(NULL) - start) < timeout_s)
  {
    device_do_work(fx, E2E_PUMP_MS);
    rc = az_iot_e2e_service_request_poll(fx->service, status, resp, resp_size);
    if (rc != 0)
    {
      break;
    }
  }
  return rc;
}

static void test_twin(void** state)
{
  e2e_fixture* fx = (e2e_fixture*)*state;

  az_iot_twin_client twin;
  assert_int_equal(az_iot_twin_client_init(&twin, &fx->dev.conn), AZ_IOT_OK);

  /* --- desired: cloud patches, device observes ------------------------- */
  desired_ctx dctx = { 0 };
  make_marker(dctx.expected, sizeof(dctx.expected), "desired");
  assert_int_equal(az_iot_twin_client_subscribe_desired(&twin, on_desired, &dctx), AZ_IOT_OK);

  for (int i = 0; i < 20; ++i)
  {
    device_do_work(fx, E2E_PUMP_MS);
  }

  char desired_json[128];
  snprintf(desired_json, sizeof(desired_json), "{\"cfg\":\"%s\"}", dctx.expected);
  assert_true(
      az_iot_e2e_service_twin_patch_desired_begin(fx->service, fx->dev.device_id, desired_json));

  int status = 0;
  /* The twin GET returns the WHOLE twin (device-level fields + desired +
   * reported, each with $metadata). Size the buffer generously so a populated
   * desired section can never push the reported marker past the end and cause
   * a false negative -- the facade truncates the copied body to this size. */
  char resp[16384] = { 0 };
  int rc = drive_request(fx, &status, resp, sizeof(resp), E2E_TWIN_TIMEOUT_S);
  if (rc != 1)
  {
    fprintf(
        stderr,
        "[e2e] twin desired patch rc=%d: %s\n",
        rc,
        az_iot_e2e_service_last_error(fx->service));
  }
  assert_int_equal(rc, 1);
  assert_int_equal(status, 200);

  time_t start = time(NULL);
  while (!dctx.received && (time(NULL) - start) < E2E_TWIN_TIMEOUT_S)
  {
    device_do_work(fx, E2E_PUMP_MS);
  }
  assert_true(dctx.received);
  assert_true(dctx.matched);

  /* --- reported: device patches, cloud reads --------------------------- */
  char reported_marker[64];
  make_marker(reported_marker, sizeof(reported_marker), "reported");
  char reported_json[128];
  snprintf(reported_json, sizeof(reported_json), "{\"rep\":\"%s\"}", reported_marker);

  patch_ack_ctx pack = { 0 };
  assert_int_equal(
      az_iot_twin_client_patch_reported(
          &twin, (const uint8_t*)reported_json, strlen(reported_json), on_patch_ack, &pack),
      AZ_IOT_OK);

  start = time(NULL);
  while (!pack.done && (time(NULL) - start) < E2E_TWIN_TIMEOUT_S)
  {
    device_do_work(fx, E2E_PUMP_MS);
  }
  assert_true(pack.done);
  assert_int_equal(pack.status, AZ_IOT_OK);

  /* Reported-property propagation to the REST twin store is eventually
   * consistent: even after the device's PATCH-reported is ACKed, the value can
   * take a moment to appear in a service-side twin GET (observed flaky on the
   * Windows leg). Re-read until the reported marker shows up, bounded by the
   * same wall-clock timeout the rest of this suite uses, pumping the device
   * (~1s) between attempts. */
  bool reported_seen = false;
  start = time(NULL);
  while (!reported_seen && (time(NULL) - start) < E2E_TWIN_PROP_TIMEOUT_S)
  {
    assert_true(az_iot_e2e_service_twin_get_begin(fx->service, fx->dev.device_id));
    status = 0;
    memset(resp, 0, sizeof(resp));
    rc = drive_request(fx, &status, resp, sizeof(resp), E2E_TWIN_TIMEOUT_S);
    if (rc != 1)
    {
      fprintf(stderr, "[e2e] twin get rc=%d: %s\n", rc, az_iot_e2e_service_last_error(fx->service));
    }
    assert_int_equal(rc, 1);
    assert_int_equal(status, 200);

    reported_seen = (strstr(resp, reported_marker) != NULL);
    if (!reported_seen)
    {
      for (int i = 0; i < 50; ++i)
        device_do_work(fx, E2E_PUMP_MS);
    }
  }
  assert_true(reported_seen);

  az_iot_twin_client_destroy(&twin);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_telemetry),
    cmocka_unit_test(test_c2d),
    cmocka_unit_test(test_direct_method),
    cmocka_unit_test(test_twin),
  };
  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
