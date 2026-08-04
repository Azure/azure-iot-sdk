// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* TwinClient (Phase 3.3).
 *
 * Supports both IoT Hub Classic (MQTT v3.1.1) and Hub-Next (MQTT v5).
 *
 * Classic:
 *   GET            : $iothub/twin/GET/?$rid=<n>
 *   PATCH reported : $iothub/twin/PATCH/properties/reported/?$rid=<n>
 *   Response       : $iothub/twin/res/<status>/?$rid=<n>[&$version=<v>]
 *   Desired        : $iothub/twin/PATCH/properties/desired/?$version=<v>
 *
 * Next:
 *   GET            : ih/{device_id}/srv/twin/get + correlation_data
 *   PATCH reported : ih/{device_id}/srv/twin/reported + correlation_data
 *   GET response   : ih/{device_id}/dev/twin/get/response + correlation_data
 *   Reported ack   : ih/{device_id}/dev/twin/reported/response + correlation_data
 *   Desired        : ih/{device_id}/dev/twin/desired
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_twin_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/protocol_profile.h"
#include "internal/span_writer.h"
#include "internal/twin_client_internal.h"

#define AZ_IOT_TWIN_TOPIC_MAX 192

/* The pending slot kind values stored in _internal.pending[].kind */
#define TWIN_PENDING_NONE 0
#define TWIN_PENDING_GET 1
#define TWIN_PENDING_PATCH 2

/* Status codes carried on the twin response topic, per the IoT Hub twin
 * documentation. The values happen to coincide with HTTP status codes, but the
 * service contract does not state that they are HTTP codes, so nothing here
 * depends on that: they are declared as twin constants rather than borrowed
 * from an HTTP header. */
#define TWIN_STATUS_SUCCESS_MIN 200
#define TWIN_STATUS_SUCCESS_LIMIT 300 /* exclusive upper bound */
#define TWIN_STATUS_BAD_REQUEST 400
#define TWIN_STATUS_NOT_FOUND 404
#define TWIN_STATUS_THROTTLED 429

/* Internal shorthand to access _internal fields */
#define TI(t) ((t)->_internal)

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Find the pending slot matching a given request-id. Returns slot index or -1. */
static int find_pending_by_rid(az_iot_twin_client* t, uint32_t rid)
{
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (TI(t).pending[i].in_use && TI(t).pending[i].rid == rid)
    {
      return i;
    }
  }
  return -1;
}

/* Allocate an unused pending slot. Returns slot index or -1 if full. */
static int alloc_pending(az_iot_twin_client* t)
{
  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (!TI(t).pending[i].in_use)
      return i;
  }
  return -1;
}

/* ------------------------------------------------------------------------- */
/* desired-property subscriber registry                                      */
/* ------------------------------------------------------------------------- */

/* Add cb/user_ctx to the given pool. Returns AZ_IOT_OK, AZ_IOT_ERR_BUSY (during
 * dispatch), AZ_IOT_ERR_INVALID_ARG, or AZ_IOT_ERR_NOT_SUPPORTED (pool full).
 * Idempotent: re-subscribing the same (cb,user_ctx) is a no-op success. */
static az_iot_result desired_subscribe(
    az_iot_twin_client* t,
    az_iot_twin_desired_sub* pool,
    size_t pool_cap,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
  if (!t || !cb)
    return AZ_IOT_ERR_INVALID_ARG;
  if (TI(t).dispatching)
    return AZ_IOT_ERR_BUSY;

  int free_slot = -1;
  for (size_t i = 0; i < pool_cap; ++i)
  {
    if (pool[i].in_use)
    {
      if (pool[i].cb == cb && pool[i].user_ctx == user_ctx)
        return AZ_IOT_OK;
    }
    else if (free_slot < 0)
    {
      free_slot = (int)i;
    }
  }
  if (free_slot < 0)
    return AZ_IOT_ERR_NOT_SUPPORTED;

  pool[free_slot].cb = cb;
  pool[free_slot].user_ctx = user_ctx;
  pool[free_slot].in_use = true;
  return AZ_IOT_OK;
}

/* Dispatch a desired patch to all subscribers: feature-client pool first (in
 * registration order), then application pool. The dispatching guard forbids
 * subscribe/unsubscribe from within a callback. */
static void dispatch_desired(
    az_iot_twin_client* t,
    const uint8_t* payload,
    size_t payload_len,
    uint64_t version)
{
  TI(t).dispatching = true;
  for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS; ++i)
  {
    az_iot_twin_desired_sub* s = &TI(t).desired_feature_subs[i];
    if (s->in_use && s->cb)
      s->cb(payload, payload_len, version, s->user_ctx);
  }
  for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS; ++i)
  {
    az_iot_twin_desired_sub* s = &TI(t).desired_app_subs[i];
    if (s->in_use && s->cb)
      s->cb(payload, payload_len, version, s->user_ctx);
  }
  TI(t).dispatching = false;
}

/* Find "<key>=<value>" inside a query string (starting at '?'); writes value
 * into out (NUL-terminated). Returns true if found. */
static bool query_value(const char* qs, const char* key, char* out, size_t cap)
{
  if (!qs || !key)
    return false;
  size_t key_len = strlen(key);
  const char* p = qs;
  while (p && *p)
  {
    /* skip past '?' or '&' */
    if (*p == '?' || *p == '&')
      p++;
    if (strncmp(p, key, key_len) == 0 && p[key_len] == '=')
    {
      const char* val = p + key_len + 1;
      const char* end = strchr(val, '&');
      size_t n = end ? (size_t)(end - val) : strlen(val);
      if (n + 1 > cap)
        return false;
      memcpy(out, val, n);
      out[n] = '\0';
      return true;
    }
    p = strchr(p, '&');
  }
  return false;
}

/* Map the service status on a twin response onto an az_iot result.
 *
 * The documented set is 200 (GET success), 204 (PATCH success), 400 (malformed
 * reported-properties JSON), 429 (throttled) and 5xx (server errors). Each one
 * the caller can act on differently gets its own code:
 *
 *   400 -> INVALID_ARG   the patch this device sent was not accepted; resending
 *                        it unchanged will fail again.
 *   404 -> NOT_FOUND     undocumented for twin but cheap to distinguish.
 *   429 -> BUSY          back off and retry. Deliberately NOT NOT_SUPPORTED,
 *                        which is what a full pending table returns -- a caller
 *                        must be able to tell "the service is throttling me"
 *                        from "I have too many requests in flight locally".
 *   else -> MQTT         server errors and anything unrecognised. */
static az_iot_result status_to_result(int status)
{
  if (status >= TWIN_STATUS_SUCCESS_MIN && status < TWIN_STATUS_SUCCESS_LIMIT)
  {
    return AZ_IOT_OK;
  }
  if (status == TWIN_STATUS_BAD_REQUEST)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (status == TWIN_STATUS_NOT_FOUND)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  if (status == TWIN_STATUS_THROTTLED)
  {
    return AZ_IOT_ERR_BUSY;
  }
  return AZ_IOT_ERR_MQTT;
}

/* Complete every pending request with an error and release its slot.
 *
 * Called when the MQTT session ends. The responses these requests were waiting
 * for would have arrived on that session, so they can never come now; holding
 * the slots would leak one per outage until AZ_IOT_TWIN_MAX_PENDING is gone and
 * the client refuses every further request.
 *
 * The slot is released BEFORE the callback runs so a callback that re-issues
 * its request immediately can claim it. */
static void twin_fail_pending(void* user_ctx)
{
  az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
  if (!t)
  {
    return;
  }

  for (int i = 0; i < AZ_IOT_TWIN_MAX_PENDING; ++i)
  {
    if (!TI(t).pending[i].in_use)
    {
      continue;
    }

    int kind = TI(t).pending[i].kind;
    az_iot_twin_get_callback get_cb = TI(t).pending[i].cb.get_cb;
    az_iot_twin_patch_ack_callback patch_cb = TI(t).pending[i].cb.patch_cb;
    void* ctx = TI(t).pending[i].user_ctx;

    TI(t).pending[i].in_use = false;
    TI(t).pending[i].kind = TWIN_PENDING_NONE;

    if (kind == TWIN_PENDING_GET && get_cb)
    {
      get_cb(AZ_IOT_ERR_NOT_CONNECTED, NULL, 0, ctx);
    }
    else if (kind == TWIN_PENDING_PATCH && patch_cb)
    {
      patch_cb(AZ_IOT_ERR_NOT_CONNECTED, 0, ctx);
    }
  }
}

/* ------------------------------------------------------------------------- */
/* dispatch handlers — Classic                                               */
/* ------------------------------------------------------------------------- */

static void on_twin_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic)
    return;

  /* Topic: "$iothub/twin/res/<status>/?$rid=<n>[&$version=<v>]". */
  static const char k_prefix[] = "$iothub/twin/res/";
  size_t prefix_len = sizeof(k_prefix) - 1;
  if (strncmp(msg->topic, k_prefix, prefix_len) != 0)
    return;
  const char* status_str = msg->topic + prefix_len;
  char* qmark = strchr(status_str, '?');
  if (!qmark)
    return;
  /* Parse status code (decimal) up to '/' or '?'. */
  int status = 0;
  for (const char* p = status_str; p < qmark && *p && *p != '/'; ++p)
  {
    if (*p < '0' || *p > '9')
      return;
    status = status * 10 + (*p - '0');
  }
  char rid_buf[16];
  if (!query_value(qmark, "$rid", rid_buf, sizeof(rid_buf)))
    return;
  uint32_t rid = (uint32_t)strtoul(rid_buf, NULL, 10);

  /* A reported-properties acknowledgement carries the new version of that
   * section: "$iothub/twin/res/204/?$rid=1&$version=6". An application that
   * tracks it can tell an applied update from a lost one. */
  uint64_t version = 0;
  {
    char ver_buf[24];
    if (query_value(qmark, "$version", ver_buf, sizeof(ver_buf)))
    {
      version = strtoull(ver_buf, NULL, 10);
    }
  }

  int idx = find_pending_by_rid(t, rid);
  if (idx < 0)
    return; /* stale or unknown rid */

  az_iot_result r = status_to_result(status);
  if (TI(t).pending[idx].kind == TWIN_PENDING_GET)
  {
    az_iot_twin_get_callback cb = TI(t).pending[idx].cb.get_cb;
    void* ctx = TI(t).pending[idx].user_ctx;
    TI(t).pending[idx].in_use = false;
    TI(t).pending[idx].kind = TWIN_PENDING_NONE;
    if (cb)
      cb(r, msg->payload, msg->payload_len, ctx);
  }
  else if (TI(t).pending[idx].kind == TWIN_PENDING_PATCH)
  {
    az_iot_twin_patch_ack_callback cb = TI(t).pending[idx].cb.patch_cb;
    void* ctx = TI(t).pending[idx].user_ctx;
    TI(t).pending[idx].in_use = false;
    TI(t).pending[idx].kind = TWIN_PENDING_NONE;
    if (cb)
    {
      /* A version only means something when the update was applied. If the
       * service ever attached one to a non-2xx response, reporting it would
       * let a caller record a version for a patch that was rejected. */
      cb(r, (r == AZ_IOT_OK) ? version : 0, ctx);
    }
  }
  else
  {
    TI(t).pending[idx].in_use = false;
  }
}

static void on_twin_desired(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
  if (!t || !msg || !msg->topic)
    return;

  /* Topic: "$iothub/twin/PATCH/properties/desired/?$version=<v>". */
  uint64_t version = 0;
  const char* qmark = strchr(msg->topic, '?');
  if (qmark)
  {
    char ver_buf[24];
    if (query_value(qmark, "$version", ver_buf, sizeof(ver_buf)))
    {
      version = strtoull(ver_buf, NULL, 10);
    }
  }
  dispatch_desired(t, msg->payload, msg->payload_len, version);
}

/* ------------------------------------------------------------------------- */
/* dispatch handlers — Hub-Next                                              */
/* ------------------------------------------------------------------------- */

/* Inbound on "ih/{device_id}/dev/twin/get/response" — correlate by rid in
 * correlation_data. */
static void on_twin_get_response_next(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
  if (!t || !msg)
    return;

  /* Match correlation_data to a pending rid */
  uint32_t rid = 0;
  if (msg->correlation_data && msg->correlation_data_len > 0)
  {
    char rid_buf[16];
    size_t n = msg->correlation_data_len < sizeof(rid_buf) - 1 ? msg->correlation_data_len
                                                               : sizeof(rid_buf) - 1;
    memcpy(rid_buf, msg->correlation_data, n);
    rid_buf[n] = '\0';
    rid = (uint32_t)strtoul(rid_buf, NULL, 10);
  }

  int idx = find_pending_by_rid(t, rid);
  if (idx < 0)
    return;

  if (TI(t).pending[idx].kind == TWIN_PENDING_GET)
  {
    az_iot_twin_get_callback cb = TI(t).pending[idx].cb.get_cb;
    void* ctx = TI(t).pending[idx].user_ctx;
    TI(t).pending[idx].in_use = false;
    TI(t).pending[idx].kind = TWIN_PENDING_NONE;
    if (cb)
      cb(AZ_IOT_OK, msg->payload, msg->payload_len, ctx);
  }
  else
  {
    TI(t).pending[idx].in_use = false;
  }
}

/* Inbound on "ih/{device_id}/dev/twin/reported/response" — ack for patch. */
static void on_twin_reported_response_next(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
  if (!t || !msg)
    return;

  uint32_t rid = 0;
  if (msg->correlation_data && msg->correlation_data_len > 0)
  {
    char rid_buf[16];
    size_t n = msg->correlation_data_len < sizeof(rid_buf) - 1 ? msg->correlation_data_len
                                                               : sizeof(rid_buf) - 1;
    memcpy(rid_buf, msg->correlation_data, n);
    rid_buf[n] = '\0';
    rid = (uint32_t)strtoul(rid_buf, NULL, 10);
  }

  int idx = find_pending_by_rid(t, rid);
  if (idx < 0)
    return;

  if (TI(t).pending[idx].kind == TWIN_PENDING_PATCH)
  {
    az_iot_twin_patch_ack_callback cb = TI(t).pending[idx].cb.patch_cb;
    void* ctx = TI(t).pending[idx].user_ctx;
    TI(t).pending[idx].in_use = false;
    TI(t).pending[idx].kind = TWIN_PENDING_NONE;
    if (cb)
    {
      /* Hub-Next acknowledges on its own topic and does not carry a reported
       * version yet, so there is nothing to report but the status. */
      cb(AZ_IOT_OK, 0, ctx);
    }
  }
  else
  {
    TI(t).pending[idx].in_use = false;
  }
}

/* Inbound on "ih/{device_id}/dev/twin/desired" — desired property push. */
static void on_twin_desired_next(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
  if (!t || !msg)
    return;

  /* Version could come from a user property; for now default to 0 */
  uint64_t version = 0;
  dispatch_desired(t, msg->payload, msg->payload_len, version);
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_twin_client_init(az_iot_twin_client* client, az_iot_connection_client* conn)
{
  if (client == NULL || conn == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  const az_iot_protocol_profile* profile = az_iot_connection_client__profile(conn);
  if (!profile)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  memset(client, 0, sizeof(*client));
  TI(client).conn = conn;
  TI(client).next_rid = 1;

  if (profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
  {
    /* Hub-Next: subscribe to:
     *   ih/{device_id}/dev/twin/get/response
     *   ih/{device_id}/dev/twin/reported/response
     *   ih/{device_id}/dev/twin/desired
     */
    const char* device_id = az_iot_connection_client__device_id(conn);
    if (!device_id)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_NOT_INITIALIZED;
    }

    char prefix[AZ_IOT_TWIN_TOPIC_MAX];
    char filter[AZ_IOT_TWIN_TOPIC_MAX];
    az_iot_result r;

    /* Register handler for twin/get/response */
    const char* get_parts[] = { "ih/", device_id, "/dev/twin/get/response" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), NULL, get_parts, 3) != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }
    r = az_iot_connection_client__register_inbound_handler(
        conn, prefix, on_twin_get_response_next, client);
    if (r != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return r;
    }

    r = az_iot_connection_client__add_subscription_on_connect(conn, prefix, AZ_IOT_MQTT_QOS_1);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }

    /* Register handler for twin/reported/response */
    const char* reported_parts[] = { "ih/", device_id, "/dev/twin/reported/response" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(prefix), NULL, reported_parts, 3)
        != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }
    r = az_iot_connection_client__register_inbound_handler(
        conn, prefix, on_twin_reported_response_next, client);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }
    r = az_iot_connection_client__add_subscription_on_connect(conn, prefix, AZ_IOT_MQTT_QOS_1);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }

    /* Register handler for twin/desired */
    const char* desired_parts[] = { "ih/", device_id, "/dev/twin/desired" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(filter), NULL, desired_parts, 3)
        != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }
    r = az_iot_connection_client__register_inbound_handler(
        conn, filter, on_twin_desired_next, client);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }
    r = az_iot_connection_client__add_subscription_on_connect(conn, filter, AZ_IOT_MQTT_QOS_1);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }
  }
  else
  {
    /* Classic path */
    if (!profile->twin_response_topic_prefix || !profile->twin_desired_topic_prefix)
    {
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    az_iot_result r = az_iot_connection_client__register_inbound_handler(
        conn, profile->twin_response_topic_prefix, on_twin_response, client);
    if (r != AZ_IOT_OK)
    {
      memset(client, 0, sizeof(*client));
      return r;
    }

    r = az_iot_connection_client__register_inbound_handler(
        conn, profile->twin_desired_topic_prefix, on_twin_desired, client);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }

    /* Persistent subscriptions: response + desired wildcards. */
    char filter[AZ_IOT_TWIN_TOPIC_MAX];
    const char* response_filter_parts[] = { profile->twin_response_topic_prefix, "#" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(filter), NULL, response_filter_parts, 2)
        != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }
    r = az_iot_connection_client__add_subscription_on_connect(conn, filter, AZ_IOT_MQTT_QOS_0);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }

    const char* desired_filter_parts[] = { profile->twin_desired_topic_prefix, "#" };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(filter), NULL, desired_filter_parts, 2)
        != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return AZ_IOT_ERR_INTERNAL;
    }
    r = az_iot_connection_client__add_subscription_on_connect(conn, filter, AZ_IOT_MQTT_QOS_0);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }
  }

  /* Be told when the session ends so pending GET/PATCH requests are completed
   * and their slots released, rather than waiting for a response that died
   * with the session. Registered last: everything above can still fail and
   * unwind, and this must not outlive a failed init. */
  {
    az_iot_result r
        = az_iot_connection_client__register_session_end_handler(conn, twin_fail_pending, client);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
      memset(client, 0, sizeof(*client));
      return r;
    }
  }

  return AZ_IOT_OK;
}

void az_iot_twin_client_destroy(az_iot_twin_client* client)
{
  if (!client)
    return;
  (void)az_iot_connection_client__unregister_session_end_handler(TI(client).conn, client);
  (void)az_iot_connection_client__unregister_inbound_handlers(TI(client).conn, client);
  memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_twin_client_get(
    az_iot_twin_client* twin,
    az_iot_twin_get_callback cb,
    void* user_ctx)
{
  if (!twin)
    return AZ_IOT_ERR_INVALID_ARG;

  int idx = alloc_pending(twin);
  if (idx < 0)
    return AZ_IOT_ERR_NOT_SUPPORTED;

  uint32_t rid = TI(twin).next_rid++;
  if (TI(twin).next_rid == 0)
    TI(twin).next_rid = 1; /* never reuse 0 */

  const az_iot_protocol_profile* profile = az_iot_connection_client__profile(TI(twin).conn);

  char topic[AZ_IOT_TWIN_TOPIC_MAX];

  /* Correlation data (rid as ASCII string) */
  char corr_buf[16];
  size_t corr_len = 0;
  az_iot_span_writer corr_writer;
  az_iot_span_writer_init(&corr_writer, AZ_SPAN_FROM_BUFFER(corr_buf));
  az_iot_span_writer_append_u32(&corr_writer, rid);
  if (az_iot_span_writer_end_str(&corr_writer, &corr_len) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(topic));
  if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
  {
    /* Next: publish to "ih/{device_id}/srv/twin/get" with correlation_data */
    az_iot_span_writer_append_str(&writer, "ih/");
    az_iot_span_writer_append_str(&writer, az_iot_connection_client__device_id(TI(twin).conn));
    az_iot_span_writer_append_str(&writer, "/srv/twin/get");
  }
  else
  {
    /* Classic: "$iothub/twin/GET/?$rid=<n>" */
    az_iot_span_writer_append_str(&writer, "$iothub/twin/GET/?$rid=");
    az_iot_span_writer_append_u32(&writer, rid);
  }
  if (az_iot_span_writer_end_str(&writer, NULL) != AZ_IOT_OK)
    return AZ_IOT_ERR_NOT_SUPPORTED;

  /* Reserve the slot before publish. */
  TI(twin).pending[idx].in_use = true;
  TI(twin).pending[idx].rid = rid;
  TI(twin).pending[idx].kind = TWIN_PENDING_GET;
  TI(twin).pending[idx].cb.get_cb = cb;
  TI(twin).pending[idx].user_ctx = user_ctx;

  az_iot_mqtt_message out = { 0 };
  out.topic = topic;
  out.qos = (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT) ? AZ_IOT_MQTT_QOS_1
                                                                   : AZ_IOT_MQTT_QOS_0;

  if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
  {
    out.correlation_data = (const uint8_t*)corr_buf;
    out.correlation_data_len = (size_t)corr_len;
  }

  az_iot_result r = az_iot_connection_client__publish(TI(twin).conn, &out, NULL, NULL);
  if (r != AZ_IOT_OK)
  {
    TI(twin).pending[idx].in_use = false;
    TI(twin).pending[idx].kind = TWIN_PENDING_NONE;
  }
  return r;
}

az_iot_result az_iot_twin_client_patch_reported(
    az_iot_twin_client* twin,
    const uint8_t* patch,
    size_t patch_len,
    az_iot_twin_patch_ack_callback cb,
    void* user_ctx)
{
  if (!twin)
    return AZ_IOT_ERR_INVALID_ARG;
  if (patch_len > 0 && patch == NULL)
    return AZ_IOT_ERR_INVALID_ARG;

  int idx = alloc_pending(twin);
  if (idx < 0)
    return AZ_IOT_ERR_NOT_SUPPORTED;

  uint32_t rid = TI(twin).next_rid++;
  if (TI(twin).next_rid == 0)
    TI(twin).next_rid = 1;

  const az_iot_protocol_profile* profile = az_iot_connection_client__profile(TI(twin).conn);

  char topic[AZ_IOT_TWIN_TOPIC_MAX];

  char corr_buf[16];
  size_t corr_len = 0;
  az_iot_span_writer corr_writer;
  az_iot_span_writer_init(&corr_writer, AZ_SPAN_FROM_BUFFER(corr_buf));
  az_iot_span_writer_append_u32(&corr_writer, rid);
  if (az_iot_span_writer_end_str(&corr_writer, &corr_len) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(topic));
  if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
  {
    az_iot_span_writer_append_str(&writer, "ih/");
    az_iot_span_writer_append_str(&writer, az_iot_connection_client__device_id(TI(twin).conn));
    az_iot_span_writer_append_str(&writer, "/srv/twin/reported");
  }
  else
  {
    az_iot_span_writer_append_str(&writer, "$iothub/twin/PATCH/properties/reported/?$rid=");
    az_iot_span_writer_append_u32(&writer, rid);
  }
  if (az_iot_span_writer_end_str(&writer, NULL) != AZ_IOT_OK)
    return AZ_IOT_ERR_NOT_SUPPORTED;

  TI(twin).pending[idx].in_use = true;
  TI(twin).pending[idx].rid = rid;
  TI(twin).pending[idx].kind = TWIN_PENDING_PATCH;
  TI(twin).pending[idx].cb.patch_cb = cb;
  TI(twin).pending[idx].user_ctx = user_ctx;

  az_iot_mqtt_message out = { 0 };
  out.topic = topic;
  out.payload = patch;
  out.payload_len = patch_len;
  out.qos = (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT) ? AZ_IOT_MQTT_QOS_1
                                                                   : AZ_IOT_MQTT_QOS_0;

  if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
  {
    out.correlation_data = (const uint8_t*)corr_buf;
    out.correlation_data_len = (size_t)corr_len;
  }

  az_iot_result r = az_iot_connection_client__publish(TI(twin).conn, &out, NULL, NULL);
  if (r != AZ_IOT_OK)
  {
    TI(twin).pending[idx].in_use = false;
    TI(twin).pending[idx].kind = TWIN_PENDING_NONE;
  }
  return r;
}

az_iot_result az_iot_twin_client_subscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
  if (!twin)
    return AZ_IOT_ERR_INVALID_ARG;
  return desired_subscribe(
      twin, TI(twin).desired_app_subs, AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS, cb, user_ctx);
}

az_iot_result az_iot_twin_client__subscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
  if (!twin)
    return AZ_IOT_ERR_INVALID_ARG;
  return desired_subscribe(
      twin, TI(twin).desired_feature_subs, AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS, cb, user_ctx);
}

az_iot_result az_iot_twin_client_unsubscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
  if (!twin || !cb)
    return AZ_IOT_ERR_INVALID_ARG;
  if (TI(twin).dispatching)
    return AZ_IOT_ERR_BUSY;

  /* Search both pools; an entry matches on (cb, user_ctx). */
  for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS; ++i)
  {
    az_iot_twin_desired_sub* s = &TI(twin).desired_feature_subs[i];
    if (s->in_use && s->cb == cb && s->user_ctx == user_ctx)
    {
      s->in_use = false;
      s->cb = NULL;
      s->user_ctx = NULL;
      return AZ_IOT_OK;
    }
  }
  for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS; ++i)
  {
    az_iot_twin_desired_sub* s = &TI(twin).desired_app_subs[i];
    if (s->in_use && s->cb == cb && s->user_ctx == user_ctx)
    {
      s->in_use = false;
      s->cb = NULL;
      s->user_ctx = NULL;
      return AZ_IOT_OK;
    }
  }
  return AZ_IOT_ERR_INVALID_ARG;
}
