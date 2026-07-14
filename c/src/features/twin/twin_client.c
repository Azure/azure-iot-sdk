// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_twin_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/connection_client_internal.h"
#include "internal/protocol_profile.h"
#include "internal/twin_client_internal.h"

#define AZ_IOT_TWIN_TOPIC_MAX   192

/* The pending slot kind values stored in _internal.pending[].kind */
#define TWIN_PENDING_NONE  0
#define TWIN_PENDING_GET   1
#define TWIN_PENDING_PATCH 2

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
        if (!TI(t).pending[i].in_use) return i;
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
    if (!t || !cb) return AZ_IOT_ERR_INVALID_ARG;
    if (TI(t).dispatching) return AZ_IOT_ERR_BUSY;

    int free_slot = -1;
    for (size_t i = 0; i < pool_cap; ++i)
    {
        if (pool[i].in_use)
        {
            if (pool[i].cb == cb && pool[i].user_ctx == user_ctx) return AZ_IOT_OK;
        }
        else if (free_slot < 0)
        {
            free_slot = (int)i;
        }
    }
    if (free_slot < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    pool[free_slot].cb       = cb;
    pool[free_slot].user_ctx = user_ctx;
    pool[free_slot].in_use   = true;
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
        if (s->in_use && s->cb) s->cb(payload, payload_len, version, s->user_ctx);
    }
    for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS; ++i)
    {
        az_iot_twin_desired_sub* s = &TI(t).desired_app_subs[i];
        if (s->in_use && s->cb) s->cb(payload, payload_len, version, s->user_ctx);
    }
    TI(t).dispatching = false;
}

/* Find "<key>=<value>" inside a query string (starting at '?'); writes value
 * into out (NUL-terminated). Returns true if found. */
static bool query_value(const char* qs, const char* key,
                        char* out, size_t cap)
{
    if (!qs || !key) return false;
    size_t key_len = strlen(key);
    const char* p = qs;
    while (p && *p)
    {
        /* skip past '?' or '&' */
        if (*p == '?' || *p == '&') p++;
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=')
        {
            const char* val = p + key_len + 1;
            const char* end = strchr(val, '&');
            size_t n = end ? (size_t)(end - val) : strlen(val);
            if (n + 1 > cap) return false;
            memcpy(out, val, n);
            out[n] = '\0';
            return true;
        }
        p = strchr(p, '&');
    }
    return false;
}

/* Map an HTTP-style status (200, 204, 404, 429, 5xx) to an az_iot result code. */
static az_iot_result status_to_result(int status)
{
    if (status >= 200 && status < 300) return AZ_IOT_OK;
    if (status == 404) return AZ_IOT_ERR_INVALID_ARG;
    if (status == 429) return AZ_IOT_ERR_NOT_SUPPORTED;
    return AZ_IOT_ERR_MQTT;
}

/* ------------------------------------------------------------------------- */
/* dispatch handlers — Classic                                               */
/* ------------------------------------------------------------------------- */

static void on_twin_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
    az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
    if (!t || !msg || !msg->topic) return;

    /* Topic: "$iothub/twin/res/<status>/?$rid=<n>[&$version=<v>]". */
    static const char k_prefix[] = "$iothub/twin/res/";
    size_t prefix_len = sizeof(k_prefix) - 1;
    if (strncmp(msg->topic, k_prefix, prefix_len) != 0) return;
    const char* status_str = msg->topic + prefix_len;
    char* qmark = strchr(status_str, '?');
    if (!qmark) return;
    /* Parse status code (decimal) up to '/' or '?'. */
    int status = 0;
    for (const char* p = status_str; p < qmark && *p && *p != '/'; ++p)
    {
        if (*p < '0' || *p > '9') return;
        status = status * 10 + (*p - '0');
    }
    char rid_buf[16];
    if (!query_value(qmark, "$rid", rid_buf, sizeof(rid_buf))) return;
    uint32_t rid = (uint32_t)strtoul(rid_buf, NULL, 10);

    int idx = find_pending_by_rid(t, rid);
    if (idx < 0) return; /* stale or unknown rid */

    az_iot_result r = status_to_result(status);
    if (TI(t).pending[idx].kind == TWIN_PENDING_GET)
    {
        az_iot_twin_get_callback cb = TI(t).pending[idx].cb.get_cb;
        void* ctx = TI(t).pending[idx].user_ctx;
        TI(t).pending[idx].in_use = false;
        TI(t).pending[idx].kind = TWIN_PENDING_NONE;
        if (cb) cb(r, msg->payload, msg->payload_len, ctx);
    }
    else if (TI(t).pending[idx].kind == TWIN_PENDING_PATCH)
    {
        az_iot_twin_patch_ack_callback cb = TI(t).pending[idx].cb.patch_cb;
        void* ctx = TI(t).pending[idx].user_ctx;
        TI(t).pending[idx].in_use = false;
        TI(t).pending[idx].kind = TWIN_PENDING_NONE;
        if (cb) cb(r, ctx);
    }
    else
    {
        TI(t).pending[idx].in_use = false;
    }
}

static void on_twin_desired(void* user_ctx, const az_iot_mqtt_message* msg)
{
    az_iot_twin_client* t = (az_iot_twin_client*)user_ctx;
    if (!t || !msg || !msg->topic) return;

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
    if (!t || !msg) return;

    /* Match correlation_data to a pending rid */
    uint32_t rid = 0;
    if (msg->correlation_data && msg->correlation_data_len > 0)
    {
        char rid_buf[16];
        size_t n = msg->correlation_data_len < sizeof(rid_buf) - 1
            ? msg->correlation_data_len : sizeof(rid_buf) - 1;
        memcpy(rid_buf, msg->correlation_data, n);
        rid_buf[n] = '\0';
        rid = (uint32_t)strtoul(rid_buf, NULL, 10);
    }

    int idx = find_pending_by_rid(t, rid);
    if (idx < 0) return;

    if (TI(t).pending[idx].kind == TWIN_PENDING_GET)
    {
        az_iot_twin_get_callback cb = TI(t).pending[idx].cb.get_cb;
        void* ctx = TI(t).pending[idx].user_ctx;
        TI(t).pending[idx].in_use = false;
        TI(t).pending[idx].kind = TWIN_PENDING_NONE;
        if (cb) cb(AZ_IOT_OK, msg->payload, msg->payload_len, ctx);
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
    if (!t || !msg) return;

    uint32_t rid = 0;
    if (msg->correlation_data && msg->correlation_data_len > 0)
    {
        char rid_buf[16];
        size_t n = msg->correlation_data_len < sizeof(rid_buf) - 1
            ? msg->correlation_data_len : sizeof(rid_buf) - 1;
        memcpy(rid_buf, msg->correlation_data, n);
        rid_buf[n] = '\0';
        rid = (uint32_t)strtoul(rid_buf, NULL, 10);
    }

    int idx = find_pending_by_rid(t, rid);
    if (idx < 0) return;

    if (TI(t).pending[idx].kind == TWIN_PENDING_PATCH)
    {
        az_iot_twin_patch_ack_callback cb = TI(t).pending[idx].cb.patch_cb;
        void* ctx = TI(t).pending[idx].user_ctx;
        TI(t).pending[idx].in_use = false;
        TI(t).pending[idx].kind = TWIN_PENDING_NONE;
        if (cb) cb(AZ_IOT_OK, ctx);
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
    if (!t || !msg) return;

    /* Version could come from a user property; for now default to 0 */
    uint64_t version = 0;
    dispatch_desired(t, msg->payload, msg->payload_len, version);
}

/* ------------------------------------------------------------------------- */
/* public API                                                                 */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_twin_client_init(
    az_iot_twin_client* client,
    az_iot_connection_client* conn)
{
    if (client == NULL || conn == NULL)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }
    const az_iot_protocol_profile* profile =
        az_iot_connection_client__profile(conn);
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
        int n;
        az_iot_result r;

        /* Register handler for twin/get/response */
        n = snprintf(prefix, sizeof(prefix), "ih/%s/dev/twin/get/response", device_id);
        if (n < 0 || (size_t)n >= sizeof(prefix))
        {
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_INTERNAL;
        }
        r = az_iot_connection_client__register_inbound_handler(
            conn, prefix, on_twin_get_response_next, client);
        if (r != AZ_IOT_OK) { memset(client, 0, sizeof(*client)); return r; }

        r = az_iot_connection_client__add_subscription_on_connect(
            conn, prefix, AZ_IOT_MQTT_QOS_1);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return r;
        }

        /* Register handler for twin/reported/response */
        n = snprintf(prefix, sizeof(prefix), "ih/%s/dev/twin/reported/response", device_id);
        if (n < 0 || (size_t)n >= sizeof(prefix))
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
        r = az_iot_connection_client__add_subscription_on_connect(
            conn, prefix, AZ_IOT_MQTT_QOS_1);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return r;
        }

        /* Register handler for twin/desired */
        n = snprintf(filter, sizeof(filter), "ih/%s/dev/twin/desired", device_id);
        if (n < 0 || (size_t)n >= sizeof(filter))
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
        r = az_iot_connection_client__add_subscription_on_connect(
            conn, filter, AZ_IOT_MQTT_QOS_1);
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
        if (!profile->twin_response_topic_prefix ||
            !profile->twin_desired_topic_prefix)
        {
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_NOT_SUPPORTED;
        }

        az_iot_result r = az_iot_connection_client__register_inbound_handler(
            conn, profile->twin_response_topic_prefix, on_twin_response, client);
        if (r != AZ_IOT_OK) { memset(client, 0, sizeof(*client)); return r; }

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
        int n = snprintf(filter, sizeof(filter), "%s#", profile->twin_response_topic_prefix);
        if (n < 0 || (size_t)n >= sizeof(filter))
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_INTERNAL;
        }
        r = az_iot_connection_client__add_subscription_on_connect(
            conn, filter, AZ_IOT_MQTT_QOS_0);
        if (r != AZ_IOT_OK)
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return r;
        }

        n = snprintf(filter, sizeof(filter), "%s#", profile->twin_desired_topic_prefix);
        if (n < 0 || (size_t)n >= sizeof(filter))
        {
            (void)az_iot_connection_client__unregister_inbound_handlers(conn, client);
            memset(client, 0, sizeof(*client));
            return AZ_IOT_ERR_INTERNAL;
        }
        r = az_iot_connection_client__add_subscription_on_connect(
            conn, filter, AZ_IOT_MQTT_QOS_0);
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
    if (!client) return;
    (void)az_iot_connection_client__unregister_inbound_handlers(TI(client).conn, client);
    memset(client, 0, sizeof(*client));
}

az_iot_result az_iot_twin_client_get(
    az_iot_twin_client* twin, az_iot_twin_get_callback cb, void* user_ctx)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;

    int idx = alloc_pending(twin);
    if (idx < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    uint32_t rid = TI(twin).next_rid++;
    if (TI(twin).next_rid == 0) TI(twin).next_rid = 1; /* never reuse 0 */

    const az_iot_protocol_profile* profile =
        az_iot_connection_client__profile(TI(twin).conn);

    char topic[AZ_IOT_TWIN_TOPIC_MAX];
    int n;

    /* Correlation data (rid as ASCII string) */
    char corr_buf[16];
    int corr_len = snprintf(corr_buf, sizeof(corr_buf), "%u", (unsigned)rid);

    if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        /* Next: publish to "ih/{device_id}/srv/twin/get" with correlation_data */
        const char* device_id = az_iot_connection_client__device_id(TI(twin).conn);
        n = snprintf(topic, sizeof(topic), "ih/%s/srv/twin/get", device_id);
    }
    else
    {
        /* Classic: "$iothub/twin/GET/?$rid=<n>" */
        n = snprintf(topic, sizeof(topic), "$iothub/twin/GET/?$rid=%u", (unsigned)rid);
    }
    if (n < 0 || (size_t)n >= sizeof(topic)) return AZ_IOT_ERR_NOT_SUPPORTED;

    /* Reserve the slot before publish. */
    TI(twin).pending[idx].in_use    = true;
    TI(twin).pending[idx].rid       = rid;
    TI(twin).pending[idx].kind      = TWIN_PENDING_GET;
    TI(twin).pending[idx].cb.get_cb = cb;
    TI(twin).pending[idx].user_ctx  = user_ctx;

    az_iot_mqtt_message out = { 0 };
    out.topic = topic;
    out.qos   = (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
        ? AZ_IOT_MQTT_QOS_1 : AZ_IOT_MQTT_QOS_0;

    if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        out.correlation_data = (const uint8_t*)corr_buf;
        out.correlation_data_len = (size_t)corr_len;
    }

    az_iot_result r = az_iot_connection_client__publish(
        TI(twin).conn, &out, NULL, NULL);
    if (r != AZ_IOT_OK)
    {
        TI(twin).pending[idx].in_use = false;
        TI(twin).pending[idx].kind   = TWIN_PENDING_NONE;
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
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;
    if (patch_len > 0 && patch == NULL) return AZ_IOT_ERR_INVALID_ARG;

    int idx = alloc_pending(twin);
    if (idx < 0) return AZ_IOT_ERR_NOT_SUPPORTED;

    uint32_t rid = TI(twin).next_rid++;
    if (TI(twin).next_rid == 0) TI(twin).next_rid = 1;

    const az_iot_protocol_profile* profile =
        az_iot_connection_client__profile(TI(twin).conn);

    char topic[AZ_IOT_TWIN_TOPIC_MAX];
    int n;

    char corr_buf[16];
    int corr_len = snprintf(corr_buf, sizeof(corr_buf), "%u", (unsigned)rid);

    if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        const char* device_id = az_iot_connection_client__device_id(TI(twin).conn);
        n = snprintf(topic, sizeof(topic), "ih/%s/srv/twin/reported", device_id);
    }
    else
    {
        n = snprintf(topic, sizeof(topic),
                     "$iothub/twin/PATCH/properties/reported/?$rid=%u", (unsigned)rid);
    }
    if (n < 0 || (size_t)n >= sizeof(topic)) return AZ_IOT_ERR_NOT_SUPPORTED;

    TI(twin).pending[idx].in_use      = true;
    TI(twin).pending[idx].rid         = rid;
    TI(twin).pending[idx].kind        = TWIN_PENDING_PATCH;
    TI(twin).pending[idx].cb.patch_cb = cb;
    TI(twin).pending[idx].user_ctx    = user_ctx;

    az_iot_mqtt_message out = { 0 };
    out.topic       = topic;
    out.payload     = patch;
    out.payload_len = patch_len;
    out.qos         = (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
        ? AZ_IOT_MQTT_QOS_1 : AZ_IOT_MQTT_QOS_0;

    if (profile && profile->flavor == AZ_IOT_HUB_FLAVOR_NEXT)
    {
        out.correlation_data = (const uint8_t*)corr_buf;
        out.correlation_data_len = (size_t)corr_len;
    }

    az_iot_result r = az_iot_connection_client__publish(
        TI(twin).conn, &out, NULL, NULL);
    if (r != AZ_IOT_OK)
    {
        TI(twin).pending[idx].in_use = false;
        TI(twin).pending[idx].kind   = TWIN_PENDING_NONE;
    }
    return r;
}

az_iot_result az_iot_twin_client_subscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;
    return desired_subscribe(
        twin, TI(twin).desired_app_subs, AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS, cb, user_ctx);
}

az_iot_result az_iot_twin_client__subscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
    if (!twin) return AZ_IOT_ERR_INVALID_ARG;
    return desired_subscribe(
        twin, TI(twin).desired_feature_subs, AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS, cb, user_ctx);
}

az_iot_result az_iot_twin_client_unsubscribe_desired(
    az_iot_twin_client* twin,
    az_iot_twin_desired_callback cb,
    void* user_ctx)
{
    if (!twin || !cb) return AZ_IOT_ERR_INVALID_ARG;
    if (TI(twin).dispatching) return AZ_IOT_ERR_BUSY;

    /* Search both pools; an entry matches on (cb, user_ctx). */
    for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_FEATURE_SUBS; ++i)
    {
        az_iot_twin_desired_sub* s = &TI(twin).desired_feature_subs[i];
        if (s->in_use && s->cb == cb && s->user_ctx == user_ctx)
        {
            s->in_use = false; s->cb = NULL; s->user_ctx = NULL;
            return AZ_IOT_OK;
        }
    }
    for (size_t i = 0; i < AZ_IOT_TWIN_MAX_DESIRED_APP_SUBS; ++i)
    {
        az_iot_twin_desired_sub* s = &TI(twin).desired_app_subs[i];
        if (s->in_use && s->cb == cb && s->user_ctx == user_ctx)
        {
            s->in_use = false; s->cb = NULL; s->user_ctx = NULL;
            return AZ_IOT_OK;
        }
    }
    return AZ_IOT_ERR_INVALID_ARG;
}
