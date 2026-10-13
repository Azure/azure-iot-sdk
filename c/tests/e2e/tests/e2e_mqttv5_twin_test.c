// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end: desired-property delivery of az_iot_mqttv5_twin_client on an mqttv5 hub, with
 * and without options.twin_push.push_desired.
 *
 * Each test opens its own connection: the push mode is read from the birth, so it is fixed per
 * connection. The service side patches desired properties over REST (az_iot_e2e_service.h). The
 * tests run in order on one device: the pull test leaves desired state for the push test's
 * snapshot on connect.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot.h"

#include "az_iot_e2e_service.h"
#include "e2e_device.h"
#include "e2e_log.h"

#define E2E_REQUEST_TIMEOUT_S 60
#define E2E_DELIVERY_TIMEOUT_S 60
/* How long pull mode must stay without a patch after a desired change. */
#define E2E_NO_PATCH_WINDOW_S 30
#define E2E_SETTLE_S 5
#define E2E_PUMP_MS 20

/** @brief Desired deliveries carrying the marker being waited for. */
typedef struct
{
  char marker[64];
  int deliveries; /* all deliveries */
  int matches; /* deliveries containing marker */
  az_iot_mqttv5_twin_desired_kind match_kind;
  uint64_t version;
} desired_record;

typedef struct
{
  bool done;
  az_iot_result status;
  uint64_t desired_version;
} get_record;

typedef struct
{
  e2e_device dev;
  az_iot_e2e_service* service;
  az_iot_mqttv5_twin_client twin;
  bool twin_ok;
  desired_record desired;
  get_record get;
} fixture;

static fixture g_fx;
/* Last desired marker the service set; empty until a test sets one. */
static char g_last_marker[64];

static bool contains(const uint8_t* buf, size_t len, const char* needle)
{
  const size_t n = strlen(needle);
  for (size_t i = 0; buf != NULL && n != 0 && i + n <= len; ++i)
  {
    if (memcmp(buf + i, needle, n) == 0)
    {
      return true;
    }
  }
  return false;
}

static void on_desired(
    az_iot_mqttv5_twin_desired_kind kind,
    uint64_t version,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  desired_record* r = (desired_record*)user_ctx;
  r->deliveries++;
  r->version = version;
  printf(
      "[e2e] desired %s v%llu (%zu bytes)\n",
      kind == AZ_IOT_MQTTV5_TWIN_DESIRED_SNAPSHOT ? "snapshot" : "patch",
      (unsigned long long)version,
      payload_len);
  if (contains(payload, payload_len, r->marker))
  {
    r->matches++;
    r->match_kind = kind;
  }
}

static void on_get(az_iot_result status, const az_iot_mqttv5_twin_state* twin, void* user_ctx)
{
  get_record* r = (get_record*)user_ctx;
  r->status = status;
  r->desired_version = twin ? twin->desired.version : 0;
  r->done = true;
}

/* The time separates runs on a shared device; the counter separates markers within a run. */
static void make_marker(char* out, size_t cap, const char* tag)
{
  static unsigned counter = 0;
  snprintf(out, cap, "%s-%08lx%03x", tag, (unsigned long)time(NULL), (counter++) & 0xfffu);
}

static void pump(fixture* fx)
{
  e2e_device_do_work(&fx->dev, E2E_PUMP_MS);
  (void)az_iot_mqttv5_twin_client_do_work(&fx->twin);
}

static void pump_for(fixture* fx, int seconds)
{
  for (time_t start = time(NULL); (time(NULL) - start) < seconds;)
  {
    pump(fx);
  }
}

/* Attached before open() so a snapshot pushed on connect is not missed. */
static int attach_twin(e2e_device* dev, void* ctx)
{
  fixture* fx = (fixture*)ctx;
  if (az_iot_mqttv5_twin_client_init(&fx->twin, &dev->conn) != AZ_IOT_OK)
  {
    return 1;
  }
  fx->twin_ok = true;
  return az_iot_mqttv5_twin_client_set_desired_handler(&fx->twin, on_desired, &fx->desired)
          == AZ_IOT_OK
      ? 0
      : 1;
}

static int connect_device(fixture* fx, bool push_desired)
{
  memset(fx, 0, sizeof(*fx));
  /* Set before open(): a snapshot pushed on connect may arrive before CONNECTED. */
  snprintf(fx->desired.marker, sizeof(fx->desired.marker), "%s", g_last_marker);
  e2e_install_log_sink();

  const char* svc_err = NULL;
  fx->service = az_iot_e2e_service_create(&svc_err);
  if (fx->service == NULL)
  {
    fprintf(stderr, "[e2e] service create failed: %s\n", svc_err ? svc_err : "unknown");
    return -1;
  }

  e2e_device_connect_options opts = { 0 };
  opts.twin_push_desired = push_desired;
  opts.before_open = attach_twin;
  opts.before_open_ctx = fx;
  if (e2e_device_connect_with(&fx->dev, &opts) != 0)
  {
    return -1;
  }

  az_iot_hub_profile profile;
  if (az_iot_connection_client_get_hub_profile(&fx->dev.conn, &profile) != AZ_IOT_OK
      || profile.connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    fprintf(stderr, "[e2e] the device is not on an mqttv5 hub\n");
    return -1;
  }
  return 0;
}

static int teardown(void** state);

/* cmocka skips teardown when setup fails, so a failed connect cleans up here. */
static int setup_with(void** state, bool push_desired)
{
  *state = &g_fx;
  if (connect_device(&g_fx, push_desired) != 0)
  {
    (void)teardown(state);
    return -1;
  }
  return 0;
}

static int setup_pull(void** state) { return setup_with(state, false); }

static int setup_push(void** state) { return setup_with(state, true); }

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx == NULL)
  {
    fx = &g_fx;
  }
  if (fx->twin_ok)
  {
    az_iot_mqttv5_twin_client_deinit(&fx->twin);
    fx->twin_ok = false;
  }
  e2e_device_disconnect(&fx->dev);
  if (fx->service != NULL)
  {
    az_iot_e2e_service_destroy(fx->service);
    fx->service = NULL;
  }
  return 0;
}

/* Sets desired property @p name to a new marker, kept in fx->desired.marker and g_last_marker. */
static void service_patch_desired(fixture* fx, const char* name)
{
  make_marker(fx->desired.marker, sizeof(fx->desired.marker), name);
  fx->desired.matches = 0;
  char json[128];
  snprintf(json, sizeof(json), "{\"%s\":\"%s\"}", name, fx->desired.marker);

  assert_true(az_iot_e2e_service_twin_patch_desired_begin(fx->service, fx->dev.device_id, json));
  int status = 0;
  char resp[1024];
  int rc = 0;
  for (time_t start = time(NULL); rc == 0 && (time(NULL) - start) < E2E_REQUEST_TIMEOUT_S;)
  {
    pump(fx);
    rc = az_iot_e2e_service_request_poll(fx->service, &status, resp, sizeof(resp));
  }
  if (rc != 1 || status != 200)
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fail_msg(
        "desired patch failed: rc=%d http=%d %s %s",
        rc,
        status,
        err ? err : "",
        rc == 1 ? resp : "");
  }
  snprintf(g_last_marker, sizeof(g_last_marker), "%s", fx->desired.marker);
}

static bool wait_match(fixture* fx, int timeout_s)
{
  for (time_t start = time(NULL); fx->desired.matches == 0 && (time(NULL) - start) < timeout_s;)
  {
    pump(fx);
  }
  return fx->desired.matches != 0;
}

/* Without push_desired the service sends no desired patches while connected; a GET finds the
 * change and the client delivers it as a snapshot. */
static void pull_mode_sees_a_desired_change_only_after_a_get(void** state)
{
  fixture* fx = (fixture*)*state;
  pump_for(fx, E2E_SETTLE_S);

  service_patch_desired(fx, "e2ePull");
  pump_for(fx, E2E_NO_PATCH_WINDOW_S);
  if (fx->desired.matches != 0)
  {
    fail_msg("pull mode: desired change delivered without a GET");
  }

  assert_int_equal(az_iot_mqttv5_twin_client_get(&fx->twin, on_get, &fx->get), AZ_IOT_OK);
  assert_true(wait_match(fx, E2E_DELIVERY_TIMEOUT_S));
  assert_true(fx->get.done);
  assert_int_equal(fx->get.status, AZ_IOT_OK);
  assert_int_equal(fx->desired.match_kind, AZ_IOT_MQTTV5_TWIN_DESIRED_SNAPSHOT);
  assert_int_equal(fx->desired.version, fx->get.desired_version);
}

/* With push_desired the service pushes the current desired state on connect. */
static void push_mode_pushes_a_snapshot_on_connect(void** state)
{
  fixture* fx = (fixture*)*state;
  if (g_last_marker[0] == '\0')
  {
    service_patch_desired(fx, "e2ePush");
    teardown(state);
    assert_int_equal(connect_device(fx, true), 0);
  }
  assert_true(wait_match(fx, E2E_DELIVERY_TIMEOUT_S));
  assert_int_equal(fx->desired.match_kind, AZ_IOT_MQTTV5_TWIN_DESIRED_SNAPSHOT);
}

/* With push_desired each desired change arrives as a patch while connected. */
static void push_mode_delivers_each_desired_patch(void** state)
{
  fixture* fx = (fixture*)*state;
  pump_for(fx, E2E_SETTLE_S);
  for (int i = 0; i < 2; ++i)
  {
    service_patch_desired(fx, "e2ePush");
    assert_true(wait_match(fx, E2E_DELIVERY_TIMEOUT_S));
    assert_int_equal(fx->desired.match_kind, AZ_IOT_MQTTV5_TWIN_DESIRED_PATCH);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(
        pull_mode_sees_a_desired_change_only_after_a_get, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(push_mode_pushes_a_snapshot_on_connect, setup_push, teardown),
    cmocka_unit_test_setup_teardown(push_mode_delivers_each_desired_patch, setup_push, teardown),
  };
  return cmocka_run_group_tests_name("e2e_mqttv5_twin", tests, NULL, NULL);
}
