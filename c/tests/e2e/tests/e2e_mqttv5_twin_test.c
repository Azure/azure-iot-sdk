// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end: az_iot_mqttv5_twin_client on an mqttv5 hub: desired delivery with and without
 * options.twin_push.push_desired, reported patches, GET with options, and push_reported.
 *
 * Each test opens its own connection: the push mode is read from the birth, so it is fixed per
 * connection. The service side patches desired properties and reads the twin back over REST
 * (az_iot_e2e_service.h). The tests run in order on one device: the pull test leaves desired
 * state for the push test's snapshot on connect. Reported properties use a fixed set of
 * e2e_<tag> keys whose values carry the per-run marker, so the twin does not grow across runs.
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

#include <azure/core/az_json.h>

#include "azure/iot/az_iot.h"

#include "az_iot_e2e_service.h"
#include "e2e_device.h"
#include "e2e_log.h"

#define E2E_REQUEST_TIMEOUT_S 60
#define E2E_DELIVERY_TIMEOUT_S 60
/* How long pull mode must stay without a patch after a desired change. */
#define E2E_NO_PATCH_WINDOW_S 30
/* How long a disabled push must stay silent. */
#define E2E_NO_PUSH_WINDOW_S 15
#define E2E_SETTLE_S 5
#define E2E_PUMP_MS 20
/* Above the client's default request timeout, so every request completes before the wait ends. */
#define E2E_DEVICE_REQUEST_WAIT_S 70
/* Reported changes reach the REST twin eventually. */
#define E2E_PROPAGATION_TIMEOUT_S 60
#define E2E_CONNECT_TIMEOUT_S 60
/* The mqttv3 reported limit is 32 KiB; this is above it. */
#define E2E_BIG_PATCH_LEN (40u * 1024u)
#define E2E_ENCODE_MAX (48u * 1024u + AZ_IOT_MQTTV5_TWIN_ENCODE_OVERHEAD)
/* Copies of GET sections kept for assertions and diagnostics. */
#define E2E_SECTION_MAX 8192
#define E2E_MAX_PATCHES 16
/* A REST twin body is at most E2E_HTTP_RESPONSE_MAX (32 KiB). */
#define E2E_TWIN_JSON_MAX (32u * 1024u + 1u)
#define E2E_LOG_BODY_MAX 2048

/** @brief Desired deliveries carrying the marker being waited for. */
typedef struct
{
  char marker[64];
  int deliveries; /* all deliveries */
  int matches; /* deliveries containing marker */
  az_iot_mqttv5_twin_desired_kind match_kind;
  uint64_t version;
  uint64_t match_version;
} desired_record;

/** @brief Reported sections pushed on connect. */
typedef struct
{
  char marker[64];
  int deliveries;
  int matches;
  uint64_t version;
} reported_record;

/** @brief Result of the last application GET, with copies of both sections. */
typedef struct
{
  bool done;
  az_iot_result status;
  uint64_t desired_version;
  uint64_t reported_version;
  bool has_desired;
  bool has_reported;
  size_t desired_len;
  size_t reported_len;
  uint8_t desired[E2E_SECTION_MAX];
  uint8_t reported[E2E_SECTION_MAX];
} get_record;

/** @brief Outcome of one reported patch. */
typedef struct
{
  az_iot_result submit;
  bool done;
  az_iot_result status;
  az_iot_mqttv5_twin_patch_status verdict;
  uint64_t version;
} patch_record;

/** @brief Connection settings for one open. */
typedef struct
{
  bool push_desired;
  bool push_reported;
  bool no_desired_handler;
  bool reported_handler;
  /* Marker the reported handler matches; may be NULL. */
  const char* reported_marker;
} connect_mode;

typedef struct
{
  e2e_device dev;
  az_iot_e2e_service* service;
  az_iot_mqttv5_twin_client twin;
  bool twin_ok;
  connect_mode mode;
  desired_record desired;
  reported_record reported;
  get_record get;
  /* Callback contexts live as long as the connection. */
  patch_record patches[E2E_MAX_PATCHES];
  size_t patches_used;
  /* Hub transitions out of CONNECTED. */
  int hub_drops;
  bool hub_connected;
} fixture;

static fixture g_fx;
/* Last desired marker the service set; empty until a test sets one. */
static char g_last_marker[64];
static uint8_t g_encode[E2E_ENCODE_MAX];
static char g_big[E2E_BIG_PATCH_LEN + 1];
/* Device id, kept across a close for the service side. */
static char g_device_id[256];
/* Body of the last service twin GET. */
static char g_twin_json[E2E_TWIN_JSON_MAX];

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
    r->match_version = version;
  }
}

static void on_reported(const az_iot_mqttv5_twin_section* section, void* user_ctx)
{
  reported_record* r = (reported_record*)user_ctx;
  r->deliveries++;
  r->version = section->version;
  printf(
      "[e2e] reported push v%llu (%zu bytes): %.*s\n",
      (unsigned long long)section->version,
      section->payload_len,
      (int)(section->payload_len < E2E_LOG_BODY_MAX ? section->payload_len : E2E_LOG_BODY_MAX),
      section->payload ? (const char*)section->payload : "");
  if (contains(section->payload, section->payload_len, r->marker))
  {
    r->matches++;
  }
}

/** @brief Copy @p s into a get_record section; returns whether a payload was present. */
static bool copy_section(const az_iot_mqttv5_twin_section* s, uint8_t* out, size_t* out_len)
{
  *out_len = 0;
  if (s->payload == NULL)
  {
    return false;
  }
  *out_len = s->payload_len < E2E_SECTION_MAX ? s->payload_len : E2E_SECTION_MAX;
  memcpy(out, s->payload, *out_len);
  return true;
}

static void on_get(az_iot_result status, const az_iot_mqttv5_twin_state* twin, void* user_ctx)
{
  get_record* r = (get_record*)user_ctx;
  r->status = status;
  r->desired_version = twin ? twin->desired.version : 0;
  r->reported_version = twin ? twin->reported.version : 0;
  if (twin != NULL)
  {
    r->has_desired = copy_section(&twin->desired, r->desired, &r->desired_len);
    r->has_reported = copy_section(&twin->reported, r->reported, &r->reported_len);
  }
  r->done = true;
}

static void on_patch(
    az_iot_result status,
    const az_iot_mqttv5_twin_patch_result* result,
    void* user_ctx)
{
  patch_record* r = (patch_record*)user_ctx;
  r->status = status;
  if (result != NULL)
  {
    r->verdict = result->status;
    r->version = result->version;
  }
  r->done = true;
}

static void on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }
  const bool connected = event->state == AZ_IOT_CONN_STATE_CONNECTED;
  if (fx->hub_connected && !connected)
  {
    fx->hub_drops++;
    printf("[e2e] hub left CONNECTED (state=%d)\n", (int)event->state);
  }
  fx->hub_connected = connected;
}

/* The time separates runs on a shared device; the counter separates markers within a run. */
static void make_marker(char* out, size_t cap, const char* tag)
{
  static unsigned counter = 0;
  snprintf(out, cap, "%s-%08lx%03x", tag, (unsigned long)time(NULL), (counter++) & 0xfffu);
}

static void pump(fixture* fx)
{
  if (fx->dev.conn_ok)
  {
    e2e_device_do_work(&fx->dev, E2E_PUMP_MS);
  }
  if (fx->twin_ok)
  {
    (void)az_iot_mqttv5_twin_client_do_work(&fx->twin);
  }
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
  if (az_iot_connection_client_add_state_observer(&dev->conn, on_state, fx) != AZ_IOT_OK
      || az_iot_mqttv5_twin_client_init(&fx->twin, &dev->conn) != AZ_IOT_OK)
  {
    return 1;
  }
  fx->twin_ok = true;
  if (az_iot_mqttv5_twin_client_set_encode_buffer(&fx->twin, AZ_SPAN_FROM_BUFFER(g_encode))
      != AZ_IOT_OK)
  {
    return 1;
  }
  if (fx->mode.reported_handler
      && az_iot_mqttv5_twin_client_set_reported_handler(&fx->twin, on_reported, &fx->reported)
          != AZ_IOT_OK)
  {
    return 1;
  }
  if (fx->mode.no_desired_handler)
  {
    return 0;
  }
  return az_iot_mqttv5_twin_client_set_desired_handler(&fx->twin, on_desired, &fx->desired)
          == AZ_IOT_OK
      ? 0
      : 1;
}

/** @brief Connect the device in @p mode; keeps fx->service, resets everything else. */
static int open_device(fixture* fx, const connect_mode* mode)
{
  az_iot_e2e_service* service = fx->service;
  memset(fx, 0, sizeof(*fx));
  fx->service = service;
  fx->mode = *mode;
  /* Set before open(): a snapshot pushed on connect may arrive before CONNECTED. */
  snprintf(fx->desired.marker, sizeof(fx->desired.marker), "%s", g_last_marker);
  snprintf(
      fx->reported.marker,
      sizeof(fx->reported.marker),
      "%s",
      mode->reported_marker ? mode->reported_marker : "");

  e2e_device_connect_options opts = { 0 };
  opts.twin_push_desired = mode->push_desired;
  opts.twin_push_reported = mode->push_reported;
  opts.before_open = attach_twin;
  opts.before_open_ctx = fx;
  if (e2e_device_connect_with(&fx->dev, &opts) != 0)
  {
    return -1;
  }
  snprintf(g_device_id, sizeof(g_device_id), "%s", fx->dev.device_id);

  az_iot_hub_profile profile;
  if (az_iot_connection_client_get_hub_profile(&fx->dev.conn, &profile) != AZ_IOT_OK
      || profile.connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    fprintf(stderr, "[e2e] the device is not on an mqttv5 hub\n");
    return -1;
  }
  return 0;
}

/** @brief Close the device connection; the service client stays. */
static void close_device(fixture* fx)
{
  if (fx->twin_ok)
  {
    az_iot_mqttv5_twin_client_deinit(&fx->twin);
    fx->twin_ok = false;
  }
  e2e_device_disconnect(&fx->dev);
}

static int connect_with(fixture* fx, const connect_mode* mode)
{
  memset(fx, 0, sizeof(*fx));
  e2e_install_log_sink();

  const char* svc_err = NULL;
  fx->service = az_iot_e2e_service_create(&svc_err);
  if (fx->service == NULL)
  {
    fprintf(stderr, "[e2e] service create failed: %s\n", svc_err ? svc_err : "unknown");
    return -1;
  }
  return open_device(fx, mode);
}

static int connect_device(fixture* fx, bool push_desired)
{
  connect_mode mode = { 0 };
  mode.push_desired = push_desired;
  return connect_with(fx, &mode);
}

static int teardown(void** state);

/* cmocka skips teardown when setup fails, so a failed connect cleans up here. */
static int setup_mode(void** state, const connect_mode* mode)
{
  *state = &g_fx;
  if (connect_with(&g_fx, mode) != 0)
  {
    (void)teardown(state);
    return -1;
  }
  return 0;
}

static int setup_with(void** state, bool push_desired)
{
  connect_mode mode = { 0 };
  mode.push_desired = push_desired;
  return setup_mode(state, &mode);
}

static int setup_pull(void** state) { return setup_with(state, false); }

static int setup_push(void** state) { return setup_with(state, true); }

static int setup_push_no_desired_handler(void** state)
{
  connect_mode mode = { 0 };
  mode.push_desired = true;
  mode.no_desired_handler = true;
  return setup_mode(state, &mode);
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx == NULL)
  {
    fx = &g_fx;
  }
  close_device(fx);
  if (fx->service != NULL)
  {
    az_iot_e2e_service_destroy(fx->service);
    fx->service = NULL;
  }
  return 0;
}

/** @brief Drive the service's in-flight REST request; fails the test unless it answers 200. */
static void service_finish(fixture* fx, const char* what, char* resp, size_t cap)
{
  int status = 0;
  int rc = 0;
  resp[0] = '\0';
  for (time_t start = time(NULL); rc == 0 && (time(NULL) - start) < E2E_REQUEST_TIMEOUT_S;)
  {
    pump(fx);
    rc = az_iot_e2e_service_request_poll(fx->service, &status, resp, cap);
  }
  if (rc != 1 || status != 200)
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fail_msg(
        "%s failed: rc=%d http=%d %s %.*s",
        what,
        rc,
        status,
        err ? err : "",
        E2E_LOG_BODY_MAX,
        rc == 1 ? resp : "");
  }
}

/* Sets desired property @p name to a new marker, kept in fx->desired.marker and g_last_marker. */
static void service_patch_desired(fixture* fx, const char* name)
{
  make_marker(fx->desired.marker, sizeof(fx->desired.marker), name);
  fx->desired.matches = 0;
  char json[128];
  snprintf(json, sizeof(json), "{\"%s\":\"%s\"}", name, fx->desired.marker);

  assert_true(az_iot_e2e_service_twin_patch_desired_begin(fx->service, g_device_id, json));
  char resp[1024];
  service_finish(fx, "desired patch", resp, sizeof(resp));
  snprintf(g_last_marker, sizeof(g_last_marker), "%s", fx->desired.marker);
}

/** @brief Read the twin over REST into g_twin_json. */
static void service_get_twin(fixture* fx)
{
  assert_true(az_iot_e2e_service_twin_get_begin(fx->service, g_device_id));
  service_finish(fx, "twin get", g_twin_json, sizeof(g_twin_json));
}

/** @brief Move @p r, on an object's BEGIN_OBJECT, to the value of member @p name. */
static bool json_enter(az_json_reader* r, const char* name)
{
  if (r->token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return false;
  }
  while (az_json_reader_next_token(r) == AZ_OK && r->token.kind == AZ_JSON_TOKEN_PROPERTY_NAME)
  {
    const bool match = az_json_token_is_text_equal(
        &r->token, az_span_create((uint8_t*)(uintptr_t)name, (int32_t)strlen(name)));
    if (az_json_reader_next_token(r) != AZ_OK)
    {
      return false;
    }
    if (match)
    {
      return true;
    }
    if (az_json_reader_skip_children(r) != AZ_OK)
    {
      return false;
    }
  }
  return false;
}

/** @brief Position @p r on `properties.<section>` of g_twin_json. */
static bool twin_section(const char* section, az_json_reader* r)
{
  return az_json_reader_init(
             r, az_span_create((uint8_t*)g_twin_json, (int32_t)strlen(g_twin_json)), NULL)
      == AZ_OK
      && az_json_reader_next_token(r) == AZ_OK && json_enter(r, "properties")
      && json_enter(r, section);
}

/** @brief `properties.<section>.$version` of g_twin_json. */
static bool twin_section_version(const char* section, uint64_t* out)
{
  az_json_reader r;
  return twin_section(section, &r) && json_enter(&r, "$version")
      && r.token.kind == AZ_JSON_TOKEN_NUMBER && az_json_token_get_uint64(&r.token, out) == AZ_OK;
}

/** @brief Whether g_twin_json is one complete JSON value (a truncated body is not). */
static bool twin_json_valid(void)
{
  az_json_reader r;
  return az_json_reader_init(
             &r, az_span_create((uint8_t*)g_twin_json, (int32_t)strlen(g_twin_json)), NULL)
      == AZ_OK
      && az_json_reader_next_token(&r) == AZ_OK && az_json_reader_skip_children(&r) == AZ_OK
      && az_json_reader_next_token(&r) == AZ_ERROR_JSON_READER_DONE;
}

/** @brief Whether reported member @p name exists in g_twin_json, whatever its value (even null). */
static bool twin_reported_exists(const char* name)
{
  az_json_reader r;
  return twin_section("reported", &r) && json_enter(&r, name);
}

/** @brief Whether reported member @p name of g_twin_json is the string @p value. */
static bool twin_reported_is(const char* name, const char* value)
{
  az_json_reader r;
  return twin_section("reported", &r) && json_enter(&r, name)
      && r.token.kind == AZ_JSON_TOKEN_STRING
      && az_json_token_is_text_equal(
             &r.token, az_span_create((uint8_t*)(uintptr_t)value, (int32_t)strlen(value)));
}

static void print_twin_json(void)
{
  printf(
      "[e2e] service twin (%zu bytes): %.*s\n", strlen(g_twin_json), E2E_LOG_BODY_MAX, g_twin_json);
}

/**
 * @brief Re-read the twin until reported member @p name is the string @p value or, with a NULL
 * @p value, until the member is gone (a member kept with a null value is still present).
 */
static bool service_wait_reported(fixture* fx, const char* name, const char* value)
{
  for (time_t start = time(NULL); (time(NULL) - start) < E2E_PROPAGATION_TIMEOUT_S;)
  {
    service_get_twin(fx);
    if (value != NULL ? twin_reported_is(name, value)
                      : twin_json_valid() && !twin_reported_exists(name))
    {
      return true;
    }
    pump_for(fx, 1);
  }
  if (value != NULL)
  {
    printf("[e2e] reported '%s' never became \"%s\"\n", name, value);
  }
  else
  {
    printf(
        "[e2e] reported '%s' not removed (exists=%d, valid json=%d)\n",
        name,
        (int)twin_reported_exists(name),
        (int)twin_json_valid());
  }
  print_twin_json();
  return false;
}

/** @brief The service twin's desired or reported $version; fails the test if it is missing. */
static uint64_t service_version(fixture* fx, const char* section)
{
  service_get_twin(fx);
  uint64_t v = 0;
  if (!twin_section_version(section, &v))
  {
    print_twin_json();
    fail_msg("no properties.%s.$version in the service twin", section);
  }
  return v;
}

static const char* verdict_name(az_iot_mqttv5_twin_patch_status s)
{
  switch (s)
  {
    case AZ_IOT_MQTTV5_TWIN_PATCH_OK:
      return "OK";
    case AZ_IOT_MQTTV5_TWIN_PATCH_VERSION_MISMATCH:
      return "VERSION_MISMATCH";
    case AZ_IOT_MQTTV5_TWIN_PATCH_PAYLOAD_INVALID:
      return "PAYLOAD_INVALID";
    case AZ_IOT_MQTTV5_TWIN_PATCH_PAYLOAD_TOO_LARGE:
      return "PAYLOAD_TOO_LARGE";
    case AZ_IOT_MQTTV5_TWIN_PATCH_INTERNAL_ERROR:
      return "INTERNAL_ERROR";
    case AZ_IOT_MQTTV5_TWIN_PATCH_UNSPECIFIED:
      return "UNSPECIFIED";
    default:
      return "UNKNOWN";
  }
}

/** @brief Send a reported patch (with @p if_match unless NULL) and wait for its outcome. */
static const patch_record* device_patch(
    fixture* fx,
    const uint64_t* if_match,
    const char* payload,
    size_t len)
{
  assert_true(fx->patches_used < E2E_MAX_PATCHES);
  patch_record* p = &fx->patches[fx->patches_used++];
  p->submit = if_match ? az_iot_mqttv5_twin_client_patch_reported_if_match(
                             &fx->twin, *if_match, (const uint8_t*)payload, len, on_patch, p)
                       : az_iot_mqttv5_twin_client_patch_reported(
                             &fx->twin, (const uint8_t*)payload, len, on_patch, p);
  for (time_t start = time(NULL);
       p->submit == AZ_IOT_OK && !p->done && (time(NULL) - start) < E2E_DEVICE_REQUEST_WAIT_S;)
  {
    pump(fx);
  }
  char if_match_text[24] = "auto";
  if (if_match)
  {
    snprintf(if_match_text, sizeof(if_match_text), "%llu", (unsigned long long)*if_match);
  }
  printf(
      "[e2e] reported patch (%zu bytes, if_match=%s): submit=%s done=%d ack=%s verdict=%s(%d) "
      "v%llu\n",
      len,
      if_match_text,
      az_iot_result_to_string(p->submit),
      (int)p->done,
      p->done ? az_iot_result_to_string(p->status) : "-",
      verdict_name(p->verdict),
      (int)p->verdict,
      (unsigned long long)p->version);
  return p;
}

static void assert_patch_ok(const patch_record* p)
{
  if (p->submit != AZ_IOT_OK || !p->done || p->status != AZ_IOT_OK
      || p->verdict != AZ_IOT_MQTTV5_TWIN_PATCH_OK || p->version == 0)
  {
    fail_msg(
        "reported patch not applied: submit=%s done=%d ack=%s verdict=%s v%llu",
        az_iot_result_to_string(p->submit),
        (int)p->done,
        az_iot_result_to_string(p->status),
        verdict_name(p->verdict),
        (unsigned long long)p->version);
  }
}

/**
 * @brief Report {"e2e_<tag>":"<marker>"} with a new marker; writes the member name and marker.
 * The key is fixed per tag, so repeated runs overwrite it instead of adding keys.
 */
static const patch_record* device_report_marker(
    fixture* fx,
    const char* tag,
    char (*name)[96],
    char (*marker)[64])
{
  make_marker(*marker, sizeof(*marker), tag);
  snprintf(*name, sizeof(*name), "e2e_%s", tag);
  char json[256];
  snprintf(json, sizeof(json), "{\"%s\":\"%s\"}", *name, *marker);
  const patch_record* p = device_patch(fx, NULL, json, strlen(json));
  assert_patch_ok(p);
  return p;
}

static void print_get(const get_record* g)
{
  printf(
      "[e2e] get: %s desired v%llu %s(%zu bytes) %.*s | reported v%llu %s(%zu bytes) %.*s\n",
      az_iot_result_to_string(g->status),
      (unsigned long long)g->desired_version,
      g->has_desired ? "" : "no payload ",
      g->desired_len,
      (int)(g->desired_len < 512 ? g->desired_len : 512),
      (const char*)g->desired,
      (unsigned long long)g->reported_version,
      g->has_reported ? "" : "no payload ",
      g->reported_len,
      (int)(g->reported_len < 512 ? g->reported_len : 512),
      (const char*)g->reported);
}

/** @brief GET with @p opts (NULL: az_iot_mqttv5_twin_client_get) into fx->get; must succeed. */
static void device_get(fixture* fx, const az_iot_mqttv5_twin_get_options* opts)
{
  memset(&fx->get, 0, sizeof(fx->get));
  az_iot_result r = opts
      ? az_iot_mqttv5_twin_client_get_with_options(&fx->twin, opts, on_get, &fx->get)
      : az_iot_mqttv5_twin_client_get(&fx->twin, on_get, &fx->get);
  assert_int_equal(r, AZ_IOT_OK);
  for (time_t start = time(NULL);
       !fx->get.done && (time(NULL) - start) < E2E_DEVICE_REQUEST_WAIT_S;)
  {
    pump(fx);
  }
  print_get(&fx->get);
  assert_true(fx->get.done);
  assert_int_equal(fx->get.status, AZ_IOT_OK);
}

/** @brief GET both sections until they hold @p desired_marker and @p reported_marker. */
static void device_get_until(fixture* fx, const char* desired_marker, const char* reported_marker)
{
  for (time_t start = time(NULL); (time(NULL) - start) < E2E_PROPAGATION_TIMEOUT_S;)
  {
    device_get(fx, NULL);
    if (contains(fx->get.desired, fx->get.desired_len, desired_marker)
        && contains(fx->get.reported, fx->get.reported_len, reported_marker))
    {
      return;
    }
    pump_for(fx, 1);
  }
  fail_msg("GET never returned desired '%s' and reported '%s'", desired_marker, reported_marker);
}

static bool wait_match(fixture* fx, int timeout_s)
{
  for (time_t start = time(NULL); fx->desired.matches == 0 && (time(NULL) - start) < timeout_s;)
  {
    pump(fx);
  }
  return fx->desired.matches != 0;
}

static bool wait_connected(fixture* fx)
{
  for (time_t start = time(NULL); fx->dev.conn_state != AZ_IOT_CONN_STATE_CONNECTED
       && (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S;)
  {
    pump(fx);
  }
  return fx->dev.conn_state == AZ_IOT_CONN_STATE_CONNECTED;
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

/* An applied patch carries the new reported version, which the REST twin then shows. */
static void reported_patch_is_acked_and_visible(void** state)
{
  fixture* fx = (fixture*)*state;
  char name[96];
  char marker[64];
  const patch_record* p = device_report_marker(fx, "rep", &name, &marker);

  assert_true(service_wait_reported(fx, name, marker));
  uint64_t v = 0;
  if (!twin_section_version("reported", &v) || v != p->version)
  {
    print_twin_json();
    fail_msg(
        "service reported $version %llu, patch acked v%llu",
        (unsigned long long)v,
        (unsigned long long)p->version);
  }
}

/* A default GET returns both sections at the versions the service twin shows. */
static void get_returns_both_sections(void** state)
{
  fixture* fx = (fixture*)*state;
  service_patch_desired(fx, "e2eBoth");
  char name[96];
  char marker[64];
  (void)device_report_marker(fx, "both", &name, &marker);
  assert_true(service_wait_reported(fx, name, marker));

  device_get_until(fx, fx->desired.marker, marker);
  const uint64_t desired_version = service_version(fx, "desired");
  const uint64_t reported_version = service_version(fx, "reported");
  printf(
      "[e2e] service desired v%llu reported v%llu\n",
      (unsigned long long)desired_version,
      (unsigned long long)reported_version);
  assert_int_equal(fx->get.desired_version, desired_version);
  assert_int_equal(fx->get.reported_version, reported_version);
}

/* Sections select what a GET returns; if_not_match at the current version omits a payload. */
static void get_with_options_selects_sections(void** state)
{
  fixture* fx = (fixture*)*state;
  service_patch_desired(fx, "e2eSel");
  char name[96];
  char marker[64];
  (void)device_report_marker(fx, "sel", &name, &marker);
  device_get_until(fx, fx->desired.marker, marker);
  const uint64_t dv = fx->get.desired_version;
  const uint64_t rv = fx->get.reported_version;

  az_iot_mqttv5_twin_get_options opts = az_iot_mqttv5_twin_get_options_default();
  opts.sections = AZ_IOT_MQTTV5_TWIN_SECTIONS_DESIRED;
  device_get(fx, &opts);
  assert_true(fx->get.has_desired);
  assert_true(contains(fx->get.desired, fx->get.desired_len, fx->desired.marker));
  assert_false(fx->get.has_reported);

  opts.sections = AZ_IOT_MQTTV5_TWIN_SECTIONS_REPORTED;
  device_get(fx, &opts);
  assert_false(fx->get.has_desired);
  assert_true(fx->get.has_reported);
  assert_true(contains(fx->get.reported, fx->get.reported_len, marker));

  /* Both held at the current versions: nothing to send. */
  opts.sections = AZ_IOT_MQTTV5_TWIN_SECTIONS_BOTH;
  opts.if_not_match_desired = dv;
  opts.if_not_match_reported = rv;
  device_get(fx, &opts);
  assert_false(fx->get.has_desired);
  assert_false(fx->get.has_reported);
  assert_int_equal(fx->get.desired_version, dv);
  assert_int_equal(fx->get.reported_version, rv);

  /* Held at an older version (0 is no filter): the payload comes back. */
  opts.if_not_match_desired = dv - 1u;
  opts.if_not_match_reported = rv - 1u;
  device_get(fx, &opts);
  assert_true(fx->get.has_desired);
  assert_true(fx->get.has_reported);
  assert_true(contains(fx->get.reported, fx->get.reported_len, marker));
}

/* A null member in a reported patch removes the key; keeping it with a null value fails. */
static void null_member_deletes_reported_property(void** state)
{
  fixture* fx = (fixture*)*state;
  char marker[64];
  char name[96];
  (void)device_report_marker(fx, "del", &name, &marker);
  assert_true(service_wait_reported(fx, name, marker));

  char json[128];
  snprintf(json, sizeof(json), "{\"%s\":null}", name);
  assert_patch_ok(device_patch(fx, NULL, json, strlen(json)));
  assert_true(service_wait_reported(fx, name, NULL));
}

/* A patch whose if_match is behind the current version is refused; the next one applies. */
static void stale_if_match_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  char name[96];
  char marker[64];
  const uint64_t old_version = device_report_marker(fx, "ifm", &name, &marker)->version;
  const uint64_t current = device_report_marker(fx, "ifm", &name, &marker)->version;
  assert_true(current > old_version);

  make_marker(marker, sizeof(marker), "ifm");
  char json[256];
  snprintf(json, sizeof(json), "{\"%s\":\"%s\"}", name, marker);
  const patch_record* stale = device_patch(fx, &old_version, json, strlen(json));
  assert_int_equal(stale->submit, AZ_IOT_OK);
  assert_true(stale->done);
  assert_int_equal(stale->status, AZ_IOT_OK);
  if (stale->verdict != AZ_IOT_MQTTV5_TWIN_PATCH_VERSION_MISMATCH)
  {
    fail_msg("stale if_match: expected VERSION_MISMATCH, got %s", verdict_name(stale->verdict));
  }

  const patch_record* next = device_patch(fx, NULL, json, strlen(json));
  assert_patch_ok(next);
  assert_true(next->version > current);
  /* The refusal reports the unchanged current version. */
  assert_int_equal(stale->version, current);
}

/* A non-JSON patch is refused without ending the session. */
static void invalid_reported_payload_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  char marker[64];
  make_marker(marker, sizeof(marker), "bad");
  char bad[128];
  snprintf(bad, sizeof(bad), "e2e not json %s {", marker);
  const patch_record* p = device_patch(fx, NULL, bad, strlen(bad));
  assert_int_equal(p->submit, AZ_IOT_OK);
  /* A timeout or an undecodable ack is not a refusal. */
  if (!p->done || p->status != AZ_IOT_OK || p->verdict != AZ_IOT_MQTTV5_TWIN_PATCH_PAYLOAD_INVALID)
  {
    fail_msg(
        "invalid payload: expected ack OK with PAYLOAD_INVALID, got done=%d ack=%s verdict=%s",
        (int)p->done,
        p->done ? az_iot_result_to_string(p->status) : "-",
        verdict_name(p->verdict));
  }
  assert_int_equal(fx->hub_drops, 0);
  assert_int_equal(fx->dev.conn_state, AZ_IOT_CONN_STATE_CONNECTED);

  char name[96];
  (void)device_report_marker(fx, "bad", &name, &marker);
}

/**
 * @brief Whether the service twin may hold reported member @p name, read over
 * E2E_PROPAGATION_TIMEOUT_S; a body that is not complete JSON (over the REST read limit) counts.
 */
static bool service_reported_may_exist(fixture* fx, const char* name)
{
  for (time_t start = time(NULL); (time(NULL) - start) < E2E_PROPAGATION_TIMEOUT_S;)
  {
    service_get_twin(fx);
    const bool valid = twin_json_valid();
    if (!valid || twin_reported_exists(name))
    {
      printf("[e2e] service twin: '%s' present or unreadable (valid json=%d)\n", name, (int)valid);
      print_twin_json();
      return true;
    }
    pump_for(fx, 1);
  }
  return false;
}

/* A reported patch above the mqttv3 limit is refused as PAYLOAD_TOO_LARGE. Without a decoded
 * ack the service twin must not hold the property; the device can still patch after it. */
static void oversized_reported_patch_is_refused(void** state)
{
  fixture* fx = (fixture*)*state;
  char marker[64];
  const char* name = "e2e_big";
  make_marker(marker, sizeof(marker), "big");
  const int head = snprintf(g_big, sizeof(g_big), "{\"%s\":\"%s", name, marker);
  assert_true(head > 0);
  memset(g_big + head, 'x', E2E_BIG_PATCH_LEN - (size_t)head - 2u);
  memcpy(g_big + E2E_BIG_PATCH_LEN - 2u, "\"}", 3);

  const patch_record* p = device_patch(fx, NULL, g_big, E2E_BIG_PATCH_LEN);
  const bool acked = p->submit == AZ_IOT_OK && p->done && p->status == AZ_IOT_OK;
  const char* outcome = p->submit != AZ_IOT_OK ? "submission failed"
      : !p->done                               ? "no ack before the wait ended"
      : p->status != AZ_IOT_OK                 ? "ack failed (timeout, disconnect or decode)"
      : p->verdict == AZ_IOT_MQTTV5_TWIN_PATCH_PAYLOAD_TOO_LARGE ? "refused as PAYLOAD_TOO_LARGE"
                                                                 : "acked with another verdict";
  printf(
      "[e2e] oversized patch: %s; submit=%s done=%d ack=%s verdict=%s v%llu hub_drops=%d\n",
      outcome,
      az_iot_result_to_string(p->submit),
      (int)p->done,
      p->done ? az_iot_result_to_string(p->status) : "-",
      verdict_name(p->verdict),
      (unsigned long long)p->version,
      fx->hub_drops);

  if (!acked || p->verdict != AZ_IOT_MQTTV5_TWIN_PATCH_PAYLOAD_TOO_LARGE)
  {
    if (service_reported_may_exist(fx, name))
    {
      /* Keep the twin under the REST read limit for the tests that follow. */
      char json[64];
      snprintf(json, sizeof(json), "{\"%s\":null}", name);
      const bool reconnected = wait_connected(fx);
      if (reconnected)
      {
        (void)device_patch(fx, NULL, json, strlen(json));
      }
      fail_msg(
          "oversized patch (%s) left '%s' in the service twin; delete %s",
          outcome,
          name,
          reconnected ? "sent" : "not sent: device not reconnected");
    }
    if (acked)
    {
      fail_msg("oversized patch: expected PAYLOAD_TOO_LARGE, got %s", verdict_name(p->verdict));
    }
    printf("[e2e] oversized patch: %s and '%s' is absent from the service twin\n", outcome, name);
  }

  assert_true(wait_connected(fx));
  char after[96];
  (void)device_report_marker(fx, "after", &after, &marker);
}

/* With push_reported the service pushes the reported section on connect; without it, nothing. */
static void push_reported_rehydrates_on_connect(void** state)
{
  fixture* fx = (fixture*)*state;
  char name[96];
  char marker[64];
  const uint64_t version = device_report_marker(fx, "rehy", &name, &marker)->version;

  connect_mode mode = { 0 };
  mode.push_reported = true;
  mode.reported_handler = true;
  mode.reported_marker = marker;
  close_device(fx);
  assert_int_equal(open_device(fx, &mode), 0);
  for (time_t start = time(NULL);
       fx->reported.matches == 0 && (time(NULL) - start) < E2E_DELIVERY_TIMEOUT_S;)
  {
    pump(fx);
  }
  if (fx->reported.matches == 0)
  {
    fail_msg(
        "push_reported: no pushed reported section with '%s' (%d deliveries)",
        marker,
        fx->reported.deliveries);
  }
  assert_true(fx->reported.version >= version);

  mode.push_reported = false;
  close_device(fx);
  assert_int_equal(open_device(fx, &mode), 0);
  pump_for(fx, E2E_NO_PUSH_WINDOW_S);
  assert_int_equal(fx->reported.deliveries, 0);
}

/* A desired change made while the device was closed arrives as a snapshot on reconnect. */
static void desired_missed_offline_is_picked_up_on_reconnect(void** state)
{
  fixture* fx = (fixture*)*state;
  close_device(fx);
  service_patch_desired(fx, "e2eOffline");

  connect_mode mode = { 0 };
  assert_int_equal(open_device(fx, &mode), 0);
  assert_true(wait_match(fx, E2E_DELIVERY_TIMEOUT_S));
  assert_int_equal(fx->desired.match_kind, AZ_IOT_MQTTV5_TWIN_DESIRED_SNAPSHOT);
  assert_int_equal(fx->desired.match_version, service_version(fx, "desired"));
}

/* A handler set while connected is given a snapshot; a NULL handler is given nothing. */
static void late_desired_handler_gets_snapshot(void** state)
{
  fixture* fx = (fixture*)*state;
  pump_for(fx, E2E_SETTLE_S);
  service_patch_desired(fx, "e2eLate");
  /* Let the pushed patch arrive with no handler to take it. */
  pump_for(fx, E2E_SETTLE_S);
  assert_int_equal(fx->desired.deliveries, 0);

  assert_int_equal(
      az_iot_mqttv5_twin_client_set_desired_handler(&fx->twin, on_desired, &fx->desired),
      AZ_IOT_OK);
  assert_true(wait_match(fx, E2E_DELIVERY_TIMEOUT_S));
  assert_int_equal(fx->desired.match_kind, AZ_IOT_MQTTV5_TWIN_DESIRED_SNAPSHOT);

  assert_int_equal(az_iot_mqttv5_twin_client_set_desired_handler(&fx->twin, NULL, NULL), AZ_IOT_OK);
  const int deliveries = fx->desired.deliveries;
  service_patch_desired(fx, "e2eLate");
  pump_for(fx, E2E_NO_PUSH_WINDOW_S);
  assert_int_equal(fx->desired.deliveries, deliveries);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(
        pull_mode_sees_a_desired_change_only_after_a_get, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(push_mode_pushes_a_snapshot_on_connect, setup_push, teardown),
    cmocka_unit_test_setup_teardown(push_mode_delivers_each_desired_patch, setup_push, teardown),
    cmocka_unit_test_setup_teardown(reported_patch_is_acked_and_visible, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(get_returns_both_sections, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(get_with_options_selects_sections, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(null_member_deletes_reported_property, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(stale_if_match_is_refused, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(invalid_reported_payload_is_refused, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(oversized_reported_patch_is_refused, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(push_reported_rehydrates_on_connect, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(
        desired_missed_offline_is_picked_up_on_reconnect, setup_pull, teardown),
    cmocka_unit_test_setup_teardown(
        late_desired_handler_gets_snapshot, setup_push_no_desired_handler, teardown),
  };
  return cmocka_run_group_tests_name("e2e_mqttv5_twin", tests, NULL, NULL);
}
