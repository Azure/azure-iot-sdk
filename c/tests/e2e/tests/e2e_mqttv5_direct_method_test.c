// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end: az_iot_mqttv5_direct_method_client on an mqttv5 hub.
 *
 * One connected device for the suite; each test declares the methods it needs and the per-test
 * teardown withdraws them. The service side invokes over REST (az_iot_e2e_service.h) with
 * request handles, so several invocations can be in flight at once (Linux only). The handler
 * only records each invocation; the test decides when and how to answer it.
 *
 * Every service outcome is printed with its HTTP status and body. Probe rejections are asserted
 * by the codes the mqttv5 hub returns: 404/404103 method not found, 403/403801 device busy,
 * 400/400801 insufficient time, 400/400800 service timeout. 400004 (an invalid request) never
 * counts as a refusal.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot.h"

#include "az_iot_e2e_service.h"
#include "e2e_device.h"
#include "e2e_log.h"

/* responseTimeoutInSeconds of an ordinary invocation, and the cap on connectTimeoutInSeconds,
 * which the service refuses (400004) when it exceeds responseTimeoutInSeconds. */
#define E2E_RESPONSE_TIMEOUT_S 30
#define E2E_CONNECT_TIMEOUT_S 30
/* Local bound on any one invocation completing. */
#define E2E_CALL_DEADLINE_S 90
/* A probe rejection must come back well inside E2E_RESPONSE_TIMEOUT_S. */
#define E2E_REFUSAL_MAX_S 15
/* How long a refused invocation is watched for a handler that should not run. */
#define E2E_QUIET_S 3
/* responseTimeoutInSeconds of the invocations left unanswered, and the slack on it. */
#define E2E_SHORT_TIMEOUT_S 5
#define E2E_TIMEOUT_SLACK_S 25
/* Extra wait past the service timeout before answering late. */
#define E2E_LATE_SLACK_S 3
/* responseTimeoutInSeconds below a method's declared minimum. */
#define E2E_INSUFFICIENT_TIMEOUT_S 10
#define E2E_SLOW_METHOD_MIN_S 30
/* Refusal bound for E2E_INSUFFICIENT_TIMEOUT_S: elapsed is whole seconds, so 7 means < 8 s, which
 * leaves 2 s margin before a plain service timeout at 10 s could pass as a probe rejection. */
#define E2E_INSUFFICIENT_REFUSAL_MAX_S 7
/* Service codes for a refused or timed-out invocation. */
#define E2E_HTTP_NOT_FOUND 404
#define E2E_CODE_NOT_FOUND "404103"
#define E2E_HTTP_BUSY 403
#define E2E_CODE_BUSY "403801"
#define E2E_HTTP_INSUFFICIENT_TIME 400
#define E2E_CODE_INSUFFICIENT_TIME "400801"
#define E2E_HTTP_TIMEOUT 400
#define E2E_CODE_TIMEOUT "400800"
#define E2E_SETTLE_S 5
#define E2E_PUMP_MS 20
#define E2E_MARKER_MAX 64
#define E2E_PAYLOAD_MAX 1024
#define E2E_BODY_MAX 4096
#define E2E_MAX_CALLS 8
#define E2E_MAX_DECLARED 4
/* Parallel invocations: the device's limit, capped by the service's request handles. */
#define E2E_CONCURRENT                                                       \
  ((AZ_IOT_MQTTV5_DM_MAX_CONCURRENT) < (AZ_IOT_E2E_SERVICE_MAX_REQUESTS - 1) \
       ? (AZ_IOT_MQTTV5_DM_MAX_CONCURRENT)                                   \
       : (AZ_IOT_E2E_SERVICE_MAX_REQUESTS - 1))

/** @brief One invocation the handler received, held until the test answers it. */
typedef struct
{
  az_iot_direct_method_request request;
  char method_name[AZ_IOT_DM_METHOD_NAME_MAX];
  char payload[E2E_PAYLOAD_MAX]; /* NUL-terminated copy */
  size_t payload_len;
  bool answered; /* respond() released the slot */
} invocation;

/** @brief One service-side invocation and its outcome. */
typedef struct
{
  char method[AZ_IOT_DM_METHOD_NAME_MAX];
  int handle;
  bool done;
  int rc; /* request_poll result: 1 complete, -1 transport error */
  int http_status;
  char body[E2E_BODY_MAX]; /* response body, or the error when rc != 1 */
  time_t started;
  long elapsed_s;
} service_call;

typedef struct
{
  e2e_device dev;
  az_iot_e2e_service* service;
  az_iot_mqttv5_direct_method_client dm;
  bool dm_ok;
  invocation calls[E2E_MAX_CALLS];
  size_t calls_used;
  int probes;
  az_iot_mqttv5_direct_method_probe_result probe_answer;
  const char* declared[E2E_MAX_DECLARED];
  size_t declared_count;
} fixture;

static fixture g_fx;

static void on_method(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  printf("[e2e] handler: '%s' (%zu bytes)\n", method_name ? method_name : "(null)", payload_len);
  if (fx->calls_used == E2E_MAX_CALLS)
  {
    printf("[e2e] handler: no room to record it\n");
    return;
  }
  invocation* call = &fx->calls[fx->calls_used++];
  memset(call, 0, sizeof(*call));
  call->request = request;
  snprintf(call->method_name, sizeof(call->method_name), "%s", method_name ? method_name : "");
  size_t copy = payload_len < sizeof(call->payload) - 1 ? payload_len : sizeof(call->payload) - 1;
  if (payload != NULL && copy > 0)
  {
    memcpy(call->payload, payload, copy);
  }
  call->payload[copy] = '\0';
  call->payload_len = payload_len;
}

static az_iot_mqttv5_direct_method_probe_result on_probe(
    const az_iot_mqttv5_direct_method_probe* probe,
    void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  fx->probes++;
  printf(
      "[e2e] probe: '%s', response timeout %us, answering %d\n",
      probe->method_name,
      (unsigned)probe->response_timeout_seconds,
      (int)fx->probe_answer);
  return fx->probe_answer;
}

/* The time separates runs on a shared device; the counter separates markers within a run. */
static void make_marker(char* out, size_t cap, const char* tag)
{
  static unsigned counter = 0;
  snprintf(out, cap, "%s-%08lx%03x", tag, (unsigned long)time(NULL), (counter++) & 0xfffu);
}

static void pump_for(fixture* fx, int seconds)
{
  for (time_t start = time(NULL); (time(NULL) - start) < seconds;)
  {
    e2e_device_do_work(&fx->dev, E2E_PUMP_MS);
  }
}

/** @brief Declares @p name for this test; the test teardown withdraws it. */
static void declare(fixture* fx, const char* name, uint32_t minimum_execution_seconds)
{
  assert_true(fx->declared_count < E2E_MAX_DECLARED);
  assert_int_equal(
      az_iot_mqttv5_direct_method_client_register_method(
          &fx->dm, name, minimum_execution_seconds, on_method, fx),
      AZ_IOT_OK);
  fx->declared[fx->declared_count++] = name;
}

/** @brief Starts invoking @p method with @p json_payload (NULL for none). */
static void call_start(
    fixture* fx,
    service_call* c,
    const char* method,
    const char* json_payload,
    int response_timeout_s)
{
  const int connect_timeout_s
      = response_timeout_s < E2E_CONNECT_TIMEOUT_S ? response_timeout_s : E2E_CONNECT_TIMEOUT_S;
  memset(c, 0, sizeof(*c));
  snprintf(c->method, sizeof(c->method), "%s", method);
  c->started = time(NULL);
  printf(
      "[e2e] invoke '%s' (response %ds, connect %ds): %s\n",
      method,
      response_timeout_s,
      connect_timeout_s,
      json_payload ? json_payload : "(no payload)");
  c->handle = az_iot_e2e_service_method_invoke_start(
      fx->service, fx->dev.device_id, method, json_payload, response_timeout_s, connect_timeout_s);
  if (c->handle < 0)
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fail_msg("invoke '%s' not started: %s", method, err ? err : "unknown");
  }
}

static void call_poll(fixture* fx, service_call* c)
{
  if (c->done)
  {
    return;
  }
  c->rc = az_iot_e2e_service_request_poll_handle(
      fx->service, c->handle, &c->http_status, c->body, sizeof(c->body));
  if (c->rc == 0)
  {
    return;
  }
  c->done = true;
  c->elapsed_s = (long)(time(NULL) - c->started);
  if (c->rc != 1)
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    snprintf(c->body, sizeof(c->body), "transport error: %s", err ? err : "unknown");
  }
  printf(
      "[e2e] '%s' completed after %lds: rc=%d http=%d body(%zu)=%s\n",
      c->method,
      c->elapsed_s,
      c->rc,
      c->http_status,
      strlen(c->body),
      c->body);
}

/** @brief Pumps the device once and polls every unfinished call. */
static bool pump_calls(fixture* fx, service_call* calls, size_t n)
{
  e2e_device_do_work(&fx->dev, E2E_PUMP_MS);
  bool all_done = true;
  for (size_t i = 0; i < n; ++i)
  {
    call_poll(fx, &calls[i]);
    all_done = all_done && calls[i].done;
  }
  return all_done;
}

/** @brief Waits for @p wanted handler invocations; fails, with the call states, without them. */
static void expect_invocations(fixture* fx, service_call* calls, size_t n, size_t wanted)
{
  for (time_t start = time(NULL);
       fx->calls_used < wanted && (time(NULL) - start) < E2E_CALL_DEADLINE_S;)
  {
    if (pump_calls(fx, calls, n))
    {
      break;
    }
  }
  if (fx->calls_used < wanted)
  {
    for (size_t i = 0; i < n; ++i)
    {
      printf(
          "[e2e] call %zu '%s': done=%d rc=%d http=%d body=%s\n",
          i,
          calls[i].method,
          (int)calls[i].done,
          calls[i].rc,
          calls[i].http_status,
          calls[i].done ? calls[i].body : "");
    }
    fail_msg("%zu of %zu invocations reached the handler", fx->calls_used, wanted);
  }
}

/** @brief Waits for every call to complete within @p timeout_s. */
static void wait_done(fixture* fx, service_call* calls, size_t n, int timeout_s)
{
  bool all_done = false;
  for (time_t start = time(NULL); !all_done && (time(NULL) - start) < timeout_s;)
  {
    all_done = pump_calls(fx, calls, n);
  }
  for (size_t i = 0; i < n; ++i)
  {
    if (!calls[i].done)
    {
      fail_msg("'%s' did not complete within %ds", calls[i].method, timeout_s);
    }
  }
}

/** @brief The device status in a {"status":N,...} result body, or -1 without one. */
static int result_status(const char* body)
{
  const char* p = strstr(body, "\"status\"");
  if (p == NULL)
  {
    return -1;
  }
  p += strlen("\"status\"");
  while (*p == ' ' || *p == '\t' || *p == ':')
  {
    p++;
  }
  if (*p < '0' || *p > '9')
  {
    return -1;
  }
  return (int)strtol(p, NULL, 10);
}

/** @brief The call completed with a final HTTP status (not 0 from a dropped connection). */
static bool http_final(const service_call* c)
{
  return c->rc == 1 && c->http_status >= 200 && c->http_status <= 599;
}

static bool http_ok(const service_call* c)
{
  return c->rc == 1 && c->http_status >= 200 && c->http_status < 300;
}

/** @brief The service rejected the request itself (400004, e.g. bad timeouts). */
static bool request_invalid(const service_call* c) { return strstr(c->body, "400004") != NULL; }

/** @brief The device answered: HTTP 2xx carrying @p device_status. */
static void assert_answered(const service_call* c, int device_status)
{
  if (!http_ok(c) || result_status(c->body) != device_status)
  {
    fail_msg(
        "'%s': expected HTTP 2xx with device status %d; got rc=%d http=%d body=%s",
        c->method,
        device_status,
        c->rc,
        c->http_status,
        c->body);
  }
}

/** @brief The service refused the call within @p max_elapsed_s: HTTP @p http with @p code in the
 *         body. */
static void assert_refused(const service_call* c, int http, const char* code, long max_elapsed_s)
{
  if (!http_final(c) || http_ok(c) || request_invalid(c))
  {
    fail_msg(
        "'%s': expected a non-2xx refusal of a valid request; got rc=%d http=%d body=%s",
        c->method,
        c->rc,
        c->http_status,
        c->body);
  }
  if (c->http_status != http || strstr(c->body, code) == NULL)
  {
    fail_msg(
        "'%s': expected HTTP %d with errorCode %s; got http=%d body=%s",
        c->method,
        http,
        code,
        c->http_status,
        c->body);
  }
  if (c->elapsed_s > max_elapsed_s)
  {
    fail_msg(
        "'%s': refused after %lds, expected within %lds (http=%d body=%s)",
        c->method,
        c->elapsed_s,
        max_elapsed_s,
        c->http_status,
        c->body);
  }
}

/** @brief Answers invocation @p i; the slot stays held only on NOT_ENOUGH_SPACE. */
static az_iot_result answer(fixture* fx, size_t i, int status, const char* body, size_t body_len)
{
  invocation* call = &fx->calls[i];
  az_iot_result result = az_iot_mqttv5_direct_method_respond(
      &fx->dm, call->request, status, (const uint8_t*)body, body_len);
  printf(
      "[e2e] respond '%s' status %d (%zu bytes): %s\n",
      call->method_name,
      status,
      body_len,
      az_iot_result_to_string(result));
  call->answered = result != AZ_IOT_ERR_NOT_ENOUGH_SPACE && result != AZ_IOT_ERR_INVALID_ARG;
  return result;
}

static void marked_payload(char* out, size_t cap, const char* marker)
{
  snprintf(out, cap, "{\"marker\":\"%s\"}", marker);
}

/** @brief Fills @p out with a JSON string of exactly @p len bytes starting with @p marker. */
static void padded_json_string(char* out, size_t len, const char* marker)
{
  const size_t marker_len = strlen(marker);
  assert_true(len >= marker_len + 2);
  out[0] = '"';
  memcpy(out + 1, marker, marker_len);
  memset(out + 1 + marker_len, 'x', len - 2 - marker_len);
  out[len - 1] = '"';
  out[len] = '\0';
}

/* Attached before open() so the method topics are bound on the first connect. */
static int attach_methods(e2e_device* dev, void* ctx)
{
  fixture* fx = (fixture*)ctx;
  if (az_iot_mqttv5_direct_method_client_init(&fx->dm, &dev->conn) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] direct method client init failed\n");
    return 1;
  }
  fx->dm_ok = true;
  return az_iot_mqttv5_direct_method_client_set_probe_handler(&fx->dm, on_probe, fx) == AZ_IOT_OK
      ? 0
      : 1;
}

static int group_setup(void** state)
{
  memset(&g_fx, 0, sizeof(g_fx));
  *state = &g_fx;
  e2e_install_log_sink();

  const char* svc_err = NULL;
  g_fx.service = az_iot_e2e_service_create(&svc_err);
  if (g_fx.service == NULL)
  {
    fprintf(stderr, "[e2e] service create failed: %s\n", svc_err ? svc_err : "unknown");
    return -1;
  }

  e2e_device_connect_options opts = { 0 };
  opts.before_open = attach_methods;
  opts.before_open_ctx = &g_fx;
  if (e2e_device_connect_with(&g_fx.dev, &opts) != 0)
  {
    return -1;
  }

  az_iot_hub_profile profile;
  if (az_iot_connection_client_get_hub_profile(&g_fx.dev.conn, &profile) != AZ_IOT_OK
      || profile.connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    fprintf(stderr, "[e2e] the device is not on an mqttv5 hub\n");
    return -1;
  }
  pump_for(&g_fx, E2E_SETTLE_S);
  return 0;
}

static int group_teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx == NULL)
  {
    fx = &g_fx;
  }
  if (fx->dm_ok)
  {
    az_iot_mqttv5_direct_method_client_deinit(&fx->dm);
    fx->dm_ok = false;
  }
  e2e_device_disconnect(&fx->dev);
  if (fx->service != NULL)
  {
    az_iot_e2e_service_destroy(fx->service);
    fx->service = NULL;
  }
  return 0;
}

static int test_setup(void** state)
{
  fixture* fx = (fixture*)*state;
  memset(fx->calls, 0, sizeof(fx->calls));
  fx->calls_used = 0;
  fx->probes = 0;
  fx->probe_answer = AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
  fx->declared_count = 0;
  for (time_t start = time(NULL); fx->dev.conn_state != AZ_IOT_CONN_STATE_CONNECTED
       && (time(NULL) - start) < E2E_CALL_DEADLINE_S;)
  {
    e2e_device_do_work(&fx->dev, E2E_PUMP_MS);
  }
  if (fx->dev.conn_state != AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(stderr, "[e2e] the device is not connected\n");
    return -1;
  }
  return 0;
}

/* Frees whatever a failed test left behind, so it cannot cost the next one capacity. */
static int test_teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  for (int handle = 1; handle < AZ_IOT_E2E_SERVICE_MAX_REQUESTS; ++handle)
  {
    az_iot_e2e_service_request_cancel(fx->service, handle);
  }
  for (size_t i = 0; i < fx->calls_used; ++i)
  {
    if (!fx->calls[i].answered)
    {
      (void)answer(fx, i, 500, NULL, 0);
    }
  }
  for (size_t i = 0; i < fx->declared_count; ++i)
  {
    (void)az_iot_mqttv5_direct_method_client_unregister_method(&fx->dm, fx->declared[i]);
  }
  fx->declared_count = 0;
  fx->probe_answer = AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
  return 0;
}

/** @brief Invokes @p method, answers it with @p status and a marked body, and checks both. */
static void invoke_and_answer(fixture* fx, const char* method, int status)
{
  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "dmOk");
  char payload[E2E_MARKER_MAX + 16];
  marked_payload(payload, sizeof(payload), marker);

  const size_t index = fx->calls_used;
  service_call c;
  call_start(fx, &c, method, payload, E2E_RESPONSE_TIMEOUT_S);
  expect_invocations(fx, &c, 1, index + 1);
  assert_int_equal(answer(fx, index, status, payload, strlen(payload)), AZ_IOT_OK);
  wait_done(fx, &c, 1, E2E_CALL_DEADLINE_S);
  assert_answered(&c, status);
  assert_non_null(strstr(c.body, marker));
}

/** @brief Invokes @p method; expects the refusal assert_refused() describes and no handler run
 *         within E2E_QUIET_S. */
static void invoke_expect_refused(
    fixture* fx,
    const char* method,
    int response_timeout_s,
    int http,
    const char* code,
    long max_refusal_s)
{
  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "dmNo");
  char payload[E2E_MARKER_MAX + 16];
  marked_payload(payload, sizeof(payload), marker);

  const size_t before = fx->calls_used;
  service_call c;
  call_start(fx, &c, method, payload, response_timeout_s);
  wait_done(fx, &c, 1, E2E_CALL_DEADLINE_S);
  assert_refused(&c, http, code, max_refusal_s);
  pump_for(fx, E2E_QUIET_S);
  if (fx->calls_used != before)
  {
    fail_msg("the handler ran for refused '%s'", fx->calls[before].method_name);
  }
}

/** @brief Invokes @p method with a short timeout and leaves it unanswered until the service
 *         reports its timeout (400/400800). */
static void invoke_unanswered(fixture* fx, const char* method, service_call* c)
{
  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "dmLate");
  char payload[E2E_MARKER_MAX + 16];
  marked_payload(payload, sizeof(payload), marker);

  call_start(fx, c, method, payload, E2E_SHORT_TIMEOUT_S);
  expect_invocations(fx, c, 1, 1);
  wait_done(fx, c, 1, E2E_SHORT_TIMEOUT_S + E2E_TIMEOUT_SLACK_S);
  if (!http_final(c) || c->http_status != E2E_HTTP_TIMEOUT
      || strstr(c->body, E2E_CODE_TIMEOUT) == NULL || c->elapsed_s < E2E_SHORT_TIMEOUT_S - 1)
  {
    fail_msg(
        "'%s': expected HTTP %d with errorCode %s after ~%ds; got rc=%d http=%d after %lds "
        "body=%s",
        method,
        E2E_HTTP_TIMEOUT,
        E2E_CODE_TIMEOUT,
        E2E_SHORT_TIMEOUT_S,
        c->rc,
        c->http_status,
        c->elapsed_s,
        c->body);
  }
}

static void registered_method_echoes_payload(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "echo", 0);

  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "dmEcho");
  char payload[E2E_MARKER_MAX + 16];
  marked_payload(payload, sizeof(payload), marker);

  service_call c;
  call_start(fx, &c, "echo", payload, E2E_RESPONSE_TIMEOUT_S);
  expect_invocations(fx, &c, 1, 1);
  assert_string_equal(fx->calls[0].method_name, "echo");
  assert_non_null(strstr(fx->calls[0].payload, marker));
  assert_int_equal(answer(fx, 0, 200, fx->calls[0].payload, fx->calls[0].payload_len), AZ_IOT_OK);
  wait_done(fx, &c, 1, E2E_CALL_DEADLINE_S);
  assert_answered(&c, 200);
  assert_non_null(strstr(c.body, marker));
}

static void non_success_status_reaches_caller(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "fail", 0);

  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "dmFail");
  char payload[E2E_MARKER_MAX + 16];
  marked_payload(payload, sizeof(payload), marker);
  char body[E2E_MARKER_MAX + 16];
  snprintf(body, sizeof(body), "{\"error\":\"%s\"}", marker);

  service_call c;
  call_start(fx, &c, "fail", payload, E2E_RESPONSE_TIMEOUT_S);
  expect_invocations(fx, &c, 1, 1);
  assert_int_equal(answer(fx, 0, 500, body, strlen(body)), AZ_IOT_OK);
  wait_done(fx, &c, 1, E2E_CALL_DEADLINE_S);
  assert_answered(&c, 500);
  assert_non_null(strstr(c.body, marker));
}

static void method_with_no_payload(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "noPayload", 0);

  service_call c;
  call_start(fx, &c, "noPayload", NULL, E2E_RESPONSE_TIMEOUT_S);
  expect_invocations(fx, &c, 1, 1);
  if (fx->calls[0].payload_len != 0 && strcmp(fx->calls[0].payload, "null") != 0)
  {
    fail_msg(
        "expected an empty or null payload, got %zu bytes: %s",
        fx->calls[0].payload_len,
        fx->calls[0].payload);
  }

  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "dmNull");
  char body[E2E_MARKER_MAX + 16];
  marked_payload(body, sizeof(body), marker);
  assert_int_equal(answer(fx, 0, 200, body, strlen(body)), AZ_IOT_OK);
  wait_done(fx, &c, 1, E2E_CALL_DEADLINE_S);
  assert_answered(&c, 200);
  assert_non_null(strstr(c.body, marker));
}

static void undeclared_method_is_rejected_at_probe(void** state)
{
  fixture* fx = (fixture*)*state;
  invoke_expect_refused(
      fx,
      "dmUndeclared",
      E2E_RESPONSE_TIMEOUT_S,
      E2E_HTTP_NOT_FOUND,
      E2E_CODE_NOT_FOUND,
      E2E_REFUSAL_MAX_S);
}

static void method_name_match_is_case_sensitive(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "echo", 0);
  invoke_expect_refused(
      fx,
      "Echo",
      E2E_RESPONSE_TIMEOUT_S,
      E2E_HTTP_NOT_FOUND,
      E2E_CODE_NOT_FOUND,
      E2E_REFUSAL_MAX_S);
}

static void unregistered_method_is_rejected_again(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "echo", 0);
  invoke_and_answer(fx, "echo", 200);

  assert_int_equal(
      az_iot_mqttv5_direct_method_client_unregister_method(&fx->dm, "echo"), AZ_IOT_OK);
  invoke_expect_refused(
      fx,
      "echo",
      E2E_RESPONSE_TIMEOUT_S,
      E2E_HTTP_NOT_FOUND,
      E2E_CODE_NOT_FOUND,
      E2E_REFUSAL_MAX_S);
  assert_int_equal(fx->calls_used, 1);
}

static void probe_handler_busy_is_reported(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "echo", 0);
  fx->probe_answer = AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY;
  invoke_expect_refused(
      fx, "echo", E2E_RESPONSE_TIMEOUT_S, E2E_HTTP_BUSY, E2E_CODE_BUSY, E2E_REFUSAL_MAX_S);
  /* The refusal came from the probe handler, not from a probe that never arrived. */
  assert_true(fx->probes >= 1);
}

static void insufficient_time_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "slow", E2E_SLOW_METHOD_MIN_S);
  invoke_expect_refused(
      fx,
      "slow",
      E2E_INSUFFICIENT_TIMEOUT_S,
      E2E_HTTP_INSUFFICIENT_TIME,
      E2E_CODE_INSUFFICIENT_TIME,
      E2E_INSUFFICIENT_REFUSAL_MAX_S);
}

static void unanswered_method_times_out_service_side(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "silent", 0);
  service_call c;
  invoke_unanswered(fx, "silent", &c);
  /* Releases the slot; the result is not under test here. */
  (void)answer(fx, 0, 200, NULL, 0);
}

static void late_response_is_not_sent(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "late", 0);
  service_call c;
  invoke_unanswered(fx, "late", &c);
  pump_for(fx, E2E_LATE_SLACK_S);
  const char body[] = "{\"late\":true}";
  assert_int_equal(answer(fx, 0, 200, body, sizeof(body) - 1), AZ_IOT_ERR_TIMEOUT);
}

static void result_body_at_max_is_delivered(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "big", 0);

  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "dmMax");
  char payload[E2E_MARKER_MAX + 16];
  marked_payload(payload, sizeof(payload), marker);
  static char body[AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX + 1];
  padded_json_string(body, AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX, marker);

  service_call c;
  call_start(fx, &c, "big", payload, E2E_RESPONSE_TIMEOUT_S);
  expect_invocations(fx, &c, 1, 1);
  assert_int_equal(answer(fx, 0, 200, body, AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX), AZ_IOT_OK);
  wait_done(fx, &c, 1, E2E_CALL_DEADLINE_S);
  assert_answered(&c, 200);
  if (strstr(c.body, body) == NULL)
  {
    fail_msg("the %d byte result did not arrive intact", AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX);
  }
}

static void oversized_result_can_be_answered_again(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "big", 0);

  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "dmOver");
  char payload[E2E_MARKER_MAX + 16];
  marked_payload(payload, sizeof(payload), marker);
  static char oversized[AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX + 2];
  padded_json_string(oversized, AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX + 1, marker);

  service_call c;
  call_start(fx, &c, "big", payload, E2E_RESPONSE_TIMEOUT_S);
  expect_invocations(fx, &c, 1, 1);
  assert_int_equal(
      answer(fx, 0, 200, oversized, AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX + 1),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  char short_marker[E2E_MARKER_MAX];
  make_marker(short_marker, sizeof(short_marker), "dmShort");
  char body[E2E_MARKER_MAX + 16];
  marked_payload(body, sizeof(body), short_marker);
  assert_int_equal(answer(fx, 0, 200, body, strlen(body)), AZ_IOT_OK);
  wait_done(fx, &c, 1, E2E_CALL_DEADLINE_S);
  assert_answered(&c, 200);
  assert_non_null(strstr(c.body, short_marker));
}

static void concurrent_invocations_are_correlated(void** state)
{
  fixture* fx = (fixture*)*state;
  declare(fx, "echo", 0);

  char markers[E2E_CONCURRENT][E2E_MARKER_MAX];
  service_call calls[E2E_CONCURRENT];
  for (size_t i = 0; i < E2E_CONCURRENT; ++i)
  {
    make_marker(markers[i], sizeof(markers[i]), "dmConc");
    char payload[E2E_MARKER_MAX + 16];
    marked_payload(payload, sizeof(payload), markers[i]);
    call_start(fx, &calls[i], "echo", payload, E2E_RESPONSE_TIMEOUT_S);
  }
  printf("[e2e] %d invocations in flight\n", (int)E2E_CONCURRENT);

  expect_invocations(fx, calls, E2E_CONCURRENT, E2E_CONCURRENT);
  for (size_t i = E2E_CONCURRENT; i-- > 0;)
  {
    assert_int_equal(answer(fx, i, 200, fx->calls[i].payload, fx->calls[i].payload_len), AZ_IOT_OK);
  }
  wait_done(fx, calls, E2E_CONCURRENT, E2E_CALL_DEADLINE_S);
  for (size_t i = 0; i < E2E_CONCURRENT; ++i)
  {
    assert_answered(&calls[i], 200);
    if (strstr(calls[i].body, markers[i]) == NULL)
    {
      fail_msg("call %zu got another call's result: %s", i, calls[i].body);
    }
  }
}

#define E2E_TEST(name) cmocka_unit_test_setup_teardown(name, test_setup, test_teardown)

int main(void)
{
  const struct CMUnitTest tests[] = {
    E2E_TEST(registered_method_echoes_payload),
    E2E_TEST(non_success_status_reaches_caller),
    E2E_TEST(method_with_no_payload),
    E2E_TEST(undeclared_method_is_rejected_at_probe),
    E2E_TEST(method_name_match_is_case_sensitive),
    E2E_TEST(unregistered_method_is_rejected_again),
    E2E_TEST(probe_handler_busy_is_reported),
    E2E_TEST(insufficient_time_is_rejected),
    E2E_TEST(unanswered_method_times_out_service_side),
    E2E_TEST(late_response_is_not_sent),
    E2E_TEST(result_body_at_max_is_delivered),
    E2E_TEST(oversized_result_can_be_answered_again),
    E2E_TEST(concurrent_invocations_are_correlated),
  };
  return cmocka_run_group_tests_name(
      "e2e_mqttv5_direct_method", tests, group_setup, group_teardown);
}
